// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/content_index.hpp>

#include <refrain/core/bytes.hpp>
#include <refrain/data/path.hpp>
#include <refrain/platform/mmap.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::data {
namespace {

using core::bytes::read_u16;
using core::bytes::read_u32;
using core::bytes::read_u64;

// ---------------------------------------------------------------------------
// RFBT v1 layout (all integers little-endian).
// ---------------------------------------------------------------------------

constexpr std::uint16_t blob_table_version{1};

constexpr std::size_t rfbt_magic_offset{0};
constexpr std::size_t rfbt_version_offset{4};
constexpr std::size_t rfbt_flags_offset{6};
constexpr std::size_t rfbt_entry_count_offset{8};
constexpr std::size_t rfbt_entry_size_offset{12};
constexpr std::size_t rfbt_table_offset_offset{16};
constexpr std::size_t rfbt_table_bytes_offset{24};
constexpr std::size_t rfbt_file_blake3_offset{32};
constexpr std::size_t rfbt_header_crc32c_offset{64};
constexpr std::size_t rfbt_reserved_offset{68};
constexpr std::size_t rfbt_reserved_bytes{60};

constexpr std::size_t rfbt_entry_digest_offset{0};
constexpr std::size_t rfbt_entry_logical_size_offset{32};
constexpr std::size_t rfbt_entry_flags_offset{40};
constexpr std::size_t rfbt_entry_refcount_offset{44};
constexpr std::size_t rfbt_entry_kind_offset{48};
constexpr std::size_t rfbt_entry_reserved16_offset{50};
constexpr std::size_t rfbt_entry_block_count_offset{52};
constexpr std::size_t rfbt_entry_stored_size_offset{56};
constexpr std::size_t rfbt_entry_segment_id_offset{60};
constexpr std::size_t rfbt_entry_segment_offset_offset{68};
constexpr std::size_t rfbt_entry_reserved32_offset{72};
// The specified fields occupy 76 bytes; the trailing 4 bytes are the record's
// reserved tail (the 80-byte record is padded with zeroes).
constexpr std::size_t rfbt_entry_tail_offset{76};
constexpr std::size_t rfbt_entry_tail_bytes{4};

// ---------------------------------------------------------------------------
// RFRX v1 layout (all integers little-endian).
// ---------------------------------------------------------------------------

constexpr std::uint16_t resolution_index_version{1};

constexpr std::size_t rfrx_magic_offset{0};
constexpr std::size_t rfrx_version_offset{4};
constexpr std::size_t rfrx_flags_offset{6};
constexpr std::size_t rfrx_entry_count_offset{8};
constexpr std::size_t rfrx_source_count_offset{12};
constexpr std::size_t rfrx_entry_offset_offset{16};
constexpr std::size_t rfrx_entry_bytes_offset{24};
constexpr std::size_t rfrx_arena_offset_offset{32};
constexpr std::size_t rfrx_arena_bytes_offset{40};
constexpr std::size_t rfrx_file_blake3_offset{48};
constexpr std::size_t rfrx_header_crc32c_offset{80};
constexpr std::size_t rfrx_blob_table_prefix_offset{84};
constexpr std::size_t rfrx_blob_table_prefix_bytes{8};
constexpr std::size_t rfrx_reserved_offset{92};
constexpr std::size_t rfrx_reserved_bytes{36};

constexpr std::size_t rfrx_entry_hash_offset{0};
constexpr std::size_t rfrx_entry_priority_offset{8};
constexpr std::size_t rfrx_entry_source_index_offset{12};
constexpr std::size_t rfrx_entry_flags_offset{14};
constexpr std::size_t rfrx_entry_blob_index_offset{16};
constexpr std::size_t rfrx_entry_path_offset_offset{20};
constexpr std::size_t rfrx_entry_path_len_offset{24};
constexpr std::size_t rfrx_entry_reserved16_offset{26};
constexpr std::size_t rfrx_entry_reserved2_offset{28};

// `source_index` is 16-bit; a source table above this cannot be addressed.
constexpr std::size_t max_resolution_sources{65535};

// ---------------------------------------------------------------------------
// Helpers.
// ---------------------------------------------------------------------------

[[nodiscard]] auto invalid_argument_error(std::string message) -> error {
    return make_error(error_code::invalid_argument, std::move(message));
}

[[nodiscard]] auto parse_error(std::string message) -> error {
    return make_error(error_code::parse_error, std::move(message));
}

[[nodiscard]] auto version_error(std::string message) -> error {
    return make_error(error_code::version_mismatch, std::move(message));
}

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

[[nodiscard]] auto digest_bytes(const digest32 &digest) noexcept
    -> std::span<const std::byte> {
    return std::span<const std::byte>{digest.data(), digest.size()};
}

[[nodiscard]] auto digest_less(const digest32 &lhs,
                               const digest32 &rhs) noexcept -> bool {
    return std::lexicographical_compare(
        lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
        [](std::byte left, std::byte right) { return left < right; });
}

[[nodiscard]] auto text_bytes(std::string_view text) noexcept
    -> std::span<const std::byte> {
    return std::as_bytes(std::span<const char>{text.data(), text.size()});
}

/// BLAKE3(location)[0:8] interpreted as little-endian.
[[nodiscard]] auto compute_location_hash(std::string_view location) noexcept
    -> std::uint64_t {
    const auto digest{blake3(text_bytes(location))};
    std::uint64_t hash{0};
    for (std::size_t index{0}; index < 8U; ++index) {
        hash |= static_cast<std::uint64_t>(
                    std::to_integer<unsigned int>(digest[index]))
                << (8U * index);
    }
    return hash;
}

/// Structural validation shared by `build_blob_table` (invalid_argument) and
/// `parse_blob_table` (parse_error). Returns an empty string when valid.
[[nodiscard]] auto blob_entry_violation(const blob_table_entry &entry)
    -> std::string {
    if ((entry.flags & ~(blob_flags_packed | blob_flags_all_raw_blocks |
                         blob_flags_in_segment)) != 0U) {
        return "blob table record uses reserved flags";
    }
    if (entry.kind > blob_kind_builtin) {
        return "blob table record uses an unknown kind";
    }
    const auto packed{(entry.flags & blob_flags_packed) != 0U};
    const auto all_raw{(entry.flags & blob_flags_all_raw_blocks) != 0U};
    const auto in_segment{(entry.flags & blob_flags_in_segment) != 0U};
    if (in_segment) {
        if (entry.segment_id == 0U) {
            return "in-segment blob table record has a zero segment id";
        }
        // A zero-byte raw blob lives in the segment with a valid offset but a
        // zero stored size; every other in-segment blob needs stored bytes.
        if (entry.stored_size == 0U && !(!packed && entry.logical_size == 0U)) {
            return "in-segment blob table record declares an empty stored "
                   "size";
        }
        if (static_cast<std::uint64_t>(entry.segment_offset) +
                static_cast<std::uint64_t>(entry.stored_size) >
            std::numeric_limits<std::uint32_t>::max()) {
            return "in-segment blob table record overflows the 32-bit segment "
                   "offset range";
        }
    } else if (entry.segment_id != 0U || entry.segment_offset != 0U) {
        return "blob table record carries segment fields without the "
               "in-segment flag";
    }
    if (!packed) {
        if (all_raw) {
            return "raw blob table record marks all blocks raw";
        }
        if (entry.block_count != 0U) {
            return "raw blob table record declares a block count";
        }
        if (entry.logical_size > std::numeric_limits<std::uint32_t>::max()) {
            return "raw blob table record has a logical size above 32 bits";
        }
        if (entry.stored_size !=
            static_cast<std::uint32_t>(entry.logical_size)) {
            return "raw blob table record stored size does not match its "
                   "logical size";
        }
    } else {
        if (entry.block_count == 0U) {
            return "packed blob table record declares no blocks";
        }
        if (entry.logical_size == 0U) {
            return "packed blob table record declares an empty blob";
        }
        if (entry.stored_size == 0U) {
            return "packed blob table record declares an empty stored size";
        }
    }
    return {};
}

/// Decode one 80-byte RFBT record. The caller guarantees the size and has
/// already run (or is about to run) `validate_blob_record()`; this function
/// only performs the little-endian field decode.
[[nodiscard]] auto
decode_blob_record(std::span<const std::byte> record) noexcept
    -> blob_table_entry {
    blob_table_entry entry{};
    std::copy_n(record.begin() +
                    static_cast<std::ptrdiff_t>(rfbt_entry_digest_offset),
                entry.digest.size(), entry.digest.begin());
    entry.logical_size = read_u64(record, rfbt_entry_logical_size_offset);
    entry.flags = read_u32(record, rfbt_entry_flags_offset);
    entry.refcount = read_u32(record, rfbt_entry_refcount_offset);
    entry.kind = read_u16(record, rfbt_entry_kind_offset);
    entry.block_count = read_u32(record, rfbt_entry_block_count_offset);
    entry.stored_size = read_u32(record, rfbt_entry_stored_size_offset);
    entry.segment_id = read_u64(record, rfbt_entry_segment_id_offset);
    entry.segment_offset = read_u32(record, rfbt_entry_segment_offset_offset);
    return entry;
}

/// Decode and validate one 80-byte RFBT record (reserved bytes, reserved
/// flags/fields and the physical metadata rules). Returns parse_error, matching
/// `parse_blob_table()`.
[[nodiscard]] auto validate_blob_record(std::span<const std::byte> record)
    -> result<blob_table_entry> {
    const auto entry{decode_blob_record(record)};
    if (read_u16(record, rfbt_entry_reserved16_offset) != 0U ||
        read_u32(record, rfbt_entry_reserved32_offset) != 0U) {
        return std::unexpected{
            parse_error("blob table entry has non-zero reserved fields")};
    }
    for (std::size_t tail{0}; tail < rfbt_entry_tail_bytes; ++tail) {
        if (record[rfbt_entry_tail_offset + tail] != std::byte{0}) {
            return std::unexpected{parse_error(
                "blob table entry has non-zero reserved tail bytes")};
        }
    }
    if (auto violation{blob_entry_violation(entry)}; !violation.empty()) {
        return std::unexpected{parse_error("blob table: " + violation)};
    }
    return entry;
}

/// Validated layout of an RFBT file: the entry table is a view into the input
/// bytes (the mapping for `mapped_blob_table`).
struct rfbt_layout {
    std::span<const std::byte> table{};
    std::uint32_t entry_count{0};
    std::array<std::byte, 8> blake3_prefix{};
};

/// Header/table/bounds/CRC/BLAKE3 and every record check of
/// `parse_blob_table()`, without owning decoded records. Shared by the owning
/// parser and the mmap-backed view.
[[nodiscard]] auto validate_rfbt_layout(std::span<const std::byte> bytes)
    -> result<rfbt_layout> {
    if (bytes.size() < blob_table_header_bytes) {
        return std::unexpected{
            parse_error("blob table is truncated before the 128-byte header")};
    }
    if (!bytes_equal(bytes, rfbt_magic_offset, "RFBT")) {
        return std::unexpected{parse_error("blob table has a bad RFBT magic")};
    }
    if (read_u16(bytes, rfbt_version_offset) != blob_table_version) {
        return std::unexpected{version_error(
            "blob table has an unsupported RFBT format version (expected " +
            std::to_string(blob_table_version) + ")")};
    }
    if (read_u16(bytes, rfbt_flags_offset) != 0U) {
        return std::unexpected{
            parse_error("blob table has non-zero header flags")};
    }
    const auto entry_size{read_u32(bytes, rfbt_entry_size_offset)};
    if (entry_size != blob_table_entry_bytes) {
        return std::unexpected{
            parse_error("blob table declares an unsupported entry size")};
    }
    for (std::size_t index{0}; index < rfbt_reserved_bytes; ++index) {
        if (bytes[rfbt_reserved_offset + index] != std::byte{0}) {
            return std::unexpected{
                parse_error("blob table has non-zero reserved header bytes")};
        }
    }
    const auto entry_count{read_u32(bytes, rfbt_entry_count_offset)};
    const auto table_offset{read_u64(bytes, rfbt_table_offset_offset)};
    if (table_offset != blob_table_header_bytes) {
        return std::unexpected{
            parse_error("blob table declares an unexpected table offset")};
    }
    const auto expected_table_bytes{static_cast<std::uint64_t>(entry_count) *
                                    static_cast<std::uint64_t>(entry_size)};
    const auto table_bytes{read_u64(bytes, rfbt_table_bytes_offset)};
    if (table_bytes != expected_table_bytes) {
        return std::unexpected{
            parse_error("blob table declares an inconsistent table size")};
    }
    if (expected_table_bytes > bytes.size() - blob_table_header_bytes) {
        return std::unexpected{
            parse_error("blob table is truncated inside the entry table")};
    }
    if (expected_table_bytes != bytes.size() - blob_table_header_bytes) {
        return std::unexpected{
            parse_error("blob table has trailing bytes after the entry table")};
    }
    if (read_u32(bytes, rfbt_header_crc32c_offset) !=
        crc32c(bytes.first(rfbt_header_crc32c_offset))) {
        return std::unexpected{
            parse_error("blob table header failed the CRC32C check")};
    }
    {
        digest32 stored{};
        std::copy_n(bytes.begin() +
                        static_cast<std::ptrdiff_t>(rfbt_file_blake3_offset),
                    stored.size(), stored.begin());
        const auto computed{blake3(bytes.subspan(blob_table_header_bytes))};
        if (stored != computed) {
            return std::unexpected{
                parse_error("blob table failed the file BLAKE3 check")};
        }
    }

    const auto table{
        bytes.subspan(blob_table_header_bytes,
                      static_cast<std::size_t>(expected_table_bytes))};
    std::optional<digest32> previous{};
    for (std::size_t index{0}; index < entry_count; ++index) {
        const auto record{table.subspan(index * blob_table_entry_bytes,
                                        blob_table_entry_bytes)};
        auto entry{validate_blob_record(record)};
        if (!entry) {
            return std::unexpected{std::move(entry.error())};
        }
        if (previous.has_value()) {
            if (*previous == entry->digest) {
                return std::unexpected{
                    parse_error("blob table contains a duplicate digest")};
            }
            if (!digest_less(*previous, entry->digest)) {
                return std::unexpected{
                    parse_error("blob table entries are not sorted by digest")};
            }
        }
        previous = entry->digest;
    }

    rfbt_layout layout;
    layout.table = table;
    layout.entry_count = entry_count;
    const auto whole_digest{blake3(bytes)};
    std::copy_n(whole_digest.begin(), layout.blake3_prefix.size(),
                layout.blake3_prefix.begin());
    return layout;
}

[[nodiscard]] auto location_violation(std::string_view location)
    -> std::string {
    if (location.empty()) {
        return "resolution location is empty";
    }
    if (location.size() > max_resolution_location_bytes) {
        return "resolution location exceeds " +
               std::to_string(max_resolution_location_bytes) + " bytes";
    }
    if (!is_safe_relative_path(location)) {
        return "resolution location is not a safe relative path";
    }
    if (!is_valid_utf8(location)) {
        return "resolution location is not valid UTF-8";
    }
    return {};
}

[[nodiscard]] auto source_violation(std::string_view source) -> std::string {
    if (source.empty()) {
        return "resolution source is empty";
    }
    if (source.size() > max_resolution_source_bytes) {
        return "resolution source exceeds " +
               std::to_string(max_resolution_source_bytes) + " bytes";
    }
    if (source.find('\0') != std::string_view::npos) {
        return "resolution source contains a NUL byte";
    }
    if (!is_valid_utf8(source)) {
        return "resolution source is not valid UTF-8";
    }
    return {};
}

/// Byte range of one string inside an RFRX arena (offset relative to the
/// arena start).
struct arena_span {
    std::size_t offset{0};
    std::uint16_t length{0};
};

/// One validated location string: arena range plus the recomputed
/// `BLAKE3(location)[0:8]` hash used by entries.
struct location_span {
    std::uint32_t offset{0};
    std::uint16_t length{0};
    std::uint64_t hash{0};
};

/// Decoded scalar fields of one 32-byte RFRX record. The caller guarantees the
/// record size (all callers use a validated layout).
struct rfrx_record {
    std::uint64_t hash{0};
    std::int32_t priority{0};
    std::uint16_t source_index{0};
    std::uint16_t flags{0};
    std::uint32_t blob_index{0};
    std::uint32_t path_offset{0};
    std::uint16_t path_len{0};
    std::uint16_t reserved16{0};
    std::uint32_t reserved2{0};
};

[[nodiscard]] auto
decode_rfrx_record(std::span<const std::byte> record) noexcept -> rfrx_record {
    return rfrx_record{
        .hash = read_u64(record, rfrx_entry_hash_offset),
        .priority = std::bit_cast<std::int32_t>(
            read_u32(record, rfrx_entry_priority_offset)),
        .source_index = read_u16(record, rfrx_entry_source_index_offset),
        .flags = read_u16(record, rfrx_entry_flags_offset),
        .blob_index = read_u32(record, rfrx_entry_blob_index_offset),
        .path_offset = read_u32(record, rfrx_entry_path_offset_offset),
        .path_len = read_u16(record, rfrx_entry_path_len_offset),
        .reserved16 = read_u16(record, rfrx_entry_reserved16_offset),
        .reserved2 = read_u32(record, rfrx_entry_reserved2_offset),
    };
}

/// View of one arena string. The range must have been validated against the
/// arena by `validate_rfrx_layout()`.
[[nodiscard]] auto arena_text(std::span<const std::byte> arena,
                              std::size_t offset, std::uint16_t length) noexcept
    -> std::string_view {
    return std::string_view{
        reinterpret_cast<const char *>(arena.data() + offset), // NOLINT
        length};
}

/// Validated RFRX layout. The source/location tables keep only arena ranges
/// (no strings are owned); `entries` points into the input bytes (the mapping
/// for `mapped_resolution_index`).
struct rfrx_layout {
    std::span<const std::byte> arena{};
    std::span<const std::byte> entries{};
    std::uint32_t entry_count{0};
    std::array<std::byte, 8> blob_table_prefix{};
    std::vector<arena_span> sources{};
    std::vector<location_span> locations{};
};

[[nodiscard]] auto same_primary_key(const rfrx_record &lhs,
                                    const rfrx_record &rhs,
                                    std::span<const std::byte> arena) -> bool {
    return lhs.hash == rhs.hash && lhs.priority == rhs.priority &&
           lhs.source_index == rhs.source_index &&
           arena_text(arena, lhs.path_offset, lhs.path_len) ==
               arena_text(arena, rhs.path_offset, rhs.path_len);
}

[[nodiscard]] auto ordered_before(const rfrx_record &lhs,
                                  const rfrx_record &rhs,
                                  std::span<const std::byte> arena) -> bool {
    if (lhs.hash != rhs.hash) {
        return lhs.hash < rhs.hash;
    }
    if (lhs.priority != rhs.priority) {
        return lhs.priority > rhs.priority;
    }
    if (lhs.source_index != rhs.source_index) {
        return lhs.source_index < rhs.source_index;
    }
    return arena_text(arena, lhs.path_offset, lhs.path_len) <
           arena_text(arena, rhs.path_offset, rhs.path_len);
}

/// Wire form used while sorting a resolution index for serialization.
struct wire_resolution {
    std::uint64_t location_hash{0};
    std::int32_t priority{0};
    std::uint16_t source_index{0};
    std::uint32_t blob_index{0};
    std::uint32_t path_offset{0};
    std::uint16_t path_len{0};
    std::string_view location{};
};

[[nodiscard]] auto wire_before(const wire_resolution &lhs,
                               const wire_resolution &rhs) -> bool {
    if (lhs.location_hash != rhs.location_hash) {
        return lhs.location_hash < rhs.location_hash;
    }
    if (lhs.priority != rhs.priority) {
        return lhs.priority > rhs.priority;
    }
    if (lhs.source_index != rhs.source_index) {
        return lhs.source_index < rhs.source_index;
    }
    if (lhs.location != rhs.location) {
        return lhs.location < rhs.location;
    }
    // Every entry for one (location, priority) is a conflict, so this last
    // key is unreachable for a valid builder input; keep it so the sort stays
    // deterministic if the check ever moves.
    return lhs.blob_index < rhs.blob_index;
}

/// Position of `digest` in the digest-sorted blob table, or nullopt.
[[nodiscard]] auto find_blob_index(const blob_table_view &blob_table,
                                   const digest32 &digest)
    -> result<std::uint32_t> {
    const auto found{std::lower_bound(
        blob_table.entries.begin(), blob_table.entries.end(), digest,
        [](const blob_table_entry &entry, const digest32 &value) {
            return digest_less(entry.digest, value);
        })};
    if (found == blob_table.entries.end() || found->digest != digest) {
        return std::unexpected{
            invalid_argument_error("resolution index: blob " + to_hex(digest) +
                                   " is not present in the blob table")};
    }
    return static_cast<std::uint32_t>(found - blob_table.entries.begin());
}

} // namespace

// ---------------------------------------------------------------------------
// RFBT: global blob table.
// ---------------------------------------------------------------------------

auto build_blob_table(std::span<const blob_table_entry> entries)
    -> result<std::vector<std::byte>> {
    if (entries.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected{invalid_argument_error(
            "blob table: too many entries for the 32-bit entry count")};
    }

    std::vector<blob_table_entry> sorted{entries.begin(), entries.end()};
    for (const auto &entry : sorted) {
        if (auto violation{blob_entry_violation(entry)}; !violation.empty()) {
            return std::unexpected{
                invalid_argument_error("blob table: " + violation + " (" +
                                       to_hex(entry.digest) + ")")};
        }
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const blob_table_entry &lhs, const blob_table_entry &rhs) {
                  return digest_less(lhs.digest, rhs.digest);
              });
    for (std::size_t index{1}; index < sorted.size(); ++index) {
        if (sorted[index - 1].digest == sorted[index].digest) {
            return std::unexpected{
                invalid_argument_error("blob table: duplicate digest " +
                                       to_hex(sorted[index].digest))};
        }
    }

    const auto entry_count{static_cast<std::uint32_t>(sorted.size())};
    const auto table_bytes{static_cast<std::uint64_t>(entry_count) *
                           static_cast<std::uint64_t>(blob_table_entry_bytes)};
    const auto total_bytes{static_cast<std::uint64_t>(blob_table_header_bytes) +
                           table_bytes};
    std::vector<std::byte> out(static_cast<std::size_t>(total_bytes),
                               std::byte{0});
    const auto view{std::span<std::byte>{out}};

    for (std::size_t index{0}; index < 4U; ++index) {
        view[rfbt_magic_offset + index] = static_cast<std::byte>("RFBT"[index]);
    }
    write_u16(view, rfbt_version_offset, blob_table_version);
    write_u16(view, rfbt_flags_offset, 0);
    write_u32(view, rfbt_entry_count_offset, entry_count);
    write_u32(view, rfbt_entry_size_offset, blob_table_entry_bytes);
    write_u64(view, rfbt_table_offset_offset, blob_table_header_bytes);
    write_u64(view, rfbt_table_bytes_offset, table_bytes);

    for (std::size_t index{0}; index < sorted.size(); ++index) {
        const auto &entry{sorted[index]};
        const auto base{blob_table_header_bytes +
                        index * blob_table_entry_bytes};
        const auto digest{digest_bytes(entry.digest)};
        std::copy(digest.begin(), digest.end(),
                  view.begin() + static_cast<std::ptrdiff_t>(base));
        write_u64(view, base + rfbt_entry_logical_size_offset,
                  entry.logical_size);
        write_u32(view, base + rfbt_entry_flags_offset, entry.flags);
        write_u32(view, base + rfbt_entry_refcount_offset, entry.refcount);
        write_u16(view, base + rfbt_entry_kind_offset, entry.kind);
        write_u32(view, base + rfbt_entry_block_count_offset,
                  entry.block_count);
        write_u32(view, base + rfbt_entry_stored_size_offset,
                  entry.stored_size);
        write_u64(view, base + rfbt_entry_segment_id_offset, entry.segment_id);
        write_u32(view, base + rfbt_entry_segment_offset_offset,
                  entry.segment_offset);
        write_u32(view, base + rfbt_entry_reserved32_offset, 0);
        write_u32(view, base + rfbt_entry_tail_offset, 0);
    }

    const auto file_digest{blake3(
        std::span<const std::byte>{view}.subspan(blob_table_header_bytes))};
    const auto digest{digest_bytes(file_digest)};
    std::copy(digest.begin(), digest.end(),
              view.begin() +
                  static_cast<std::ptrdiff_t>(rfbt_file_blake3_offset));
    write_u32(view, rfbt_header_crc32c_offset,
              crc32c(std::span<const std::byte>{view}.first(
                  rfbt_header_crc32c_offset)));
    return out;
}

auto parse_blob_table(std::span<const std::byte> bytes)
    -> result<blob_table_view> {
    auto layout{validate_rfbt_layout(bytes)};
    if (!layout) {
        return std::unexpected{std::move(layout.error())};
    }
    std::vector<blob_table_entry> parsed;
    parsed.reserve(layout->entry_count);
    for (std::size_t index{0}; index < layout->entry_count; ++index) {
        parsed.push_back(decode_blob_record(layout->table.subspan(
            index * blob_table_entry_bytes, blob_table_entry_bytes)));
    }

    blob_table_view view;
    view.storage_ = std::move(parsed);
    view.entries = view.storage_;
    view.blob_table_blake3_prefix = layout->blake3_prefix;
    return view;
}

// ---------------------------------------------------------------------------
// RFRX: per-composition resolution index.
// ---------------------------------------------------------------------------

auto build_resolution_index(std::span<const resolution_input> entries,
                            const blob_table_view &blob_table)
    -> result<std::vector<std::byte>> {
    if (entries.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected{invalid_argument_error(
            "resolution index: too many entries for the 32-bit entry count")};
    }
    for (const auto &entry : entries) {
        if (auto violation{location_violation(entry.location)};
            !violation.empty()) {
            return std::unexpected{
                invalid_argument_error("resolution index: " + violation)};
        }
        if (auto violation{source_violation(entry.source)};
            !violation.empty()) {
            return std::unexpected{
                invalid_argument_error("resolution index: " + violation)};
        }
        if (entry.flags != 0U) {
            return std::unexpected{invalid_argument_error(
                "resolution index: entry uses reserved flags")};
        }
    }

    // One priority per layer: any two records for the same location at the
    // same priority are a conflict, even when they come from the same source
    // or point at the same blob. The layer order must assign distinct
    // priorities (startup derives them from the gamepack `[mods].list`), so
    // this is a hard error instead of an implicit source-order fallback chain.
    std::vector<std::size_t> order(entries.size());
    for (std::size_t index{0}; index < entries.size(); ++index) {
        order[index] = index;
    }
    std::sort(order.begin(), order.end(),
              [&entries](std::size_t lhs, std::size_t rhs) {
                  const auto &left{entries[lhs]};
                  const auto &right{entries[rhs]};
                  if (left.location != right.location) {
                      return left.location < right.location;
                  }
                  if (left.priority != right.priority) {
                      return left.priority < right.priority;
                  }
                  if (left.source != right.source) {
                      return left.source < right.source;
                  }
                  return digest_less(left.blob, right.blob);
              });
    for (std::size_t index{1}; index < order.size(); ++index) {
        const auto &previous{entries[order[index - 1]]};
        const auto &current{entries[order[index]]};
        if (previous.location == current.location &&
            previous.priority == current.priority) {
            return std::unexpected{invalid_argument_error(
                "resolution index: conflicting entries for location '" +
                current.location + "' at priority " +
                std::to_string(current.priority) + " from sources '" +
                previous.source + "' and '" + current.source + "'")};
        }
    }

    // Sorted, unique source table.
    std::vector<std::string> sources;
    sources.reserve(entries.size());
    for (const auto &entry : entries) {
        sources.push_back(entry.source);
    }
    std::sort(sources.begin(), sources.end());
    sources.erase(std::unique(sources.begin(), sources.end()), sources.end());
    if (sources.size() > max_resolution_sources) {
        return std::unexpected{invalid_argument_error(
            "resolution index: more than " +
            std::to_string(max_resolution_sources) + " distinct sources")};
    }

    // Sorted, unique location strings; offsets are assigned once the source
    // table has been written into the arena.
    std::vector<std::string> locations;
    locations.reserve(entries.size());
    for (const auto &entry : entries) {
        locations.push_back(entry.location);
    }
    std::sort(locations.begin(), locations.end());
    locations.erase(std::unique(locations.begin(), locations.end()),
                    locations.end());

    const auto append_text = [](std::vector<std::byte> &arena,
                                std::string_view text) -> bool {
        if (arena.size() > static_cast<std::size_t>(
                               std::numeric_limits<std::uint32_t>::max())) {
            return false;
        }
        const auto text_bytes_span{text_bytes(text)};
        arena.insert(arena.end(), text_bytes_span.begin(),
                     text_bytes_span.end());
        arena.push_back(std::byte{0});
        return true;
    };

    std::vector<std::byte> arena;
    for (const auto &source : sources) {
        if (!append_text(arena, source)) {
            return std::unexpected{invalid_argument_error(
                "resolution index: arena exceeds the 32-bit offset range")};
        }
    }
    std::vector<std::uint32_t> location_offsets(locations.size(), 0);
    for (std::size_t index{0}; index < locations.size(); ++index) {
        if (arena.size() > static_cast<std::size_t>(
                               std::numeric_limits<std::uint32_t>::max())) {
            return std::unexpected{invalid_argument_error(
                "resolution index: arena exceeds the 32-bit offset range")};
        }
        location_offsets[index] = static_cast<std::uint32_t>(arena.size());
        if (!append_text(arena, locations[index])) {
            return std::unexpected{invalid_argument_error(
                "resolution index: arena exceeds the 32-bit offset range")};
        }
    }

    std::vector<wire_resolution> wire;
    wire.reserve(entries.size());
    for (const auto &entry : entries) {
        auto blob_index{find_blob_index(blob_table, entry.blob)};
        if (!blob_index) {
            return std::unexpected{std::move(blob_index.error())};
        }
        const auto source_position{
            std::lower_bound(sources.begin(), sources.end(), entry.source)};
        const auto location_position{std::lower_bound(
            locations.begin(), locations.end(), entry.location)};
        wire.push_back(wire_resolution{
            .location_hash = compute_location_hash(entry.location),
            .priority = entry.priority,
            .source_index =
                static_cast<std::uint16_t>(source_position - sources.begin()),
            .blob_index = *blob_index,
            .path_offset = location_offsets[static_cast<std::size_t>(
                location_position - locations.begin())],
            .path_len = static_cast<std::uint16_t>(entry.location.size()),
            .location = entry.location,
        });
    }
    std::sort(wire.begin(), wire.end(), wire_before);

    const auto entry_count{static_cast<std::uint32_t>(wire.size())};
    const auto entry_bytes{
        static_cast<std::uint64_t>(entry_count) *
        static_cast<std::uint64_t>(resolution_index_entry_bytes)};
    const auto arena_offset{
        static_cast<std::uint64_t>(resolution_index_header_bytes) +
        entry_bytes};
    const auto total_bytes{arena_offset +
                           static_cast<std::uint64_t>(arena.size())};
    std::vector<std::byte> out(static_cast<std::size_t>(total_bytes),
                               std::byte{0});
    const auto view{std::span<std::byte>{out}};

    for (std::size_t index{0}; index < 4U; ++index) {
        view[rfrx_magic_offset + index] = static_cast<std::byte>("RFRX"[index]);
    }
    write_u16(view, rfrx_version_offset, resolution_index_version);
    write_u16(view, rfrx_flags_offset, 0);
    write_u32(view, rfrx_entry_count_offset, entry_count);
    write_u32(view, rfrx_source_count_offset,
              static_cast<std::uint32_t>(sources.size()));
    write_u64(view, rfrx_entry_offset_offset, resolution_index_header_bytes);
    write_u64(view, rfrx_entry_bytes_offset, entry_bytes);
    write_u64(view, rfrx_arena_offset_offset, arena_offset);
    write_u64(view, rfrx_arena_bytes_offset,
              static_cast<std::uint64_t>(arena.size()));
    std::copy(blob_table.blob_table_blake3_prefix.begin(),
              blob_table.blob_table_blake3_prefix.end(),
              view.begin() +
                  static_cast<std::ptrdiff_t>(rfrx_blob_table_prefix_offset));

    for (std::size_t index{0}; index < wire.size(); ++index) {
        const auto &entry{wire[index]};
        const auto base{resolution_index_header_bytes +
                        index * resolution_index_entry_bytes};
        write_u64(view, base + rfrx_entry_hash_offset, entry.location_hash);
        write_u32(view, base + rfrx_entry_priority_offset,
                  std::bit_cast<std::uint32_t>(entry.priority));
        write_u16(view, base + rfrx_entry_source_index_offset,
                  entry.source_index);
        write_u16(view, base + rfrx_entry_flags_offset, 0);
        write_u32(view, base + rfrx_entry_blob_index_offset, entry.blob_index);
        write_u32(view, base + rfrx_entry_path_offset_offset,
                  entry.path_offset);
        write_u16(view, base + rfrx_entry_path_len_offset, entry.path_len);
    }
    std::copy(arena.begin(), arena.end(),
              view.begin() + static_cast<std::ptrdiff_t>(arena_offset));

    const auto file_digest{blake3(std::span<const std::byte>{view}.subspan(
        resolution_index_header_bytes))};
    const auto digest{digest_bytes(file_digest)};
    std::copy(digest.begin(), digest.end(),
              view.begin() +
                  static_cast<std::ptrdiff_t>(rfrx_file_blake3_offset));
    write_u32(view, rfrx_header_crc32c_offset,
              crc32c(std::span<const std::byte>{view}.first(
                  rfrx_header_crc32c_offset)));
    return out;
}

/// Header/arena/entry validation of `parse_resolution_index()` against the
/// expected blob table prefix and record count, without owning decoded entries.
/// Shared by the owning parser and the mmap-backed view.
[[nodiscard]] auto
validate_rfrx_layout(std::span<const std::byte> bytes,
                     const std::array<std::byte, 8> &expected_blob_table_prefix,
                     std::size_t blob_count) -> result<rfrx_layout> {
    if (bytes.size() < resolution_index_header_bytes) {
        return std::unexpected{parse_error(
            "resolution index is truncated before the 128-byte header")};
    }
    if (!bytes_equal(bytes, rfrx_magic_offset, "RFRX")) {
        return std::unexpected{
            parse_error("resolution index has a bad RFRX magic")};
    }
    if (read_u16(bytes, rfrx_version_offset) != resolution_index_version) {
        return std::unexpected{version_error(
            "resolution index has an unsupported RFRX format version "
            "(expected " +
            std::to_string(resolution_index_version) + ")")};
    }
    if (read_u16(bytes, rfrx_flags_offset) != 0U) {
        return std::unexpected{
            parse_error("resolution index has non-zero header flags")};
    }
    const auto entry_count{read_u32(bytes, rfrx_entry_count_offset)};
    const auto source_count{read_u32(bytes, rfrx_source_count_offset)};
    if (source_count > max_resolution_sources) {
        return std::unexpected{
            parse_error("resolution index declares too many source strings")};
    }
    const auto entry_offset{read_u64(bytes, rfrx_entry_offset_offset)};
    if (entry_offset != resolution_index_header_bytes) {
        return std::unexpected{parse_error(
            "resolution index declares an unexpected entry offset")};
    }
    const auto expected_entry_bytes{
        static_cast<std::uint64_t>(entry_count) *
        static_cast<std::uint64_t>(resolution_index_entry_bytes)};
    const auto entry_bytes{read_u64(bytes, rfrx_entry_bytes_offset)};
    if (entry_bytes != expected_entry_bytes) {
        return std::unexpected{parse_error(
            "resolution index declares an inconsistent entry size")};
    }
    if (expected_entry_bytes > bytes.size() - resolution_index_header_bytes) {
        return std::unexpected{parse_error(
            "resolution index is truncated inside the entry table")};
    }
    const auto expected_arena_offset{
        static_cast<std::uint64_t>(resolution_index_header_bytes) +
        expected_entry_bytes};
    const auto arena_offset{read_u64(bytes, rfrx_arena_offset_offset)};
    if (arena_offset != expected_arena_offset) {
        return std::unexpected{parse_error(
            "resolution index declares an unexpected arena offset")};
    }
    const auto arena_bytes{read_u64(bytes, rfrx_arena_bytes_offset)};
    if (arena_bytes > bytes.size() || arena_offset > bytes.size() ||
        arena_bytes != bytes.size() - arena_offset) {
        return std::unexpected{parse_error(
            "resolution index declares an inconsistent arena size")};
    }
    for (std::size_t index{0}; index < rfrx_reserved_bytes; ++index) {
        if (bytes[rfrx_reserved_offset + index] != std::byte{0}) {
            return std::unexpected{parse_error(
                "resolution index has non-zero reserved header bytes")};
        }
    }
    if (read_u32(bytes, rfrx_header_crc32c_offset) !=
        crc32c(bytes.first(rfrx_header_crc32c_offset))) {
        return std::unexpected{
            parse_error("resolution index header failed the CRC32C check")};
    }
    for (std::size_t index{0}; index < rfrx_blob_table_prefix_bytes; ++index) {
        if (bytes[rfrx_blob_table_prefix_offset + index] !=
            expected_blob_table_prefix[index]) {
            return std::unexpected{parse_error(
                "resolution index is bound to a different blob table "
                "(blake3 prefix mismatch)")};
        }
    }
    {
        digest32 stored{};
        std::copy_n(bytes.begin() +
                        static_cast<std::ptrdiff_t>(rfrx_file_blake3_offset),
                    stored.size(), stored.begin());
        const auto computed{
            blake3(bytes.subspan(resolution_index_header_bytes))};
        if (stored != computed) {
            return std::unexpected{
                parse_error("resolution index failed the file BLAKE3 check")};
        }
    }

    const auto arena{bytes.subspan(static_cast<std::size_t>(arena_offset),
                                   static_cast<std::size_t>(arena_bytes))};

    // Source string table: NUL-separated, non-empty, sorted and unique. Every
    // source needs at least two arena bytes (one character plus the NUL).
    if (source_count != 0U && source_count > arena.size() / 2U) {
        return std::unexpected{
            parse_error("resolution index source table exceeds the arena")};
    }
    std::vector<arena_span> sources;
    sources.reserve(source_count);
    std::size_t cursor{0};
    std::string_view previous_source{};
    for (std::uint32_t index{0}; index < source_count; ++index) {
        const auto start{cursor};
        while (cursor < arena.size() && arena[cursor] != std::byte{0}) {
            ++cursor;
        }
        if (cursor == arena.size()) {
            return std::unexpected{
                parse_error("resolution index source table is not "
                            "NUL-terminated")};
        }
        const auto length{cursor - start};
        if (length == 0U || length > max_resolution_source_bytes) {
            return std::unexpected{
                parse_error("resolution index has an invalid source length")};
        }
        const auto text{
            arena_text(arena, start, static_cast<std::uint16_t>(length))};
        if (auto violation{source_violation(text)}; !violation.empty()) {
            return std::unexpected{
                parse_error("resolution index: " + violation)};
        }
        if (index != 0U && !(previous_source < text)) {
            return std::unexpected{parse_error(
                "resolution index source table is not sorted and unique")};
        }
        sources.push_back(arena_span{
            .offset = start, .length = static_cast<std::uint16_t>(length)});
        previous_source = text;
        ++cursor; // consume the NUL
    }

    // Location strings: NUL-separated, non-empty, safe relative UTF-8 paths,
    // sorted and unique.
    std::vector<location_span> locations;
    std::string_view previous_location{};
    while (cursor < arena.size()) {
        const auto start{cursor};
        while (cursor < arena.size() && arena[cursor] != std::byte{0}) {
            ++cursor;
        }
        if (cursor == arena.size()) {
            return std::unexpected{parse_error(
                "resolution index location table is not NUL-terminated")};
        }
        const auto length{cursor - start};
        if (length > max_resolution_location_bytes) {
            return std::unexpected{parse_error(
                "resolution index has an oversized location string")};
        }
        if (start > static_cast<std::size_t>(
                        std::numeric_limits<std::uint32_t>::max())) {
            return std::unexpected{parse_error(
                "resolution index location offset overflows 32 bits")};
        }
        const auto text{
            arena_text(arena, start, static_cast<std::uint16_t>(length))};
        if (auto violation{location_violation(text)}; !violation.empty()) {
            return std::unexpected{
                parse_error("resolution index: " + violation)};
        }
        if (!locations.empty() && !(previous_location < text)) {
            return std::unexpected{parse_error(
                "resolution index location table is not sorted and unique")};
        }
        locations.push_back(location_span{
            .offset = static_cast<std::uint32_t>(start),
            .length = static_cast<std::uint16_t>(length),
            .hash = compute_location_hash(text),
        });
        previous_location = text;
        ++cursor; // consume the NUL
    }

    const auto entries{
        bytes.subspan(resolution_index_header_bytes,
                      static_cast<std::size_t>(expected_entry_bytes))};
    for (std::size_t index{0}; index < entry_count; ++index) {
        const auto record{decode_rfrx_record(
            entries.subspan(index * resolution_index_entry_bytes,
                            resolution_index_entry_bytes))};
        if (record.flags != 0U) {
            return std::unexpected{
                parse_error("resolution index entry uses reserved flags")};
        }
        if (record.reserved16 != 0U || record.reserved2 != 0U) {
            return std::unexpected{parse_error(
                "resolution index entry has non-zero reserved fields")};
        }
        if (record.source_index >= sources.size()) {
            return std::unexpected{parse_error(
                "resolution index entry references a missing source")};
        }
        if (record.blob_index >= blob_count) {
            return std::unexpected{parse_error(
                "resolution index entry references a missing blob table "
                "record")};
        }
        const auto location{std::lower_bound(
            locations.begin(), locations.end(), record.path_offset,
            [](const location_span &candidate, std::uint32_t offset) {
                return candidate.offset < offset;
            })};
        if (location == locations.end() ||
            location->offset != record.path_offset ||
            location->length != record.path_len) {
            return std::unexpected{parse_error(
                "resolution index entry references an invalid location "
                "string")};
        }
        if (record.hash != location->hash) {
            return std::unexpected{parse_error(
                "resolution index entry has an invalid location hash")};
        }
    }

    // Entries must be in canonical `(hash, priority desc, source_index, path)`
    // order. Entries that tie on that key are legacy/manual data: the builder
    // rejects every same-(location, priority) pair, but a previously published
    // index may still carry different blobs at the same priority. Such records
    // are accepted in either relative order (the documented key does not
    // include the blob) and `resolve()` applies a deterministic tie-break that
    // ignores file order. An exact duplicate (same location, priority, source
    // and blob) is rejected. The whole tie group is checked, not just adjacent
    // records. The check runs after every record has been structurally
    // validated so the first reported error matches `parse_resolution_index()`.
    std::optional<rfrx_record> previous{};
    std::vector<std::uint32_t> group_blobs;
    bool group_open{false};
    const auto finish_group = [&group_blobs]() -> bool {
        std::sort(group_blobs.begin(), group_blobs.end());
        return std::adjacent_find(group_blobs.begin(), group_blobs.end()) ==
               group_blobs.end();
    };
    for (std::size_t index{0}; index < entry_count; ++index) {
        const auto record{decode_rfrx_record(
            entries.subspan(index * resolution_index_entry_bytes,
                            resolution_index_entry_bytes))};
        if (!previous.has_value()) {
            previous = record;
            continue;
        }
        if (!same_primary_key(*previous, record, arena)) {
            if (group_open) {
                if (!finish_group()) {
                    return std::unexpected{parse_error(
                        "resolution index contains a duplicate entry")};
                }
                group_open = false;
            }
            if (!ordered_before(*previous, record, arena)) {
                return std::unexpected{parse_error(
                    "resolution index entries are not sorted or contain "
                    "duplicates")};
            }
        } else {
            if (!group_open) {
                group_open = true;
                group_blobs.clear();
                group_blobs.push_back(previous->blob_index);
            }
            group_blobs.push_back(record.blob_index);
        }
        previous = record;
    }
    if (group_open && !finish_group()) {
        return std::unexpected{
            parse_error("resolution index contains a duplicate entry")};
    }

    rfrx_layout layout;
    layout.arena = arena;
    layout.entries = entries;
    layout.entry_count = entry_count;
    for (std::size_t index{0}; index < rfrx_blob_table_prefix_bytes; ++index) {
        layout.blob_table_prefix[index] =
            bytes[rfrx_blob_table_prefix_offset + index];
    }
    layout.sources = std::move(sources);
    layout.locations = std::move(locations);
    return layout;
}

auto parse_resolution_index(std::span<const std::byte> bytes,
                            const blob_table_view &blob_table)
    -> result<resolution_index_view> {
    auto layout{validate_rfrx_layout(bytes, blob_table.blob_table_blake3_prefix,
                                     blob_table.entries.size())};
    if (!layout) {
        return std::unexpected{std::move(layout.error())};
    }
    resolution_index_view view;
    view.entries.reserve(layout->entry_count);
    for (std::size_t index{0}; index < layout->entry_count; ++index) {
        const auto record{decode_rfrx_record(
            layout->entries.subspan(index * resolution_index_entry_bytes,
                                    resolution_index_entry_bytes))};
        const auto source{layout->sources[record.source_index]};
        view.entries.push_back(resolution_entry{
            .location = std::string{arena_text(
                layout->arena, record.path_offset, record.path_len)},
            .source = std::string{arena_text(layout->arena, source.offset,
                                             source.length)},
            .priority = record.priority,
            .blob_index = record.blob_index,
            .flags = 0,
        });
    }
    return view;
}

auto resolution_precedes(const resolution_entry &candidate,
                         const resolution_entry &current) noexcept -> bool {
    if (candidate.priority != current.priority) {
        return candidate.priority > current.priority;
    }
    if (candidate.blob_index != current.blob_index) {
        return candidate.blob_index < current.blob_index;
    }
    return candidate.source < current.source;
}

auto resolve(const resolution_index_view &index, std::string_view location)
    -> result<resolution> {
    std::vector<const resolution_entry *> matches;
    for (const auto &entry : index.entries) {
        if (entry.location == location) {
            matches.push_back(&entry);
        }
    }
    if (matches.empty()) {
        return std::unexpected{make_error(
            error_code::not_found, "resolution index has no entry for '" +
                                       std::string{location} + "'")};
    }
    std::sort(matches.begin(), matches.end(),
              [](const resolution_entry *lhs, const resolution_entry *rhs) {
                  return resolution_precedes(*lhs, *rhs);
              });
    resolution resolved;
    resolved.winner = matches.front();
    resolved.fallback.assign(matches.begin() + 1, matches.end());
    return resolved;
}

auto blob_digest(const resolution_entry &entry,
                 const blob_table_view &blob_table) -> result<digest32> {
    if (entry.blob_index >= blob_table.entries.size()) {
        return std::unexpected{invalid_argument_error(
            "resolution entry references a missing blob table record")};
    }
    return blob_table.entries[entry.blob_index].digest;
}

auto resolved_blob(const resolution &resolved,
                   const blob_table_view &blob_table) -> result<digest32> {
    if (resolved.winner == nullptr) {
        return std::unexpected{
            make_error(error_code::not_found, "resolution has no winner")};
    }
    return blob_digest(*resolved.winner, blob_table);
}

// ---------------------------------------------------------------------------
// Read-only mmap-backed views.
// ---------------------------------------------------------------------------

struct mapped_blob_table::impl {
    platform::mapped_file mapping{};
    std::span<const std::byte> table{};
    std::uint32_t entry_count{0};
    std::array<std::byte, 8> blake3_prefix{};

    [[nodiscard]] auto record_at(std::size_t index) const noexcept
        -> std::span<const std::byte> {
        return table.subspan(index * blob_table_entry_bytes,
                             blob_table_entry_bytes);
    }

    [[nodiscard]] auto digest_at(std::size_t index) const noexcept -> digest32 {
        digest32 digest{};
        const auto record{record_at(index)};
        std::copy_n(record.begin() +
                        static_cast<std::ptrdiff_t>(rfbt_entry_digest_offset),
                    digest.size(), digest.begin());
        return digest;
    }
};

mapped_blob_table::mapped_blob_table() noexcept = default;

mapped_blob_table::mapped_blob_table(std::unique_ptr<impl> state) noexcept
    : impl_{std::move(state)} {}

mapped_blob_table::mapped_blob_table(mapped_blob_table &&other) noexcept =
    default;

auto mapped_blob_table::operator=(mapped_blob_table &&other) noexcept
    -> mapped_blob_table & = default;

mapped_blob_table::~mapped_blob_table() = default;

auto mapped_blob_table::open(std::string_view path)
    -> result<mapped_blob_table> {
    if (path.empty()) {
        return std::unexpected{
            invalid_argument_error("blob table needs a non-empty path")};
    }
    auto mapping{platform::mapped_file::open(path)};
    if (!mapping) {
        return std::unexpected{std::move(mapping.error())};
    }
    auto state{std::make_unique<impl>()};
    state->mapping = std::move(*mapping);
    auto layout{validate_rfbt_layout(state->mapping.data())};
    if (!layout) {
        return std::unexpected{std::move(layout.error())};
    }
    state->table = layout->table;
    state->entry_count = layout->entry_count;
    state->blake3_prefix = layout->blake3_prefix;
    return mapped_blob_table{std::move(state)};
}

auto mapped_blob_table::valid() const noexcept -> bool {
    return impl_ != nullptr;
}

auto mapped_blob_table::size() const noexcept -> std::size_t {
    return impl_ != nullptr ? impl_->entry_count : 0U;
}

auto mapped_blob_table::at(std::size_t index) const -> blob_table_entry {
    assert(impl_ != nullptr);
    if (impl_ == nullptr || index >= impl_->entry_count) {
        return {};
    }
    return decode_blob_record(impl_->record_at(index));
}

auto mapped_blob_table::find(const digest32 &digest) const
    -> std::optional<blob_table_entry> {
    if (impl_ == nullptr || impl_->entry_count == 0U) {
        return std::nullopt;
    }
    std::size_t low{0};
    std::size_t high{impl_->entry_count};
    while (low < high) {
        const auto middle{low + (high - low) / 2U};
        if (digest_less(impl_->digest_at(middle), digest)) {
            low = middle + 1U;
        } else {
            high = middle;
        }
    }
    if (low >= impl_->entry_count || impl_->digest_at(low) != digest) {
        return std::nullopt;
    }
    return decode_blob_record(impl_->record_at(low));
}

auto mapped_blob_table::blake3_prefix() const noexcept
    -> std::array<std::byte, 8> {
    return impl_ != nullptr ? impl_->blake3_prefix : std::array<std::byte, 8>{};
}

struct mapped_resolution_index::impl {
    platform::mapped_file mapping{};
    std::span<const std::byte> arena{};
    std::span<const std::byte> entries{};
    std::uint32_t entry_count{0};
    std::array<std::byte, 8> blob_table_prefix{};
    std::vector<arena_span> sources{};

    [[nodiscard]] auto record_at(std::size_t index) const noexcept
        -> rfrx_record {
        return decode_rfrx_record(
            entries.subspan(index * resolution_index_entry_bytes,
                            resolution_index_entry_bytes));
    }

    [[nodiscard]] auto entry_at(std::size_t index) const -> resolution_entry {
        const auto record{record_at(index)};
        const auto source{sources[record.source_index]};
        return resolution_entry{
            .location = std::string{arena_text(arena, record.path_offset,
                                               record.path_len)},
            .source =
                std::string{arena_text(arena, source.offset, source.length)},
            .priority = record.priority,
            .blob_index = record.blob_index,
            .flags = 0,
        };
    }

    [[nodiscard]] auto hash_lower_bound(std::uint64_t hash) const noexcept
        -> std::size_t {
        std::size_t low{0};
        std::size_t high{entry_count};
        while (low < high) {
            const auto middle{low + (high - low) / 2U};
            if (record_at(middle).hash < hash) {
                low = middle + 1U;
            } else {
                high = middle;
            }
        }
        return low;
    }
};

mapped_resolution_index::mapped_resolution_index() noexcept = default;

mapped_resolution_index::mapped_resolution_index(
    std::unique_ptr<impl> state) noexcept
    : impl_{std::move(state)} {}

mapped_resolution_index::mapped_resolution_index(
    mapped_resolution_index &&other) noexcept = default;

auto mapped_resolution_index::operator=(
    mapped_resolution_index &&other) noexcept
    -> mapped_resolution_index & = default;

mapped_resolution_index::~mapped_resolution_index() = default;

auto mapped_resolution_index::open(std::string_view path,
                                   const blob_table_view &blob_table)
    -> result<mapped_resolution_index> {
    return open_bound(path, blob_table.blob_table_blake3_prefix,
                      blob_table.entries.size());
}

auto mapped_resolution_index::open(std::string_view path,
                                   const mapped_blob_table &blob_table)
    -> result<mapped_resolution_index> {
    if (!blob_table.valid()) {
        return std::unexpected{invalid_argument_error(
            "resolution index needs a valid mapped blob table")};
    }
    return open_bound(path, blob_table.blake3_prefix(), blob_table.size());
}

auto mapped_resolution_index::open_bound(
    std::string_view path, const std::array<std::byte, 8> &table_prefix,
    std::size_t blob_count) -> result<mapped_resolution_index> {
    if (path.empty()) {
        return std::unexpected{
            invalid_argument_error("resolution index needs a non-empty path")};
    }
    auto mapping{platform::mapped_file::open(path)};
    if (!mapping) {
        return std::unexpected{std::move(mapping.error())};
    }
    auto state{std::make_unique<impl>()};
    state->mapping = std::move(*mapping);
    auto layout{
        validate_rfrx_layout(state->mapping.data(), table_prefix, blob_count)};
    if (!layout) {
        return std::unexpected{std::move(layout.error())};
    }
    state->arena = layout->arena;
    state->entries = layout->entries;
    state->entry_count = layout->entry_count;
    state->blob_table_prefix = layout->blob_table_prefix;
    state->sources = std::move(layout->sources);
    return mapped_resolution_index{std::move(state)};
}

auto mapped_resolution_index::valid() const noexcept -> bool {
    return impl_ != nullptr;
}

auto mapped_resolution_index::size() const noexcept -> std::size_t {
    return impl_ != nullptr ? impl_->entry_count : 0U;
}

auto mapped_resolution_index::at(std::size_t index) const -> resolution_entry {
    assert(impl_ != nullptr);
    if (impl_ == nullptr || index >= impl_->entry_count) {
        return {};
    }
    return impl_->entry_at(index);
}

auto mapped_resolution_index::find(std::string_view location) const
    -> std::vector<resolution_entry> {
    if (impl_ == nullptr) {
        return {};
    }
    auto matches{find(compute_location_hash(location))};
    std::erase_if(matches, [location](const resolution_entry &entry) {
        return entry.location != location;
    });
    std::sort(matches.begin(), matches.end(),
              [](const resolution_entry &lhs, const resolution_entry &rhs) {
                  return resolution_precedes(lhs, rhs);
              });
    return matches;
}

auto mapped_resolution_index::find(std::uint64_t location_hash) const
    -> std::vector<resolution_entry> {
    std::vector<resolution_entry> matches;
    if (impl_ == nullptr || impl_->entry_count == 0U) {
        return matches;
    }
    const auto first{impl_->hash_lower_bound(location_hash)};
    for (std::size_t index{first}; index < impl_->entry_count; ++index) {
        if (impl_->record_at(index).hash != location_hash) {
            break;
        }
        matches.push_back(impl_->entry_at(index));
    }
    return matches;
}

auto mapped_resolution_index::blob_table_prefix() const noexcept
    -> std::array<std::byte, 8> {
    return impl_ != nullptr ? impl_->blob_table_prefix
                            : std::array<std::byte, 8>{};
}

} // namespace refrain::data
