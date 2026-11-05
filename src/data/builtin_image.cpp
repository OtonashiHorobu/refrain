// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/builtin_image.hpp>

#include <refrain/core/bytes.hpp>
#include <refrain/core/hex.hpp>
#include <refrain/core/io.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/content_index.hpp>
#include <refrain/data/content_maintenance.hpp>
#include <refrain/data/content_segment.hpp>
#include <refrain/data/content_store.hpp>
#include <refrain/data/path.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace refrain::data {
namespace {

namespace io = core::io;

using core::bytes::align_up;
using core::bytes::read_u16;
using core::bytes::read_u32;
using core::bytes::read_u64;
using core::hex::hex_u64;

[[nodiscard]] auto fail(error_code code, std::string message) -> error {
    return make_error(code, std::move(message));
}

[[nodiscard]] auto parse_error(std::string message) -> error {
    return fail(error_code::parse_error, std::move(message));
}

[[nodiscard]] auto io_error(std::string message) -> error {
    return fail(error_code::io_error, std::move(message));
}

/// Prefix the diagnostic of `failure` while keeping its code (`parse_error` /
/// `version_mismatch` must stay branchable by the caller).
[[nodiscard]] auto wrap_error(error failure, std::string_view context)
    -> error {
    failure.message = std::string{context} + ": " + failure.message;
    return failure;
}

/// UTF-8 -> std::filesystem::path through u8string, matching core::io (the
/// narrow `path` constructor would use the ANSI code page on Windows).
[[nodiscard]] auto to_path(std::string_view utf8_path)
    -> std::filesystem::path {
    std::u8string text;
    text.reserve(utf8_path.size());
    for (const char character : utf8_path) {
        text.push_back(
            static_cast<char8_t>(static_cast<unsigned char>(character)));
    }
    return std::filesystem::path{text};
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

/// One region of the image (metadata section, segment file or blob file).
struct image_region {
    std::uint64_t offset{0};
    std::uint64_t size{0};
    std::string_view what{};
};

/// One sealed segment embedded in the image (generation side).
struct embedded_segment {
    std::uint64_t id{0};
    std::vector<std::byte> bytes{};
};

/// One standalone blob embedded in the image (generation side).
struct embedded_blob {
    digest32 digest{};
    std::vector<std::byte> bytes{};
};

// ---------------------------------------------------------------------------
// Generation helpers.
// ---------------------------------------------------------------------------

/// Best-effort recursive removal of the private staging store. The directory
/// only ever holds content the builder itself created, and a failed cleanup is
/// inert (an orphan under the OS temp directory).
class scratch_directory {
  public:
    scratch_directory() = default;
    scratch_directory(const scratch_directory &) = delete;
    auto operator=(const scratch_directory &) -> scratch_directory & = delete;
    ~scratch_directory() {
        if (!path_.empty()) {
            std::error_code error;
            std::filesystem::remove_all(to_path(path_), error);
        }
    }

    [[nodiscard]] auto path() const noexcept -> const std::string & {
        return path_;
    }
    void reset(std::string path) { path_ = std::move(path); }

  private:
    std::string path_{};
};

/// Create a process-unique directory under `core::io::temp_directory()`.
[[nodiscard]] auto create_scratch_root() -> result<std::string> {
    const auto base{io::temp_directory()};
    if (base.empty()) {
        return std::unexpected{
            io_error("builtin image: the temporary directory is unknown")};
    }
    static std::atomic<std::uint64_t> sequence{0};
    const auto stamp{static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count())};
    for (int attempt{0}; attempt < 16; ++attempt) {
        const auto index{sequence.fetch_add(1, std::memory_order_relaxed)};
        auto path{join_path(base, "refrain-builtin-image-" +
                                      std::to_string(stamp) + "-" +
                                      std::to_string(index))};
        std::error_code error;
        if (std::filesystem::create_directories(to_path(path), error)) {
            return path;
        }
        if (error && error != std::errc::file_exists) {
            return std::unexpected{io_error(
                "builtin image: cannot create the temporary directory '" +
                path + "': " + error.message())};
        }
    }
    return std::unexpected{
        io_error("builtin image: cannot create a unique temporary directory "
                 "under '" +
                 base + "'")};
}

/// Lay out and serialize one RFSI v1 image. Section starts use 64-byte
/// alignment (the format only requires 8), directories are emitted in the
/// order given (segments by id, blobs by digest), padding is zero and the
/// header digest/CRC are computed last.
[[nodiscard]] auto
serialize_image(std::span<const std::byte> table_bytes,
                std::span<const std::byte> index_bytes,
                const std::vector<embedded_segment> &segments,
                const std::vector<embedded_blob> &blobs)
    -> result<std::vector<std::byte>> {
    if (segments.size() > std::numeric_limits<std::uint32_t>::max() ||
        blobs.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "builtin image: too many segments or blobs for the 32-bit "
                 "directory counts")};
    }
    constexpr auto section_alignment{
        static_cast<std::uint64_t>(builtin_image_section_alignment)};
    std::uint64_t cursor{builtin_image_header_bytes};
    const auto segment_dir_offset{cursor};
    cursor = align_up(cursor + static_cast<std::uint64_t>(segments.size()) *
                                   builtin_image_segment_directory_entry_bytes,
                      section_alignment);
    const auto blob_dir_offset{cursor};
    cursor = align_up(cursor + static_cast<std::uint64_t>(blobs.size()) *
                                   builtin_image_blob_directory_entry_bytes,
                      section_alignment);
    const auto table_offset{cursor};
    cursor = align_up(cursor + table_bytes.size(), section_alignment);
    const auto index_offset{cursor};
    cursor = align_up(cursor + index_bytes.size(), section_alignment);

    std::vector<std::uint64_t> segment_offsets(segments.size());
    for (std::size_t index{0}; index < segments.size(); ++index) {
        segment_offsets[index] = cursor;
        cursor =
            align_up(cursor + segments[index].bytes.size(), section_alignment);
    }
    std::vector<std::uint64_t> blob_offsets(blobs.size());
    for (std::size_t index{0}; index < blobs.size(); ++index) {
        blob_offsets[index] = cursor;
        cursor =
            align_up(cursor + blobs[index].bytes.size(), section_alignment);
    }
    if (cursor > std::numeric_limits<std::size_t>::max()) {
        return std::unexpected{fail(
            error_code::invalid_argument,
            "builtin image: the image does not fit the host address space")};
    }

    std::vector<std::byte> image(static_cast<std::size_t>(cursor));
    const auto target{std::span<std::byte>{image}};
    const auto copy_into = [&target](std::uint64_t offset,
                                     std::span<const std::byte> bytes) {
        if (!bytes.empty()) {
            std::copy(bytes.begin(), bytes.end(),
                      target.begin() + static_cast<std::ptrdiff_t>(offset));
        }
    };

    // Header.
    for (std::size_t index{0}; index < builtin_image_magic.size(); ++index) {
        image[index] = static_cast<std::byte>(builtin_image_magic[index]);
    }
    write_u16(target, 4, builtin_image_format_version);
    write_u16(target, 6, 0);
    write_u64(target, 8, table_offset);
    write_u64(target, 16, table_bytes.size());
    write_u64(target, 24, index_offset);
    write_u64(target, 32, index_bytes.size());
    write_u64(target, 40, segment_dir_offset);
    write_u32(target, 48, static_cast<std::uint32_t>(segments.size()));
    write_u32(target, 52, 0);
    write_u64(target, 56, blob_dir_offset);
    write_u32(target, 64, static_cast<std::uint32_t>(blobs.size()));
    write_u32(target, 68, 0);

    // Segment directory.
    for (std::size_t index{0}; index < segments.size(); ++index) {
        const auto base{static_cast<std::size_t>(segment_dir_offset) +
                        index * builtin_image_segment_directory_entry_bytes};
        write_u64(target, base, segments[index].id);
        write_u64(target, base + 8U, segment_offsets[index]);
        write_u64(target, base + 16U, segments[index].bytes.size());
    }
    // Blob directory.
    for (std::size_t index{0}; index < blobs.size(); ++index) {
        const auto base{static_cast<std::size_t>(blob_dir_offset) +
                        index * builtin_image_blob_directory_entry_bytes};
        std::copy(blobs[index].digest.begin(), blobs[index].digest.end(),
                  target.begin() + static_cast<std::ptrdiff_t>(base));
        if (blobs[index].bytes.size() >
            std::numeric_limits<std::uint32_t>::max()) {
            return std::unexpected{fail(
                error_code::invalid_argument,
                "builtin image: blob '" + to_hex(blobs[index].digest) +
                    "' is larger than the 32-bit blob directory size field")};
        }
        write_u64(target, base + 32U, blob_offsets[index]);
        write_u32(target, base + 40U,
                  static_cast<std::uint32_t>(blobs[index].bytes.size()));
        write_u32(target, base + 44U, 0);
    }

    copy_into(table_offset, table_bytes);
    copy_into(index_offset, index_bytes);
    for (std::size_t index{0}; index < segments.size(); ++index) {
        copy_into(segment_offsets[index], segments[index].bytes);
    }
    for (std::size_t index{0}; index < blobs.size(); ++index) {
        copy_into(blob_offsets[index], blobs[index].bytes);
    }

    // file_blake3 covers everything after the header; header_crc32c covers
    // [0, 104), i.e. the magic through file_blake3.
    const auto file_digest{blake3(
        std::span<const std::byte>{image}.subspan(builtin_image_header_bytes))};
    std::copy(file_digest.begin(), file_digest.end(), image.begin() + 72);
    write_u32(target, 104,
              crc32c(std::span<const std::byte>{image}.first(104)));
    return image;
}

} // namespace

// ---------------------------------------------------------------------------
// Reader.
// ---------------------------------------------------------------------------

auto builtin_image::open(std::span<const std::byte> bytes)
    -> result<builtin_image> {
    if (bytes.size() < builtin_image_header_bytes) {
        return std::unexpected{parse_error(
            "builtin image is truncated before the 128-byte header (" +
            std::to_string(bytes.size()) + " byte(s))")};
    }
    if (!bytes_equal(bytes, 0, builtin_image_magic)) {
        return std::unexpected{
            parse_error("builtin image has a bad RFSI magic")};
    }
    if (read_u16(bytes, 4) != builtin_image_format_version) {
        return std::unexpected{fail(
            error_code::version_mismatch,
            "builtin image has an unsupported RFSI format version (expected " +
                std::to_string(builtin_image_format_version) + ")")};
    }
    if (read_u16(bytes, 6) != 0U) {
        return std::unexpected{
            parse_error("builtin image has non-zero header flags")};
    }
    if (read_u32(bytes, 52) != 0U || read_u32(bytes, 68) != 0U) {
        return std::unexpected{
            parse_error("builtin image has non-zero reserved header fields")};
    }
    for (std::size_t offset{108}; offset < builtin_image_header_bytes;
         ++offset) {
        if (bytes[offset] != std::byte{0}) {
            return std::unexpected{parse_error(
                "builtin image has non-zero reserved header bytes")};
        }
    }
    if (read_u32(bytes, 104) != crc32c(bytes.first(104))) {
        return std::unexpected{
            parse_error("builtin image header failed the CRC32C check")};
    }

    digest32 file_digest{};
    std::copy_n(bytes.begin() + 72, file_digest.size(), file_digest.begin());
    if (blake3(bytes.subspan(builtin_image_header_bytes)) != file_digest) {
        return std::unexpected{parse_error(
            "builtin image failed the BLAKE3 verification (content is "
            "corrupted)")};
    }

    const auto table_offset{read_u64(bytes, 8)};
    const auto table_bytes{read_u64(bytes, 16)};
    const auto index_offset{read_u64(bytes, 24)};
    const auto index_bytes{read_u64(bytes, 32)};
    const auto segment_dir_offset{read_u64(bytes, 40)};
    const auto segment_count{read_u32(bytes, 48)};
    const auto blob_dir_offset{read_u64(bytes, 56)};
    const auto blob_count{read_u32(bytes, 64)};

    const auto image_size{static_cast<std::uint64_t>(bytes.size())};
    // The header is a region too, so no section may start before byte 128
    // (the documented header + directories + table + data order).
    std::vector<image_region> regions{
        image_region{0, builtin_image_header_bytes, "header"}};
    const auto add_region =
        [&regions, image_size](std::uint64_t offset, std::uint64_t size,
                               std::string_view what) -> status {
        if (offset % builtin_image_min_alignment != 0U) {
            return std::unexpected{
                parse_error("builtin image " + std::string{what} +
                            " is not 8-byte aligned (offset " +
                            std::to_string(offset) + ")")};
        }
        if (offset > image_size || size > image_size - offset) {
            return std::unexpected{parse_error(
                "builtin image " + std::string{what} +
                " lies outside the image (offset " + std::to_string(offset) +
                ", size " + std::to_string(size) + ", image size " +
                std::to_string(image_size) + ")")};
        }
        if (size != 0U) {
            regions.push_back(image_region{offset, size, what});
        }
        return status_ok();
    };

    const auto segment_dir_bytes{static_cast<std::uint64_t>(segment_count) *
                                 builtin_image_segment_directory_entry_bytes};
    const auto blob_dir_bytes{static_cast<std::uint64_t>(blob_count) *
                              builtin_image_blob_directory_entry_bytes};
    if (auto added{add_region(segment_dir_offset, segment_dir_bytes,
                              "segment directory")};
        !added) {
        return std::unexpected{std::move(added.error())};
    }
    if (auto added{
            add_region(blob_dir_offset, blob_dir_bytes, "blob directory")};
        !added) {
        return std::unexpected{std::move(added.error())};
    }
    if (table_bytes < blob_table_header_bytes) {
        return std::unexpected{parse_error(
            "builtin image blob table is shorter than its 128-byte header")};
    }
    if (index_bytes < resolution_index_header_bytes) {
        return std::unexpected{
            parse_error("builtin image resolution index is shorter than its "
                        "128-byte header")};
    }
    if (auto added{add_region(table_offset, table_bytes, "blob table")};
        !added) {
        return std::unexpected{std::move(added.error())};
    }
    if (auto added{add_region(index_offset, index_bytes, "resolution index")};
        !added) {
        return std::unexpected{std::move(added.error())};
    }

    // Segment directory records.
    struct segment_entry {
        std::uint64_t id{0};
        std::uint64_t offset{0};
        std::uint64_t size{0};
    };
    std::vector<segment_entry> segment_entries;
    segment_entries.reserve(segment_count);
    for (std::uint32_t index{0}; index < segment_count; ++index) {
        const auto base{static_cast<std::size_t>(segment_dir_offset) +
                        static_cast<std::size_t>(index) *
                            builtin_image_segment_directory_entry_bytes};
        segment_entry entry{read_u64(bytes, base), read_u64(bytes, base + 8U),
                            read_u64(bytes, base + 16U)};
        if (entry.size < content_segment_header_bytes) {
            return std::unexpected{
                parse_error("builtin image segment '" + hex_u64(entry.id) +
                            "' is shorter than the 80-byte RFSG header")};
        }
        if (auto added{add_region(entry.offset, entry.size, "segment")};
            !added) {
            return std::unexpected{std::move(added.error())};
        }
        segment_entries.push_back(entry);
    }
    std::sort(segment_entries.begin(), segment_entries.end(),
              [](const segment_entry &lhs, const segment_entry &rhs) {
                  return lhs.id < rhs.id;
              });
    for (std::size_t index{1}; index < segment_entries.size(); ++index) {
        if (segment_entries[index - 1U].id == segment_entries[index].id) {
            return std::unexpected{
                parse_error("builtin image has a duplicate segment id '" +
                            hex_u64(segment_entries[index].id) + "'")};
        }
    }

    // Blob directory records.
    struct blob_entry {
        digest32 digest{};
        std::uint64_t offset{0};
        std::uint32_t size{0};
    };
    std::vector<blob_entry> blob_entries;
    blob_entries.reserve(blob_count);
    for (std::uint32_t index{0}; index < blob_count; ++index) {
        const auto base{static_cast<std::size_t>(blob_dir_offset) +
                        static_cast<std::size_t>(index) *
                            builtin_image_blob_directory_entry_bytes};
        blob_entry entry{};
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(base),
                    entry.digest.size(), entry.digest.begin());
        entry.offset = read_u64(bytes, base + 32U);
        entry.size = read_u32(bytes, base + 40U);
        if (read_u32(bytes, base + 44U) != 0U) {
            return std::unexpected{parse_error(
                "builtin image blob directory entry '" + to_hex(entry.digest) +
                "' has non-zero reserved bytes")};
        }
        if (auto added{add_region(entry.offset, entry.size, "blob")}; !added) {
            return std::unexpected{std::move(added.error())};
        }
        blob_entries.push_back(entry);
    }
    std::sort(blob_entries.begin(), blob_entries.end(),
              [](const blob_entry &lhs, const blob_entry &rhs) {
                  return lhs.digest < rhs.digest;
              });
    for (std::size_t index{1}; index < blob_entries.size(); ++index) {
        if (blob_entries[index - 1U].digest == blob_entries[index].digest) {
            return std::unexpected{
                parse_error("builtin image has a duplicate blob digest '" +
                            to_hex(blob_entries[index].digest) + "'")};
        }
    }

    // Regions are non-overlapping and sorted by offset.
    std::sort(regions.begin(), regions.end(),
              [](const image_region &lhs, const image_region &rhs) {
                  return lhs.offset < rhs.offset;
              });
    for (std::size_t index{1}; index < regions.size(); ++index) {
        const auto &previous{regions[index - 1U]};
        const auto &current{regions[index]};
        if (previous.offset + previous.size > current.offset) {
            return std::unexpected{parse_error(
                "builtin image regions overlap: " + std::string{previous.what} +
                " and " + std::string{current.what} + " at byte " +
                std::to_string(current.offset))};
        }
    }

    builtin_image image;
    image.image_ = bytes;
    image.digest_ = file_digest;

    // Sealed segments: header (magic/version/id/data_bytes/CRC) plus body
    // BLAKE3, exactly like `verify_segment()` but keeping the header fields.
    image.segments_.reserve(segment_entries.size());
    for (const auto &entry : segment_entries) {
        const auto span{bytes.subspan(static_cast<std::size_t>(entry.offset),
                                      static_cast<std::size_t>(entry.size))};
        auto header{read_segment_header(span)};
        if (!header) {
            return std::unexpected{wrap_error(std::move(header.error()),
                                              "builtin image segment '" +
                                                  hex_u64(entry.id) + "'")};
        }
        if (header->id != entry.id) {
            return std::unexpected{parse_error(
                "builtin image segment directory id '" + hex_u64(entry.id) +
                "' does not match its RFSG header id '" + hex_u64(header->id) +
                "'")};
        }
        if (blake3(span.subspan(content_segment_header_bytes)) !=
            header->body_hash) {
            return std::unexpected{
                parse_error("builtin image segment '" + hex_u64(entry.id) +
                            "' body failed the BLAKE3 check")};
        }
        image.segments_.push_back(segment_record{
            .id = entry.id,
            .data_bytes = header->data_bytes,
            .bytes = span,
        });
    }
    image.blobs_.reserve(blob_entries.size());
    for (const auto &entry : blob_entries) {
        image.blobs_.push_back(blob_record{
            .digest = entry.digest,
            .bytes = bytes.subspan(static_cast<std::size_t>(entry.offset),
                                   entry.size),
        });
    }

    // Blob table and resolution index (the RFRX parse also verifies the
    // `blob_table_blake3_prefix` binding to these exact table bytes).
    auto table{
        parse_blob_table(bytes.subspan(static_cast<std::size_t>(table_offset),
                                       static_cast<std::size_t>(table_bytes)))};
    if (!table) {
        return std::unexpected{
            wrap_error(std::move(table.error()), "builtin image blob table")};
    }
    auto parsed_index{parse_resolution_index(
        bytes.subspan(static_cast<std::size_t>(index_offset),
                      static_cast<std::size_t>(index_bytes)),
        *table)};
    if (!parsed_index) {
        return std::unexpected{wrap_error(std::move(parsed_index.error()),
                                          "builtin image resolution index")};
    }

    // Every table record must be materialized in the image: in-segment
    // records through a segment directory entry, standalone records through a
    // blob directory entry whose size matches the RFBT `stored_size`.
    struct segment_span {
        std::size_t segment{0};
        std::uint32_t offset{0};
        std::uint32_t size{0};
    };
    std::vector<segment_span> spans;
    for (const auto &entry : table->entries) {
        if ((entry.flags & blob_flags_in_segment) != 0U) {
            const auto *segment{image.find_segment(entry.segment_id)};
            if (segment == nullptr) {
                return std::unexpected{parse_error(
                    "builtin image blob table record '" + to_hex(entry.digest) +
                    "' references segment '" + hex_u64(entry.segment_id) +
                    "' which is missing from the segment directory")};
            }
            if (entry.segment_offset % content_segment_alignment != 0U) {
                return std::unexpected{parse_error(
                    "builtin image blob table record '" + to_hex(entry.digest) +
                    "' is not 8-byte aligned inside its segment")};
            }
            if (static_cast<std::uint64_t>(entry.segment_offset) +
                    static_cast<std::uint64_t>(entry.stored_size) >
                segment->data_bytes) {
                return std::unexpected{parse_error(
                    "builtin image blob table record '" + to_hex(entry.digest) +
                    "' lies outside its segment body")};
            }
            spans.push_back(segment_span{
                .segment =
                    static_cast<std::size_t>(segment - image.segments_.data()),
                .offset = entry.segment_offset,
                .size = entry.stored_size,
            });
            continue;
        }
        const auto *blob{image.find_blob(entry.digest)};
        if (blob == nullptr) {
            return std::unexpected{parse_error(
                "builtin image blob table record '" + to_hex(entry.digest) +
                "' has no blob directory entry")};
        }
        if (blob->bytes.size() != entry.stored_size) {
            return std::unexpected{parse_error(
                "builtin image blob directory size for '" +
                to_hex(entry.digest) + "' does not match the blob table")};
        }
    }
    std::sort(spans.begin(), spans.end(),
              [](const segment_span &lhs, const segment_span &rhs) {
                  return lhs.segment != rhs.segment ? lhs.segment < rhs.segment
                                                    : lhs.offset < rhs.offset;
              });
    // A zero-byte raw blob occupies no bytes and may share an aligned offset
    // with the next blob (the batch writer appends it first), so it can never
    // overlap anything and does not extend the previous end.
    std::size_t previous_segment{0};
    std::uint64_t previous_end{0};
    bool has_previous{false};
    for (const auto &span : spans) {
        if (span.size == 0U) {
            continue;
        }
        if (has_previous && span.segment == previous_segment &&
            span.offset < previous_end) {
            return std::unexpected{
                parse_error("builtin image segment '" +
                            hex_u64(image.segments_[span.segment].id) +
                            "' blobs overlap or are out of order")};
        }
        previous_segment = span.segment;
        previous_end = static_cast<std::uint64_t>(span.offset) + span.size;
        has_previous = true;
    }

    // Winner per location via the shared winner order
    // (`resolution_precedes`: priority desc, then blob_index asc, then source
    // asc), re-sorted by path bytes as the pack entry order. Images generated
    // by build_resolution_index cannot contain same-priority ties (the builder
    // rejects them), but a hand-crafted legacy image still reads
    // deterministically because this comparison ignores the record order.
    if (parsed_index->entries.size() > vfs_max_files) {
        return std::unexpected{
            parse_error("builtin image resolution index has more than " +
                        std::to_string(vfs_max_files) +
                        " entries (refused before indexing)")};
    }
    std::map<std::string, std::size_t> winners;
    for (std::size_t position{0}; position < parsed_index->entries.size();
         ++position) {
        const auto &record{parsed_index->entries[position]};
        const auto found{winners.find(record.location)};
        if (found == winners.end()) {
            winners.emplace(record.location, position);
        } else if (resolution_precedes(record,
                                       parsed_index->entries[found->second])) {
            found->second = position;
        }
    }
    image.entries_.reserve(winners.size());
    image.locations_.reserve(winners.size());
    for (const auto &[path, position] : winners) {
        const auto &record{parsed_index->entries[position]};
        if (record.blob_index >= table->entries.size()) {
            // Unreachable for a successfully parsed index; kept as a guard so
            // the lookup below can never read past the table.
            return std::unexpected{parse_error(
                "builtin image resolution index references a missing blob "
                "table record")};
        }
        const auto &blob{table->entries[record.blob_index]};
        image.entries_.push_back(
            pack_file_info{.path = path, .size = blob.logical_size});
        image.locations_.push_back(blob_location_from_table_entry(blob));
    }
    return image;
}

auto builtin_image::file_count() const noexcept -> std::size_t {
    return entries_.size();
}

auto builtin_image::entries() const noexcept
    -> std::span<const pack_file_info> {
    return entries_;
}

auto builtin_image::find(std::string_view path) const noexcept
    -> const pack_file_info * {
    const auto match{
        std::lower_bound(entries_.begin(), entries_.end(), path,
                         [](const pack_file_info &entry, std::string_view key) {
                             return std::string_view{entry.path} < key;
                         })};
    if (match == entries_.end() || std::string_view{match->path} != path) {
        return nullptr;
    }
    return &*match;
}

auto builtin_image::read(std::string_view path) const
    -> result<std::vector<std::byte>> {
    const auto *entry{find(path)};
    if (entry == nullptr) {
        return std::unexpected{
            fail(error_code::not_found,
                 "builtin image has no file '" + std::string{path} + "'")};
    }
    const auto index{static_cast<std::size_t>(entry - entries_.data())};
    const auto &location{locations_[index]};

    std::span<const std::byte> stored{};
    if (location.in_segment) {
        const auto *segment{find_segment(location.segment_id)};
        if (segment == nullptr) {
            return std::unexpected{
                parse_error("builtin image file '" + std::string{path} +
                            "' references a segment missing from the image")};
        }
        const auto body{segment->bytes.subspan(content_segment_header_bytes)};
        if (static_cast<std::uint64_t>(location.segment_offset) > body.size() ||
            static_cast<std::uint64_t>(location.stored_size) >
                body.size() - location.segment_offset) {
            return std::unexpected{
                parse_error("builtin image file '" + std::string{path} +
                            "' lies outside its segment body")};
        }
        stored = body.subspan(location.segment_offset, location.stored_size);
    } else {
        const auto *blob{find_blob(location.digest)};
        if (blob == nullptr) {
            return std::unexpected{
                parse_error("builtin image file '" + std::string{path} +
                            "' has no blob directory entry")};
        }
        stored = blob->bytes;
    }

    auto decoded{decode_stored_blob(stored, location)};
    if (!decoded) {
        auto failure{std::move(decoded.error())};
        failure.message = "builtin image file '" + std::string{path} +
                          "': " + failure.message;
        return std::unexpected{std::move(failure)};
    }
    return decoded;
}

auto builtin_image::digest() const noexcept -> const digest32 & {
    return digest_;
}

auto builtin_image::find_segment(std::uint64_t id) const noexcept
    -> const segment_record * {
    const auto match{
        std::lower_bound(segments_.begin(), segments_.end(), id,
                         [](const segment_record &record, std::uint64_t key) {
                             return record.id < key;
                         })};
    if (match == segments_.end() || match->id != id) {
        return nullptr;
    }
    return &*match;
}

auto builtin_image::find_blob(const digest32 &digest) const noexcept
    -> const blob_record * {
    const auto match{
        std::lower_bound(blobs_.begin(), blobs_.end(), digest,
                         [](const blob_record &record, const digest32 &key) {
                             return record.digest < key;
                         })};
    if (match == blobs_.end() || match->digest != digest) {
        return nullptr;
    }
    return &*match;
}

// ---------------------------------------------------------------------------
// Pack adapter.
// ---------------------------------------------------------------------------

namespace {

/// Borrowed RFSI image exposed as a VFS pack; `read()` delegates to the image,
/// so the caller-owned bytes stay the single source of truth.
class builtin_image_pack final : public pack {
  public:
    builtin_image_pack(builtin_image image, std::string label)
        : image_{std::move(image)}, label_{std::move(label)} {}

    [[nodiscard]] auto format() const noexcept -> pack_format override {
        return pack_format::memory;
    }
    [[nodiscard]] auto label() const noexcept -> std::string_view override {
        return label_;
    }
    [[nodiscard]] auto file_count() const noexcept -> std::size_t override {
        return image_.file_count();
    }
    [[nodiscard]] auto entries() const noexcept
        -> std::span<const pack_file_info> override {
        return image_.entries();
    }
    [[nodiscard]] auto find(std::string_view path) const noexcept
        -> const pack_file_info * override {
        return image_.find(path);
    }
    [[nodiscard]] auto read(std::string_view path) const
        -> result<std::vector<std::byte>> override {
        if (image_.find(path) == nullptr) {
            return std::unexpected{
                fail(error_code::not_found, "builtin image pack '" + label_ +
                                                "' has no file '" +
                                                std::string{path} + "'")};
        }
        auto bytes{image_.read(path)};
        if (!bytes) {
            auto failure{std::move(bytes.error())};
            failure.message =
                "builtin image pack '" + label_ + "': " + failure.message;
            return std::unexpected{std::move(failure)};
        }
        return bytes;
    }

  private:
    builtin_image image_;
    std::string label_;
};

} // namespace

auto open_builtin_image_pack(std::span<const std::byte> bytes,
                             std::string_view label)
    -> result<std::unique_ptr<pack>> {
    auto image{builtin_image::open(bytes)};
    if (!image) {
        return std::unexpected{std::move(image.error())};
    }
    return std::unique_ptr<pack>{std::make_unique<builtin_image_pack>(
        std::move(*image),
        label.empty() ? std::string{"builtin.image"} : std::string{label})};
}

// ---------------------------------------------------------------------------
// Generator.
// ---------------------------------------------------------------------------

auto build_builtin_image(std::span<const builtin_image_input> files)
    -> result<std::vector<std::byte>> {
    if (files.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected{fail(
            error_code::invalid_argument,
            "builtin image: more input files than the 32-bit entry count")};
    }

    // Validation and deterministic (path byte order) processing order.
    std::vector<const builtin_image_input *> ordered;
    ordered.reserve(files.size());
    for (const auto &file : files) {
        if (file.path.empty()) {
            return std::unexpected{
                fail(error_code::invalid_argument,
                     "builtin image: input file paths must not be empty")};
        }
        if (file.path.size() > max_resolution_location_bytes) {
            return std::unexpected{fail(
                error_code::invalid_argument,
                "builtin image: input path '" + file.path +
                    "' is longer than " +
                    std::to_string(max_resolution_location_bytes) + " bytes")};
        }
        if (!is_valid_utf8(file.path)) {
            return std::unexpected{
                fail(error_code::invalid_argument,
                     "builtin image: input path is not valid UTF-8")};
        }
        if (!is_safe_relative_path(file.path)) {
            return std::unexpected{fail(error_code::invalid_argument,
                                        "builtin image: input path '" +
                                            file.path +
                                            "' is not a safe relative path")};
        }
        ordered.push_back(&file);
    }
    std::sort(
        ordered.begin(), ordered.end(),
        [](const builtin_image_input *lhs, const builtin_image_input *rhs) {
            return lhs->path < rhs->path;
        });
    for (std::size_t index{1}; index < ordered.size(); ++index) {
        if (ordered[index - 1U]->path == ordered[index]->path) {
            return std::unexpected{
                fail(error_code::invalid_argument,
                     "builtin image: duplicate input path '" +
                         ordered[index]->path + "'")};
        }
    }

    // Stage the content through the regular batch rules in a private
    // temporary store: <= 64 KiB (including zero-byte files) goes to a
    // content segment, larger files become standalone packed blobs.
    scratch_directory scratch;
    auto root{create_scratch_root()};
    if (!root) {
        return std::unexpected{std::move(root.error())};
    }
    scratch.reset(*root);

    auto staged{
        batch::begin(scratch.path(), nullptr, content_segment_target_bytes)};
    if (!staged) {
        return std::unexpected{std::move(staged.error())};
    }
    const std::string builtin_source{"builtin"};
    std::vector<resolution_input> inputs;
    inputs.reserve(ordered.size());
    for (const auto *file : ordered) {
        auto digest{staged->put(file->data)};
        if (!digest) {
            return std::unexpected{wrap_error(
                std::move(digest.error()),
                "builtin image: cannot stage file '" + file->path + "'")};
        }
        inputs.push_back(resolution_input{
            .location = file->path,
            .source = builtin_source,
            .priority = 0,
            .blob = *digest,
            .flags = 0,
        });
    }
    if (auto committed{staged->commit()}; !committed) {
        return std::unexpected{wrap_error(std::move(committed.error()),
                                          "builtin image: cannot commit the "
                                          "staged content")};
    }

    std::map<digest32, std::uint32_t> references;
    for (const auto &input : inputs) {
        auto &count{references[input.blob]};
        if (count == std::numeric_limits<std::uint32_t>::max()) {
            return std::unexpected{fail(
                error_code::invalid_argument,
                "builtin image: a blob has more references than the 32-bit "
                "refcount field")};
        }
        ++count;
    }

    std::vector<blob_table_entry> table_entries;
    table_entries.reserve(staged->new_blobs().size());
    for (const auto &metadata : staged->new_blobs()) {
        const auto found{references.find(metadata.digest)};
        if (found == references.end()) {
            return std::unexpected{parse_error("builtin image: staged blob '" +
                                               to_hex(metadata.digest) +
                                               "' has no resolution entry")};
        }
        blob_table_entry entry{};
        entry.digest = metadata.digest;
        entry.logical_size = metadata.logical_size;
        entry.flags = metadata.flags;
        entry.refcount = found->second;
        entry.kind = blob_kind_builtin;
        entry.block_count = (metadata.flags & blob_flags_packed) != 0U
                                ? static_cast<std::uint32_t>(
                                      (metadata.logical_size + 65535U) / 65536U)
                                : 0U;
        entry.stored_size = metadata.stored_size;
        entry.segment_id = metadata.segment_id;
        entry.segment_offset = metadata.segment_offset;
        table_entries.push_back(entry);
    }

    auto table_bytes{build_blob_table(table_entries)};
    if (!table_bytes) {
        return std::unexpected{wrap_error(std::move(table_bytes.error()),
                                          "builtin image: cannot build the "
                                          "blob table")};
    }
    auto table{parse_blob_table(*table_bytes)};
    if (!table) {
        return std::unexpected{wrap_error(std::move(table.error()),
                                          "builtin image: cannot parse the "
                                          "built blob table")};
    }
    auto index_bytes{build_resolution_index(inputs, *table)};
    if (!index_bytes) {
        return std::unexpected{wrap_error(std::move(index_bytes.error()),
                                          "builtin image: cannot build the "
                                          "resolution index")};
    }

    // Embed every sealed segment (sorted by id) whole.
    auto segments{scan_content_segments(scratch.path())};
    if (!segments) {
        return std::unexpected{wrap_error(std::move(segments.error()),
                                          "builtin image: cannot scan the "
                                          "staged segments")};
    }
    std::vector<embedded_segment> segment_data;
    segment_data.reserve(segments->size());
    for (const auto &segment : *segments) {
        if (!segment.header_ok) {
            return std::unexpected{
                parse_error("builtin image: staged segment '" + segment.path +
                            "' has an invalid RFSG header")};
        }
        auto bytes{io::read_file(segment.path)};
        if (!bytes) {
            return std::unexpected{
                wrap_error(std::move(bytes.error()),
                           "builtin image: cannot read the staged segment '" +
                               segment.path + "'")};
        }
        segment_data.push_back(embedded_segment{
            .id = segment.id,
            .bytes = std::move(*bytes),
        });
    }

    // Embed every standalone blob file (the table is digest-sorted, so the
    // directory order is deterministic).
    auto store{content_store::open(scratch.path())};
    if (!store) {
        return std::unexpected{wrap_error(std::move(store.error()),
                                          "builtin image: cannot reopen the "
                                          "staged store")};
    }
    std::vector<embedded_blob> blob_data;
    blob_data.reserve(table->entries.size());
    for (const auto &entry : table->entries) {
        if ((entry.flags & blob_flags_in_segment) != 0U) {
            continue;
        }
        const auto path{store->path_for(entry.digest)};
        auto bytes{io::read_file(path)};
        if (!bytes) {
            return std::unexpected{wrap_error(
                std::move(bytes.error()),
                "builtin image: cannot read the staged blob '" + path + "'")};
        }
        if (bytes->size() != entry.stored_size) {
            return std::unexpected{
                parse_error("builtin image: staged blob '" + path +
                            "' does not match the table stored size")};
        }
        blob_data.push_back(embedded_blob{
            .digest = entry.digest,
            .bytes = std::move(*bytes),
        });
    }

    return serialize_image(*table_bytes, *index_bytes, segment_data, blob_data);
}

} // namespace refrain::data
