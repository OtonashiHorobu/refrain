// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/rfs.hpp>

#include <refrain/core/bytes.hpp>
#include <refrain/core/io.hpp>

#include <zstd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::data {
namespace {

using core::bytes::align_up;
using core::bytes::read_u16;
using core::bytes::read_u32;
using core::bytes::read_u64;

// ---------------------------------------------------------------------------
// Layout constants (mirror the RFS1 comment in <refrain/data/rfs.hpp>).
// ---------------------------------------------------------------------------

constexpr std::size_t header_version_offset{4};
constexpr std::size_t header_flags_offset{6};
constexpr std::size_t header_schema_hash_offset{8};
constexpr std::size_t header_record_count_offset{24};
constexpr std::size_t header_reserved_28_offset{28};
constexpr std::size_t header_total_size_offset{32};
constexpr std::size_t header_data_hash_offset{40};
constexpr std::size_t header_reserved_72_offset{72};
constexpr std::size_t header_reserved_to{rfs_header_bytes};

constexpr std::size_t record_key_hash_offset{0};
constexpr std::size_t record_key_offset_offset{8};
constexpr std::size_t record_key_length_offset{12};
constexpr std::size_t record_flags_offset{14};
constexpr std::size_t record_data_offset_offset{16};
constexpr std::size_t record_data_len_offset{24};

// ---------------------------------------------------------------------------
// Little-endian byte accessors (the format is endian-fixed; no memcpy of host
// integers so the reader/writer work on any host byte order).
// ---------------------------------------------------------------------------

constexpr auto write_u16 = core::bytes::write_le<std::uint16_t>;

constexpr auto write_u32 = core::bytes::write_le<std::uint32_t>;

constexpr auto write_u64 = core::bytes::write_le<std::uint64_t>;

[[nodiscard]] auto bytes_equal(std::span<const std::byte> bytes,
                               std::size_t offset,
                               std::string_view expected) noexcept -> bool {
    if (offset > bytes.size() || expected.size() > bytes.size() - offset) {
        return false;
    }
    for (std::size_t index{0}; index < expected.size(); ++index) {
        if (std::to_integer<char>(bytes[offset + index]) != expected[index]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] auto is_zeroed(std::span<const std::byte> bytes, std::size_t from,
                             std::size_t to) noexcept -> bool {
    for (std::size_t offset{from}; offset < to; ++offset) {
        if (std::to_integer<unsigned int>(bytes[offset]) != 0U) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] auto invalid_argument_error(std::string message) -> error {
    return make_error(error_code::invalid_argument, message);
}

[[nodiscard]] auto parse_error(std::string message) -> error {
    return make_error(error_code::parse_error, message);
}

/// Key bytes viewed as a span (keys are opaque UTF-8/binary for hashing).
[[nodiscard]] auto key_bytes(std::string_view key) noexcept
    -> std::span<const std::byte> {
    return std::as_bytes(std::span<const char>{key.data(), key.size()});
}

/// First 8 bytes of BLAKE3(key), interpreted little-endian (fast-reject field).
[[nodiscard]] auto key_hash_of(std::string_view key) noexcept -> std::uint64_t {
    const auto digest{blake3(key_bytes(key))};
    std::uint64_t value{0};
    for (std::size_t index{0}; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(
                     std::to_integer<unsigned int>(digest[index]))
                 << (8U * index);
    }
    return value;
}

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

/// One input after optional compression. `source` views the caller's payload
/// while it is stored verbatim and `compressed` owns it otherwise.
struct prepared_record {
    std::string key{};
    std::span<const std::byte> source{};
    std::vector<std::byte> compressed{};
    std::uint64_t data_len{0};
    std::uint64_t stored_len{0};
    std::uint64_t key_offset{0};
    std::uint16_t flags{0};
};

[[nodiscard]] auto build_rfs_impl(std::span<const rfs_input> records,
                                  const rfs_schema_hash &schema_hash,
                                  const rfs_write_options &options)
    -> result<std::vector<std::byte>> {
    if (records.size() > rfs_max_records) {
        return std::unexpected{invalid_argument_error(
            "RFS1 v1 supports at most " + std::to_string(rfs_max_records) +
            " records")};
    }
    if (options.zstd_level < 1 || options.zstd_level > 19) {
        return std::unexpected{invalid_argument_error(
            "zstd compression level must be in [1, 19]")};
    }

    std::vector<prepared_record> prepared;
    prepared.reserve(records.size());
    std::uint64_t total_uncompressed{0};
    for (const auto &input : records) {
        if (input.key.empty()) {
            return std::unexpected{
                invalid_argument_error("RFS1 record key must not be empty")};
        }
        if (input.key.size() > rfs_max_key_bytes) {
            return std::unexpected{invalid_argument_error(
                "RFS1 record key exceeds the " +
                std::to_string(rfs_max_key_bytes) + "-byte limit")};
        }
        if (input.data.size() > rfs_max_record_bytes) {
            return std::unexpected{invalid_argument_error(
                "RFS1 record '" + input.key +
                "' exceeds the 4 GiB per-record ceiling")};
        }
        if (input.data.size() >
            std::numeric_limits<std::uint64_t>::max() - total_uncompressed) {
            return std::unexpected{invalid_argument_error(
                "RFS1 uncompressed size total overflow")};
        }
        total_uncompressed += input.data.size();
        if (total_uncompressed > rfs_max_total_uncompressed_bytes) {
            return std::unexpected{invalid_argument_error(
                "RFS1 records declare more than " +
                std::to_string(rfs_max_total_uncompressed_bytes) +
                " uncompressed bytes in total")};
        }
        prepared.push_back(prepared_record{
            .key = input.key,
            .source = input.data,
            .compressed = {},
            .data_len = input.data.size(),
            .stored_len = 0,
            .key_offset = 0,
            .flags = 0,
        });
    }

    // Deterministic order: records are stored sorted by key bytes; the writer
    // never depends on the caller's input order.
    std::sort(prepared.begin(), prepared.end(),
              [](const prepared_record &lhs, const prepared_record &rhs) {
                  return lhs.key < rhs.key;
              });
    for (std::size_t index{1}; index < prepared.size(); ++index) {
        if (prepared[index - 1U].key == prepared[index].key) {
            return std::unexpected{invalid_argument_error(
                "duplicate RFS1 record key: '" + prepared[index].key + "'")};
        }
    }

    std::uint64_t total_key_bytes{0};
    for (auto &record : prepared) {
        if (total_key_bytes > static_cast<std::uint64_t>(
                                  std::numeric_limits<std::uint32_t>::max()) -
                                  record.key.size()) {
            return std::unexpected{invalid_argument_error(
                "RFS1 key arena exceeds the v1 32-bit offset space")};
        }
        record.key_offset = total_key_bytes;
        total_key_bytes += record.key.size();

        if (record.data_len == 0) {
            // Empty records are stored verbatim (zero bytes, no zstd frame).
            record.source = {};
            record.stored_len = 0;
            record.flags = 0;
            continue;
        }
        if (!options.compress ||
            record.data_len < options.compression_threshold) {
            record.stored_len = record.data_len;
            continue;
        }

        const auto bound{
            ZSTD_compressBound(static_cast<std::size_t>(record.data_len))};
        record.compressed.resize(bound);
        const auto written{
            ZSTD_compress(record.compressed.data(), bound, record.source.data(),
                          record.source.size(), options.zstd_level)};
        if (ZSTD_isError(written) != 0U) {
            return std::unexpected{make_error(
                error_code::io_error, "zstd failed to compress RFS1 record '" +
                                          record.key +
                                          "': " + ZSTD_getErrorName(written))};
        }
        if (written < record.data_len) {
            // zstd only replaces the payload when it actually saves space.
            record.source = std::span<const std::byte>{
                record.compressed.data(), record.compressed.size()};
            record.stored_len = written;
            record.flags = rfs_flag_compressed;
        } else {
            record.compressed.clear();
            record.compressed.shrink_to_fit();
            record.stored_len = record.data_len;
            record.flags = 0;
        }
    }

    const auto record_count{static_cast<std::uint64_t>(prepared.size())};
    const auto table_end{static_cast<std::uint64_t>(rfs_header_bytes) +
                         record_count *
                             static_cast<std::uint64_t>(rfs_record_bytes)};
    const auto key_arena_end{table_end + total_key_bytes};
    const auto data_start{align_up(
        key_arena_end, static_cast<std::uint64_t>(rfs_data_alignment))};

    std::uint64_t data_size{0};
    for (const auto &record : prepared) {
        if (record.stored_len >
            std::numeric_limits<std::uint64_t>::max() - data_size) {
            return std::unexpected{
                invalid_argument_error("RFS1 data area size overflow")};
        }
        data_size += record.stored_len;
    }
    const auto total_size{data_start + data_size};
    if (total_size < data_start ||
        total_size > std::numeric_limits<std::size_t>::max()) {
        return std::unexpected{
            invalid_argument_error("RFS1 container is too large to build")};
    }

    // `std::vector<std::byte>` value-initializes, so the header reserved area
    // and the key-arena alignment padding are already zero.
    std::vector<std::byte> container(static_cast<std::size_t>(total_size));
    const auto view{std::span<std::byte>{container.data(), container.size()}};

    // Header.
    constexpr std::string_view magic{"RFS1"};
    for (std::size_t index{0}; index < magic.size(); ++index) {
        view[index] = static_cast<std::byte>(magic[index]);
    }
    write_u16(view, header_version_offset, rfs_format_version);
    write_u16(view, header_flags_offset, 0);
    for (std::size_t index{0}; index < schema_hash.size(); ++index) {
        view[header_schema_hash_offset + index] = schema_hash[index];
    }
    write_u32(view, header_record_count_offset,
              static_cast<std::uint32_t>(record_count));
    write_u32(view, header_reserved_28_offset, 0);
    write_u64(view, header_total_size_offset, total_size);

    // Record table, key arena and data region.
    std::uint64_t running_data{data_start};
    for (std::size_t index{0}; index < prepared.size(); ++index) {
        const auto &record = prepared[index];
        const auto record_offset{static_cast<std::size_t>(
            static_cast<std::uint64_t>(rfs_header_bytes) +
            static_cast<std::uint64_t>(index) *
                static_cast<std::uint64_t>(rfs_record_bytes))};
        write_u64(view, record_offset + record_key_hash_offset,
                  key_hash_of(record.key));
        write_u32(view, record_offset + record_key_offset_offset,
                  static_cast<std::uint32_t>(record.key_offset));
        write_u16(view, record_offset + record_key_length_offset,
                  static_cast<std::uint16_t>(record.key.size()));
        write_u16(view, record_offset + record_flags_offset, record.flags);
        write_u64(view, record_offset + record_data_offset_offset,
                  running_data);
        write_u64(view, record_offset + record_data_len_offset,
                  record.data_len);

        std::memcpy(view.data() + static_cast<std::size_t>(table_end) +
                        static_cast<std::size_t>(record.key_offset),
                    record.key.data(), record.key.size());
        if (record.stored_len > 0) {
            std::memcpy(view.data() + static_cast<std::size_t>(running_data),
                        record.source.data(),
                        static_cast<std::size_t>(record.stored_len));
        }
        running_data += record.stored_len;
    }

    // data_hash = BLAKE3(record table || key arena || padding || data region);
    // it is the stable identity of the container body.
    const auto digest{blake3(
        std::span<const std::byte>{container.data() + rfs_header_bytes,
                                   container.size() - rfs_header_bytes})};
    for (std::size_t index{0}; index < digest.size(); ++index) {
        view[header_data_hash_offset + index] = digest[index];
    }
    return container;
}

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------

/// Record-table fields kept while validating the container.
struct record_row {
    std::uint64_t key_hash{0};
    std::uint32_t key_offset{0};
    std::uint16_t key_length{0};
    std::uint16_t flags{0};
    std::uint64_t data_offset{0};
    std::uint64_t data_length{0};
    std::uint64_t stored_length{0};
};

struct parsed_container {
    std::vector<rfs_entry> entries{};
    rfs_schema_hash schema_hash{};
    digest32 data_hash{};
    std::uint64_t file_size{0};
};

[[nodiscard]] auto parse_container(std::span<const std::byte> bytes)
    -> result<parsed_container> {
    if (bytes.size() < rfs_header_bytes) {
        return std::unexpected{parse_error("RFS1 header is truncated: need " +
                                           std::to_string(rfs_header_bytes) +
                                           " bytes, got " +
                                           std::to_string(bytes.size()) + ")")};
    }
    if (!bytes_equal(bytes, 0, "RFS1")) {
        return std::unexpected{parse_error("bad RFS1 magic")};
    }
    if (read_u16(bytes, header_version_offset) != rfs_format_version) {
        return std::unexpected{
            parse_error("unsupported RFS1 version (expected " +
                        std::to_string(rfs_format_version) + ")")};
    }
    if (read_u16(bytes, header_flags_offset) != 0U) {
        return std::unexpected{
            parse_error("RFS1 v1 header flags must be zero")};
    }
    if (read_u32(bytes, header_reserved_28_offset) != 0U) {
        return std::unexpected{
            parse_error("RFS1 v1 reserved header fields must be zero")};
    }
    if (!is_zeroed(bytes, header_reserved_72_offset, header_reserved_to)) {
        return std::unexpected{
            parse_error("RFS1 v1 reserved header area must be zero")};
    }

    const auto record_count{read_u32(bytes, header_record_count_offset)};
    if (record_count > rfs_max_records) {
        return std::unexpected{
            parse_error("RFS1 record count exceeds the v1 limit (" +
                        std::to_string(rfs_max_records) + ")")};
    }
    const auto file_size{static_cast<std::uint64_t>(bytes.size())};
    const auto table_end{static_cast<std::uint64_t>(rfs_header_bytes) +
                         static_cast<std::uint64_t>(record_count) *
                             static_cast<std::uint64_t>(rfs_record_bytes)};
    if (table_end > file_size) {
        return std::unexpected{parse_error("RFS1 record table is truncated")};
    }
    const auto total_size{read_u64(bytes, header_total_size_offset)};
    if (total_size != file_size) {
        return std::unexpected{
            parse_error("RFS1 total size does not match the header (truncated "
                        "or trailing bytes)")};
    }

    // Integrity gate: verify the body hash before the (more expensive)
    // per-record semantic work, so ordinary bit rot reports one clear cause.
    digest32 stored_hash{};
    std::copy_n(bytes.begin() + header_data_hash_offset, stored_hash.size(),
                stored_hash.begin());
    const auto computed_hash{blake3(bytes.subspan(rfs_header_bytes))};
    if (computed_hash != stored_hash) {
        return std::unexpected{parse_error(
            "RFS1 data hash mismatch (record table or data region is "
            "corrupted)")};
    }

    parsed_container parsed;
    parsed.data_hash = stored_hash;
    parsed.file_size = file_size;
    for (std::size_t index{0}; index < parsed.schema_hash.size(); ++index) {
        parsed.schema_hash[index] = bytes[header_schema_hash_offset + index];
    }

    std::vector<record_row> rows(record_count);
    std::uint64_t running_key_bytes{0};
    for (std::uint32_t index{0}; index < record_count; ++index) {
        const auto record_offset{static_cast<std::size_t>(
            static_cast<std::uint64_t>(rfs_header_bytes) +
            static_cast<std::uint64_t>(index) *
                static_cast<std::uint64_t>(rfs_record_bytes))};
        auto &row = rows[index];
        row.key_hash = read_u64(bytes, record_offset + record_key_hash_offset);
        row.key_offset =
            read_u32(bytes, record_offset + record_key_offset_offset);
        row.key_length =
            read_u16(bytes, record_offset + record_key_length_offset);
        row.flags = read_u16(bytes, record_offset + record_flags_offset);
        row.data_offset =
            read_u64(bytes, record_offset + record_data_offset_offset);
        row.data_length =
            read_u64(bytes, record_offset + record_data_len_offset);

        if ((row.flags & ~rfs_flag_compressed) != 0U) {
            return std::unexpected{
                parse_error("RFS1 record uses reserved flag bits")};
        }
        if (row.key_length == 0U || row.key_length > rfs_max_key_bytes) {
            return std::unexpected{
                parse_error("RFS1 record key length is out of range")};
        }
        // Canonical key arena: keys are packed back-to-back in table order.
        if (row.key_offset != running_key_bytes) {
            return std::unexpected{parse_error(
                "RFS1 key arena is not canonical (key offsets are not "
                "contiguous)")};
        }
        if (running_key_bytes > static_cast<std::uint64_t>(
                                    std::numeric_limits<std::uint32_t>::max()) -
                                    row.key_length) {
            return std::unexpected{
                parse_error("RFS1 key arena exceeds the v1 32-bit offset "
                            "space")};
        }
        running_key_bytes += row.key_length;
    }

    const auto key_arena_end{table_end + running_key_bytes};
    if (key_arena_end > file_size) {
        return std::unexpected{parse_error("RFS1 key arena is truncated")};
    }
    const auto data_start{align_up(
        key_arena_end, static_cast<std::uint64_t>(rfs_data_alignment))};
    if (data_start > file_size) {
        return std::unexpected{
            parse_error("RFS1 data region starts past the end of the file")};
    }
    if (!is_zeroed(bytes, static_cast<std::size_t>(key_arena_end),
                   static_cast<std::size_t>(data_start))) {
        return std::unexpected{
            parse_error("RFS1 key-arena alignment padding must be zero")};
    }

    if (record_count == 0U) {
        if (data_start != file_size) {
            return std::unexpected{parse_error(
                "empty RFS1 container has an unexpected data region")};
        }
        return parsed;
    }

    // Data ranges: payloads are contiguous in record (sorted key) order. The
    // stored size of a compressed record follows from the next record's offset
    // (the file end for the last record), which is why the format needs no
    // separate stored-length field.
    std::uint64_t total_uncompressed{0};
    for (std::uint32_t index{0}; index < record_count; ++index) {
        auto &row = rows[index];
        if (row.data_offset < data_start || row.data_offset > file_size) {
            return std::unexpected{
                parse_error("RFS1 record data range is outside the data "
                            "region")};
        }
        if (index == 0U) {
            if (row.data_offset != data_start) {
                return std::unexpected{parse_error(
                    "RFS1 first record payload does not start at the data "
                    "offset")};
            }
        } else {
            const auto &previous = rows[index - 1U];
            if (row.data_offset < previous.data_offset) {
                return std::unexpected{parse_error(
                    "RFS1 record payloads are not in record order")};
            }
            if ((previous.flags & rfs_flag_compressed) == 0U) {
                if (previous.data_offset + previous.data_length !=
                    row.data_offset) {
                    return std::unexpected{parse_error(
                        "RFS1 stored record payloads are not contiguous")};
                }
            } else if (row.data_offset == previous.data_offset) {
                return std::unexpected{parse_error(
                    "RFS1 compressed record has no stored payload")};
            }
        }
        if (row.data_length > rfs_max_record_bytes) {
            return std::unexpected{parse_error(
                "RFS1 record declares an uncompressed size above the 4 GiB "
                "ceiling")};
        }
        if (row.data_length >
            std::numeric_limits<std::uint64_t>::max() - total_uncompressed) {
            return std::unexpected{
                parse_error("RFS1 uncompressed size total overflow")};
        }
        total_uncompressed += row.data_length;
        if (total_uncompressed > rfs_max_total_uncompressed_bytes) {
            return std::unexpected{
                parse_error("RFS1 container declares more than " +
                            std::to_string(rfs_max_total_uncompressed_bytes) +
                            " uncompressed bytes in total")};
        }
    }

    // Derive the stored length of every payload and validate it against the
    // declared uncompressed size (uncompressed records must match exactly; a
    // compressed record must have a non-empty frame).
    for (std::uint32_t index{0}; index < record_count; ++index) {
        auto &row = rows[index];
        const auto next_offset{index + 1U < record_count
                                   ? rows[index + 1U].data_offset
                                   : file_size};
        if (next_offset < row.data_offset) {
            return std::unexpected{
                parse_error("RFS1 record payloads are not in record order")};
        }
        row.stored_length = next_offset - row.data_offset;
        if ((row.flags & rfs_flag_compressed) != 0U) {
            if (row.data_length == 0U) {
                return std::unexpected{parse_error(
                    "RFS1 compressed record declares an empty payload")};
            }
            if (row.stored_length == 0U) {
                return std::unexpected{parse_error(
                    "RFS1 compressed record has no stored payload")};
            }
        } else if (row.stored_length != row.data_length) {
            return std::unexpected{
                parse_error("RFS1 stored record payload size mismatch")};
        }
    }

    // Keys: canonical arena, strictly increasing byte order (sorted + unique)
    // and a trustworthy fast-reject hash.
    parsed.entries.reserve(record_count);
    for (std::uint32_t index{0}; index < record_count; ++index) {
        const auto &row = rows[index];
        const auto key_start{static_cast<std::size_t>(
            table_end + static_cast<std::uint64_t>(row.key_offset))};
        std::string key{
            reinterpret_cast<const char *>(bytes.data() + key_start),
            row.key_length};
        if (index > 0U && !(parsed.entries.back().key < key)) {
            return std::unexpected{parse_error(
                "RFS1 record keys are not strictly sorted and unique")};
        }
        if (key_hash_of(key) != row.key_hash) {
            return std::unexpected{
                parse_error("RFS1 record key hash mismatch")};
        }
        parsed.entries.push_back(rfs_entry{
            .key = std::move(key),
            .data_offset = row.data_offset,
            .data_len = row.data_length,
            .stored_len = row.stored_length,
            .flags = row.flags,
        });
    }
    return parsed;
}

} // namespace

auto build_rfs(std::span<const rfs_input> records,
               const rfs_schema_hash &schema_hash,
               const rfs_write_options &options)
    -> result<std::vector<std::byte>> {
    try {
        return build_rfs_impl(records, schema_hash, options);
    } catch (const std::bad_alloc &) {
        return std::unexpected{
            make_error(error_code::io_error,
                       "out of memory while building an RFS1 container")};
    }
}

auto write_rfs(std::string_view path, std::span<const rfs_input> records,
               const rfs_schema_hash &schema_hash,
               const rfs_write_options &options) -> status {
    auto container{build_rfs(records, schema_hash, options)};
    if (!container) {
        return std::unexpected{std::move(container.error())};
    }
    // Atomic replace (tmp + fsync + rename): readers see the old or the new
    // container, never a torn file.
    return core::io::atomic_write_file(path, *container);
}

auto rfs_reader::open(std::span<const std::byte> bytes) -> result<rfs_reader> {
    auto parsed{parse_container(bytes)};
    if (!parsed) {
        return std::unexpected{std::move(parsed.error())};
    }
    rfs_reader reader;
    reader.bytes_ = bytes;
    reader.entries_ = std::move(parsed->entries);
    reader.schema_hash_ = parsed->schema_hash;
    reader.data_hash_ = parsed->data_hash;
    reader.file_size_ = parsed->file_size;
    reader.header_valid_ = true;
    return reader;
}

auto rfs_reader::open_file(std::string_view path) -> result<rfs_reader> {
    auto mapping{platform::mapped_file::open(path)};
    if (!mapping) {
        return std::unexpected{std::move(mapping.error())};
    }
    // Qualified on purpose: keeps the platform guard's bare-OS-call heuristic
    // from flagging this static factory call as the POSIX `::open` syscall.
    auto reader{rfs_reader::open(mapping->data())};
    if (!reader) {
        return std::unexpected{std::move(reader.error())};
    }
    reader->mapping_ =
        std::make_shared<const platform::mapped_file>(std::move(*mapping));
    return reader;
}

auto rfs_reader::find(std::string_view key) const noexcept
    -> const rfs_entry * {
    const auto match{
        std::lower_bound(entries_.begin(), entries_.end(), key,
                         [](const rfs_entry &entry, std::string_view wanted) {
                             return std::string_view{entry.key} < wanted;
                         })};
    if (match == entries_.end() || std::string_view{match->key} != key) {
        return nullptr;
    }
    return &*match;
}

auto rfs_reader::view(const rfs_entry &entry) const
    -> result<std::span<const std::byte>> {
    if (!header_valid_) {
        return std::unexpected{
            parse_error("RFS reader is not attached to a container")};
    }
    if (entry.is_compressed()) {
        return std::unexpected{parse_error(
            "record '" + entry.key +
            "' is zstd-compressed and has no zero-copy view; use read()")};
    }
    if (entry.data_offset > file_size_ ||
        entry.stored_len > file_size_ - entry.data_offset) {
        return std::unexpected{parse_error("record '" + entry.key +
                                           "' data range is outside the "
                                           "container")};
    }
    if (entry.stored_len != entry.data_len) {
        return std::unexpected{parse_error("record '" + entry.key +
                                           "' stored/uncompressed size "
                                           "mismatch")};
    }
    return bytes_.subspan(static_cast<std::size_t>(entry.data_offset),
                          static_cast<std::size_t>(entry.stored_len));
}

auto rfs_reader::view(std::string_view key) const
    -> result<std::span<const std::byte>> {
    const auto *entry{find(key)};
    if (entry == nullptr) {
        return std::unexpected{
            make_error(error_code::not_found,
                       "no RFS record named '" + std::string{key} + "'")};
    }
    return view(*entry);
}

auto rfs_reader::read(const rfs_entry &entry) const
    -> result<std::vector<std::byte>> {
    if (!header_valid_) {
        return std::unexpected{
            parse_error("RFS reader is not attached to a container")};
    }
    if (entry.data_offset > file_size_ ||
        entry.stored_len > file_size_ - entry.data_offset) {
        return std::unexpected{parse_error("record '" + entry.key +
                                           "' data range is outside the "
                                           "container")};
    }
    if (!entry.is_compressed()) {
        if (entry.stored_len != entry.data_len) {
            return std::unexpected{parse_error("record '" + entry.key +
                                               "' stored/uncompressed size "
                                               "mismatch")};
        }
        const auto payload{
            bytes_.subspan(static_cast<std::size_t>(entry.data_offset),
                           static_cast<std::size_t>(entry.stored_len))};
        return std::vector<std::byte>{payload.begin(), payload.end()};
    }

    if (entry.data_len == 0U) {
        return std::unexpected{parse_error("compressed record '" + entry.key +
                                           "' declares an empty payload")};
    }
    if (entry.data_len > rfs_max_record_bytes ||
        entry.data_len > std::numeric_limits<std::size_t>::max()) {
        return std::unexpected{parse_error(
            "record '" + entry.key +
            "' declares an uncompressed size above the supported ceiling")};
    }
    std::vector<std::byte> payload(static_cast<std::size_t>(entry.data_len));
    const auto source{
        bytes_.subspan(static_cast<std::size_t>(entry.data_offset),
                       static_cast<std::size_t>(entry.stored_len))};
    const auto written{ZSTD_decompress(payload.data(), payload.size(),
                                       source.data(), source.size())};
    if (ZSTD_isError(written) != 0U) {
        return std::unexpected{parse_error(
            "record '" + entry.key +
            "' has a corrupt zstd payload: " + ZSTD_getErrorName(written))};
    }
    if (written != entry.data_len) {
        return std::unexpected{parse_error(
            "record '" + entry.key + "' decompressed to an unexpected size")};
    }
    return payload;
}

auto rfs_reader::read(std::string_view key) const
    -> result<std::vector<std::byte>> {
    const auto *entry{find(key)};
    if (entry == nullptr) {
        return std::unexpected{
            make_error(error_code::not_found,
                       "no RFS record named '" + std::string{key} + "'")};
    }
    return read(*entry);
}

} // namespace refrain::data
