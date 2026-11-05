// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/platform/mmap.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::data {

// RFS1 — Refrain Store container.
//
// A read-only, deterministic, memory-mappable record file used for parsed
// data-layer results (registry dumps, compiled datapack records,
// protocol blobs, worldgen programs, ...). Records are keyed by an opaque UTF-8
// string; the file is content-addressed in the cache layout
// `<cache_root>/refrain/<engine_version>/...` and validated
// exhaustively on open so a corrupt or forged file is rejected before any
// record is returned (failure => `error_code::parse_error`).
//
// Layout (all integers little-endian):
//
//   header (128 bytes)
//      0  magic "RFS1" (4 bytes)
//      4  format_version u16 = 1
//      6  flags u16 (reserved, must be 0 in v1)
//      8  schema_hash[16] (caller-provided opaque bytes; record semantics key)
//     24  record_count u32
//     28  (reserved u32, zero)
//     32  total_size u64 (exact whole-file size)
//     40  data_hash[32] = BLAKE3(bytes[128 .. total_size)) — record table, key
//         arena, alignment padding and data region; the stable file identity.
//     72  (reserved, 56 bytes of zero)
//
//   record table (128 + record_count * 32, sorted by key bytes ascending)
//      0  key_hash u64 = first 8 bytes of BLAKE3(key), little-endian
//      8  key_off u32 (offset into the key arena; canonical layout: equal to
//         the sum of the previous records' key_len)
//     12  key_len u16 (1 .. 4096)
//     14  flags u16 (bit 0 = payload is zstd-compressed; other bits reserved=0)
//     16  data_off u64 (absolute file offset of the stored payload)
//     24  data_len u64 (uncompressed payload size)
//
//   Note: the record fields below add up to 8+4+2+2+8+8 = 32 bytes, and this
//   implementation keeps exactly those fields in natural alignment, so the
//   stride is 32 bytes. The reader validates the stride; a future format
//   revision may compact it (that would change format_version).
//
//   key arena (immediately after the record table)
//      concatenated key bytes, no terminators and no padding between keys.
//
//   zero padding to the next 64-byte boundary, then the data region: the
//   stored payloads, in record (sorted key) order. Uncompressed records are
//   stored verbatim in `data_len` bytes; a compressed record stores a zstd
//   frame whose uncompressed size is `data_len`. Because the payloads are
//   contiguous and in sorted order, a compressed record's stored size is
//   `next.data_off - data_off` (the file end for the last record); there is no
//   separate stored-length field. Per-record compression is optional and
//   only used when it actually shrinks the payload; records below the
//   compression threshold (1 KiB by default) are always stored verbatim.
//
// Determinism: records sorted by key bytes, canonical key arena, zeroed
// padding, no timestamps and fixed zstd parameters, so rebuilding the same
// records with the same zstd version yields byte-identical files. `data_hash`
// is the stable cache identity; two builds with the same records always agree
// on it (the header — including `schema_hash` — is not part of the hash).
//
// Hostile input: the header/table are validated structurally (bounds,
// alignment, sorted + unique keys, canonical layout, non-overlapping data
// ranges, exact total size) before the BLAKE3 body hash is compared; per-record
// and total uncompressed sizes are bounded so a forged table cannot trigger an
// unbounded decompression allocation (zip-bomb defence, same policy as the
// content store).

inline constexpr std::uint16_t rfs_format_version{1};
inline constexpr std::size_t rfs_header_bytes{128};
inline constexpr std::size_t rfs_record_bytes{32};
inline constexpr std::size_t rfs_data_alignment{64};
inline constexpr std::uint32_t rfs_max_records{1U << 20U}; // 1,048,576
inline constexpr std::size_t rfs_max_key_bytes{4096};

/// Practical per-record ceiling (4 GiB, the same bound as one content blob); a
/// forged table may not make `read()` allocate more than this for one record.
inline constexpr std::uint64_t rfs_max_record_bytes{4ULL * 1024ULL * 1024ULL *
                                                    1024ULL};

/// Bound for the declared sum of all uncompressed record sizes. The per-record
/// cap alone would still allow ~1M x 4 GiB; this bounds a forged index up
/// front.
inline constexpr std::uint64_t rfs_max_total_uncompressed_bytes{
    8ULL * 1024ULL * 1024ULL * 1024ULL};

/// Record flag: the stored payload is a zstd frame (`data_len` is the
/// uncompressed size). All other bits are reserved and rejected by the reader.
inline constexpr std::uint16_t rfs_flag_compressed{0x0001U};

/// Opaque 16-byte schema identity carried by the container (caller-provided;
/// e.g. a schema/format fingerprint). The reader returns it verbatim and never
/// interprets it.
using rfs_schema_hash = std::array<std::byte, 16>;

/// One record to be written. `key` is UTF-8 (sorted/compared as raw bytes) and
/// must be unique and non-empty; `data` is borrowed for the call only.
struct rfs_input {
    std::string key{};
    std::span<const std::byte> data{};
};

struct rfs_write_options {
    /// Compress records with zstd when it saves space. Only records at or above
    /// `compression_threshold` are candidates (small records compress badly).
    bool compress{true};
    /// Minimum payload size that is considered for compression.
    std::uint64_t compression_threshold{1024};
    /// zstd level in [1, 19]; the fixed default keeps the output deterministic.
    std::int32_t zstd_level{3};
};

/// One record of an opened container.
struct rfs_entry {
    std::string key{};
    std::uint64_t data_offset{
        0};                      ///< absolute file offset of the stored payload
    std::uint64_t data_len{0};   ///< uncompressed payload size
    std::uint64_t stored_len{0}; ///< on-disk payload size
    std::uint16_t flags{0};

    [[nodiscard]] auto is_compressed() const noexcept -> bool {
        return (flags & rfs_flag_compressed) != 0U;
    }
};

/// Build an RFS1 container in memory (deterministic; records are sorted by
/// key).
///
/// Fails with `invalid_argument` for empty/duplicate/oversized keys, oversized
/// payloads, bad compression options or an archive that cannot be laid out
/// within the format's 64-bit fields; with `io_error` on allocation failure.
/// Input order is irrelevant: the produced bytes do not depend on it.
[[nodiscard]] auto build_rfs(std::span<const rfs_input> records,
                             const rfs_schema_hash &schema_hash,
                             const rfs_write_options &options = {})
    -> result<std::vector<std::byte>>;

/// Build + atomically write an RFS1 container to `path`
/// (core::io::atomic_write_file: tmp + fsync + rename).
[[nodiscard]] auto write_rfs(std::string_view path,
                             std::span<const rfs_input> records,
                             const rfs_schema_hash &schema_hash,
                             const rfs_write_options &options = {}) -> status;

/// Read-only view of an RFS1 container.
///
/// `open()` borrows the caller's buffer (no copy; the buffer must outlive the
/// reader — e.g. a memory-mapped RFS file). `open_file()` loads the file
/// into an owning buffer, and the reader keeps that buffer alive across
/// copies. The reader is cheap to copy/move and thread-safe for concurrent
/// reads; all validation happens in `open()`.
class rfs_reader {
  public:
    rfs_reader() = default;

    /// Validate + index a borrowed container buffer.
    [[nodiscard]] static auto open(std::span<const std::byte> bytes)
        -> result<rfs_reader>;

    /// Map a container file read-only and index it (zero-copy; the mapping
    /// stays alive inside the reader).
    [[nodiscard]] static auto open_file(std::string_view path)
        -> result<rfs_reader>;

    [[nodiscard]] auto valid() const noexcept -> bool {
        return !entries_.empty() || header_valid_;
    }

    /// Records sorted by key bytes (the deterministic order used by the
    /// writer).
    [[nodiscard]] auto entries() const noexcept -> std::span<const rfs_entry> {
        return entries_;
    }

    /// Lookup by exact key; nullptr when absent.
    [[nodiscard]] auto find(std::string_view key) const noexcept
        -> const rfs_entry *;

    [[nodiscard]] auto contains(std::string_view key) const noexcept -> bool {
        return find(key) != nullptr;
    }

    /// Zero-copy view of an uncompressed record's payload. Compressed records
    /// must go through `read()` (they cannot be exposed without decompressing).
    /// The view points into the reader's buffer and is valid as long as the
    /// buffer / reader is (for `open_file()`, as long as any reader copy
    /// lives).
    [[nodiscard]] auto view(const rfs_entry &entry) const
        -> result<std::span<const std::byte>>;
    [[nodiscard]] auto view(std::string_view key) const
        -> result<std::span<const std::byte>>;

    /// Payload as owned bytes; decompresses zstd records (bounded by the size
    /// validated in `open()`).
    [[nodiscard]] auto read(const rfs_entry &entry) const
        -> result<std::vector<std::byte>>;
    [[nodiscard]] auto read(std::string_view key) const
        -> result<std::vector<std::byte>>;

    /// Caller-provided schema identity, verbatim.
    [[nodiscard]] auto schema_hash() const noexcept -> const rfs_schema_hash & {
        return schema_hash_;
    }

    /// BLAKE3 over the file body (record table + key arena + data region).
    [[nodiscard]] auto data_hash() const noexcept -> const digest32 & {
        return data_hash_;
    }

    [[nodiscard]] auto file_count() const noexcept -> std::size_t {
        return entries_.size();
    }

    /// Total bytes of the container file.
    [[nodiscard]] auto file_size() const noexcept -> std::uint64_t {
        return file_size_;
    }

  private:
    /// Keeps an mmap-backed `open_file()` alive while `bytes_` points into it.
    std::shared_ptr<const platform::mapped_file> mapping_{};
    std::span<const std::byte> bytes_{};
    std::vector<rfs_entry> entries_{};
    rfs_schema_hash schema_hash_{};
    digest32 data_hash_{};
    std::uint64_t file_size_{0};
    bool header_valid_{false};
};

} // namespace refrain::data
