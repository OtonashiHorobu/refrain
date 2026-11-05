// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// RWF v1 containers.
//
// * `region_file` — 32×32 chunk region: 4 KiB header, two 36 KiB index slots
//   (A/B), an append-only data area. Writes land at the tail; `commit()`
//   flushes the data, writes the inactive index slot and switches the header,
//   which is the only durable commit point. Recovery picks the newest valid
//   slot, so a crash after the slot write but before the header switch loses
//   nothing.
// * `read_record_file`/`write_record_file` — single-record files (`meta.rfw`,
//   `blobs/**`) with the same checksum/compression discipline and atomic
//   replace.
// * `blob_store` — key-addressed view over the object store (blobs/).
// * `check_region`/`compact_region` — `world fsck` and garbage collection.
//
// All integers little-endian; every checksum is CRC32C. Payloads are bounded
// by `max_record_bytes` before any allocation. Handles are single-threaded.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/world/model.hpp>
#include <refrain/world/object_store.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::world::rwf {

/// RWF container version written by this build.
inline constexpr std::uint16_t container_version = 1;

/// Region file geometry.
inline constexpr std::uint32_t sector_size = 4096;
inline constexpr std::uint32_t index_slots = 1024;
inline constexpr std::int32_t region_edge = 32;
inline constexpr std::size_t index_slot_bytes = 36864;
inline constexpr std::size_t index_a_offset = 4096;
inline constexpr std::size_t index_b_offset = index_a_offset + index_slot_bytes;
inline constexpr std::size_t data_offset = index_b_offset + index_slot_bytes;
inline constexpr std::size_t record_header_bytes = 32;
inline constexpr std::size_t record_file_header_bytes = 4096;

/// Maximum uncompressed chunk payload. Vanilla 26.3 chunks are typically well
/// below 1 MiB; the bound keeps a corrupt index from driving a huge allocation.
inline constexpr std::size_t max_record_bytes = 4ULL * 1024ULL * 1024ULL;

/// Hard upper bound for a region file (header + A/B indexes + data area).
/// fsck/first-open reject larger files before scanning so a crafted sparse
/// region cannot drive an unbounded linear scan.
inline constexpr std::size_t max_region_file_bytes =
    4ULL * 1024ULL * 1024ULL * 1024ULL;

/// A zstd dictionary plus its stable 64-bit id (BLAKE3 prefix). Dictionary
/// bytes are opaque; region headers store the id so a wrong/missing dictionary
/// fails closed.
struct zstd_dictionary {
    std::vector<std::byte> bytes;
    std::uint64_t id{0};
};

/// Build a dictionary value and its BLAKE3-prefix id.
[[nodiscard]] auto make_zstd_dictionary(std::span<const std::byte> bytes)
    -> zstd_dictionary;

/// Container-level output compression.
enum class compression : std::uint8_t {
    none = 0,
    zstd = 1,
};

/// zstd level bounds for RWF payload compression. `region_file::write` takes
/// an explicit per-record level and rejects values outside the range with
/// `invalid_argument` before any bytes are appended.
inline constexpr int min_zstd_level = 1;
inline constexpr int max_zstd_level = 19;
inline constexpr int default_zstd_level = 3;

[[nodiscard]] constexpr auto is_valid_zstd_level(int level) noexcept -> bool {
    return level >= min_zstd_level && level <= max_zstd_level;
}

/// Per-region configuration. `x`/`z` must match the file's header (a mismatch
/// means the file name and content disagree → `parse_error`).
struct region_options {
    std::int32_t x{0};
    std::int32_t z{0};
    compression codec{compression::zstd};
    int zstd_level{default_zstd_level};
    /// Optional zstd dictionary; its id is written to the region header.
    /// Non-owning: the caller keeps it alive for the region handle's lifetime.
    const zstd_dictionary *dictionary{nullptr};
};

/// One 32-byte index entry.
struct region_entry {
    std::uint64_t offset{0};
    std::uint32_t length{0};
    std::uint32_t crc32c{0};
    std::uint32_t uncompressed{0};
    compression codec{compression::none};
    std::uint8_t flags{0};
    std::uint64_t generation{0};

    [[nodiscard]] auto empty() const noexcept -> bool { return offset == 0; }
};

struct region_stats {
    std::size_t chunks{0};
    std::uint64_t file_bytes{0};
    std::uint64_t live_bytes{0};
    std::uint64_t garbage_bytes{0};
    std::uint64_t data_tail{data_offset};
    std::uint64_t generation{0};
};

class region_file {
  public:
    region_file() noexcept;
    ~region_file();

    region_file(region_file &&other) noexcept;
    auto operator=(region_file &&other) noexcept -> region_file &;

    region_file(const region_file &) = delete;
    auto operator=(const region_file &) -> region_file & = delete;

    /// Create a new region (exclusive). Fails with `already_exists` otherwise.
    [[nodiscard]] static auto create(std::string_view path,
                                     region_options options)
        -> result<region_file>;

    /// Load an existing region and recover the newest valid index slot.
    [[nodiscard]] static auto load(std::string_view path,
                                   region_options options)
        -> result<region_file>;

    /// `load`, or `create` when the file does not exist.
    [[nodiscard]] static auto load_or_create(std::string_view path,
                                             region_options options)
        -> result<region_file>;

    [[nodiscard]] auto valid() const noexcept -> bool;
    [[nodiscard]] auto path() const -> std::string_view;

    /// Read one chunk payload (uncompressed TLV fields). Fails with `not_found`
    /// for an empty slot, `parse_error` on checksum/structure corruption.
    [[nodiscard]] auto read(const chunk_pos &pos)
        -> result<std::vector<std::byte>>;

    /// Append one chunk record and mark the index dirty. The payload is
    /// compressed according to the region options (zstd is skipped when it does
    /// not shrink the payload). Durable only after `commit()`.
    [[nodiscard]] auto write(const chunk_pos &pos, chunk_status state,
                             std::span<const std::byte> payload) -> status;

    /// Same as above with an explicit zstd level in
    /// [`min_zstd_level`, `max_zstd_level`] for this record only. The region
    /// codec and dictionary still apply. Invalid levels fail with
    /// `invalid_argument` before anything is appended.
    [[nodiscard]] auto write(const chunk_pos &pos, chunk_status state,
                             std::span<const std::byte> payload, int zstd_level)
        -> status;

    [[nodiscard]] auto contains(const chunk_pos &pos) const -> bool;

    /// Group commit: flush data, write the inactive index slot, switch the
    /// header, flush again. A no-op when nothing changed.
    [[nodiscard]] auto commit() -> status;

    [[nodiscard]] auto stats() -> result<region_stats>;

    /// Index entries in slot order (1024).
    [[nodiscard]] auto entries() const -> std::span<const region_entry>;

    [[nodiscard]] static auto slot_for(const chunk_pos &pos) -> std::uint32_t;

    /// Rewrite the region file with only live records. The current handle is
    /// reopened afterwards; returns the compacted statistics.
    [[nodiscard]] auto compact() -> result<region_stats>;

    /// Commit (best effort) and close the handle.
    [[nodiscard]] auto close() -> status;

  private:
    struct impl;
    explicit region_file(std::unique_ptr<impl> state);

    std::unique_ptr<impl> impl_;
};

/// Result of `check_region`.
struct region_check {
    std::size_t chunks{0};
    std::uint64_t file_bytes{0};
    std::uint64_t live_bytes{0};
    std::uint64_t garbage_bytes{0};
    bool header_valid{false};
    bool index_valid{false};
    bool repaired{false};
    std::vector<std::string> problems;
};

/// Validate a region file, optionally rebuilding its index from the data area
/// (drops unreadable records, keeps the newest record per slot).
[[nodiscard]] auto check_region(std::string_view path, region_options options,
                                bool repair) -> result<region_check>;

/// Read and verify a single-record file (`meta.rfw`, `blobs/**`).
[[nodiscard]] auto read_record_file(std::string_view path,
                                    std::size_t max_bytes = max_record_bytes)
    -> result<std::vector<std::byte>>;

/// Atomically write a single-record file (temporary sibling + fsync + rename +
/// directory sync). `durable == false` writes the complete file without the
/// fsync/rename dance (bulk import into a fresh world, where an interrupted
/// run is simply restarted; the caller syncs directories once at the end).
[[nodiscard]] auto write_record_file(std::string_view path,
                                     std::span<const std::byte> payload,
                                     compression codec = compression::zstd,
                                     int zstd_level = default_zstd_level,
                                     bool durable = true) -> status;

/// Key-addressed view over an `object_store` (`blobs/`): path = `kind/key`.
///
/// `write(..., durable = false)` batches; `commit()` seals one segment and
/// publishes the index. Reads see pending (uncommitted) payloads. The object
/// store implements the RFOS index + RFSG segment/commit protocol.
class blob_store {
  public:
    explicit blob_store(std::string root);
    ~blob_store();

    blob_store(const blob_store &) = delete;
    auto operator=(const blob_store &) -> blob_store & = delete;
    blob_store(blob_store &&other) noexcept;
    auto operator=(blob_store &&other) noexcept -> blob_store &;

    [[nodiscard]] auto root() const -> const std::string &;

    [[nodiscard]] auto write(std::string_view kind, std::string_view key,
                             std::span<const std::byte> payload,
                             bool durable = true) -> status;

    [[nodiscard]] auto read(std::string_view kind, std::string_view key,
                            std::size_t max_bytes = max_object_bytes)
        -> result<std::vector<std::byte>>;

    [[nodiscard]] auto erase(std::string_view kind, std::string_view key)
        -> status;

    [[nodiscard]] auto exists(std::string_view kind, std::string_view key) const
        -> bool;

    /// Keys in `kind`, sorted. A missing kind directory is an empty list.
    [[nodiscard]] auto list(std::string_view kind) const
        -> result<std::vector<std::string>>;

    /// Seal every pending segment and publish the affected kind indexes.
    /// A no-op when nothing is pending. Called by `world_store::commit()`.
    [[nodiscard]] auto commit() -> status;

    /// Compact the underlying object store (drop overwritten/erased blobs).
    [[nodiscard]] auto compact() -> result<object_store_stats>;

    /// True for non-empty `[A-Za-z0-9._-]{1,96}` without a leading dot and
    /// without `..`. Keys become index paths, so this is the path-traversal
    /// barrier of the blob store.
    [[nodiscard]] static auto valid_key(std::string_view key) -> bool;

    /// Same rule with a shorter bound (64), used for kind directories.
    [[nodiscard]] static auto valid_kind(std::string_view kind) -> bool;

  private:
    object_store store_;
};

} // namespace refrain::world::rwf
