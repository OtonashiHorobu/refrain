// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/content_store.hpp>

#include <refrain/core/bytes.hpp>
#include <refrain/core/io.hpp>
#include <refrain/data/content_segment.hpp>
#include <refrain/platform/mmap.hpp>

#include <fmt/format.h>

#include <zstd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::data {

/// Bounded LRU of decompressed packed 64 KiB blocks, keyed by `(logical blob
/// digest, block index)` and bounded by the total decompressed bytes it holds.
/// Capacity 0 disables it.
///
/// All access is serialized by an internal mutex, while a returned block stays
/// alive through its `shared_ptr` even when another thread evicts or replaces
/// it, so callers copy their slice out after dropping the lock. Entries larger
/// than the whole capacity are never cached. This is runtime state only; it
/// never changes the on-disk format.
///
/// Defined only in this translation unit: `content_store::cache_state` owns
/// one, and the free read helpers take a pointer to it.
class block_cache {
  public:
    explicit block_cache(std::size_t capacity_bytes) noexcept
        : capacity_bytes_{capacity_bytes} {}

    block_cache(const block_cache &) = delete;
    auto operator=(const block_cache &) -> block_cache & = delete;

    /// True when a non-zero byte budget was configured.
    [[nodiscard]] auto enabled() const noexcept -> bool {
        return capacity_bytes_ != 0;
    }

    /// Cached block bytes, or null on a miss/disabled cache.
    [[nodiscard]] auto get(const digest32 &digest, std::uint32_t block_index)
        -> std::shared_ptr<const std::vector<std::byte>> {
        if (capacity_bytes_ == 0U) {
            return nullptr;
        }
        const key_type key{digest, block_index};
        const std::lock_guard lock{mutex_};
        const auto found{entries_.find(key)};
        if (found == entries_.end()) {
            return nullptr;
        }
        order_.splice(order_.begin(), order_, found->second.position);
        return found->second.bytes;
    }

    /// Insert/replace one block and evict least-recently-used entries until the
    /// byte budget holds. `bytes` must be the complete decompressed block.
    void put(const digest32 &digest, std::uint32_t block_index,
             std::shared_ptr<const std::vector<std::byte>> bytes) {
        if (capacity_bytes_ == 0U || bytes == nullptr ||
            bytes->size() > capacity_bytes_) {
            return;
        }
        const auto size{bytes->size()};
        const key_type key{digest, block_index};
        const std::lock_guard lock{mutex_};
        if (const auto found{entries_.find(key)}; found != entries_.end()) {
            total_bytes_ -= found->second.bytes->size();
            order_.erase(found->second.position);
            entries_.erase(found);
        }
        order_.push_front(key);
        entries_.emplace(key, entry{std::move(bytes), order_.begin()});
        total_bytes_ += size;
        while (total_bytes_ > capacity_bytes_) {
            const auto victim{order_.back()};
            order_.pop_back();
            const auto found{entries_.find(victim)};
            total_bytes_ -= found->second.bytes->size();
            entries_.erase(found);
        }
    }

  private:
    using key_type = std::pair<digest32, std::uint32_t>;

    struct entry {
        std::shared_ptr<const std::vector<std::byte>> bytes{};
        std::list<key_type>::iterator position{};
    };

    std::size_t capacity_bytes_{0};
    std::mutex mutex_{};
    std::size_t total_bytes_{0};
    std::list<key_type> order_{};
    std::map<key_type, entry> entries_{};
};

namespace {

using core::bytes::align_up;
using core::bytes::read_u16;
using core::bytes::read_u32;
using core::bytes::read_u64;

// ---------------------------------------------------------------------------
// RFBL v1 layout (all integers little-endian).
// ---------------------------------------------------------------------------

constexpr std::size_t blob_header_bytes{96};
constexpr std::size_t block_table_entry_bytes{48};
constexpr std::uint32_t blob_block_size{65536};
constexpr std::uint32_t raw_blob_limit{65536};
constexpr std::uint32_t max_block_count{1U << 20};
constexpr std::uint64_t block_alignment{64};
constexpr std::uint16_t blob_format_version{1};
constexpr std::uint16_t flag_packed{0x0001};
constexpr std::uint16_t flag_all_raw{0x0002};
constexpr std::uint16_t flag_known{
    static_cast<std::uint16_t>(flag_packed | flag_all_raw)};
constexpr std::int32_t blob_zstd_level{3};

constexpr std::size_t header_magic_offset{0};
constexpr std::size_t header_version_offset{4};
constexpr std::size_t header_flags_offset{6};
constexpr std::size_t header_block_size_offset{8};
constexpr std::size_t header_block_count_offset{12};
constexpr std::size_t header_logical_size_offset{16};
constexpr std::size_t header_file_blake3_offset{24};
constexpr std::size_t header_table_size_offset{56};
constexpr std::size_t header_crc32c_offset{60};
constexpr std::size_t header_table_blake3_offset{64};
constexpr std::size_t header_table_blake3_prefix_bytes{16};
constexpr std::size_t header_reserved_offset{80};
constexpr std::size_t header_reserved_bytes{16};

constexpr std::size_t table_stored_offset_offset{0};
constexpr std::size_t table_stored_size_offset{8};
constexpr std::size_t table_raw_size_offset{12};
constexpr std::size_t table_block_blake3_offset{16};

// ---------------------------------------------------------------------------
// Helpers.
// ---------------------------------------------------------------------------

/// Keep exactly one '/' between a directory and a child name.
[[nodiscard]] auto join_path(std::string_view base, std::string_view child)
    -> std::string {
    std::string joined{base};
    if (joined.empty() || joined.back() != '/') {
        joined.push_back('/');
    }
    joined.append(child);
    return joined;
}

/// Strip trailing separators from a store root (keeping a bare "/").
[[nodiscard]] auto normalize_root(std::string_view root) -> std::string {
    std::string normalized{root};
    while (normalized.size() > 1U && normalized.back() == '/') {
        normalized.pop_back();
    }
    return normalized;
}

[[nodiscard]] auto invalid_argument_error(std::string message) -> error {
    return make_error(error_code::invalid_argument, message);
}

[[nodiscard]] auto parse_error(std::string message) -> error {
    return make_error(error_code::parse_error, message);
}

[[nodiscard]] auto io_error(std::string message) -> error {
    return make_error(error_code::io_error, message);
}

/// Lowercase hex of the digest (the on-disk file name).
[[nodiscard]] auto digest_hex(const digest32 &digest) -> std::string {
    return to_hex(digest);
}

/// The format is endian-fixed: never memcpy host integers.
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

/// A raw blob or an RFBL header are told apart by the magic. A raw payload is
/// still allowed to begin with "RFBL"; callers fall back to the raw digest
/// check when the packed structure does not validate (a real packed blob can
/// never pass as raw because its logical digest differs from the file digest).
[[nodiscard]] auto looks_like_packed(std::span<const std::byte> bytes) noexcept
    -> bool {
    return bytes.size() >= 4U &&
           bytes_equal(bytes, header_magic_offset, "RFBL");
}

// ---------------------------------------------------------------------------
// Writer.
// ---------------------------------------------------------------------------

struct prepared_block {
    std::uint64_t stored_offset{0};
    std::uint32_t stored_size{0};
    std::uint32_t raw_size{0};
    digest32 block_blake3{};
    std::vector<std::byte> compressed{};
    std::span<const std::byte> raw{};
};

/// Deterministic packed serialization: same logical bytes -> same file bytes.
[[nodiscard]] auto serialize_packed(std::span<const std::byte> data,
                                    const digest32 &digest)
    -> result<std::vector<std::byte>> {
    const auto logical_size{static_cast<std::uint64_t>(data.size())};
    if (logical_size > static_cast<std::uint64_t>(max_block_count) *
                           static_cast<std::uint64_t>(blob_block_size)) {
        return std::unexpected{invalid_argument_error(
            "content store blob exceeds the packed layout's " +
            std::to_string(max_block_count) + "-block ceiling")};
    }
    const auto block_count64{(logical_size + blob_block_size - 1U) /
                             blob_block_size};
    const auto block_count{static_cast<std::uint32_t>(block_count64)};
    const auto table_size{static_cast<std::uint64_t>(block_count) *
                          static_cast<std::uint64_t>(block_table_entry_bytes)};
    const auto data_start{
        align_up(static_cast<std::uint64_t>(blob_header_bytes) + table_size,
                 block_alignment)};

    std::vector<prepared_block> blocks;
    blocks.reserve(block_count);
    bool all_raw{true};
    std::uint64_t cursor{data_start};
    std::uint64_t file_end{data_start};
    for (std::uint32_t index{0}; index < block_count; ++index) {
        const auto block_start{static_cast<std::uint64_t>(index) *
                               static_cast<std::uint64_t>(blob_block_size)};
        const auto remaining{logical_size - block_start};
        const auto raw_size{static_cast<std::uint32_t>(std::min<std::uint64_t>(
            static_cast<std::uint64_t>(blob_block_size), remaining))};
        const auto raw{
            data.subspan(static_cast<std::size_t>(block_start), raw_size)};

        prepared_block block;
        block.raw_size = raw_size;
        block.block_blake3 = blake3(raw);
        block.raw = raw;

        const auto bound{ZSTD_compressBound(raw_size)};
        block.compressed.resize(bound);
        const auto written{ZSTD_compress(block.compressed.data(), bound,
                                         raw.data(), raw.size(),
                                         blob_zstd_level)};
        if (ZSTD_isError(written) != 0U) {
            return std::unexpected{
                io_error("zstd failed to compress a content blob block: " +
                         std::string{ZSTD_getErrorName(written)})};
        }
        if (written < raw_size) {
            block.compressed.resize(written);
            block.stored_size = static_cast<std::uint32_t>(written);
            all_raw = false;
        } else {
            // Incompressible blocks are stored verbatim; the reader uses
            // stored_size == raw_size to tell them apart.
            block.compressed.clear();
            block.compressed.shrink_to_fit();
            block.stored_size = raw_size;
        }

        block.stored_offset = cursor;
        if (block.stored_size >
            std::numeric_limits<std::uint64_t>::max() - cursor) {
            return std::unexpected{
                invalid_argument_error("content blob layout offset overflow")};
        }
        file_end = cursor + block.stored_size;
        cursor = align_up(file_end, block_alignment);
        blocks.push_back(std::move(block));
    }

    if (file_end > std::numeric_limits<std::size_t>::max()) {
        return std::unexpected{invalid_argument_error(
            "content blob is too large for the host address space")};
    }
    std::vector<std::byte> file(static_cast<std::size_t>(file_end));
    auto file_span{std::span<std::byte>{file}};

    constexpr std::string_view magic{"RFBL"};
    for (std::size_t index{0}; index < magic.size(); ++index) {
        file[header_magic_offset + index] =
            static_cast<std::byte>(magic[index]);
    }
    write_u16(file_span, header_version_offset, blob_format_version);
    write_u16(file_span, header_flags_offset,
              static_cast<std::uint16_t>(flag_packed |
                                         (all_raw ? flag_all_raw : 0x0000U)));
    write_u32(file_span, header_block_size_offset, blob_block_size);
    write_u32(file_span, header_block_count_offset, block_count);
    write_u64(file_span, header_logical_size_offset, logical_size);
    std::copy(digest.begin(), digest.end(),
              file.begin() +
                  static_cast<std::ptrdiff_t>(header_file_blake3_offset));
    write_u32(file_span, header_table_size_offset,
              static_cast<std::uint32_t>(table_size));

    auto table{file_span.subspan(blob_header_bytes,
                                 static_cast<std::size_t>(table_size))};
    for (std::uint32_t index{0}; index < block_count; ++index) {
        const auto entry_offset{static_cast<std::size_t>(index) *
                                block_table_entry_bytes};
        const auto &block{blocks[index]};
        write_u64(table, entry_offset + table_stored_offset_offset,
                  block.stored_offset);
        write_u32(table, entry_offset + table_stored_size_offset,
                  block.stored_size);
        write_u32(table, entry_offset + table_raw_size_offset, block.raw_size);
        std::copy(block.block_blake3.begin(), block.block_blake3.end(),
                  table.begin() +
                      static_cast<std::ptrdiff_t>(entry_offset +
                                                  table_block_blake3_offset));
    }

    for (const auto &block : blocks) {
        auto destination{file_span.subspan(
            static_cast<std::size_t>(block.stored_offset), block.stored_size)};
        if (block.stored_size == block.raw_size) {
            std::copy(block.raw.begin(), block.raw.end(), destination.begin());
        } else {
            std::copy(block.compressed.begin(), block.compressed.end(),
                      destination.begin());
        }
    }

    const auto table_digest{blake3(table)};
    std::copy_n(table_digest.begin(), header_table_blake3_prefix_bytes,
                file.begin() +
                    static_cast<std::ptrdiff_t>(header_table_blake3_offset));
    write_u32(
        file_span, header_crc32c_offset,
        crc32c(std::span<const std::byte>{file}.first(header_crc32c_offset)));
    return file;
}

// ---------------------------------------------------------------------------
// Reader.
// ---------------------------------------------------------------------------

struct parsed_block {
    std::uint64_t stored_offset{0};
    std::uint32_t stored_size{0};
    std::uint32_t raw_size{0};
    digest32 block_blake3{};
};

struct packed_blob {
    std::uint32_t block_count{0};
    std::uint64_t logical_size{0};
    bool all_raw_blocks{false};
    std::vector<parsed_block> blocks{};
};

/// Bounded LRU of parsed packed-blob block tables.
/// Parsing is O(block_count) (a 48-byte entry per block plus a BLAKE3 over the
/// table) and used to run on every `read_range`, dwarfing the decode of the
/// requested 4..256 KiB on large blobs. A blob is content-addressed and
/// immutable, so a parsed table stays valid while its file exists; entries are
/// shared and evicted LRU-style. Capacity 0 disables it.
class packed_table_cache {
  public:
    explicit packed_table_cache(std::size_t slots) noexcept : slots_{slots} {}

    packed_table_cache(const packed_table_cache &) = delete;
    auto operator=(const packed_table_cache &) -> packed_table_cache & = delete;

    [[nodiscard]] auto get(const digest32 &digest)
        -> std::shared_ptr<const packed_blob> {
        if (slots_ == 0U) {
            return nullptr;
        }
        const std::lock_guard lock{mutex_};
        const auto found{entries_.find(digest)};
        if (found == entries_.end()) {
            return nullptr;
        }
        order_.splice(order_.begin(), order_, found->second.position);
        return found->second.table;
    }

    void put(const digest32 &digest, std::shared_ptr<const packed_blob> table) {
        if (slots_ == 0U || table == nullptr) {
            return;
        }
        const std::lock_guard lock{mutex_};
        if (const auto found{entries_.find(digest)}; found != entries_.end()) {
            order_.erase(found->second.position);
            entries_.erase(found);
        }
        order_.push_front(digest);
        entries_.emplace(digest, entry{std::move(table), order_.begin()});
        while (entries_.size() > slots_) {
            const auto victim{order_.back()};
            order_.pop_back();
            entries_.erase(victim);
        }
    }

  private:
    struct entry {
        std::shared_ptr<const packed_blob> table{};
        std::list<digest32>::iterator position{};
    };

    std::size_t slots_{0};
    std::mutex mutex_{};
    std::list<digest32> order_{};
    std::map<digest32, entry> entries_{};
};

/// Validate the RFBL header and block table without decoding any block.
[[nodiscard]] auto parse_packed(std::span<const std::byte> bytes,
                                const digest32 &digest) -> result<packed_blob> {
    if (bytes.size() < blob_header_bytes) {
        return std::unexpected{
            parse_error("packed blob is truncated before the RFBL header")};
    }
    if (!bytes_equal(bytes, header_magic_offset, "RFBL")) {
        return std::unexpected{parse_error("packed blob has a bad RFBL magic")};
    }
    if (read_u16(bytes, header_version_offset) != blob_format_version) {
        return std::unexpected{
            parse_error("packed blob has an unsupported RFBL format version")};
    }
    if (read_u32(bytes, header_crc32c_offset) !=
        crc32c(bytes.first(header_crc32c_offset))) {
        return std::unexpected{
            parse_error("packed blob header failed the CRC32C check")};
    }
    const auto flags{read_u16(bytes, header_flags_offset)};
    if ((flags & static_cast<std::uint16_t>(~flag_known)) != 0U) {
        return std::unexpected{
            parse_error("packed blob uses reserved RFBL flags")};
    }
    if ((flags & flag_packed) == 0U) {
        return std::unexpected{
            parse_error("RFBL header does not mark the blob as packed")};
    }
    if (read_u32(bytes, header_block_size_offset) != blob_block_size) {
        return std::unexpected{
            parse_error("packed blob declares an unsupported block size")};
    }
    const auto logical_size{read_u64(bytes, header_logical_size_offset)};
    // The writer only packs blobs above `raw_blob_limit`, but the reader stays
    // permissive: the format allows an explicit packed request for
    // tests/tools, which a later API may expose. `put()` still picks the
    // deterministic layout.
    if (logical_size > static_cast<std::uint64_t>(max_block_count) *
                           static_cast<std::uint64_t>(blob_block_size)) {
        return std::unexpected{
            parse_error("packed blob declares an oversized logical size")};
    }
    const auto block_count{read_u32(bytes, header_block_count_offset)};
    const auto expected_block_count{static_cast<std::uint32_t>(
        (logical_size + blob_block_size - 1U) / blob_block_size)};
    if (block_count == 0U || block_count > max_block_count ||
        block_count != expected_block_count) {
        return std::unexpected{
            parse_error("packed blob declares an invalid block count")};
    }
    const auto table_size{read_u32(bytes, header_table_size_offset)};
    const auto expected_table_size{
        static_cast<std::uint64_t>(block_count) *
        static_cast<std::uint64_t>(block_table_entry_bytes)};
    if (table_size != expected_table_size) {
        return std::unexpected{
            parse_error("packed blob declares an invalid block table size")};
    }
    const auto file_size{static_cast<std::uint64_t>(bytes.size())};
    const auto table_end{static_cast<std::uint64_t>(blob_header_bytes) +
                         static_cast<std::uint64_t>(table_size)};
    if (table_end > file_size) {
        return std::unexpected{
            parse_error("packed blob block table is truncated")};
    }
    digest32 header_digest{};
    std::copy_n(bytes.begin() +
                    static_cast<std::ptrdiff_t>(header_file_blake3_offset),
                header_digest.size(), header_digest.begin());
    if (header_digest != digest) {
        return std::unexpected{
            parse_error("packed blob header digest does not match its name")};
    }
    for (std::size_t index{0}; index < header_reserved_bytes; ++index) {
        if (bytes[header_reserved_offset + index] != std::byte{0}) {
            return std::unexpected{
                parse_error("packed blob has non-zero reserved header bytes")};
        }
    }
    const auto table_digest{
        blake3(bytes.subspan(blob_header_bytes, table_size))};
    for (std::size_t index{0}; index < header_table_blake3_prefix_bytes;
         ++index) {
        if (bytes[header_table_blake3_offset + index] != table_digest[index]) {
            return std::unexpected{
                parse_error("packed blob block table failed the BLAKE3 check")};
        }
    }

    const auto table{bytes.subspan(blob_header_bytes, table_size)};
    const auto data_start{align_up(table_end, block_alignment)};
    const auto last_raw_size{logical_size -
                             static_cast<std::uint64_t>(block_count - 1U) *
                                 static_cast<std::uint64_t>(blob_block_size)};

    packed_blob parsed;
    parsed.block_count = block_count;
    parsed.logical_size = logical_size;
    parsed.blocks.reserve(block_count);
    std::uint64_t total_raw{0};
    std::uint64_t previous_end{0};
    bool all_raw{true};
    for (std::uint32_t index{0}; index < block_count; ++index) {
        const auto entry_offset{static_cast<std::size_t>(index) *
                                block_table_entry_bytes};
        const auto entry{table.subspan(entry_offset, block_table_entry_bytes)};

        parsed_block block;
        block.stored_offset = read_u64(entry, table_stored_offset_offset);
        block.stored_size = read_u32(entry, table_stored_size_offset);
        block.raw_size = read_u32(entry, table_raw_size_offset);
        std::copy_n(entry.begin() +
                        static_cast<std::ptrdiff_t>(table_block_blake3_offset),
                    block.block_blake3.size(), block.block_blake3.begin());

        const auto expected_raw_size{
            index + 1U == block_count
                ? static_cast<std::uint32_t>(last_raw_size)
                : blob_block_size};
        if (block.raw_size == 0U || block.raw_size != expected_raw_size) {
            return std::unexpected{
                parse_error("packed blob block declares an invalid raw size")};
        }
        if (block.stored_size == 0U || block.stored_size > block.raw_size) {
            return std::unexpected{parse_error(
                "packed blob block declares an invalid stored size")};
        }
        if (block.stored_offset % block_alignment != 0U) {
            return std::unexpected{
                parse_error("packed blob block is not 64-byte aligned")};
        }
        if (block.stored_offset < data_start) {
            return std::unexpected{
                parse_error("packed blob block overlaps the header/table")};
        }
        if (block.stored_offset > file_size ||
            block.stored_size > file_size - block.stored_offset) {
            return std::unexpected{
                parse_error("packed blob block is outside the file")};
        }
        if (index != 0U) {
            auto minimum{previous_end};
            const auto remainder{minimum % block_alignment};
            if (remainder != 0U) {
                const auto padding{block_alignment - remainder};
                if (minimum >
                    std::numeric_limits<std::uint64_t>::max() - padding) {
                    return std::unexpected{
                        parse_error("packed blob block offset overflow")};
                }
                minimum += padding;
            }
            if (block.stored_offset < minimum) {
                return std::unexpected{parse_error(
                    "packed blob blocks overlap or are out of order")};
            }
        }
        previous_end = block.stored_offset + block.stored_size;
        if (total_raw > logical_size - block.raw_size) {
            return std::unexpected{
                parse_error("packed blob block raw sizes exceed the logical "
                            "size")};
        }
        total_raw += block.raw_size;
        if (block.stored_size != block.raw_size) {
            all_raw = false;
        }
        parsed.blocks.push_back(block);
    }
    if (total_raw != logical_size) {
        return std::unexpected{parse_error(
            "packed blob block raw sizes do not add up to the logical size")};
    }
    if (((flags & flag_all_raw) != 0U) != all_raw) {
        return std::unexpected{
            parse_error("packed blob all-raw flag does not match its blocks")};
    }
    parsed.all_raw_blocks = all_raw;
    return parsed;
}

/// Copy/decompress one block into `target`. Boundaries were validated by
/// `parse_packed`; the caller supplies exactly `block.raw_size` bytes.
[[nodiscard]] auto decode_block(std::span<const std::byte> file,
                                const parsed_block &block,
                                std::span<std::byte> target) -> status {
    if (target.size() != block.raw_size) {
        return std::unexpected{
            parse_error("packed blob block buffer has the wrong size")};
    }
    // Re-check against the bytes actually read: a cached block table may come
    // from an earlier read of the same (content-addressed) file, and a
    // truncated/replaced file must be a parse error, never an out-of-range
    // `subspan`.
    if (block.stored_offset > file.size() ||
        block.stored_size > file.size() - block.stored_offset) {
        return std::unexpected{
            parse_error("packed blob block is outside the file")};
    }
    const auto source{file.subspan(
        static_cast<std::size_t>(block.stored_offset), block.stored_size)};
    if (block.stored_size == block.raw_size) {
        std::copy(source.begin(), source.end(), target.begin());
        return status_ok();
    }
    const auto written{ZSTD_decompress(target.data(), target.size(),
                                       source.data(), source.size())};
    if (ZSTD_isError(written) != 0U) {
        return std::unexpected{
            parse_error("packed blob block has a corrupt zstd payload: " +
                        std::string{ZSTD_getErrorName(written)})};
    }
    if (written != block.raw_size) {
        return std::unexpected{parse_error(
            "packed blob block decompressed to an unexpected size")};
    }
    return status_ok();
}

/// Decode every block and assemble the logical bytes, then verify the logical
/// BLAKE3 digest. Per-block hashes are skipped here: default reads do not
/// re-verify them, and the whole-blob digest catches damage.
[[nodiscard]] auto decode_packed(std::span<const std::byte> file,
                                 const packed_blob &packed,
                                 const digest32 &digest)
    -> result<std::vector<std::byte>> {
    if (packed.logical_size > std::numeric_limits<std::size_t>::max()) {
        return std::unexpected{parse_error(
            "packed blob logical size does not fit the host address space")};
    }
    std::vector<std::byte> logical(
        static_cast<std::size_t>(packed.logical_size));
    auto logical_span{std::span<std::byte>{logical}};
    std::size_t position{0};
    for (const auto &block : packed.blocks) {
        auto target{logical_span.subspan(position, block.raw_size)};
        if (auto decoded{decode_block(file, block, target)}; !decoded) {
            return std::unexpected{std::move(decoded.error())};
        }
        position += block.raw_size;
    }
    if (blake3(logical) != digest) {
        return std::unexpected{parse_error(
            "packed blob failed the BLAKE3 verification (content is "
            "corrupted)")};
    }
    return logical;
}

/// Structural verification for `layout()`: decode every block and check its
/// recorded BLAKE3. This catches block damage that a pure header/table check
/// cannot see, without materializing the whole logical blob.
[[nodiscard]] auto verify_packed_blocks(std::span<const std::byte> file,
                                        const packed_blob &packed) -> status {
    std::vector<std::byte> scratch(blob_block_size);
    for (const auto &block : packed.blocks) {
        auto target{std::span<std::byte>{scratch}.first(block.raw_size)};
        if (auto decoded{decode_block(file, block, target)}; !decoded) {
            return decoded;
        }
        if (blake3(target) != block.block_blake3) {
            return std::unexpected{
                parse_error("packed blob block failed the BLAKE3 check")};
        }
    }
    return status_ok();
}

/// Load a blob, accepting both layouts. Missing files are reported by the
/// caller; damaged blobs are `parse_error`.
[[nodiscard]] auto load_blob(std::span<const std::byte> bytes,
                             const digest32 &digest)
    -> result<std::vector<std::byte>> {
    if (looks_like_packed(bytes)) {
        auto packed{parse_packed(bytes, digest)};
        if (packed) {
            return decode_packed(bytes, *packed, digest);
        }
        // A raw payload may legitimately start with "RFBL": fall through to
        // the raw digest check before declaring the blob corrupt.
    }
    if (blake3(bytes) == digest) {
        return std::vector<std::byte>{bytes.begin(), bytes.end()};
    }
    return std::unexpected{parse_error(
        "CAS blob failed the BLAKE3 verification (content is corrupted)")};
}

[[nodiscard]] auto validate_range(std::uint64_t logical_size,
                                  std::uint64_t offset, std::size_t length)
    -> status {
    const auto length64{static_cast<std::uint64_t>(length)};
    if (offset > logical_size || length64 > logical_size - offset) {
        return std::unexpected{invalid_argument_error(
            "content store read_range is outside the blob (offset " +
            std::to_string(offset) + ", length " + std::to_string(length64) +
            ", logical size " + std::to_string(logical_size) + ")")};
    }
    return status_ok();
}

[[nodiscard]] auto
read_packed_range(std::span<const std::byte> file, const packed_blob &packed,
                  std::uint64_t offset, std::span<std::byte> out,
                  block_cache *cache, const digest32 &digest)
    -> result<std::size_t> {
    if (auto valid{validate_range(packed.logical_size, offset, out.size())};
        !valid) {
        return std::unexpected{std::move(valid.error())};
    }
    if (out.empty()) {
        return std::size_t{0};
    }
    const auto end{offset + static_cast<std::uint64_t>(out.size())};
    const auto first_block{static_cast<std::size_t>(offset / blob_block_size)};
    const auto last_block{
        static_cast<std::size_t>((end - 1U) / blob_block_size)};

    // Disabled cache: decode into one reusable scratch block, as before.
    const auto cache_enabled{cache != nullptr && cache->enabled()};
    std::vector<std::byte> scratch(cache_enabled ? 0U : blob_block_size);
    std::size_t copied{0};
    for (auto index{first_block}; index <= last_block; ++index) {
        const auto &block{packed.blocks[index]};
        const auto block_start{static_cast<std::uint64_t>(index) *
                               static_cast<std::uint64_t>(blob_block_size)};
        const auto slice_begin{std::max(offset, block_start)};
        const auto slice_end{std::min(end, block_start + block.raw_size)};
        const auto slice_size{
            static_cast<std::size_t>(slice_end - slice_begin)};
        const auto slice_offset{
            static_cast<std::ptrdiff_t>(slice_begin - block_start)};
        auto destination{out.begin() + static_cast<std::ptrdiff_t>(copied)};

        if (!cache_enabled) {
            auto target{std::span<std::byte>{scratch}.first(block.raw_size)};
            if (auto decoded{decode_block(file, block, target)}; !decoded) {
                return std::unexpected{std::move(decoded.error())};
            }
            std::copy_n(target.begin() + slice_offset, slice_size, destination);
        } else {
            // The cache owns the decoded block; copying the slice happens
            // outside the cache lock, and the shared_ptr keeps the block alive
            // even if another thread evicts it concurrently.
            const auto block_index{static_cast<std::uint32_t>(index)};
            auto block_bytes{cache->get(digest, block_index)};
            if (!block_bytes) {
                auto decoded{std::make_shared<std::vector<std::byte>>(
                    static_cast<std::size_t>(block.raw_size))};
                if (auto status{decode_block(file, block, *decoded)}; !status) {
                    return std::unexpected{std::move(status.error())};
                }
                cache->put(digest, block_index, decoded);
                block_bytes = std::move(decoded);
            }
            std::copy_n(block_bytes->begin() + slice_offset, slice_size,
                        destination);
        }
        copied += slice_size;
    }
    if (copied != out.size()) {
        return std::unexpected{
            parse_error("packed blob range could not be fully read")};
    }
    return copied;
}

[[nodiscard]] auto digest_less(const digest32 &lhs,
                               const digest32 &rhs) noexcept -> bool {
    return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(),
                                        rhs.end());
}

/// Structural validation for a caller-provided `blob_location` (an RFBT record
/// is already validated by the index parser; this guards direct construction).
[[nodiscard]] auto location_violation(const blob_location &location)
    -> std::string {
    if (location.in_segment) {
        if (location.segment_id == 0U) {
            return "in-segment blob location has a zero segment id";
        }
        if (static_cast<std::uint64_t>(location.segment_offset) +
                static_cast<std::uint64_t>(location.stored_size) >
            std::numeric_limits<std::uint32_t>::max()) {
            return "in-segment blob location overflows the 32-bit segment "
                   "offset range";
        }
    } else if (location.segment_id != 0U || location.segment_offset != 0U) {
        return "blob location carries segment fields without in_segment";
    }
    if (location.kind == blob_kind::raw) {
        if (location.block_count != 0U) {
            return "raw blob location declares a block count";
        }
        if (location.logical_size > std::numeric_limits<std::uint32_t>::max()) {
            return "raw blob location has a logical size above 32 bits";
        }
        if (location.stored_size !=
            static_cast<std::uint32_t>(location.logical_size)) {
            return "raw blob location stored size does not match its logical "
                   "size";
        }
    } else {
        if (location.block_count == 0U) {
            return "packed blob location declares no blocks";
        }
        if (location.logical_size == 0U) {
            return "packed blob location declares an empty blob";
        }
        if (location.stored_size == 0U) {
            return "packed blob location declares an empty stored size";
        }
    }
    return {};
}

/// The digest-only overloads can only see `content/blobs/<hh>/<digest>`. A
/// segment-resident blob has no such file, so both report the same hint.
[[nodiscard]] auto missing_blob_error(const digest32 &digest) -> error {
    return make_error(error_code::not_found,
                      "CAS blob '" + digest_hex(digest) +
                          "' has no standalone file; in-segment blobs must be "
                          "read through content_store::read(const "
                          "blob_location&) / read_range(const blob_location&, "
                          "...) using the RFBT record");
}

/// Decode stored blob bytes (raw logical bytes or an RFBL file) and check them
/// against the location's digest and logical size.
[[nodiscard]] auto decode_stored(std::span<const std::byte> stored,
                                 const blob_location &location,
                                 std::string_view where)
    -> result<std::vector<std::byte>> {
    auto loaded{load_blob(stored, location.digest)};
    if (!loaded) {
        return std::unexpected{
            parse_error(std::string{where} + ": " + loaded.error().message)};
    }
    if (loaded->size() != location.logical_size) {
        return std::unexpected{
            parse_error(std::string{where} + ": decoded size " +
                        std::to_string(loaded->size()) +
                        " does not match the recorded "
                        "logical size " +
                        std::to_string(location.logical_size))};
    }
    return loaded;
}

/// Range read from stored blob bytes, shared by the file-backed and in-segment
/// paths. `invalid_argument` stays a range error; structural damage becomes
/// `parse_error`. `cache` may be null (block cache disabled).
[[nodiscard]] auto
read_range_from_stored(std::span<const std::byte> stored,
                       const digest32 &digest, std::uint64_t offset,
                       std::span<std::byte> out, block_cache *cache,
                       packed_table_cache *tables) -> result<std::size_t> {
    if (looks_like_packed(stored)) {
        std::shared_ptr<const packed_blob> parsed{};
        if (tables != nullptr) {
            parsed = tables->get(digest);
        }
        if (parsed == nullptr) {
            auto fresh{parse_packed(stored, digest)};
            if (fresh) {
                if (tables == nullptr) {
                    auto copied{read_packed_range(stored, *fresh, offset, out,
                                                  cache, digest)};
                    if (!copied) {
                        if (copied.error().code ==
                            error_code::invalid_argument) {
                            return std::unexpected{std::move(copied.error())};
                        }
                        return std::unexpected{
                            parse_error(copied.error().message)};
                    }
                    return copied;
                }
                parsed = std::make_shared<const packed_blob>(std::move(*fresh));
                tables->put(digest, parsed);
            }
        }
        if (parsed != nullptr) {
            auto copied{
                read_packed_range(stored, *parsed, offset, out, cache, digest)};
            if (!copied) {
                if (copied.error().code == error_code::invalid_argument) {
                    return std::unexpected{std::move(copied.error())};
                }
                return std::unexpected{parse_error(copied.error().message)};
            }
            return copied;
        }
        // A raw payload may legitimately start with "RFBL"; fall through to the
        // raw digest check (same rule as `load_blob`).
    }
    if (blake3(stored) != digest) {
        return std::unexpected{parse_error(
            "CAS blob failed the BLAKE3 verification (content is corrupted)")};
    }
    if (auto valid{validate_range(static_cast<std::uint64_t>(stored.size()),
                                  offset, out.size())};
        !valid) {
        return std::unexpected{std::move(valid.error())};
    }
    if (!out.empty()) {
        std::copy_n(stored.begin() + static_cast<std::ptrdiff_t>(offset),
                    out.size(), out.begin());
    }
    return out.size();
}

/// Read-only stored-blob bytes backed by a mapping (large files) or a heap
/// copy (small files, or when mapping is unavailable). The span never escapes
/// the read call: callers still receive a `std::vector<std::byte>`.
struct stored_blob_bytes {
    platform::mapped_file mapping{};
    std::vector<std::byte> owned{};
    std::span<const std::byte> bytes{};
};

/// Files at or above this size are mapped instead of copied: the 256 KiB read
/// window only pays off once a blob is comfortably larger than one page
/// cluster, and small blobs keep the cheap `read_file` path. Mapping failure
/// falls back to `read_file`, so sandboxes without mmap permission still serve
/// content.
constexpr std::uint64_t mmap_threshold_bytes{1024U * 1024U};

[[nodiscard]] auto read_stored_file(std::string_view path)
    -> result<stored_blob_bytes> {
    stored_blob_bytes stored;
    auto size{core::io::file_size(path)};
    if (!size) {
        return std::unexpected{std::move(size.error())};
    }
    if (*size >= mmap_threshold_bytes) {
        auto mapped{platform::mapped_file::open(path)};
        if (mapped) {
            stored.mapping = std::move(*mapped);
            stored.bytes = stored.mapping.data();
            return stored;
        }
        // Fall through: a platform/sandbox that cannot map the file must still
        // be able to read the blob through the ordinary path.
    }
    auto bytes{core::io::read_file(path)};
    if (!bytes) {
        return std::unexpected{std::move(bytes.error())};
    }
    stored.owned = std::move(*bytes);
    stored.bytes = stored.owned;
    return stored;
}

} // namespace

// ---------------------------------------------------------------------------
// Per-store read caches (shared by copies of one content_store).
// ---------------------------------------------------------------------------

struct content_store::cache_state {
    /// One cached mmap reader plus its position in the LRU list.
    struct cached_segment {
        std::shared_ptr<const segment_reader> reader{};
        std::list<std::uint64_t>::iterator position{};
    };

    explicit cache_state(const content_store_options &options)
        : segment_slots{options.segment_cache_slots},
          blocks{options.block_cache_bytes} {}

    /// Serializes `segments` and its LRU order. The block cache has its own
    /// mutex, and the two are never held at the same time.
    std::mutex mutex{};
    std::size_t segment_slots{0};
    std::map<std::uint64_t, cached_segment> segments{};
    /// Most recently used segment id first.
    std::list<std::uint64_t> segment_lru{};
    block_cache blocks;
    /// Parsed packed-blob tables; fixed small capacity (a table is ~48 B per
    /// 64 KiB block, so 8 entries cover the active large blobs without a knob).
    packed_table_cache tables{8U};
};

auto content_store::open(std::string_view root) -> result<content_store> {
    // Qualify the call: the platform guard treats a bare `open(` outside
    // src/platform as an unabstracted OS call.
    return content_store::open(root, content_store_options{});
}

auto content_store::open(std::string_view root,
                         const content_store_options &options)
    -> result<content_store> {
    if (root.empty()) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "empty content store root")};
    }
    content_store store;
    store.root_ = normalize_root(root);
    store.cache_ = std::make_shared<cache_state>(options);
    // Root creation goes through core::io only (the platform boundary).
    if (auto created{core::io::create_directories(store.blobs_root())};
        !created) {
        return std::unexpected{std::move(created.error())};
    }
    return store;
}

auto content_store::open_existing(std::string_view root)
    -> result<content_store> {
    if (root.empty()) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "empty content store root")};
    }
    content_store store;
    store.root_ = normalize_root(root);
    store.cache_ = std::make_shared<cache_state>(content_store_options{});
    // Read-only open: like open(), but it never creates `<root>/content/blobs`
    // (a diagnostic must not mutate the store). A missing/unreadable root is
    // an error instead of being silently materialized; `list_directory` is the
    // portable readability check (stat alone succeeds on an unreadable dir).
    if (!core::io::is_directory(store.root_)) {
        return std::unexpected{io_error(fmt::format(
            "content store '{}' does not exist or is not a directory",
            store.root_))};
    }
    if (auto entries{core::io::list_directory(store.root_)}; !entries) {
        return std::unexpected{
            io_error(fmt::format("content store '{}' is not readable: {}",
                                 store.root_, entries.error().message))};
    }
    // Read-only diagnostics must fail closed on a published-but-corrupt table
    // instead of silently treating the store as empty.
    const auto table_path{join_path(store.root_, "content/blobs.idx")};
    if (core::io::exists(table_path)) {
        // Map instead of reading the whole table into a vector: open_existing
        // is the read-only diagnostic path and only needs validation.
        auto parsed{mapped_blob_table::open(table_path)};
        if (!parsed) {
            return std::unexpected{make_error(
                parsed.error().code,
                "content store '" + store.root_ +
                    "' has a corrupt blobs.idx: " + parsed.error().message)};
        }
    }
    return store;
}

auto content_store::open_segment(const content_store &store,
                                 std::uint64_t segment_id)
    -> result<std::shared_ptr<const segment_reader>> {
    const auto path{segment_path(store.root_, segment_id)};
    // Keep the shared state alive for the whole call even if the handle is
    // copied/moved concurrently; the options are immutable after open().
    const auto state{store.cache_};
    const auto cacheable{state != nullptr && state->segment_slots != 0U};

    if (cacheable) {
        const std::lock_guard lock{state->mutex};
        if (const auto found{state->segments.find(segment_id)};
            found != state->segments.end()) {
            state->segment_lru.splice(state->segment_lru.begin(),
                                      state->segment_lru,
                                      found->second.position);
            return found->second.reader;
        }
    }

    // Open outside the lock: a cold miss must not serialize other readers.
    auto opened{segment_reader::open(path)};
    if (!opened) {
        auto failure{std::move(opened.error())};
        failure.message = "content segment '" + path + "': " + failure.message;
        return std::unexpected{std::move(failure)};
    }
    auto reader{std::make_shared<segment_reader>(std::move(*opened))};
    if (!cacheable || !reader->mapped()) {
        // The ifstream fallback is not safe for concurrent reads, so it stays a
        // one-off handle; small segments keep the previous reopen cost.
        return reader;
    }

    const std::lock_guard lock{state->mutex};
    if (const auto found{state->segments.find(segment_id)};
        found != state->segments.end()) {
        // Another thread cached the same segment while this one was opening:
        // drop the duplicate mapping and share the existing reader.
        state->segment_lru.splice(state->segment_lru.begin(),
                                  state->segment_lru, found->second.position);
        return found->second.reader;
    }
    state->segment_lru.push_front(segment_id);
    state->segments.emplace(
        segment_id,
        cache_state::cached_segment{reader, state->segment_lru.begin()});
    while (state->segments.size() > state->segment_slots) {
        const auto victim{state->segment_lru.back()};
        state->segment_lru.pop_back();
        state->segments.erase(victim);
    }
    return reader;
}

auto content_store::read_segment_bytes(const blob_location &location,
                                       std::uint32_t offset,
                                       std::uint32_t size) const
    -> result<std::vector<std::byte>> {
    auto reader{open_segment(*this, location.segment_id)};
    if (!reader) {
        return std::unexpected{std::move(reader.error())};
    }
    const auto physical{static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(location.segment_offset) +
        static_cast<std::uint64_t>(offset))};
    auto bytes{(*reader)->read_at(physical, size)};
    if (!bytes) {
        auto failure{std::move(bytes.error())};
        failure.message = "content segment '" + std::string{(*reader)->path()} +
                          "': " + failure.message;
        return std::unexpected{std::move(failure)};
    }
    return bytes;
}

auto content_store::blobs_root() const -> std::string {
    return join_path(root_, content_store_blobs_directory);
}

auto content_store::blob_directory(const digest32 &digest) const
    -> std::string {
    const auto hex{digest_hex(digest)};
    return join_path(blobs_root(), hex.substr(0, 2));
}

auto content_store::path_for(const digest32 &digest) const -> std::string {
    const auto hex{digest_hex(digest)};
    return join_path(blob_directory(digest), hex);
}

auto content_store::put(std::span<const std::byte> data) const
    -> result<digest32> {
    if (!valid()) {
        return std::unexpected{make_error(error_code::not_initialized,
                                          "content store is not open")};
    }
    const auto digest{blake3(data)};
    const auto path{path_for(digest)};

    // Dedupe: a blob with this name may already exist (another mod/version, a
    // concurrent install or a previous run). Verify it instead of rewriting.
    if (core::io::exists(path)) {
        auto existing{core::io::read_file(path)};
        if (!existing) {
            return std::unexpected{std::move(existing.error())};
        }
        if (!load_blob(*existing, digest)) {
            return std::unexpected{parse_error(
                "existing CAS blob '" + path +
                "' does not match its content hash (corrupt store entry)")};
        }
        return digest;
    }

    if (auto created{core::io::create_directories(blob_directory(digest))};
        !created) {
        return std::unexpected{std::move(created.error())};
    }
    if (data.size() > raw_blob_limit) {
        auto packed{serialize_packed(data, digest)};
        if (!packed) {
            return std::unexpected{std::move(packed.error())};
        }
        if (auto written{core::io::atomic_write_file(path, *packed)};
            !written) {
            return std::unexpected{std::move(written.error())};
        }
    } else {
        if (auto written{core::io::atomic_write_file(path, data)}; !written) {
            return std::unexpected{std::move(written.error())};
        }
    }
    return digest;
}

auto content_store::put_file(std::string_view path) const -> result<digest32> {
    if (!valid()) {
        return std::unexpected{make_error(error_code::not_initialized,
                                          "content store is not open")};
    }
    auto bytes{core::io::read_file(path)};
    if (!bytes) {
        return std::unexpected{std::move(bytes.error())};
    }
    return put(*bytes);
}

auto content_store::contains(const digest32 &digest) const -> bool {
    if (!valid()) {
        return false;
    }
    return core::io::exists(path_for(digest));
}

auto content_store::read(const digest32 &digest) const
    -> result<std::vector<std::byte>> {
    if (!valid()) {
        return std::unexpected{make_error(error_code::not_initialized,
                                          "content store is not open")};
    }
    const auto path{path_for(digest)};
    if (!core::io::exists(path)) {
        return std::unexpected{missing_blob_error(digest)};
    }
    auto stored{read_stored_file(path)};
    if (!stored) {
        return std::unexpected{std::move(stored.error())};
    }
    auto loaded{load_blob(stored->bytes, digest)};
    if (!loaded) {
        return std::unexpected{
            parse_error("CAS blob '" + path + "': " + loaded.error().message)};
    }
    return std::move(*loaded);
}

auto content_store::read(const blob_location &location) const
    -> result<std::vector<std::byte>> {
    if (!valid()) {
        return std::unexpected{make_error(error_code::not_initialized,
                                          "content store is not open")};
    }
    if (auto violation{location_violation(location)}; !violation.empty()) {
        return std::unexpected{
            invalid_argument_error("content store: " + violation)};
    }
    if (location.in_segment) {
        auto stored{read_segment_bytes(location, 0, location.stored_size)};
        if (!stored) {
            return std::unexpected{std::move(stored.error())};
        }
        return decode_stored(
            *stored, location,
            "CAS blob '" + digest_hex(location.digest) + "' in segment '" +
                segment_path(root_, location.segment_id) + "'");
    }
    const auto path{path_for(location.digest)};
    auto stored{read_stored_file(path)};
    if (!stored) {
        return std::unexpected{std::move(stored.error())};
    }
    return decode_stored(stored->bytes, location, "CAS blob '" + path + "'");
}

auto decode_stored_blob(std::span<const std::byte> stored,
                        const blob_location &location)
    -> result<std::vector<std::byte>> {
    if (auto violation{location_violation(location)}; !violation.empty()) {
        return std::unexpected{
            invalid_argument_error("stored blob: " + violation)};
    }
    return decode_stored(stored, location,
                         "stored blob '" + digest_hex(location.digest) + "'");
}

auto content_store::layout(const digest32 &digest) const
    -> result<blob_layout> {
    if (!valid()) {
        return std::unexpected{make_error(error_code::not_initialized,
                                          "content store is not open")};
    }
    const auto path{path_for(digest)};
    auto stored{read_stored_file(path)};
    if (!stored) {
        return std::unexpected{std::move(stored.error())};
    }
    if (looks_like_packed(stored->bytes)) {
        auto packed{parse_packed(stored->bytes, digest)};
        if (packed) {
            if (auto verified{verify_packed_blocks(stored->bytes, *packed)};
                !verified) {
                return std::unexpected{parse_error(
                    "CAS blob '" + path + "': " + verified.error().message)};
            }
            return blob_layout{blob_kind::packed, blob_block_size,
                               packed->block_count, packed->logical_size,
                               packed->all_raw_blocks};
        }
    }
    if (blake3(stored->bytes) == digest) {
        return blob_layout{blob_kind::raw, 0, 0,
                           static_cast<std::uint64_t>(stored->bytes.size()),
                           false};
    }
    return std::unexpected{
        parse_error("CAS blob '" + path +
                    "' failed the BLAKE3 verification (content is corrupted)")};
}

auto content_store::read_range(const digest32 &digest, std::uint64_t offset,
                               std::span<std::byte> out) const
    -> result<std::size_t> {
    if (!valid()) {
        return std::unexpected{make_error(error_code::not_initialized,
                                          "content store is not open")};
    }
    const auto path{path_for(digest)};
    if (!core::io::exists(path)) {
        return std::unexpected{missing_blob_error(digest)};
    }
    auto stored{read_stored_file(path)};
    if (!stored) {
        return std::unexpected{std::move(stored.error())};
    }
    auto copied{read_range_from_stored(stored->bytes, digest, offset, out,
                                       cache_ ? &cache_->blocks : nullptr,
                                       cache_ ? &cache_->tables : nullptr)};
    if (!copied) {
        if (copied.error().code == error_code::invalid_argument) {
            return std::unexpected{std::move(copied.error())};
        }
        return std::unexpected{
            parse_error("CAS blob '" + path + "': " + copied.error().message)};
    }
    return copied;
}

auto content_store::read_range(const blob_location &location,
                               std::uint64_t offset,
                               std::span<std::byte> out) const
    -> result<std::size_t> {
    if (!valid()) {
        return std::unexpected{make_error(error_code::not_initialized,
                                          "content store is not open")};
    }
    if (auto violation{location_violation(location)}; !violation.empty()) {
        return std::unexpected{
            invalid_argument_error("content store: " + violation)};
    }
    if (auto valid{validate_range(location.logical_size, offset, out.size())};
        !valid) {
        return std::unexpected{std::move(valid.error())};
    }
    if (out.empty()) {
        return std::size_t{0};
    }
    if (location.in_segment) {
        auto stored{read_segment_bytes(location, 0, location.stored_size)};
        if (!stored) {
            return std::unexpected{std::move(stored.error())};
        }
        auto copied{read_range_from_stored(*stored, location.digest, offset,
                                           out,
                                           cache_ ? &cache_->blocks : nullptr,
                                           cache_ ? &cache_->tables : nullptr)};
        if (!copied) {
            if (copied.error().code == error_code::invalid_argument) {
                return std::unexpected{std::move(copied.error())};
            }
            return std::unexpected{parse_error(
                "CAS blob '" + digest_hex(location.digest) + "' in segment '" +
                segment_path(root_, location.segment_id) +
                "': " + copied.error().message)};
        }
        return copied;
    }
    const auto path{path_for(location.digest)};
    auto stored{read_stored_file(path)};
    if (!stored) {
        return std::unexpected{std::move(stored.error())};
    }
    auto copied{read_range_from_stored(stored->bytes, location.digest, offset,
                                       out, cache_ ? &cache_->blocks : nullptr,
                                       cache_ ? &cache_->tables : nullptr)};
    if (!copied) {
        if (copied.error().code == error_code::invalid_argument) {
            return std::unexpected{std::move(copied.error())};
        }
        return std::unexpected{
            parse_error("CAS blob '" + path + "': " + copied.error().message)};
    }
    return copied;
}

auto blob_location_from_table_entry(const blob_table_entry &entry)
    -> blob_location {
    return blob_location{
        .digest = entry.digest,
        .kind = (entry.flags & blob_flags_packed) != 0U ? blob_kind::packed
                                                        : blob_kind::raw,
        .block_count = entry.block_count,
        .logical_size = entry.logical_size,
        .stored_size = entry.stored_size,
        .in_segment = (entry.flags & blob_flags_in_segment) != 0U,
        .segment_id = entry.segment_id,
        .segment_offset = entry.segment_offset,
    };
}

// ---------------------------------------------------------------------------
// Batch writer.
// ---------------------------------------------------------------------------

struct batch::impl {
    content_store store{};
    std::uint32_t target_bytes{0};
    std::string segments_dir{};
    std::vector<digest32> existing_digests{};
    std::set<digest32> seen{};
    std::vector<blob_metadata> blobs{};
    std::optional<segment_writer> writer{};
    /// Index of the first blob that belongs to the currently open segment.
    std::size_t segment_begin{0};
    bool committed{false};
    bool failed{false};

    [[nodiscard]] auto already_present(const digest32 &digest) const -> bool {
        if (seen.contains(digest)) {
            return true;
        }
        return std::binary_search(existing_digests.begin(),
                                  existing_digests.end(), digest, digest_less);
    }

    /// Seal the open segment (if any) and stamp its id onto the matching
    /// `blob_metadata` records.
    [[nodiscard]] auto seal_current() -> status {
        if (!writer) {
            return status_ok();
        }
        auto header{writer->seal()};
        if (!header) {
            failed = true;
            return std::unexpected{std::move(header.error())};
        }
        for (std::size_t index{segment_begin}; index < blobs.size(); ++index) {
            if ((blobs[index].flags & blob_flags_in_segment) != 0U) {
                blobs[index].segment_id = header->id;
            }
        }
        writer.reset();
        return status_ok();
    }
};

batch::batch() noexcept = default;

batch::batch(std::unique_ptr<impl> state) noexcept : impl_{std::move(state)} {}

batch::batch(batch &&other) noexcept = default;

auto batch::operator=(batch &&other) noexcept -> batch & = default;

batch::~batch() = default;

auto batch::begin(std::string_view store_root, const blob_table_view *existing,
                  std::uint32_t segment_target_bytes) -> result<batch> {
    if (store_root.empty()) {
        return std::unexpected{
            invalid_argument_error("content batch needs a non-empty store "
                                   "root")};
    }
    if (segment_target_bytes == 0U) {
        return std::unexpected{invalid_argument_error(
            "content batch needs a non-zero segment target size")};
    }
    auto store{content_store::open(store_root)};
    if (!store) {
        return std::unexpected{std::move(store.error())};
    }
    auto state{std::make_unique<impl>()};
    state->store = std::move(*store);
    state->target_bytes = segment_target_bytes;
    state->segments_dir =
        join_path(state->store.root(), content_segment_directory);
    if (existing != nullptr) {
        state->existing_digests.reserve(existing->entries.size());
        for (const auto &entry : existing->entries) {
            state->existing_digests.push_back(entry.digest);
        }
        std::sort(state->existing_digests.begin(),
                  state->existing_digests.end(), digest_less);
    }
    return batch{std::move(state)};
}

auto batch::valid() const noexcept -> bool {
    return impl_ != nullptr && !impl_->failed;
}

auto batch::root() const noexcept -> std::string_view {
    return impl_ ? std::string_view{impl_->store.root()} : std::string_view{};
}

auto batch::put(std::span<const std::byte> data) -> result<digest32> {
    if (!impl_) {
        return std::unexpected{make_error(error_code::not_initialized,
                                          "content batch is not initialized")};
    }
    if (impl_->committed) {
        return std::unexpected{
            invalid_argument_error("content batch is already committed")};
    }
    if (impl_->failed) {
        return std::unexpected{make_error(
            error_code::io_error,
            "content batch failed earlier and cannot accept more blobs")};
    }

    const auto digest{blake3(data)};
    if (impl_->already_present(digest)) {
        return digest;
    }

    if (data.size() > raw_blob_limit) {
        // Large blobs keep one packed file each; `put()` publishes atomically
        // and verifies an existing file (durable at this point).
        auto written{impl_->store.put(data)};
        if (!written) {
            return std::unexpected{std::move(written.error())};
        }
        auto layout{impl_->store.layout(*written)};
        if (!layout) {
            return std::unexpected{std::move(layout.error())};
        }
        auto stored_size{core::io::file_size(impl_->store.path_for(*written))};
        if (!stored_size) {
            return std::unexpected{std::move(stored_size.error())};
        }
        if (*stored_size > std::numeric_limits<std::uint32_t>::max()) {
            return std::unexpected{invalid_argument_error(
                "content batch: packed blob '" + digest_hex(*written) +
                "' is larger than the 32-bit stored-size field")};
        }
        std::uint32_t flags{0};
        if (layout->kind == blob_kind::packed) {
            flags |= blob_flags_packed;
            if (layout->all_raw_blocks) {
                flags |= blob_flags_all_raw_blocks;
            }
        }
        impl_->blobs.push_back(blob_metadata{
            .digest = *written,
            .logical_size = layout->logical_size,
            .stored_size = static_cast<std::uint32_t>(*stored_size),
            .flags = flags,
            .segment_id = 0,
            .segment_offset = 0,
        });
        impl_->seen.insert(*written);
        return *written;
    }

    // Small raw blob: append to the current segment, sealing and reopening when
    // the target would be exceeded (a single blob may exceed the target; it
    // then simply lives alone in the new segment).
    const auto size{static_cast<std::uint64_t>(data.size())};
    if (!impl_->writer) {
        auto writer{
            segment_writer::create(impl_->segments_dir, impl_->target_bytes)};
        if (!writer) {
            impl_->failed = true;
            return std::unexpected{std::move(writer.error())};
        }
        impl_->writer.emplace(std::move(*writer));
        impl_->segment_begin = impl_->blobs.size();
    } else if (impl_->writer->blob_count() != 0U) {
        const auto remainder{impl_->writer->data_bytes() %
                             content_segment_alignment};
        const auto padding{
            remainder == 0U ? 0U : content_segment_alignment - remainder};
        const auto projected{impl_->writer->data_bytes() + padding + size};
        if (projected > impl_->target_bytes) {
            if (auto sealed{impl_->seal_current()}; !sealed) {
                return std::unexpected{std::move(sealed.error())};
            }
            auto writer{segment_writer::create(impl_->segments_dir,
                                               impl_->target_bytes)};
            if (!writer) {
                impl_->failed = true;
                return std::unexpected{std::move(writer.error())};
            }
            impl_->writer.emplace(std::move(*writer));
            impl_->segment_begin = impl_->blobs.size();
        }
    }

    auto offset{impl_->writer->append(data)};
    if (!offset) {
        impl_->failed = true;
        return std::unexpected{std::move(offset.error())};
    }
    impl_->blobs.push_back(blob_metadata{
        .digest = digest,
        .logical_size = size,
        .stored_size = static_cast<std::uint32_t>(data.size()),
        .flags = blob_flags_in_segment,
        .segment_id = 0, // filled in when the segment is sealed
        .segment_offset = *offset,
    });
    impl_->seen.insert(digest);
    return digest;
}

auto batch::put_file(std::string_view path) -> result<digest32> {
    if (!impl_) {
        return std::unexpected{make_error(error_code::not_initialized,
                                          "content batch is not initialized")};
    }
    auto bytes{core::io::read_file(path)};
    if (!bytes) {
        return std::unexpected{std::move(bytes.error())};
    }
    return put(*bytes);
}

auto batch::commit() -> status {
    if (!impl_) {
        return std::unexpected{make_error(error_code::not_initialized,
                                          "content batch is not initialized")};
    }
    if (impl_->committed) {
        return std::unexpected{
            invalid_argument_error("content batch is already committed")};
    }
    if (impl_->failed) {
        return std::unexpected{
            make_error(error_code::io_error,
                       "content batch failed earlier and cannot be committed")};
    }
    // Large blobs were already fsynced by atomic_write_file; only the last
    // segment still needs its staging file sealed and atomically published.
    if (auto sealed{impl_->seal_current()}; !sealed) {
        return sealed;
    }
    impl_->committed = true;
    return status_ok();
}

auto batch::new_blobs() const -> std::span<const blob_metadata> {
    if (!impl_) {
        return {};
    }
    return impl_->blobs;
}

auto encode_packed_blob(std::span<const std::byte> data)
    -> result<std::vector<std::byte>> {
    const auto digest{blake3(data)};
    return serialize_packed(data, digest);
}

auto decode_packed_blob(std::span<const std::byte> stored)
    -> result<std::vector<std::byte>> {
    if (stored.size() < blob_header_bytes ||
        !bytes_equal(stored, header_magic_offset, "RFBL")) {
        return std::unexpected{
            parse_error("packed blob is truncated or has a bad RFBL magic")};
    }
    if (read_u16(stored, header_version_offset) != blob_format_version) {
        return std::unexpected{
            parse_error("packed blob has an unsupported RFBL version")};
    }
    blob_location location{};
    location.kind = blob_kind::packed;
    location.block_count = read_u32(stored, header_block_count_offset);
    location.logical_size = read_u64(stored, header_logical_size_offset);
    location.stored_size = static_cast<std::uint32_t>(stored.size());
    std::copy_n(stored.begin() +
                    static_cast<std::ptrdiff_t>(header_file_blake3_offset),
                location.digest.size(), location.digest.begin());
    return decode_stored_blob(stored, location);
}

} // namespace refrain::data
