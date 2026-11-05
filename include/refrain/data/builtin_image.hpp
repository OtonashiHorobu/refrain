// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/content_store.hpp>
#include <refrain/data/vfs.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::data {

// Built-in content image (RFSI v1) and its deterministic generator.
//
// The image layout is defined by this project rather than borrowed from a
// third party, so the hand-written builder/reader is deliberate: no archive
// reader is involved.
//
// The built-in mod tree ships inside the executable as one immutable byte
// image: a fixed header, a segment directory, a blob directory, the complete
// RFBT (`blobs.idx`) bytes, the complete built-in RFRX (`<composition>.idx`)
// bytes, the sealed RFSG segments and the standalone blob files. Nothing is
// written to disk at run time and no archive is decoded: `open()` validates
// the image once and the resulting `builtin_image` is a borrowed, immutable
// view over the caller's bytes.
//
// Image layout (all little-endian, sections in this order):
//
//   header 128 B
//   segment directory: segment_count × 24 B { id u64 | offset u64 | size u64 }
//   blob directory:    blob_count × 48 B { digest[32] | offset u64 |
//                                          size u32 | reserved u32 }
//   RFBT bytes, RFRX bytes, sealed RFSG files, standalone blob files
//
// A segment directory offset points at the complete `.rfseg` file (RFSG magic
// first; `size == 80 + data_bytes`). A blob directory offset points at a
// standalone raw/packed blob file exactly as `content/blobs/<hh>/<digest>`
// would hold it. The table, the index and every segment/blob live inside the
// image, are 8-byte aligned (the writer uses 64-byte section alignment) and
// never overlap.
//
// Header (128 B):
//
//   offset size field
//        0    4 magic "RFSI"
//        4    2 version = 1
//        6    2 flags = 0
//        8    8 table_offset
//       16    8 table_bytes   (complete `blobs.idx`, 80-byte entries)
//       24    8 index_offset
//       32    8 index_bytes   (complete built-in RFRX, 32-byte entries)
//       40    8 segment_dir_offset
//       48    4 segment_count
//       52    4 reserved (0)
//       56    8 blob_dir_offset
//       64    4 blob_count
//       68    4 reserved (0)
//       72   32 file_blake3   (over [128, size))
//      104    4 header_crc32c (over [0, 104))
//      108   20 reserved (0)
//
// `open()` is fail-closed (`parse_error`; a foreign version is
// `version_mismatch`): header magic/version/flags/reserved bytes and CRC32C,
// the file BLAKE3, every region's bounds/alignment/non-overlap, the RFBT and
// RFRX structure plus their BLAKE3 prefix binding, unique segment ids and blobs
// digests, every in-segment RFBT record resolved to a segment directory entry,
// every standalone record resolved to a blob directory entry, and each sealed
// segment's RFSG header and body BLAKE3. Blobs are decoded lazily.
//
// `read()` resolves the RFRX winner of the path, maps it through the RFBT to a
// `blob_location`, slices the image (segment body or blob file) and decodes it
// with the shared `content_store` raw/packed decoder: a raw blob is verified
// against its digest and a packed blob has every block decoded and checked
// against the logical digest.
//
// The class only borrows the byte span: the caller must keep the image alive
// for the lifetime of the `builtin_image` and of every pack returned by
// `open_builtin_image_pack()` (same contract as `open_memory_pack()`). A
// `builtin_image` is immutable and safe to read from multiple threads.

/// Image magic at offset 0.
inline constexpr std::string_view builtin_image_magic{"RFSI"};

/// Image format version (`format_version = 1` until the format is frozen).
inline constexpr std::uint16_t builtin_image_format_version{1};

/// Fixed header size; section and data offsets are relative to the image start.
inline constexpr std::size_t builtin_image_header_bytes{128};

/// Segment directory entry: `id u64 | offset u64 | size u64`.
inline constexpr std::size_t builtin_image_segment_directory_entry_bytes{24};

/// Blob directory entry: `digest[32] | offset u64 | size u32 | reserved u32`.
inline constexpr std::size_t builtin_image_blob_directory_entry_bytes{48};

/// Minimum alignment of every table/index/segment/blob region.
inline constexpr std::size_t builtin_image_min_alignment{8};

/// Alignment the builder uses for section and blob/segment starts.
inline constexpr std::size_t builtin_image_section_alignment{64};

/// One file fed to `build_builtin_image`: virtual path (safe relative UTF-8,
/// at most `max_resolution_location_bytes`) and its complete logical bytes.
struct builtin_image_input {
    std::string path{};
    std::vector<std::byte> data{};
};

/// Immutable, borrowed view over one RFSI v1 image.
class builtin_image {
  public:
    /// Validate and open `bytes`. The image is not copied; every read slices
    /// the caller's span. Fails with `parse_error` for any structural or
    /// integrity problem and `version_mismatch` for a foreign format version.
    [[nodiscard]] static auto open(std::span<const std::byte> bytes)
        -> result<builtin_image>;

    /// Number of distinct virtual paths (RFRX winners).
    [[nodiscard]] auto file_count() const noexcept -> std::size_t;

    /// Winners sorted by path bytes ascending (`size` is the RFBT logical size
    /// of the winning blob).
    [[nodiscard]] auto entries() const noexcept
        -> std::span<const pack_file_info>;

    /// Lookup by exact virtual path; nullptr when the image has no such file.
    [[nodiscard]] auto find(std::string_view path) const noexcept
        -> const pack_file_info *;

    /// Read one file: RFRX winner -> RFBT location -> segment body or blob
    /// file bytes -> raw digest check / packed block decode. `not_found` for
    /// an unknown path, `parse_error` for damaged image data.
    [[nodiscard]] auto read(std::string_view path) const
        -> result<std::vector<std::byte>>;

    /// `file_blake3` of the image (BLAKE3 over `[128, size)`), stable for the
    /// same logical input.
    [[nodiscard]] auto digest() const noexcept -> const digest32 &;

  private:
    struct segment_record {
        std::uint64_t id{0};
        std::uint64_t data_bytes{0};
        std::span<const std::byte> bytes{};
    };

    struct blob_record {
        digest32 digest{};
        std::span<const std::byte> bytes{};
    };

    builtin_image() = default;

    [[nodiscard]] auto find_segment(std::uint64_t id) const noexcept
        -> const segment_record *;
    [[nodiscard]] auto find_blob(const digest32 &digest) const noexcept
        -> const blob_record *;

    std::span<const std::byte> image_{};
    digest32 digest_{};
    /// Sorted by segment id.
    std::vector<segment_record> segments_{};
    /// Sorted by digest.
    std::vector<blob_record> blobs_{};
    /// Sorted by path; `locations_[i]` is the physical blob of `entries_[i]`.
    std::vector<pack_file_info> entries_{};
    std::vector<blob_location> locations_{};
};

/// Open one RFSI v1 image as a read-only VFS pack (`pack_format::memory`).
///
/// The pack borrows `bytes` exactly like `open_memory_pack()`: the buffer must
/// outlive the pack and every `vfs` it is mounted into. `label` is diagnostic
/// only and defaults to `"builtin.image"` when empty. `read()` maps the same
/// `not_found`/`parse_error` codes as `builtin_image::read()`.
[[nodiscard]] auto open_builtin_image_pack(std::span<const std::byte> bytes,
                                           std::string_view label)
    -> result<std::unique_ptr<pack>>;

/// Build one deterministic RFSI v1 image from the built-in mod files.
///
/// Inputs are validated (`invalid_argument` for an empty/unsafe/non-UTF-8 path
/// above 1024 bytes or a duplicate path) and sorted by path. The files are
/// staged through `content_store::batch` in a private temporary directory
/// (`core::io::temp_directory()` + a unique suffix, removed on every exit
/// path), so the usual layout rule applies: `<= 64 KiB` (including zero-byte
/// files) goes to a content segment, larger files become standalone packed
/// blobs. The RFBT carries `kind = builtin`, `refcount` = number of image
/// locations per blob; the RFRX uses `source = "builtin"`, `priority = 0`.
///
/// Determinism: input path order and content alone decide the table/index
/// bytes, the segment ids and the directory order (segments by id, blobs by
/// digest), no timestamp or temporary path is embedded, so the same input
/// always yields the same bytes. An empty input yields a valid empty image
/// (`file_count() == 0`; `vfs::mount()` still rejects empty packs, which is the
/// caller's semantic decision).
[[nodiscard]] auto
build_builtin_image(std::span<const builtin_image_input> files)
    -> result<std::vector<std::byte>>;

} // namespace refrain::data
