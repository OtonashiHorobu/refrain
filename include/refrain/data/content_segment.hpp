// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/data/checksum.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::data {

// Immutable content segments.
//
// This project defines the segment layout byte for byte, so the hand-written
// codec is deliberate.
//
// Small raw blobs (<= 64 KiB) do not get one file each: a batch appends them
// to a segment file and the RFBT entry carries `(segment_id, segment_offset)`
// instead of a per-blob path. A segment is a `<store>/content/segments/
// <16-hex>.rfseg` file made of an 80-byte header followed by the blob bytes:
//
//   offset size field
//        0    4 magic "RFSG"
//        4    2 version = 1
//        6    2 flags = 0
//        8    8 segment_id = BLAKE3(body)[0:8] interpreted little-endian
//       16    4 blob_count
//       20    4 reserved (0)
//       24    8 data_bytes (body length in bytes)
//       32   32 body_blake3 (BLAKE3 over exactly the body)
//       64    4 header_crc32c (over bytes [0, 64))
//       68   12 reserved (0)
//       80    - body: blobs concatenated in append order, every blob start
//                8-byte aligned (the gap is zero-filled). There is no TOC and
//                no footer: RFBT is the only directory of a segment's blobs.
//
// Blob offsets are relative to the body start (byte 80), which is itself
// 8-byte aligned, so a relative offset is also the absolute file offset minus
// 80. The segment id is derived from the body bytes alone, so sealing the same
// body twice produces the same id and a byte-identical file; a sealed segment
// is immutable and re-sealing an existing valid id is a no-op.
//
// Sealing writes a placeholder header, appends the body to a
// `segments/.tmp-<...>.part` staging file, reads it back, fills the header
// fields in memory and publishes the complete file atomically through
// core::io::atomic_write_file (fsync + rename), then removes the staging
// file. A segment with no appended blob is never published. The staging file
// is removed on failure/abort (`~segment_writer`) and after a successful seal.
//
// Header reads validate the structure (magic/version/flags/reserved bytes,
// header CRC32C, `data_bytes == file_size - 80`) but not the body hash;
// `verify_segment()`/`segment_reader::verify()` additionally hash the body and
// check caller-supplied blob spans (alignment, bounds, no overlap). Boundaries
// are supplied by the caller because a segment has no table of its own, and
// RFBT entries carry the authority.
//
// No OS API is used here: byte streams and staging-file cleanup use the C++
// standard library, and durable publication goes through <refrain/core/io.hpp>.

/// Relative directory under the store root that holds sealed segments.
inline constexpr std::string_view content_segment_directory{"content/segments"};

/// Bytes of the fixed segment header; the body starts right after it.
inline constexpr std::size_t content_segment_header_bytes{80};

/// Every blob start inside a segment body is aligned to this many bytes.
inline constexpr std::size_t content_segment_alignment{8};

/// File-name suffix of a sealed segment.
inline constexpr std::string_view content_segment_extension{".rfseg"};

/// Validated segment header.
struct segment_header {
    /// `BLAKE3(body)[0:8]` interpreted little-endian; also the file-name stem.
    std::uint64_t id{0};
    /// Number of blobs appended before sealing (0 would mean an empty segment,
    /// which is never published).
    std::uint32_t blob_count{0};
    /// Body length in bytes (the body is exactly this long).
    std::uint64_t data_bytes{0};
    /// BLAKE3 over exactly the body bytes.
    digest32 body_hash{};
};

/// One blob boundary supplied to `verify_segment()` (offset relative to the
/// body start).
struct segment_blob_span {
    std::uint32_t offset{0};
    std::uint32_t size{0};
};

/// Parse and validate a complete segment file (header + body). Validates the
/// size, magic, version, flags, reserved bytes, header CRC32C and
/// `data_bytes == bytes.size() - 80`, plus `blob_count != 0`; the body BLAKE3
/// is checked by `verify_segment()` (a header-only read cannot hash the body).
[[nodiscard]] auto read_segment_header(std::span<const std::byte> bytes)
    -> result<segment_header>;

/// Fully verify a segment file: `read_segment_header()` plus BLAKE3(body) and,
/// when `blobs` is non-empty, each supplied span. Spans must be 8-byte aligned,
/// inside the body, sorted by offset and non-overlapping. Every failure is
/// `parse_error` (structure/corruption), so a damaged segment is never
/// mistaken for a short one.
[[nodiscard]] auto verify_segment(std::span<const std::byte> bytes,
                                  std::span<const segment_blob_span> blobs = {})
    -> status;

/// `<store_root>/content/segments/<16-hex>.rfseg`. The id is rendered as 16
/// lowercase hexadecimal digits of the `uint64_t` value (most significant
/// nibble first), not as its little-endian byte spelling.
[[nodiscard]] auto segment_path(std::string_view store_root, std::uint64_t id)
    -> std::string;

/// Move-only staging writer for one segment (PIMPL; no exceptions).
///
/// `create()` makes the segments directory, creates the `.part` staging file
/// and writes the placeholder header. `append()` adds one blob and returns its
/// 8-byte-aligned body offset; the writer is responsible for the zero padding.
/// `seal()` finalizes the header and publishes `<id>.rfseg`. Destroying an
/// unsealed writer removes the `.part`; sealing an already-published id with a
/// different header is `parse_error` (re-sealing the same body is a no-op).
class segment_writer {
  public:
    segment_writer() noexcept;
    segment_writer(const segment_writer &) = delete;
    auto operator=(const segment_writer &) -> segment_writer & = delete;
    segment_writer(segment_writer &&other) noexcept;
    auto operator=(segment_writer &&other) noexcept -> segment_writer &;
    ~segment_writer();

    /// Start a segment under `segments_dir` (created recursively).
    /// `target_bytes` is the caller's soft size hint (stored for the batch
    /// logic); it must be non-zero. Fails with `invalid_argument` for an empty
    /// directory or a zero target and `io_error` when the staging file cannot
    /// be created.
    [[nodiscard]] static auto create(std::string_view segments_dir,
                                     std::uint32_t target_bytes)
        -> result<segment_writer>;

    /// True when this handle owns an unsealed staging file.
    [[nodiscard]] auto valid() const noexcept -> bool;

    /// `append()` offset ceiling: offsets and sizes are 32-bit in RFBT, so a
    /// segment body must stay below 4 GiB.
    [[nodiscard]] auto blob_count() const noexcept -> std::uint32_t;
    [[nodiscard]] auto data_bytes() const noexcept -> std::uint64_t;
    [[nodiscard]] auto target_bytes() const noexcept -> std::uint32_t;
    /// Staging-file path (diagnostics/tests).
    [[nodiscard]] auto path() const noexcept -> std::string_view;

    /// Append one blob, zero-padding up to the next 8-byte boundary first.
    /// Returns the blob's body offset. `invalid_argument` when the blob would
    /// push the body past the 32-bit offset range, `invalid_argument` after a
    /// seal and `io_error` on a write failure (the writer is then unusable).
    [[nodiscard]] auto append(std::span<const std::byte> bytes)
        -> result<std::uint32_t>;

    /// Finish the segment and publish it atomically. An empty segment (no
    /// `append()` calls) is `invalid_argument` and leaves nothing behind.
    [[nodiscard]] auto seal() -> result<segment_header>;

  private:
    struct impl;
    explicit segment_writer(std::unique_ptr<impl> state) noexcept;

    std::unique_ptr<impl> impl_{};
};

/// Read-only segment handle. `open()` validates the header and keeps the file
/// open; `read_at()` reads one range without re-reading the whole body.
///
/// A mapped handle (`mapped() == true`) is backed by an immutable
/// `platform::mapped_file`, so its const `read_at()`/`verify()` may be called
/// from several threads at once. The ifstream fallback is not internally
/// synchronized: use one handle per thread (or external locking) for
/// concurrent reads of a non-mapped handle.
class segment_reader {
  public:
    segment_reader() noexcept;
    segment_reader(const segment_reader &) = delete;
    auto operator=(const segment_reader &) -> segment_reader & = delete;
    segment_reader(segment_reader &&other) noexcept;
    auto operator=(segment_reader &&other) noexcept -> segment_reader &;
    ~segment_reader();

    /// Open and structurally validate `path`. An empty path is
    /// `invalid_argument`; a missing/unreadable file keeps `io_error`; a
    /// truncated or corrupt header is `parse_error` (foreign version is
    /// `version_mismatch`).
    [[nodiscard]] static auto open(std::string_view path)
        -> result<segment_reader>;

    [[nodiscard]] auto valid() const noexcept -> bool;

    /// True when this handle is backed by `platform::mapped_file` rather than
    /// the ifstream fallback. A mapped handle is safe for concurrent const
    /// reads (see the class comment); the mapping is immutable once open.
    [[nodiscard]] auto mapped() const noexcept -> bool;

    [[nodiscard]] auto header() const noexcept -> const segment_header &;
    [[nodiscard]] auto path() const noexcept -> std::string_view;

    /// Copy `size` bytes at `offset` (relative to the body start). Requires
    /// `offset <= data_bytes` and `size <= data_bytes - offset` (otherwise
    /// `invalid_argument`); a zero-size read at the end is valid. Host read
    /// failures keep `io_error`.
    [[nodiscard]] auto read_at(std::uint32_t offset, std::size_t size) const
        -> result<std::vector<std::byte>>;

    /// Re-read the file and run `verify_segment()` (body hash + spans).
    [[nodiscard]] auto
    verify(std::span<const segment_blob_span> blobs = {}) const -> status;

  private:
    struct impl;
    explicit segment_reader(std::unique_ptr<impl> state) noexcept;

    std::unique_ptr<impl> impl_{};
};

} // namespace refrain::data
