// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/io.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/content_index.hpp>

#include <boost/ut.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace boost::ut;

namespace fs = std::filesystem;
namespace io = refrain::core::io;

using refrain::data::blake3;
using refrain::data::blob_digest;
using refrain::data::blob_flags_all_raw_blocks;
using refrain::data::blob_flags_in_segment;
using refrain::data::blob_flags_packed;
using refrain::data::blob_kind_builtin;
using refrain::data::blob_kind_content;
using refrain::data::blob_kind_cook;
using refrain::data::blob_kind_world;
using refrain::data::blob_table_entry;
using refrain::data::blob_table_view;
using refrain::data::build_blob_table;
using refrain::data::build_resolution_index;
using refrain::data::crc32c;
using refrain::data::digest32;
using refrain::data::mapped_blob_table;
using refrain::data::mapped_resolution_index;
using refrain::data::parse_blob_table;
using refrain::data::parse_resolution_index;
using refrain::data::resolution_entry;
using refrain::data::resolution_index_view;
using refrain::data::resolution_input;
using refrain::data::resolve;
using refrain::data::resolved_blob;
using refrain::data::to_hex;

constexpr std::size_t rfbt_header_bytes = 128;
constexpr std::size_t rfbt_entry_bytes = 80;
constexpr std::size_t rfrx_header_bytes = 128;
constexpr std::size_t rfrx_entry_bytes = 32;

[[nodiscard]] auto bytes_of(std::string_view text) -> std::vector<std::byte> {
    const auto bytes{
        std::as_bytes(std::span<const char>{text.data(), text.size()})};
    return std::vector<std::byte>{bytes.begin(), bytes.end()};
}

[[nodiscard]] auto digest_of(std::string_view text) -> digest32 {
    return blake3(bytes_of(text));
}

/// Raw entry with `stored_size == logical_size` unless a physical size is
/// passed explicitly (packed entries need it). Segment fields default to zero
/// (one file per blob); `blob_flags_in_segment` records pass them explicitly.
[[nodiscard]] auto
make_entry(std::string_view label, std::uint64_t logical_size = 3,
           std::uint32_t flags = 0, std::uint32_t refcount = 1,
           std::uint16_t kind = blob_kind_content,
           std::uint32_t block_count = 0, std::uint32_t stored_size = 0,
           std::uint64_t segment_id = 0, std::uint32_t segment_offset = 0)
    -> blob_table_entry {
    if (stored_size == 0U && logical_size != 0U &&
        logical_size <= std::numeric_limits<std::uint32_t>::max()) {
        stored_size = static_cast<std::uint32_t>(logical_size);
    }
    return blob_table_entry{
        .digest = digest_of(label),
        .logical_size = logical_size,
        .flags = flags,
        .refcount = refcount,
        .kind = kind,
        .block_count = block_count,
        .stored_size = stored_size,
        .segment_id = segment_id,
        .segment_offset = segment_offset,
    };
}

[[nodiscard]] auto make_resolution(std::string location, std::string source,
                                   std::int32_t priority, digest32 blob,
                                   std::uint16_t flags = 0)
    -> resolution_input {
    return resolution_input{
        .location = std::move(location),
        .source = std::move(source),
        .priority = priority,
        .blob = blob,
        .flags = flags,
    };
}

// Independent little-endian readers so the on-disk layouts are pinned by the
// tests instead of by the implementation.
[[nodiscard]] auto read_u16_le(std::span<const std::byte> bytes,
                               std::size_t offset) -> std::uint16_t {
    return static_cast<std::uint16_t>(
        std::to_integer<unsigned int>(bytes[offset]) |
        (std::to_integer<unsigned int>(bytes[offset + 1U]) << 8U));
}

[[nodiscard]] auto read_u32_le(std::span<const std::byte> bytes,
                               std::size_t offset) -> std::uint32_t {
    std::uint32_t value{0};
    for (std::size_t index{0}; index < 4U; ++index) {
        value |= std::to_integer<unsigned int>(bytes[offset + index])
                 << (8U * index);
    }
    return value;
}

[[nodiscard]] auto read_u64_le(std::span<const std::byte> bytes,
                               std::size_t offset) -> std::uint64_t {
    std::uint64_t value{0};
    for (std::size_t index{0}; index < 8U; ++index) {
        value |= static_cast<std::uint64_t>(
                     std::to_integer<unsigned int>(bytes[offset + index]))
                 << (8U * index);
    }
    return value;
}

void write_u16_le(std::span<std::byte> bytes, std::size_t offset,
                  std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xFFU);
    bytes[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
}

void write_u32_le(std::span<std::byte> bytes, std::size_t offset,
                  std::uint32_t value) {
    for (std::size_t index{0}; index < 4U; ++index) {
        bytes[offset + index] =
            static_cast<std::byte>((value >> (8U * index)) & 0xFFU);
    }
}

void write_u64_le(std::span<std::byte> bytes, std::size_t offset,
                  std::uint64_t value) {
    for (std::size_t index{0}; index < 8U; ++index) {
        bytes[offset + index] =
            static_cast<std::byte>((value >> (8U * index)) & 0xFFU);
    }
}

[[nodiscard]] auto same_digest(const digest32 &lhs, const digest32 &rhs)
    -> bool {
    return std::equal(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
}

/// Recompute the RFBT header fields that cover the (possibly tampered) body so
/// a test reaches the intended deeper validation.
void refresh_rfbt(std::vector<std::byte> &bytes) {
    const auto digest{
        blake3(std::span<const std::byte>{bytes}.subspan(rfbt_header_bytes))};
    std::copy(digest.begin(), digest.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(32));
    write_u32_le(bytes, 64,
                 crc32c(std::span<const std::byte>{bytes}.first(64)));
}

/// Same for RFRX: header digest covers everything after byte 128.
void refresh_rfrx(std::vector<std::byte> &bytes) {
    const auto digest{
        blake3(std::span<const std::byte>{bytes}.subspan(rfrx_header_bytes))};
    std::copy(digest.begin(), digest.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(48));
    write_u32_le(bytes, 80,
                 crc32c(std::span<const std::byte>{bytes}.first(80)));
}

[[nodiscard]] auto error_is(const refrain::error &error,
                            refrain::error_code code) -> bool {
    return error.code == code;
}

[[nodiscard]] auto field_digest(std::span<const std::byte> bytes,
                                std::size_t offset) -> digest32 {
    digest32 digest{};
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                digest.size(), digest.begin());
    return digest;
}

[[nodiscard]] auto index_location_hash(std::string_view location)
    -> std::uint64_t {
    const auto bytes{bytes_of(location)};
    const auto digest{blake3(bytes)};
    std::uint64_t hash{0};
    for (std::size_t index{0}; index < 8U; ++index) {
        hash |= static_cast<std::uint64_t>(
                    std::to_integer<unsigned int>(digest[index]))
                << (8U * index);
    }
    return hash;
}

/// Build a valid blob table over `digests` and return its parsed view.
[[nodiscard]] auto make_blob_table(std::span<const digest32> digests)
    -> blob_table_view {
    std::vector<blob_table_entry> entries;
    entries.reserve(digests.size());
    for (const auto &digest : digests) {
        entries.push_back(blob_table_entry{
            .digest = digest,
            .logical_size = 3,
            .flags = 0,
            .refcount = 0,
            .kind = blob_kind_content,
            .block_count = 0,
            .stored_size = 3,
        });
    }
    auto built = build_blob_table(entries);
    if (!built) {
        return {};
    }
    auto parsed = parse_blob_table(*built);
    if (!parsed) {
        return {};
    }
    return std::move(*parsed);
}

[[nodiscard]] auto table_index(const blob_table_view &table,
                               const digest32 &digest) -> std::uint32_t {
    const auto found{std::find_if(table.entries.begin(), table.entries.end(),
                                  [&digest](const blob_table_entry &entry) {
                                      return same_digest(entry.digest, digest);
                                  })};
    return found == table.entries.end()
               ? std::numeric_limits<std::uint32_t>::max()
               : static_cast<std::uint32_t>(found - table.entries.begin());
}

/// Self-cleaning unique scratch directory for the mmap-backed view tests.
class mapped_scratch {
  public:
    mapped_scratch() {
        static std::atomic<std::uint64_t> sequence{0};
        std::error_code error;
        auto base{fs::temp_directory_path(error)};
        if (error) {
            base = fs::path{"."};
        }
        const auto stamp{static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count())};
        base /= fmt::format("refrain_content_index_mmap_test_{}_{}", stamp,
                            sequence.fetch_add(1, std::memory_order_relaxed));
        fs::create_directories(base, error);
        path_ = base;
    }

    ~mapped_scratch() {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    mapped_scratch(const mapped_scratch &) = delete;
    auto operator=(const mapped_scratch &) -> mapped_scratch & = delete;
    mapped_scratch(mapped_scratch &&) = delete;
    auto operator=(mapped_scratch &&) -> mapped_scratch & = delete;

    [[nodiscard]] auto path() const -> const fs::path & { return path_; }

  private:
    fs::path path_{};
};

[[nodiscard]] auto scratch_path(const mapped_scratch &scratch,
                                std::string_view name) -> std::string {
    return (scratch.path() / std::string{name}).string();
}

[[nodiscard]] auto scratch_write(const mapped_scratch &scratch,
                                 std::string_view name,
                                 std::span<const std::byte> bytes) -> bool {
    return static_cast<bool>(
        io::write_file(scratch_path(scratch, name), bytes));
}

/// Plain raw blob-table records over `digests` (the same shape
/// `make_blob_table()` uses).
[[nodiscard]] auto make_table_records(std::span<const digest32> digests)
    -> std::vector<blob_table_entry> {
    std::vector<blob_table_entry> entries;
    entries.reserve(digests.size());
    for (const auto &digest : digests) {
        entries.push_back(blob_table_entry{
            .digest = digest,
            .logical_size = 3,
            .flags = 0,
            .refcount = 0,
            .kind = blob_kind_content,
            .block_count = 0,
            .stored_size = 3,
        });
    }
    return entries;
}

/// A built RFBT byte image plus its parsed view.
struct table_fixture {
    std::vector<std::byte> bytes{};
    blob_table_view view{};
};

[[nodiscard]] auto make_table_fixture(std::span<const digest32> digests)
    -> table_fixture {
    auto records{make_table_records(digests)};
    auto built{build_blob_table(records)};
    if (!built) {
        return {};
    }
    auto parsed{parse_blob_table(*built)};
    if (!parsed) {
        return {};
    }
    return table_fixture{.bytes = std::move(*built),
                         .view = std::move(*parsed)};
}

const suite content_index_tests = [] {
    // -----------------------------------------------------------------------
    // RFBT (global blob table).
    // -----------------------------------------------------------------------

    "rfbt round trips sorted entries"_test = [] {
        const std::vector<blob_table_entry> entries{
            make_entry("charlie", 70000, blob_flags_packed, 2, blob_kind_cook,
                       2, 42000),
            make_entry("alpha", 12, 0, 7, blob_kind_content, 0),
            make_entry("bravo", 0, 0, 0, blob_kind_builtin, 0),
        };
        auto built = build_blob_table(entries);
        expect(built.has_value());
        if (!built) {
            return;
        }
        expect(built->size() == rfbt_header_bytes + 3U * rfbt_entry_bytes);

        auto parsed = parse_blob_table(*built);
        expect(parsed.has_value());
        if (!parsed) {
            return;
        }
        expect(parsed->entries.size() == 3U);
        if (parsed->entries.size() != 3U) {
            return;
        }
        const auto find_entry = [&parsed](const digest32 &digest) {
            return std::find_if(parsed->entries.begin(), parsed->entries.end(),
                                [&digest](const blob_table_entry &entry) {
                                    return same_digest(entry.digest, digest);
                                });
        };
        const auto alpha{find_entry(digest_of("alpha"))};
        expect(alpha != parsed->entries.end());
        if (alpha != parsed->entries.end()) {
            expect(alpha->refcount == 7U);
            expect(alpha->stored_size == 12U);
        }
        const auto bravo{find_entry(digest_of("bravo"))};
        expect(bravo != parsed->entries.end());
        if (bravo != parsed->entries.end()) {
            expect(bravo->refcount == 0U);
            expect(bravo->stored_size == 0U);
        }
        const auto charlie{find_entry(digest_of("charlie"))};
        expect(charlie != parsed->entries.end());
        if (charlie != parsed->entries.end()) {
            expect(charlie->logical_size == 70000U);
            expect(charlie->flags == blob_flags_packed);
            expect(charlie->block_count == 2U);
            expect(charlie->stored_size == 42000U);
            expect(charlie->kind == blob_kind_cook);
        }
    };

    "rfbt build is deterministic and order independent"_test = [] {
        const auto alpha{make_entry("alpha", 1, 0, 3, blob_kind_world, 0)};
        const auto bravo{make_entry("bravo", 70000, blob_flags_packed, 1,
                                    blob_kind_content, 2, 50000)};
        const auto charlie{
            make_entry("charlie", 0, 0, 0, blob_kind_builtin, 0)};
        const std::vector<blob_table_entry> forward{alpha, bravo, charlie};
        const std::vector<blob_table_entry> shuffled{charlie, alpha, bravo};

        auto first = build_blob_table(forward);
        auto second = build_blob_table(shuffled);
        expect(first.has_value());
        expect(second.has_value());
        if (!first || !second) {
            return;
        }
        expect(std::equal(first->begin(), first->end(), second->begin(),
                          second->end()));
    };

    "rfbt header layout is pinned"_test = [] {
        const auto entries{std::vector<blob_table_entry>{
            make_entry("alpha", 42, 0, 5, blob_kind_world, 0)}};
        auto built = build_blob_table(entries);
        expect(built.has_value());
        if (!built) {
            return;
        }
        const auto bytes{std::span<const std::byte>{*built}};
        expect(bytes.size() == rfbt_header_bytes + rfbt_entry_bytes);
        expect(std::to_integer<char>(bytes[0]) == 'R');
        expect(std::to_integer<char>(bytes[1]) == 'F');
        expect(std::to_integer<char>(bytes[2]) == 'B');
        expect(std::to_integer<char>(bytes[3]) == 'T');
        expect(read_u16_le(bytes, 4) == 1U);
        expect(read_u16_le(bytes, 6) == 0U);
        expect(read_u32_le(bytes, 8) == 1U);
        expect(read_u32_le(bytes, 12) == 80U); // entry_size
        expect(read_u64_le(bytes, 16) == rfbt_header_bytes);
        expect(read_u64_le(bytes, 24) == rfbt_entry_bytes);
        expect(same_digest(field_digest(bytes, 32),
                           blake3(bytes.subspan(rfbt_header_bytes))));
        expect(read_u32_le(bytes, 64) == crc32c(bytes.first(64)));
        for (std::size_t offset = 68; offset < 128U; ++offset) {
            expect(std::to_integer<unsigned int>(bytes[offset]) == 0U);
        }
        // Record: digest[32] | logical_size | flags | refcount | kind |
        // reserved | block_count | stored_size | segment_id |
        // segment_offset | reserved | reserved tail (80 bytes total).
        expect(same_digest(field_digest(bytes, 128), digest_of("alpha")));
        expect(read_u64_le(bytes, 160) == 42U);
        expect(read_u32_le(bytes, 168) == 0U);
        expect(read_u32_le(bytes, 172) == 5U);
        expect(read_u16_le(bytes, 176) == blob_kind_world);
        expect(read_u16_le(bytes, 178) == 0U);
        expect(read_u32_le(bytes, 180) == 0U);
        expect(read_u32_le(bytes, 184) == 42U); // stored_size
        expect(read_u64_le(bytes, 188) == 0U);  // segment_id
        expect(read_u32_le(bytes, 196) == 0U);  // segment_offset
        expect(read_u32_le(bytes, 200) == 0U);  // reserved
        expect(read_u32_le(bytes, 204) == 0U);  // reserved tail
    };

    "rfbt stores the whole-file blake3 prefix in the view"_test = [] {
        auto built = build_blob_table(
            std::vector<blob_table_entry>{make_entry("alpha")});
        expect(built.has_value());
        if (!built) {
            return;
        }
        auto parsed = parse_blob_table(*built);
        expect(parsed.has_value());
        if (!parsed) {
            return;
        }
        const auto whole{blake3(std::span<const std::byte>{*built})};
        for (std::size_t index{0}; index < 8U; ++index) {
            expect(std::to_integer<unsigned int>(
                       parsed->blob_table_blake3_prefix[index]) ==
                   std::to_integer<unsigned int>(whole[index]));
        }
    };

    "rfbt duplicate digests are rejected"_test = [] {
        const auto entry{make_entry("alpha")};
        const std::vector<blob_table_entry> entries{entry, entry};
        auto built = build_blob_table(entries);
        expect(!built);
        if (!built) {
            expect(
                error_is(built.error(), refrain::error_code::invalid_argument));
        }
    };

    "rfbt rejects invalid record metadata"_test = [] {
        std::vector<blob_table_entry> cases{
            make_entry("flags", 3, 0x8U),
            make_entry("kind", 3, 0, 1, 4),
            make_entry("raw-block-count", 3, 0, 1, blob_kind_content, 1),
            make_entry("packed-empty", 0, blob_flags_packed, 0,
                       blob_kind_content, 1),
            make_entry("packed-no-blocks", 70000, blob_flags_packed, 1,
                       blob_kind_content, 0, 70000),
            make_entry("raw-all-raw", 3, blob_flags_all_raw_blocks),
            make_entry("packed-no-stored-size", 70000, blob_flags_packed, 1,
                       blob_kind_content, 1, 1),
            make_entry("raw-stored-mismatch", 3, 0, 1, blob_kind_content, 0, 4),
            make_entry("segment-zero-id", 3, blob_flags_in_segment, 1,
                       blob_kind_content, 0, 3, 0, 8),
            make_entry("segment-overflow", 3, blob_flags_in_segment, 1,
                       blob_kind_content, 0, 3, 7, 0xFFFFFFFEU),
            make_entry("segment-id-without-flag", 3, 0, 1, blob_kind_content, 0,
                       3, 7, 0),
            make_entry("segment-offset-without-flag", 3, 0, 1,
                       blob_kind_content, 0, 3, 0, 8),
            make_entry("segment-no-stored-size", 3, blob_flags_in_segment, 1,
                       blob_kind_content, 0, 3, 7, 8),
        };
        cases[6].stored_size = 0;  // packed with an empty physical size
        cases[8].stored_size = 0;  // in-segment zero id (checked first)
        cases[12].stored_size = 0; // in-segment nonzero blob, no stored bytes
        for (const auto &entry : cases) {
            auto built =
                build_blob_table(std::span<const blob_table_entry>{&entry, 1});
            expect(!built);
            if (!built) {
                expect(error_is(built.error(),
                                refrain::error_code::invalid_argument));
            }
        }
    };

    "rfbt in-segment fields round trip"_test = [] {
        const auto segment_id{std::uint64_t{0x0102030405060708ULL}};
        const std::vector<blob_table_entry> entries{
            make_entry("segment-raw", 3, blob_flags_in_segment, 2,
                       blob_kind_content, 0, 3, segment_id, 8),
            // A zero-byte raw blob is the one in-segment record allowed to have
            // no stored bytes; its offset is still meaningful.
            make_entry("segment-empty", 0, blob_flags_in_segment, 1,
                       blob_kind_world, 0, 0, segment_id + 1U, 0),
            make_entry("segment-packed", 70000,
                       blob_flags_packed | blob_flags_all_raw_blocks |
                           blob_flags_in_segment,
                       1, blob_kind_cook, 2, 50000, segment_id + 2U, 16),
        };
        auto built = build_blob_table(entries);
        expect(built.has_value());
        if (!built) {
            return;
        }
        auto parsed = parse_blob_table(*built);
        expect(parsed.has_value());
        if (!parsed) {
            return;
        }
        for (const auto &entry : parsed->entries) {
            if (entry.digest == digest_of("segment-raw")) {
                expect(entry.flags == blob_flags_in_segment);
                expect(entry.segment_id == segment_id);
                expect(entry.segment_offset == 8U);
                expect(entry.stored_size == 3U);
            }
            if (entry.digest == digest_of("segment-empty")) {
                expect(entry.flags == blob_flags_in_segment);
                expect(entry.logical_size == 0U);
                expect(entry.stored_size == 0U);
                expect(entry.segment_id == segment_id + 1U);
                expect(entry.segment_offset == 0U);
            }
            if (entry.digest == digest_of("segment-packed")) {
                expect(entry.flags ==
                       (blob_flags_packed | blob_flags_all_raw_blocks |
                        blob_flags_in_segment));
                expect(entry.block_count == 2U);
                expect(entry.segment_id == segment_id + 2U);
                expect(entry.segment_offset == 16U);
            }
        }
    };

    "rfbt refcount is preserved for every value"_test = [] {
        const std::vector<blob_table_entry> entries{
            make_entry("zero", 3, 0, 0),
            make_entry("max", 3, 0, 0xFFFFFFFFU),
        };
        auto built = build_blob_table(entries);
        expect(built.has_value());
        if (!built) {
            return;
        }
        auto parsed = parse_blob_table(*built);
        expect(parsed.has_value());
        if (!parsed) {
            return;
        }
        for (const auto &entry : parsed->entries) {
            if (same_digest(entry.digest, digest_of("zero"))) {
                expect(entry.refcount == 0U);
            }
            if (same_digest(entry.digest, digest_of("max"))) {
                expect(entry.refcount == 0xFFFFFFFFU);
            }
        }
    };

    "rfbt accepts an empty table"_test = [] {
        auto built = build_blob_table({});
        expect(built.has_value());
        if (!built) {
            return;
        }
        expect(built->size() == rfbt_header_bytes);
        auto parsed = parse_blob_table(*built);
        expect(parsed.has_value());
        if (parsed) {
            expect(parsed->entries.empty());
        }
    };

    "rfbt rejects a bad header crc"_test = [] {
        auto built = build_blob_table(
            std::vector<blob_table_entry>{make_entry("alpha")});
        expect(built.has_value());
        if (!built) {
            return;
        }
        (*built)[64] = static_cast<std::byte>(0xFF);
        auto parsed = parse_blob_table(*built);
        expect(!parsed);
        if (!parsed) {
            expect(error_is(parsed.error(), refrain::error_code::parse_error));
        }
    };

    "rfbt rejects a bad file blake3"_test = [] {
        auto built = build_blob_table(
            std::vector<blob_table_entry>{make_entry("alpha")});
        expect(built.has_value());
        if (!built) {
            return;
        }
        (*built)[128 + 40] = static_cast<std::byte>(0xFF); // record flags byte
        auto parsed = parse_blob_table(*built);
        expect(!parsed);
        if (!parsed) {
            expect(error_is(parsed.error(), refrain::error_code::parse_error));
        }
    };

    "rfbt rejects reserved header and record bytes"_test = [] {
        auto built = build_blob_table(
            std::vector<blob_table_entry>{make_entry("alpha")});
        expect(built.has_value());
        if (!built) {
            return;
        }
        auto header_reserved{*built};
        header_reserved[100] = std::byte{1};
        auto parsed_header = parse_blob_table(header_reserved);
        expect(!parsed_header);
        if (!parsed_header) {
            expect(error_is(parsed_header.error(),
                            refrain::error_code::parse_error));
        }

        auto record_reserved{*built};
        record_reserved[128 + 50] = std::byte{1}; // reserved u16
        refresh_rfbt(record_reserved);
        auto parsed_record = parse_blob_table(record_reserved);
        expect(!parsed_record);
        if (!parsed_record) {
            expect(error_is(parsed_record.error(),
                            refrain::error_code::parse_error));
        }

        auto tail_reserved{*built};
        tail_reserved[128 + 76] = std::byte{1}; // reserved tail
        refresh_rfbt(tail_reserved);
        auto parsed_tail = parse_blob_table(tail_reserved);
        expect(!parsed_tail);
        if (!parsed_tail) {
            expect(error_is(parsed_tail.error(),
                            refrain::error_code::parse_error));
        }
    };

    "rfbt rejects an unsupported entry size"_test = [] {
        auto built = build_blob_table(
            std::vector<blob_table_entry>{make_entry("alpha")});
        expect(built.has_value());
        if (!built) {
            return;
        }
        write_u32_le(*built, 12, 32);
        auto parsed = parse_blob_table(*built);
        expect(!parsed);
        if (!parsed) {
            expect(error_is(parsed.error(), refrain::error_code::parse_error));
        }
    };

    "rfbt rejects unsorted or duplicate records"_test = [] {
        const std::vector<blob_table_entry> entries{make_entry("alpha"),
                                                    make_entry("bravo")};
        auto built = build_blob_table(entries);
        expect(built.has_value());
        if (!built) {
            return;
        }
        auto swapped{*built};
        for (std::size_t index{0}; index < rfbt_entry_bytes; ++index) {
            std::swap(swapped[128 + index],
                      swapped[128 + rfbt_entry_bytes + index]);
        }
        refresh_rfbt(swapped);
        auto parsed_unsorted = parse_blob_table(swapped);
        expect(!parsed_unsorted);
        if (!parsed_unsorted) {
            expect(error_is(parsed_unsorted.error(),
                            refrain::error_code::parse_error));
        }

        auto duplicated{*built};
        for (std::size_t index{0}; index < 32U; ++index) {
            duplicated[128 + rfbt_entry_bytes + index] =
                duplicated[128 + index];
        }
        refresh_rfbt(duplicated);
        auto parsed_duplicate = parse_blob_table(duplicated);
        expect(!parsed_duplicate);
        if (!parsed_duplicate) {
            expect(error_is(parsed_duplicate.error(),
                            refrain::error_code::parse_error));
        }
    };

    "rfbt rejects truncated and foreign headers"_test = [] {
        auto built = build_blob_table(
            std::vector<blob_table_entry>{make_entry("alpha")});
        expect(built.has_value());
        if (!built) {
            return;
        }
        for (const std::size_t size :
             {std::size_t{0}, std::size_t{100}, rfbt_header_bytes + 32U}) {
            auto parsed = parse_blob_table(
                std::span<const std::byte>{*built}.first(size));
            expect(!parsed);
            if (!parsed) {
                expect(
                    error_is(parsed.error(), refrain::error_code::parse_error));
            }
        }

        auto bad_magic{*built};
        bad_magic[0] = static_cast<std::byte>('X');
        auto parsed_magic = parse_blob_table(bad_magic);
        expect(!parsed_magic);
        if (!parsed_magic) {
            expect(error_is(parsed_magic.error(),
                            refrain::error_code::parse_error));
        }

        auto bad_version{*built};
        write_u16_le(bad_version, 4, 2);
        auto parsed_version = parse_blob_table(bad_version);
        expect(!parsed_version);
        if (!parsed_version) {
            expect(error_is(parsed_version.error(),
                            refrain::error_code::version_mismatch));
        }

        auto bad_flags{*built};
        write_u16_le(bad_flags, 6, 1);
        auto parsed_flags = parse_blob_table(bad_flags);
        expect(!parsed_flags);
        if (!parsed_flags) {
            expect(error_is(parsed_flags.error(),
                            refrain::error_code::parse_error));
        }
    };

    // -----------------------------------------------------------------------
    // RFRX (per-composition resolution index).
    // -----------------------------------------------------------------------

    "rfrx round trips entries and sources"_test = [] {
        const std::vector<digest32> digests{digest_of("a"), digest_of("b"),
                                            digest_of("c")};
        auto table{make_blob_table(digests)};
        expect(table.entries.size() == 3U);
        if (table.entries.size() != 3U) {
            return;
        }
        const std::vector<resolution_input> entries{
            make_resolution("data/refrain_test/a.json", "mod:alpha", 10,
                            digest_of("a")),
            make_resolution("data/refrain_test/a.json", "builtin", 5,
                            digest_of("b")),
            make_resolution("assets/refrain_test/lang/en_us.json", "mod:alpha",
                            20, digest_of("c")),
        };
        auto built = build_resolution_index(entries, table);
        expect(built.has_value());
        if (!built) {
            return;
        }
        auto parsed = parse_resolution_index(*built, table);
        expect(parsed.has_value());
        if (!parsed) {
            return;
        }
        expect(parsed->entries.size() == 3U);
        if (parsed->entries.size() != 3U) {
            return;
        }
        const auto a{std::find_if(
            parsed->entries.begin(), parsed->entries.end(),
            [](const refrain::data::resolution_entry &entry) {
                return entry.location == "data/refrain_test/a.json" &&
                       entry.source == "mod:alpha";
            })};
        expect(a != parsed->entries.end());
        if (a != parsed->entries.end()) {
            expect(a->priority == 10);
            expect(a->flags == 0U);
            expect(a->blob_index == table_index(table, digest_of("a")));
            auto digest{blob_digest(*a, table)};
            expect(digest.has_value());
            if (digest) {
                expect(same_digest(*digest, digest_of("a")));
            }
        }
    };

    "rfrx build is deterministic and order independent"_test = [] {
        const std::vector<digest32> digests{digest_of("a"), digest_of("b"),
                                            digest_of("c")};
        auto table{make_blob_table(digests)};
        expect(table.entries.size() == 3U);
        if (table.entries.size() != 3U) {
            return;
        }
        const std::vector<resolution_input> forward{
            make_resolution("data/a.json", "builtin", 1, digest_of("a")),
            make_resolution("data/b.json", "mod:x", 7, digest_of("b")),
            make_resolution("data/a.json", "mod:x", 9, digest_of("c")),
        };
        const std::vector<resolution_input> shuffled{forward[2], forward[0],
                                                     forward[1]};
        auto first = build_resolution_index(forward, table);
        auto second = build_resolution_index(shuffled, table);
        expect(first.has_value());
        expect(second.has_value());
        if (!first || !second) {
            return;
        }
        expect(std::equal(first->begin(), first->end(), second->begin(),
                          second->end()));
    };

    "rfrx header and entry layout are pinned"_test = [] {
        const std::vector<digest32> digests{digest_of("a"), digest_of("b")};
        auto table{make_blob_table(digests)};
        expect(table.entries.size() == 2U);
        if (table.entries.size() != 2U) {
            return;
        }
        const std::vector<resolution_input> entries{
            make_resolution("data/a.json", "builtin", 3, digest_of("a")),
            make_resolution("data/a.json", "mod:x", 1, digest_of("b")),
        };
        auto built = build_resolution_index(entries, table);
        expect(built.has_value());
        if (!built) {
            return;
        }
        const auto bytes{std::span<const std::byte>{*built}};
        expect(std::to_integer<char>(bytes[0]) == 'R');
        expect(std::to_integer<char>(bytes[1]) == 'F');
        expect(std::to_integer<char>(bytes[2]) == 'R');
        expect(std::to_integer<char>(bytes[3]) == 'X');
        expect(read_u16_le(bytes, 4) == 1U);
        expect(read_u16_le(bytes, 6) == 0U);
        expect(read_u32_le(bytes, 8) == 2U);
        expect(read_u32_le(bytes, 12) == 2U);
        expect(read_u64_le(bytes, 16) == rfrx_header_bytes);
        expect(read_u64_le(bytes, 24) == 2U * rfrx_entry_bytes);
        const auto arena_offset{read_u64_le(bytes, 32)};
        expect(arena_offset == rfrx_header_bytes + 2U * rfrx_entry_bytes);
        expect(read_u64_le(bytes, 40) == bytes.size() - arena_offset);
        expect(same_digest(field_digest(bytes, 48),
                           blake3(bytes.subspan(rfrx_header_bytes))));
        expect(read_u32_le(bytes, 80) == crc32c(bytes.first(80)));
        for (std::size_t index{0}; index < 8U; ++index) {
            expect(std::to_integer<unsigned int>(bytes[84 + index]) ==
                   std::to_integer<unsigned int>(
                       table.blob_table_blake3_prefix[index]));
        }
        for (std::size_t offset = 92; offset < 128U; ++offset) {
            expect(std::to_integer<unsigned int>(bytes[offset]) == 0U);
        }
        // First entry: hash | priority | source_index | flags | blob_index |
        // path_offset | path_len | reserved | reserved2.
        expect(read_u64_le(bytes, 128) == index_location_hash("data/a.json"));
        expect(read_u32_le(bytes, 136) == 3U);
        expect(read_u32_le(bytes, 144) == table_index(table, digest_of("a")));
        expect(read_u16_le(bytes, 152) == 11U); // strlen("data/a.json")
        expect(read_u16_le(bytes, 154) == 0U);
        expect(read_u32_le(bytes, 156) == 0U);
        // Arena starts with the sorted source table: "builtin\0mod:x\0".
        const auto arena{bytes.subspan(static_cast<std::size_t>(arena_offset))};
        expect(arena.size() >= 15U);
        if (arena.size() >= 15U) {
            expect(std::to_integer<char>(arena[0]) == 'b');
            expect(std::to_integer<char>(arena[7]) == '\0');
            expect(std::to_integer<char>(arena[8]) == 'm');
            expect(std::to_integer<char>(arena[13]) == '\0');
        }
    };

    "rfrx location hash and blob index are pinned"_test = [] {
        const std::string location{"data/refrain_test/a.json"};
        const std::vector<digest32> digests{digest_of("a")};
        auto table{make_blob_table(digests)};
        expect(table.entries.size() == 1U);
        if (table.entries.size() != 1U) {
            return;
        }
        const std::vector<resolution_input> entries{
            make_resolution(location, "builtin", 1, digest_of("a"))};
        auto built = build_resolution_index(entries, table);
        expect(built.has_value());
        if (!built) {
            return;
        }
        const auto bytes{std::span<const std::byte>{*built}};
        const auto arena_offset{read_u64_le(bytes, 32)};
        const auto path_offset{read_u32_le(bytes, 128 + 20)};
        const auto path_len{read_u16_le(bytes, 128 + 24)};
        expect(path_len == location.size());
        expect(read_u64_le(bytes, 128) == index_location_hash(location));
        expect(read_u32_le(bytes, 128 + 16) ==
               table_index(table, digest_of("a")));
        const auto arena{bytes.subspan(static_cast<std::size_t>(arena_offset) +
                                       path_offset)};
        expect(std::string_view{
                   reinterpret_cast<const char *>(arena.data()), // NOLINT
                   path_len} == location);
    };

    "rfrx resolve returns winner, fallback and blob digest"_test = [] {
        const std::vector<digest32> digests{digest_of("a"), digest_of("b"),
                                            digest_of("c")};
        auto table{make_blob_table(digests)};
        expect(table.entries.size() == 3U);
        if (table.entries.size() != 3U) {
            return;
        }
        const std::vector<resolution_input> entries{
            make_resolution("data/a.json", "low", 10, digest_of("a")),
            make_resolution("data/a.json", "high", 30, digest_of("c")),
            make_resolution("data/a.json", "middle", 20, digest_of("b")),
        };
        auto built = build_resolution_index(entries, table);
        expect(built.has_value());
        if (!built) {
            return;
        }
        auto parsed = parse_resolution_index(*built, table);
        expect(parsed.has_value());
        if (!parsed) {
            return;
        }
        auto resolved = resolve(*parsed, "data/a.json");
        expect(resolved.has_value());
        if (!resolved) {
            return;
        }
        expect(resolved->winner != nullptr);
        if (resolved->winner == nullptr) {
            return;
        }
        expect(resolved->winner->source == "high");
        expect(resolved->winner->priority == 30);
        auto winner_blob{resolved_blob(*resolved, table)};
        expect(winner_blob.has_value());
        if (winner_blob) {
            expect(same_digest(*winner_blob, digest_of("c")));
        }
        expect(resolved->fallback.size() == 2U);
        if (resolved->fallback.size() == 2U) {
            expect(resolved->fallback[0]->priority == 20);
            expect(resolved->fallback[0]->source == "middle");
            expect(resolved->fallback[1]->priority == 10);
            expect(resolved->fallback[1]->source == "low");
        }
    };

    "rfrx resolve missing location is not_found"_test = [] {
        const std::vector<digest32> digests{digest_of("a")};
        auto table{make_blob_table(digests)};
        expect(table.entries.size() == 1U);
        if (table.entries.size() != 1U) {
            return;
        }
        const std::vector<resolution_input> entries{
            make_resolution("data/a.json", "builtin", 1, digest_of("a"))};
        auto built = build_resolution_index(entries, table);
        expect(built.has_value());
        if (!built) {
            return;
        }
        auto parsed = parse_resolution_index(*built, table);
        expect(parsed.has_value());
        if (!parsed) {
            return;
        }
        auto resolved = resolve(*parsed, "data/missing.json");
        expect(!resolved);
        if (!resolved) {
            expect(error_is(resolved.error(), refrain::error_code::not_found));
        }
    };

    "rfrx resolve tie-break ignores file order"_test = [] {
        const std::vector<resolution_entry> entries{
            resolution_entry{.location = "data/a.json",
                             .source = "mod.b",
                             .priority = 5,
                             .blob_index = 7,
                             .flags = 0},
            resolution_entry{.location = "data/a.json",
                             .source = "mod.a",
                             .priority = 5,
                             .blob_index = 3,
                             .flags = 0},
            resolution_entry{.location = "data/a.json",
                             .source = "mod.0",
                             .priority = 5,
                             .blob_index = 3,
                             .flags = 0},
            resolution_entry{.location = "data/a.json",
                             .source = "mod.c",
                             .priority = 2,
                             .blob_index = 1,
                             .flags = 0},
        };
        const auto check = [](const std::vector<resolution_entry> &ordered) {
            resolution_index_view index{.entries = ordered};
            auto resolved = resolve(index, "data/a.json");
            expect(resolved.has_value());
            if (!resolved) {
                return;
            }
            expect(resolved->winner != nullptr);
            if (resolved->winner == nullptr) {
                return;
            }
            // Same priority: smallest blob_index, then smallest source.
            expect(resolved->winner->source == "mod.0");
            expect(resolved->winner->blob_index == 3U);
            expect(resolved->fallback.size() == 3U);
            if (resolved->fallback.size() != 3U) {
                return;
            }
            expect(resolved->fallback[0]->source == "mod.a");
            expect(resolved->fallback[0]->blob_index == 3U);
            expect(resolved->fallback[1]->source == "mod.b");
            expect(resolved->fallback[1]->blob_index == 7U);
            expect(resolved->fallback[2]->source == "mod.c");
            expect(resolved->fallback[2]->priority == 2);
        };
        check(entries);
        const std::vector<resolution_entry> reversed{entries.rbegin(),
                                                     entries.rend()};
        check(reversed);
    };

    "rfrx rejects same-priority entries from different sources"_test = [] {
        const std::vector<digest32> digests{digest_of("a"), digest_of("b")};
        auto table{make_blob_table(digests)};
        expect(table.entries.size() == 2U);
        if (table.entries.size() != 2U) {
            return;
        }
        // Two mods ship a different `LICENSE` at the same priority: the layer
        // order must give each layer a distinct priority, so this is a hard
        // conflict instead of a source-order fallback chain.
        const std::vector<resolution_input> entries{
            make_resolution("LICENSE", "mod.b", 1000, digest_of("b")),
            make_resolution("LICENSE", "mod.a", 1000, digest_of("a")),
        };
        auto built = build_resolution_index(entries, table);
        expect(!built);
        if (!built) {
            expect(
                error_is(built.error(), refrain::error_code::invalid_argument));
            const auto &message{built.error().message};
            expect(message.find("LICENSE") != std::string::npos);
            expect(message.find("1000") != std::string::npos);
            expect(message.find("mod.a") != std::string::npos);
            expect(message.find("mod.b") != std::string::npos);
        }
    };

    "rfrx rejects same-source same-priority entries"_test = [] {
        const std::vector<digest32> digests{digest_of("a"), digest_of("b")};
        auto table{make_blob_table(digests)};
        expect(table.entries.size() == 2U);
        if (table.entries.size() != 2U) {
            return;
        }
        // The conflict is the (location, priority) pair, not the source: one
        // source cannot publish two blobs for the same location at one
        // priority either.
        const std::vector<resolution_input> entries{
            make_resolution("data/a.json", "mod:a", 5, digest_of("a")),
            make_resolution("data/a.json", "mod:a", 5, digest_of("b")),
        };
        auto built = build_resolution_index(entries, table);
        expect(!built);
        if (built) {
            return;
        }
        expect(error_is(built.error(), refrain::error_code::invalid_argument));
        const auto &message{built.error().message};
        expect(message.find("data/a.json") != std::string::npos);
        expect(message.find(" priority 5") != std::string::npos);
    };

    "rfrx parser accepts a legacy same-priority tie in either order"_test = [] {
        const std::vector<digest32> digests{digest_of("a"), digest_of("b")};
        auto table{make_blob_table(digests)};
        expect(table.entries.size() == 2U);
        if (table.entries.size() != 2U) {
            return;
        }
        // Build a valid index with distinct priorities, then equalize them.
        // The builder rejects this input, but published indexes may still carry
        // a same-source tie like this and must stay parseable in either record
        // order.
        const std::vector<resolution_input> entries{
            make_resolution("data/a.json", "mod:a", 6, digest_of("a")),
            make_resolution("data/a.json", "mod:a", 5, digest_of("b")),
        };
        auto built = build_resolution_index(entries, table);
        expect(built.has_value());
        if (!built) {
            return;
        }
        auto legacy{*built};
        write_u32_le(legacy, rfrx_header_bytes + 8, 6U);
        write_u32_le(legacy, rfrx_header_bytes + rfrx_entry_bytes + 8, 6U);
        // Swap the two records: same priority, now in reverse blob order.
        const auto first{std::span<std::byte>{legacy}.subspan(
            rfrx_header_bytes, rfrx_entry_bytes)};
        const auto second{std::span<std::byte>{legacy}.subspan(
            rfrx_header_bytes + rfrx_entry_bytes, rfrx_entry_bytes)};
        std::vector<std::byte> saved(first.begin(), first.end());
        std::copy(second.begin(), second.end(), first.begin());
        std::copy(saved.begin(), saved.end(), second.begin());
        refresh_rfrx(legacy);

        auto parsed = parse_resolution_index(legacy, table);
        expect(parsed.has_value());
        if (!parsed) {
            return;
        }
        expect(parsed->entries.size() == 2U);
        if (parsed->entries.size() != 2U) {
            return;
        }
        expect(parsed->entries[0].source == "mod:a");
        expect(parsed->entries[0].blob_index ==
               table_index(table, digest_of("b")));
        expect(parsed->entries[1].blob_index ==
               table_index(table, digest_of("a")));

        auto resolved = resolve(*parsed, "data/a.json");
        expect(resolved.has_value());
        if (!resolved) {
            return;
        }
        expect(resolved->winner != nullptr);
        if (resolved->winner != nullptr) {
            const auto a_index{table_index(table, digest_of("a"))};
            const auto b_index{table_index(table, digest_of("b"))};
            expect(resolved->winner->blob_index == std::min(a_index, b_index));
        }
        expect(resolved->fallback.size() == 1U);
    };

    "rfrx rejects a duplicated record"_test = [] {
        const std::vector<digest32> digests{digest_of("a"), digest_of("b"),
                                            digest_of("c")};
        auto table{make_blob_table(digests)};
        expect(table.entries.size() == 3U);
        if (table.entries.size() != 3U) {
            return;
        }
        // Three distinct priorities keep the builder happy; then equalize the
        // priorities to form a legacy tie group. Copy the first record over
        // the last so the duplicate is separated by another blob (the whole
        // group must be checked).
        const std::vector<resolution_input> entries{
            make_resolution("data/a.json", "builtin", 3, digest_of("a")),
            make_resolution("data/a.json", "builtin", 2, digest_of("b")),
            make_resolution("data/a.json", "builtin", 1, digest_of("c")),
        };
        auto built = build_resolution_index(entries, table);
        expect(built.has_value());
        if (!built) {
            return;
        }
        auto duplicated{*built};
        write_u32_le(duplicated, rfrx_header_bytes + rfrx_entry_bytes + 8, 3U);
        write_u32_le(duplicated, rfrx_header_bytes + 2U * rfrx_entry_bytes + 8,
                     3U);
        for (std::size_t index{0}; index < rfrx_entry_bytes; ++index) {
            duplicated[rfrx_header_bytes + 2U * rfrx_entry_bytes + index] =
                duplicated[rfrx_header_bytes + index];
        }
        refresh_rfrx(duplicated);
        auto parsed = parse_resolution_index(duplicated, table);
        expect(!parsed);
        if (!parsed) {
            expect(error_is(parsed.error(), refrain::error_code::parse_error));
        }
    };

    "rfrx rejects exact duplicate entries"_test = [] {
        const auto entry{
            make_resolution("data/a.json", "builtin", 5, digest_of("a"))};
        const std::vector<digest32> digests{digest_of("a")};
        auto table{make_blob_table(digests)};
        expect(table.entries.size() == 1U);
        if (table.entries.size() != 1U) {
            return;
        }
        const std::vector<resolution_input> entries{entry, entry};
        auto built = build_resolution_index(entries, table);
        expect(!built);
        if (!built) {
            expect(
                error_is(built.error(), refrain::error_code::invalid_argument));
        }
    };

    "rfrx rejects digests missing from the blob table"_test = [] {
        const std::vector<digest32> digests{digest_of("a")};
        auto table{make_blob_table(digests)};
        expect(table.entries.size() == 1U);
        if (table.entries.size() != 1U) {
            return;
        }
        const std::vector<resolution_input> entries{
            make_resolution("data/a.json", "builtin", 1, digest_of("b"))};
        auto built = build_resolution_index(entries, table);
        expect(!built);
        if (!built) {
            expect(
                error_is(built.error(), refrain::error_code::invalid_argument));
        }
    };

    "rfrx rejects unsafe locations"_test = [] {
        const std::vector<digest32> digests{digest_of("a")};
        auto table{make_blob_table(digests)};
        expect(table.entries.size() == 1U);
        if (table.entries.size() != 1U) {
            return;
        }
        const std::vector<std::string> locations{
            "",      "/absolute", "..",  "data/../a.json",        "data//a",
            "data/", "a\\b",      "./a", std::string{"data/\x01"}};
        for (const auto &location : locations) {
            const std::vector<resolution_input> entries{
                make_resolution(location, "builtin", 1, digest_of("a"))};
            auto built = build_resolution_index(entries, table);
            expect(!built) << "location: " << location;
            if (!built) {
                expect(error_is(built.error(),
                                refrain::error_code::invalid_argument));
            }
        }
    };

    "rfrx rejects invalid utf-8 locations"_test = [] {
        const std::vector<digest32> digests{digest_of("a")};
        auto table{make_blob_table(digests)};
        expect(table.entries.size() == 1U);
        if (table.entries.size() != 1U) {
            return;
        }
        const std::string invalid{"data/\xC3\x28", 7};
        const std::vector<resolution_input> entries{
            make_resolution(invalid, "builtin", 1, digest_of("a"))};
        auto built = build_resolution_index(entries, table);
        expect(!built);
        if (!built) {
            expect(
                error_is(built.error(), refrain::error_code::invalid_argument));
        }
    };

    "rfrx enforces field limits and reserved flags"_test = [] {
        const std::vector<digest32> digests{digest_of("a")};
        auto table{make_blob_table(digests)};
        expect(table.entries.size() == 1U);
        if (table.entries.size() != 1U) {
            return;
        }
        const std::vector<resolution_input> too_long_location{make_resolution(
            std::string(1025, 'a'), "builtin", 1, digest_of("a"))};
        auto built_location = build_resolution_index(too_long_location, table);
        expect(!built_location);

        const std::vector<resolution_input> maximal_location{make_resolution(
            std::string(1024, 'a'), "builtin", 1, digest_of("a"))};
        auto built_max_location =
            build_resolution_index(maximal_location, table);
        expect(built_max_location.has_value());

        const std::vector<resolution_input> too_long_source{make_resolution(
            "data/a.json", std::string(257, 's'), 1, digest_of("a"))};
        auto built_source = build_resolution_index(too_long_source, table);
        expect(!built_source);

        const std::vector<resolution_input> maximal_source{make_resolution(
            "data/a.json", std::string(256, 's'), 1, digest_of("a"))};
        auto built_max_source = build_resolution_index(maximal_source, table);
        expect(built_max_source.has_value());

        const std::vector<resolution_input> reserved_flags{
            make_resolution("data/a.json", "builtin", 1, digest_of("a"), 1)};
        auto built_flags = build_resolution_index(reserved_flags, table);
        expect(!built_flags);
    };

    "rfrx sorts and dedupes the source table"_test = [] {
        const std::vector<digest32> digests{digest_of("a"), digest_of("b"),
                                            digest_of("c")};
        auto table{make_blob_table(digests)};
        expect(table.entries.size() == 3U);
        if (table.entries.size() != 3U) {
            return;
        }
        const std::vector<resolution_input> entries{
            make_resolution("data/a.json", "z", 1, digest_of("a")),
            make_resolution("data/b.json", "a", 1, digest_of("b")),
            make_resolution("data/c.json", "z", 2, digest_of("c")),
        };
        auto built = build_resolution_index(entries, table);
        expect(built.has_value());
        if (!built) {
            return;
        }
        const auto bytes{std::span<const std::byte>{*built}};
        expect(read_u32_le(bytes, 12) == 2U);
        const auto arena_offset{
            static_cast<std::size_t>(read_u64_le(bytes, 32))};
        const auto arena{bytes.subspan(arena_offset)};
        expect(arena.size() >= 4U);
        if (arena.size() >= 4U) {
            expect(std::to_integer<char>(arena[0]) == 'a');
            expect(std::to_integer<char>(arena[1]) == '\0');
            expect(std::to_integer<char>(arena[2]) == 'z');
            expect(std::to_integer<char>(arena[3]) == '\0');
        }
        auto parsed = parse_resolution_index(*built, table);
        expect(parsed.has_value());
        if (!parsed) {
            return;
        }
        for (const auto &entry : parsed->entries) {
            expect(entry.source == "a" || entry.source == "z");
        }
    };

    "rfrx preserves int32 priority extremes"_test = [] {
        const std::vector<digest32> digests{digest_of("a"), digest_of("b")};
        auto table{make_blob_table(digests)};
        expect(table.entries.size() == 2U);
        if (table.entries.size() != 2U) {
            return;
        }
        const std::vector<resolution_input> entries{
            make_resolution("data/a.json", "min", -2147483647 - 1,
                            digest_of("a")),
            make_resolution("data/a.json", "max", 2147483647, digest_of("b")),
        };
        auto built = build_resolution_index(entries, table);
        expect(built.has_value());
        if (!built) {
            return;
        }
        auto parsed = parse_resolution_index(*built, table);
        expect(parsed.has_value());
        if (!parsed) {
            return;
        }
        auto resolved = resolve(*parsed, "data/a.json");
        expect(resolved.has_value());
        if (resolved && resolved->winner != nullptr) {
            expect(resolved->winner->priority == 2147483647);
            expect(resolved->fallback.size() == 1U);
            if (resolved->fallback.size() == 1U) {
                expect(resolved->fallback[0]->priority == -2147483647 - 1);
            }
        }
    };

    "rfrx accepts an empty index"_test = [] {
        const std::vector<digest32> digests{digest_of("a")};
        auto table{make_blob_table(digests)};
        expect(table.entries.size() == 1U);
        if (table.entries.size() != 1U) {
            return;
        }
        auto built = build_resolution_index({}, table);
        expect(built.has_value());
        if (!built) {
            return;
        }
        expect(built->size() == rfrx_header_bytes);
        expect(read_u32_le(*built, 8) == 0U);
        expect(read_u32_le(*built, 12) == 0U);
        expect(read_u64_le(*built, 32) == rfrx_header_bytes);
        expect(read_u64_le(*built, 40) == 0U);
        auto parsed = parse_resolution_index(*built, table);
        expect(parsed.has_value());
        if (parsed) {
            expect(parsed->entries.empty());
        }
    };

    "rfrx rejects a bad header crc"_test = [] {
        const std::vector<digest32> digests{digest_of("a")};
        auto table{make_blob_table(digests)};
        expect(table.entries.size() == 1U);
        if (table.entries.size() != 1U) {
            return;
        }
        auto built = build_resolution_index(
            std::vector<resolution_input>{
                make_resolution("data/a.json", "builtin", 1, digest_of("a"))},
            table);
        expect(built.has_value());
        if (!built) {
            return;
        }
        (*built)[80] = static_cast<std::byte>(0xFF);
        auto parsed = parse_resolution_index(*built, table);
        expect(!parsed);
        if (!parsed) {
            expect(error_is(parsed.error(), refrain::error_code::parse_error));
        }
    };

    "rfrx rejects a bad file blake3"_test = [] {
        const std::vector<digest32> digests{digest_of("a")};
        auto table{make_blob_table(digests)};
        expect(table.entries.size() == 1U);
        if (table.entries.size() != 1U) {
            return;
        }
        auto built = build_resolution_index(
            std::vector<resolution_input>{
                make_resolution("data/a.json", "builtin", 1, digest_of("a"))},
            table);
        expect(built.has_value());
        if (!built) {
            return;
        }
        (*built)[128 + 16] = static_cast<std::byte>(0xFF); // blob_index byte
        auto parsed = parse_resolution_index(*built, table);
        expect(!parsed);
        if (!parsed) {
            expect(error_is(parsed.error(), refrain::error_code::parse_error));
        }
    };

    "rfrx rejects reserved header and entry bytes"_test = [] {
        const std::vector<digest32> digests{digest_of("a")};
        auto table{make_blob_table(digests)};
        expect(table.entries.size() == 1U);
        if (table.entries.size() != 1U) {
            return;
        }
        auto built = build_resolution_index(
            std::vector<resolution_input>{
                make_resolution("data/a.json", "builtin", 1, digest_of("a"))},
            table);
        expect(built.has_value());
        if (!built) {
            return;
        }
        auto header_reserved{*built};
        header_reserved[100] = std::byte{1};
        auto parsed_header = parse_resolution_index(header_reserved, table);
        expect(!parsed_header);

        auto entry_reserved{*built};
        write_u16_le(entry_reserved, 128 + 14, 1); // entry flags
        refresh_rfrx(entry_reserved);
        auto parsed_entry = parse_resolution_index(entry_reserved, table);
        expect(!parsed_entry);
        if (!parsed_entry) {
            expect(error_is(parsed_entry.error(),
                            refrain::error_code::parse_error));
        }

        auto entry_reserved2{*built};
        write_u32_le(entry_reserved2, 128 + 28, 1); // reserved2
        refresh_rfrx(entry_reserved2);
        auto parsed_entry2 = parse_resolution_index(entry_reserved2, table);
        expect(!parsed_entry2);
        if (!parsed_entry2) {
            expect(error_is(parsed_entry2.error(),
                            refrain::error_code::parse_error));
        }
    };

    "rfrx rejects a different blob table"_test = [] {
        const auto first_table{make_blob_table(
            std::vector<digest32>{digest_of("a"), digest_of("b")})};
        const auto second_table{make_blob_table(std::vector<digest32>{
            digest_of("a"), digest_of("b"), digest_of("c")})};
        expect(first_table.entries.size() == 2U);
        expect(second_table.entries.size() == 3U);
        if (first_table.entries.size() != 2U ||
            second_table.entries.size() != 3U) {
            return;
        }
        auto built = build_resolution_index(
            std::vector<resolution_input>{
                make_resolution("data/a.json", "builtin", 1, digest_of("a"))},
            first_table);
        expect(built.has_value());
        if (!built) {
            return;
        }
        auto parsed = parse_resolution_index(*built, second_table);
        expect(!parsed);
        if (!parsed) {
            expect(error_is(parsed.error(), refrain::error_code::parse_error));
        }
    };

    "rfrx rejects an out-of-range blob index"_test = [] {
        const std::vector<digest32> digests{digest_of("a")};
        auto table{make_blob_table(digests)};
        expect(table.entries.size() == 1U);
        if (table.entries.size() != 1U) {
            return;
        }
        auto built = build_resolution_index(
            std::vector<resolution_input>{
                make_resolution("data/a.json", "builtin", 1, digest_of("a"))},
            table);
        expect(built.has_value());
        if (!built) {
            return;
        }
        write_u32_le(*built, 128 + 16, 99); // blob_index
        refresh_rfrx(*built);
        auto parsed = parse_resolution_index(*built, table);
        expect(!parsed);
        if (!parsed) {
            expect(error_is(parsed.error(), refrain::error_code::parse_error));
        }
    };

    "rfrx rejects unsorted records"_test = [] {
        const std::vector<digest32> digests{digest_of("a"), digest_of("b")};
        auto table{make_blob_table(digests)};
        expect(table.entries.size() == 2U);
        if (table.entries.size() != 2U) {
            return;
        }
        const std::vector<resolution_input> entries{
            make_resolution("data/a.json", "builtin", 1, digest_of("a")),
            make_resolution("data/b.json", "builtin", 1, digest_of("b")),
        };
        auto built = build_resolution_index(entries, table);
        expect(built.has_value());
        if (!built) {
            return;
        }
        auto swapped{*built};
        for (std::size_t index{0}; index < rfrx_entry_bytes; ++index) {
            std::swap(swapped[128 + index],
                      swapped[128 + rfrx_entry_bytes + index]);
        }
        refresh_rfrx(swapped);
        auto parsed = parse_resolution_index(swapped, table);
        expect(!parsed);
        if (!parsed) {
            expect(error_is(parsed.error(), refrain::error_code::parse_error));
        }
    };

    "rfrx rejects truncated and foreign headers"_test = [] {
        const std::vector<digest32> digests{digest_of("a")};
        auto table{make_blob_table(digests)};
        expect(table.entries.size() == 1U);
        if (table.entries.size() != 1U) {
            return;
        }
        auto built = build_resolution_index(
            std::vector<resolution_input>{
                make_resolution("data/a.json", "builtin", 1, digest_of("a"))},
            table);
        expect(built.has_value());
        if (!built) {
            return;
        }
        for (const std::size_t size :
             {std::size_t{0}, std::size_t{100}, rfrx_header_bytes + 16U}) {
            auto parsed = parse_resolution_index(
                std::span<const std::byte>{*built}.first(size), table);
            expect(!parsed);
            if (!parsed) {
                expect(
                    error_is(parsed.error(), refrain::error_code::parse_error));
            }
        }

        auto bad_magic{*built};
        bad_magic[0] = static_cast<std::byte>('X');
        auto parsed_magic = parse_resolution_index(bad_magic, table);
        expect(!parsed_magic);
        if (!parsed_magic) {
            expect(error_is(parsed_magic.error(),
                            refrain::error_code::parse_error));
        }

        auto bad_version{*built};
        write_u16_le(bad_version, 4, 2);
        auto parsed_version = parse_resolution_index(bad_version, table);
        expect(!parsed_version);
        if (!parsed_version) {
            expect(error_is(parsed_version.error(),
                            refrain::error_code::version_mismatch));
        }

        auto bad_flags{*built};
        write_u16_le(bad_flags, 6, 1);
        auto parsed_flags = parse_resolution_index(bad_flags, table);
        expect(!parsed_flags);
        if (!parsed_flags) {
            expect(error_is(parsed_flags.error(),
                            refrain::error_code::parse_error));
        }
    };
};

// ---------------------------------------------------------------------------
// Read-only mmap-backed views.
// ---------------------------------------------------------------------------

const suite content_index_mapped_tests = [] {
    "mapped rfbt exposes validated records on demand"_test = [] {
        const std::vector<blob_table_entry> entries{
            make_entry("charlie", 70000, blob_flags_packed, 2, blob_kind_cook,
                       2, 42000),
            make_entry("alpha", 12, 0, 7, blob_kind_content, 0),
            make_entry("bravo", 0, 0, 0, blob_kind_builtin, 0),
        };
        auto built{build_blob_table(entries)};
        expect(built.has_value());
        if (!built) {
            return;
        }
        auto parsed{parse_blob_table(*built)};
        expect(parsed.has_value());
        if (!parsed) {
            return;
        }

        mapped_scratch scratch;
        const auto path{scratch_path(scratch, "blobs.idx")};
        expect(scratch_write(scratch, "blobs.idx", *built));
        auto mapped{mapped_blob_table::open(path)};
        expect(mapped.has_value());
        if (!mapped) {
            return;
        }
        expect(mapped->valid());
        expect(mapped->size() == parsed->entries.size());
        if (mapped->size() != parsed->entries.size()) {
            return;
        }
        for (std::size_t index{0}; index < parsed->entries.size(); ++index) {
            const auto actual{mapped->at(index)};
            const auto &expected{parsed->entries[index]};
            expect(same_digest(actual.digest, expected.digest));
            expect(actual.logical_size == expected.logical_size);
            expect(actual.flags == expected.flags);
            expect(actual.refcount == expected.refcount);
            expect(actual.kind == expected.kind);
            expect(actual.block_count == expected.block_count);
            expect(actual.stored_size == expected.stored_size);
            expect(actual.segment_id == expected.segment_id);
            expect(actual.segment_offset == expected.segment_offset);
        }

        const auto alpha{mapped->find(digest_of("alpha"))};
        expect(alpha.has_value());
        if (alpha) {
            expect(alpha->refcount == 7U);
            expect(alpha->stored_size == 12U);
        }
        expect(!mapped->find(digest_of("missing")).has_value());

        const auto prefix{mapped->blake3_prefix()};
        for (std::size_t index{0}; index < prefix.size(); ++index) {
            expect(prefix[index] == parsed->blob_table_blake3_prefix[index]);
        }
    };

    "mapped rfbt accepts an empty table and rejects file errors"_test = [] {
        auto built{build_blob_table({})};
        expect(built.has_value());
        if (!built) {
            return;
        }
        expect(built->size() == rfbt_header_bytes);

        mapped_scratch scratch;
        const auto path{scratch_path(scratch, "blobs.idx")};
        expect(scratch_write(scratch, "blobs.idx", *built));
        auto mapped{mapped_blob_table::open(path)};
        expect(mapped.has_value());
        if (!mapped) {
            return;
        }
        expect(mapped->valid());
        expect(mapped->size() == 0U);
        expect(!mapped->find(digest_of("alpha")).has_value());

        const auto digest{blake3(*built)};
        const auto prefix{mapped->blake3_prefix()};
        for (std::size_t index{0}; index < prefix.size(); ++index) {
            expect(prefix[index] == digest[index]);
        }

        const auto missing{mapped_blob_table::open(
            scratch_path(scratch, "does-not-exist.idx"))};
        expect(!missing.has_value());
        if (!missing) {
            expect(error_is(missing.error(), refrain::error_code::io_error));
        }
        const auto empty_path{mapped_blob_table::open("")};
        expect(!empty_path.has_value());
        if (!empty_path) {
            expect(error_is(empty_path.error(),
                            refrain::error_code::invalid_argument));
        }

        expect(scratch_write(scratch, "empty-file.idx",
                             std::span<const std::byte>{}));
        const auto empty_file{
            mapped_blob_table::open(scratch_path(scratch, "empty-file.idx"))};
        expect(!empty_file.has_value());
        if (!empty_file) {
            expect(
                error_is(empty_file.error(), refrain::error_code::parse_error));
        }
    };

    "mapped rfbt rejects header and record corruption"_test = [] {
        auto built{build_blob_table(
            std::vector<blob_table_entry>{make_entry("alpha")})};
        expect(built.has_value());
        if (!built) {
            return;
        }
        mapped_scratch scratch;
        const auto expect_failure = [](const auto &opened,
                                       refrain::error_code code) {
            expect(!opened.has_value());
            if (!opened) {
                expect(error_is(opened.error(), code));
            }
        };

        auto bad_magic{*built};
        bad_magic[0] = static_cast<std::byte>('X');
        expect(scratch_write(scratch, "bad-magic.idx", bad_magic));
        expect_failure(
            mapped_blob_table::open(scratch_path(scratch, "bad-magic.idx")),
            refrain::error_code::parse_error);

        auto bad_version{*built};
        write_u16_le(bad_version, 4, 2);
        expect(scratch_write(scratch, "bad-version.idx", bad_version));
        expect_failure(
            mapped_blob_table::open(scratch_path(scratch, "bad-version.idx")),
            refrain::error_code::version_mismatch);

        auto bad_flags{*built};
        write_u16_le(bad_flags, 6, 1);
        expect(scratch_write(scratch, "bad-flags.idx", bad_flags));
        expect_failure(
            mapped_blob_table::open(scratch_path(scratch, "bad-flags.idx")),
            refrain::error_code::parse_error);

        auto bad_entry_size{*built};
        write_u32_le(bad_entry_size, 12, 32);
        expect(scratch_write(scratch, "bad-entry-size.idx", bad_entry_size));
        expect_failure(mapped_blob_table::open(
                           scratch_path(scratch, "bad-entry-size.idx")),
                       refrain::error_code::parse_error);

        auto bad_reserved{*built};
        bad_reserved[100] = std::byte{1};
        expect(scratch_write(scratch, "bad-reserved.idx", bad_reserved));
        expect_failure(
            mapped_blob_table::open(scratch_path(scratch, "bad-reserved.idx")),
            refrain::error_code::parse_error);

        auto bad_crc{*built};
        bad_crc[64] = static_cast<std::byte>(0xFF);
        expect(scratch_write(scratch, "bad-crc.idx", bad_crc));
        expect_failure(
            mapped_blob_table::open(scratch_path(scratch, "bad-crc.idx")),
            refrain::error_code::parse_error);

        auto bad_blake3{*built};
        bad_blake3[rfbt_header_bytes + 40] = static_cast<std::byte>(0xFF);
        expect(scratch_write(scratch, "bad-blake3.idx", bad_blake3));
        expect_failure(
            mapped_blob_table::open(scratch_path(scratch, "bad-blake3.idx")),
            refrain::error_code::parse_error);

        expect(scratch_write(scratch, "truncated.idx",
                             std::span<const std::byte>{*built}.first(100)));
        expect_failure(
            mapped_blob_table::open(scratch_path(scratch, "truncated.idx")),
            refrain::error_code::parse_error);

        auto trailing{*built};
        trailing.push_back(std::byte{0});
        expect(scratch_write(scratch, "trailing.idx", trailing));
        expect_failure(
            mapped_blob_table::open(scratch_path(scratch, "trailing.idx")),
            refrain::error_code::parse_error);

        auto bad_metadata{*built};
        write_u32_le(bad_metadata, rfbt_header_bytes + 52, 1);
        refresh_rfbt(bad_metadata);
        expect(scratch_write(scratch, "bad-metadata.idx", bad_metadata));
        expect_failure(
            mapped_blob_table::open(scratch_path(scratch, "bad-metadata.idx")),
            refrain::error_code::parse_error);

        auto bad_record_reserved{*built};
        bad_record_reserved[rfbt_header_bytes + 50] = std::byte{1};
        refresh_rfbt(bad_record_reserved);
        expect(scratch_write(scratch, "bad-record-reserved.idx",
                             bad_record_reserved));
        expect_failure(mapped_blob_table::open(
                           scratch_path(scratch, "bad-record-reserved.idx")),
                       refrain::error_code::parse_error);
    };

    "mapped rfbt rejects unsorted and duplicate records"_test = [] {
        auto built{build_blob_table(std::vector<blob_table_entry>{
            make_entry("alpha"), make_entry("bravo")})};
        expect(built.has_value());
        if (!built) {
            return;
        }
        mapped_scratch scratch;
        const auto expect_failure = [](const auto &opened,
                                       refrain::error_code code) {
            expect(!opened.has_value());
            if (!opened) {
                expect(error_is(opened.error(), code));
            }
        };

        auto swapped{*built};
        for (std::size_t index{0}; index < rfbt_entry_bytes; ++index) {
            std::swap(swapped[rfbt_header_bytes + index],
                      swapped[rfbt_header_bytes + rfbt_entry_bytes + index]);
        }
        refresh_rfbt(swapped);
        expect(scratch_write(scratch, "unsorted.idx", swapped));
        expect_failure(
            mapped_blob_table::open(scratch_path(scratch, "unsorted.idx")),
            refrain::error_code::parse_error);

        auto duplicated{*built};
        for (std::size_t index{0}; index < 32U; ++index) {
            duplicated[rfbt_header_bytes + rfbt_entry_bytes + index] =
                duplicated[rfbt_header_bytes + index];
        }
        refresh_rfbt(duplicated);
        expect(scratch_write(scratch, "duplicate.idx", duplicated));
        expect_failure(
            mapped_blob_table::open(scratch_path(scratch, "duplicate.idx")),
            refrain::error_code::parse_error);
    };

    "mapped rfrx decodes entries and binds the blob table"_test = [] {
        const std::vector<digest32> digests{digest_of("a"), digest_of("b"),
                                            digest_of("c")};
        auto fixture{make_table_fixture(digests)};
        expect(fixture.view.entries.size() == 3U);
        if (fixture.view.entries.size() != 3U) {
            return;
        }
        const std::vector<resolution_input> entries{
            make_resolution("data/refrain_test/a.json", "mod:alpha", 10,
                            digest_of("a")),
            make_resolution("data/refrain_test/a.json", "builtin", 5,
                            digest_of("b")),
            make_resolution("assets/refrain_test/lang/en_us.json", "mod:alpha",
                            20, digest_of("c")),
        };
        auto index_bytes{build_resolution_index(entries, fixture.view)};
        expect(index_bytes.has_value());
        if (!index_bytes) {
            return;
        }
        auto parsed{parse_resolution_index(*index_bytes, fixture.view)};
        expect(parsed.has_value());
        if (!parsed) {
            return;
        }

        mapped_scratch scratch;
        expect(scratch_write(scratch, "blobs.idx", fixture.bytes));
        expect(scratch_write(scratch, "composition.idx", *index_bytes));
        const auto table_path{scratch_path(scratch, "blobs.idx")};
        const auto index_path{scratch_path(scratch, "composition.idx")};

        auto mapped_table{mapped_blob_table::open(table_path)};
        expect(mapped_table.has_value());
        if (!mapped_table) {
            return;
        }
        auto mapped_index{
            mapped_resolution_index::open(index_path, *mapped_table)};
        expect(mapped_index.has_value());
        if (!mapped_index) {
            return;
        }
        expect(mapped_index->valid());
        expect(mapped_index->size() == parsed->entries.size());
        if (mapped_index->size() != parsed->entries.size()) {
            return;
        }
        for (std::size_t index{0}; index < parsed->entries.size(); ++index) {
            const auto actual{mapped_index->at(index)};
            const auto &expected{parsed->entries[index]};
            expect(actual.location == expected.location);
            expect(actual.source == expected.source);
            expect(actual.priority == expected.priority);
            expect(actual.blob_index == expected.blob_index);
            expect(actual.flags == expected.flags);
        }

        for (std::size_t index{0}; index < 8U; ++index) {
            expect(mapped_index->blob_table_prefix()[index] ==
                   mapped_table->blake3_prefix()[index]);
            expect(mapped_index->blob_table_prefix()[index] ==
                   fixture.view.blob_table_blake3_prefix[index]);
        }

        auto by_view{mapped_resolution_index::open(index_path, fixture.view)};
        expect(by_view.has_value());
        if (by_view) {
            expect(by_view->size() == parsed->entries.size());
        }

        const mapped_blob_table invalid_table{};
        const auto bad_binding{
            mapped_resolution_index::open(index_path, invalid_table)};
        expect(!bad_binding.has_value());
        if (!bad_binding) {
            expect(error_is(bad_binding.error(),
                            refrain::error_code::invalid_argument));
        }

        const auto chain{mapped_index->find("data/refrain_test/a.json")};
        expect(chain.size() == 2U);
        if (chain.size() == 2U) {
            expect(chain.front().priority == 10);
            expect(chain.front().source == "mod:alpha");
            expect(chain.front().blob_index ==
                   table_index(fixture.view, digest_of("a")));
            expect(chain[1].priority == 5);
            expect(chain[1].source == "builtin");
            expect(chain[1].blob_index ==
                   table_index(fixture.view, digest_of("b")));
        }
        expect(
            mapped_index->find(std::string_view{"data/missing.json"}).empty());

        const auto assets_hash{
            index_location_hash("assets/refrain_test/lang/en_us.json")};
        const auto by_hash{mapped_index->find(assets_hash)};
        expect(by_hash.size() == 1U);
        if (!by_hash.empty()) {
            expect(by_hash.front().location ==
                   "assets/refrain_test/lang/en_us.json");
            expect(by_hash.front().priority == 20);
        }
        expect(mapped_index->find(index_location_hash("data/missing.json"))
                   .empty());
    };

    "mapped rfrx accepts an empty index"_test = [] {
        auto fixture{make_table_fixture(std::vector<digest32>{digest_of("a")})};
        expect(fixture.view.entries.size() == 1U);
        if (fixture.view.entries.size() != 1U) {
            return;
        }
        auto index_bytes{build_resolution_index({}, fixture.view)};
        expect(index_bytes.has_value());
        if (!index_bytes) {
            return;
        }
        expect(index_bytes->size() == rfrx_header_bytes);
        expect(read_u32_le(*index_bytes, 8) == 0U);
        expect(read_u32_le(*index_bytes, 12) == 0U);

        mapped_scratch scratch;
        expect(scratch_write(scratch, "blobs.idx", fixture.bytes));
        expect(scratch_write(scratch, "empty.idx", *index_bytes));
        auto mapped_table{
            mapped_blob_table::open(scratch_path(scratch, "blobs.idx"))};
        expect(mapped_table.has_value());
        if (!mapped_table) {
            return;
        }
        auto mapped{mapped_resolution_index::open(
            scratch_path(scratch, "empty.idx"), *mapped_table)};
        expect(mapped.has_value());
        if (!mapped) {
            return;
        }
        expect(mapped->valid());
        expect(mapped->size() == 0U);
        expect(mapped->find("data/a.json").empty());
        expect(mapped->find(index_location_hash("data/a.json")).empty());
        for (std::size_t index{0}; index < 8U; ++index) {
            expect(mapped->blob_table_prefix()[index] ==
                   fixture.view.blob_table_blake3_prefix[index]);
        }
    };

    "mapped rfrx rejects header, entry and binding corruption"_test = [] {
        const std::vector<digest32> digests{digest_of("a"), digest_of("b")};
        auto fixture{make_table_fixture(digests)};
        expect(fixture.view.entries.size() == 2U);
        if (fixture.view.entries.size() != 2U) {
            return;
        }
        const std::vector<resolution_input> entries{
            make_resolution("data/a.json", "builtin", 1, digest_of("a")),
            make_resolution("data/b.json", "builtin", 1, digest_of("b")),
        };
        auto built{build_resolution_index(entries, fixture.view)};
        expect(built.has_value());
        if (!built) {
            return;
        }
        mapped_scratch scratch;
        const auto expect_failure = [](const auto &opened,
                                       refrain::error_code code) {
            expect(!opened.has_value());
            if (!opened) {
                expect(error_is(opened.error(), code));
            }
        };
        const auto open_index = [&scratch, &fixture](std::string_view name) {
            return mapped_resolution_index::open(scratch_path(scratch, name),
                                                 fixture.view);
        };

        auto bad_magic{*built};
        bad_magic[0] = static_cast<std::byte>('X');
        expect(scratch_write(scratch, "bad-magic.idx", bad_magic));
        expect_failure(open_index("bad-magic.idx"),
                       refrain::error_code::parse_error);

        auto bad_version{*built};
        write_u16_le(bad_version, 4, 2);
        expect(scratch_write(scratch, "bad-version.idx", bad_version));
        expect_failure(open_index("bad-version.idx"),
                       refrain::error_code::version_mismatch);

        auto bad_flags{*built};
        write_u16_le(bad_flags, 6, 1);
        expect(scratch_write(scratch, "bad-flags.idx", bad_flags));
        expect_failure(open_index("bad-flags.idx"),
                       refrain::error_code::parse_error);

        auto bad_reserved{*built};
        bad_reserved[100] = std::byte{1};
        expect(scratch_write(scratch, "bad-reserved.idx", bad_reserved));
        expect_failure(open_index("bad-reserved.idx"),
                       refrain::error_code::parse_error);

        auto bad_crc{*built};
        bad_crc[80] = static_cast<std::byte>(0xFF);
        expect(scratch_write(scratch, "bad-crc.idx", bad_crc));
        expect_failure(open_index("bad-crc.idx"),
                       refrain::error_code::parse_error);

        auto bad_blake3{*built};
        bad_blake3[rfrx_header_bytes + 8] = static_cast<std::byte>(0xFF);
        expect(scratch_write(scratch, "bad-blake3.idx", bad_blake3));
        expect_failure(open_index("bad-blake3.idx"),
                       refrain::error_code::parse_error);

        expect(scratch_write(scratch, "truncated.idx",
                             std::span<const std::byte>{*built}.first(100)));
        expect_failure(open_index("truncated.idx"),
                       refrain::error_code::parse_error);

        expect(scratch_write(
            scratch, "truncated-table.idx",
            std::span<const std::byte>{*built}.first(rfrx_header_bytes + 16U)));
        expect_failure(open_index("truncated-table.idx"),
                       refrain::error_code::parse_error);

        auto unsorted{*built};
        for (std::size_t index{0}; index < rfrx_entry_bytes; ++index) {
            std::swap(unsorted[rfrx_header_bytes + index],
                      unsorted[rfrx_header_bytes + rfrx_entry_bytes + index]);
        }
        refresh_rfrx(unsorted);
        expect(scratch_write(scratch, "unsorted.idx", unsorted));
        expect_failure(open_index("unsorted.idx"),
                       refrain::error_code::parse_error);

        auto duplicated{*built};
        for (std::size_t index{0}; index < rfrx_entry_bytes; ++index) {
            duplicated[rfrx_header_bytes + rfrx_entry_bytes + index] =
                duplicated[rfrx_header_bytes + index];
        }
        refresh_rfrx(duplicated);
        expect(scratch_write(scratch, "duplicate.idx", duplicated));
        expect_failure(open_index("duplicate.idx"),
                       refrain::error_code::parse_error);

        auto bad_blob_index{*built};
        write_u32_le(bad_blob_index, rfrx_header_bytes + 16, 99);
        refresh_rfrx(bad_blob_index);
        expect(scratch_write(scratch, "bad-blob-index.idx", bad_blob_index));
        expect_failure(open_index("bad-blob-index.idx"),
                       refrain::error_code::parse_error);

        auto bad_entry_flags{*built};
        write_u16_le(bad_entry_flags, rfrx_header_bytes + 14, 1);
        refresh_rfrx(bad_entry_flags);
        expect(scratch_write(scratch, "bad-entry-flags.idx", bad_entry_flags));
        expect_failure(open_index("bad-entry-flags.idx"),
                       refrain::error_code::parse_error);

        auto bad_entry_reserved{*built};
        write_u32_le(bad_entry_reserved, rfrx_header_bytes + 28, 1);
        refresh_rfrx(bad_entry_reserved);
        expect(scratch_write(scratch, "bad-entry-reserved.idx",
                             bad_entry_reserved));
        expect_failure(open_index("bad-entry-reserved.idx"),
                       refrain::error_code::parse_error);

        auto bad_hash{*built};
        write_u64_le(bad_hash, rfrx_header_bytes,
                     read_u64_le(*built, rfrx_header_bytes) ^ 1U);
        refresh_rfrx(bad_hash);
        expect(scratch_write(scratch, "bad-hash.idx", bad_hash));
        expect_failure(open_index("bad-hash.idx"),
                       refrain::error_code::parse_error);

        auto bad_path{*built};
        write_u32_le(bad_path, rfrx_header_bytes + 20, 12345);
        refresh_rfrx(bad_path);
        expect(scratch_write(scratch, "bad-path.idx", bad_path));
        expect_failure(open_index("bad-path.idx"),
                       refrain::error_code::parse_error);

        expect(scratch_write(scratch, "good.idx", *built));
        const auto other_table{
            make_blob_table(std::vector<digest32>{digest_of("z")})};
        expect(other_table.entries.size() == 1U);
        if (other_table.entries.size() == 1U) {
            const auto mismatch{mapped_resolution_index::open(
                scratch_path(scratch, "good.idx"), other_table)};
            expect(!mismatch.has_value());
            if (!mismatch) {
                expect(error_is(mismatch.error(),
                                refrain::error_code::parse_error));
            }
        }

        const auto missing{mapped_resolution_index::open(
            scratch_path(scratch, "does-not-exist.idx"), fixture.view)};
        expect(!missing.has_value());
        if (!missing) {
            expect(error_is(missing.error(), refrain::error_code::io_error));
        }
        const auto empty_path{mapped_resolution_index::open("", fixture.view)};
        expect(!empty_path.has_value());
        if (!empty_path) {
            expect(error_is(empty_path.error(),
                            refrain::error_code::invalid_argument));
        }

        expect(scratch_write(scratch, "empty-file.idx",
                             std::span<const std::byte>{}));
        expect_failure(open_index("empty-file.idx"),
                       refrain::error_code::parse_error);
    };
};

} // namespace
