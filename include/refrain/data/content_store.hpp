// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/content_index.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::data {

// Content-addressed BLAKE3 blob store (CAS).
//
// The packed-blob layout is defined by this project rather than borrowed from
// a third party, so the hand-written codec is deliberate.
//
// Every blob is stored under `<root>/content/blobs/<hash[0:2]>/<hash>` where
// `<hash>` is the lowercase 64-character hex encoding of the logical file's
// BLAKE3 digest. The layout is shared by the merged mod-content store and the
// RFS content area, so identical files from different mods or versions are
// deduplicated automatically and can be mounted read-only across
// server/client/tools.
//
// Physical layout is deterministic:
//  - `logical_size <= 64 KiB` -> raw: the file bytes are the logical bytes;
//  - `logical_size > 64 KiB` -> packed: a 96-byte `RFBL` header, a 48-byte
//    block table and 64 KiB blocks (each independently zstd level 3, stored
//    verbatim when compression does not save space), every block start 64-byte
//    aligned. The file digest is always BLAKE3(logical bytes), never the
//    physical file bytes.
//
// Writes publish atomically through core::io::atomic_write_file (temporary
// sibling + fsync + rename), so a reader either sees the previous blob or the
// complete new one, never a torn file; concurrent publishers of the same
// content race harmlessly because both write identical bytes and the rename is
// atomic. `put()` deduplicates: if the blob already exists it is verified
// (raw: BLAKE3 of the file; packed: every block decoded and BLAKE3 of the
// logical bytes) and reused, and a corrupted existing blob is reported
// (`parse_error`) instead of being silently overwritten. `read()` re-verifies
// the logical digest on every access; `read_range()` checks the structure and
// block boundaries but skips the per-block hashes.
//
// Removing unreferenced blobs lives in `data/content_maintenance` (mark-sweep
// against the global blob table + resolution indexes).
//
// Small blobs written through `put()` still get one raw file each (the legacy
// CAS behaviour). `batch` instead appends every small raw blob to an immutable
// content segment (content_segment.hpp). Such a blob is recorded in RFBT as a
// `(segment_id, segment_offset)` pair and is readable only through
// `read(const blob_location&)`, never through the digest-only overloads.
// Packed blobs (> 64 KiB) keep one file per blob.
//
// No OS API is used here: all filesystem access goes through
// <refrain/core/io.hpp>.

/// Relative directory under the store root that holds the CAS, shared with the
/// cache layout. Exposed for diagnostics/tests.
inline constexpr std::string_view content_store_blobs_directory{
    "content/blobs"};

/// Physical layout of a stored blob.
enum class blob_kind : std::uint8_t {
    raw,    ///< File bytes == logical bytes (logical size <= 64 KiB).
    packed, ///< RFBL header + block table + 64 KiB zstd/raw blocks.
};

/// Structural description of a blob, as reported by `content_store::layout`.
struct blob_layout {
    blob_kind kind{blob_kind::raw};
    /// Block size in bytes: 0 for raw blobs, 65536 for packed blobs.
    std::uint32_t block_size{0};
    /// Number of blocks: 0 for raw blobs, the RFBL block count otherwise.
    std::uint32_t block_count{0};
    /// Uncompressed logical size (the digest covers exactly these bytes).
    std::uint64_t logical_size{0};
    /// True for packed blobs whose blocks are all stored verbatim (RFBT
    /// `blob_flags_all_raw_blocks`); always false for raw blobs.
    bool all_raw_blocks{false};
};

/// Where one blob physically lives, as carried by an RFBT record.
///
/// `in_segment == false` means the blob has its own
/// `content/blobs/<hh>/<digest>` file. `in_segment == true` means its bytes are
/// at `[segment_offset, segment_offset + stored_size)` in the body of the
/// sealed segment `<store>/content/segments/<16-hex>.rfseg` named by
/// `segment_id`; `stored_size` is 0 only for a zero-byte raw blob.
struct blob_location {
    digest32 digest{};
    blob_kind kind{blob_kind::raw};
    std::uint32_t block_count{0};
    std::uint64_t logical_size{0};
    std::uint32_t stored_size{0};
    bool in_segment{false};
    std::uint64_t segment_id{0};
    std::uint32_t segment_offset{0};
};

/// Construct a `blob_location` from a validated RFBT record (one-to-one field
/// mapping; useful to content_pack and to tests that publish a batch).
[[nodiscard]] auto blob_location_from_table_entry(const blob_table_entry &entry)
    -> blob_location;

/// Decode stored blob bytes (raw logical bytes, or one complete RFBL file) and
/// verify them against `location`: the logical BLAKE3 digest and the recorded
/// logical size. It applies the same structural checks and raw/packed decoder
/// as `read(const blob_location&)`; the builtin image reader calls it for blobs
/// held in one borrowed byte image instead of `content/blobs/<hh>/<digest>`.
///
/// `invalid_argument` when `location` is structurally impossible (the same
/// checks `read(const blob_location&)` applies), `parse_error` when the bytes
/// cannot be decoded or do not match the digest/size.
[[nodiscard]] auto decode_stored_blob(std::span<const std::byte> stored,
                                      const blob_location &location)
    -> result<std::vector<std::byte>>;

/// Encode logical bytes as a deterministic RFBL packed blob (96 B header +
/// 64 KiB block table + blocks). Used by RWF large objects; the content store
/// itself uses the same serializer.
[[nodiscard]] auto encode_packed_blob(std::span<const std::byte> data)
    -> result<std::vector<std::byte>>;

/// Decode a complete RFBL file, validating its header/table and the logical
/// BLAKE3 digest carried by the header.
[[nodiscard]] auto decode_packed_blob(std::span<const std::byte> stored)
    -> result<std::vector<std::byte>>;

/// New-blob record produced by a committed `batch`; feed it straight into a
/// `blob_table_entry` (`digest`, `logical_size`, `stored_size`, `flags`,
/// `segment_id`, `segment_offset`) when publishing the RFBT.
struct blob_metadata {
    digest32 digest{};
    std::uint64_t logical_size{0};
    std::uint32_t stored_size{0};
    /// RFBT flags: `blob_flags_packed`, `blob_flags_all_raw_blocks` and/or
    /// `blob_flags_in_segment`.
    std::uint32_t flags{0};
    std::uint64_t segment_id{0};
    std::uint32_t segment_offset{0};
};

class segment_reader;

/// Read-path caches of a content store. Both are runtime
/// state only: they never change the on-disk format (`format_version = 1`) and
/// are shared by copies of one `content_store`.
struct content_store_options {
    /// Total byte budget of the decompressed packed-block LRU. 0 (the default)
    /// disables the block cache.
    std::size_t block_cache_bytes{0};
    /// Number of mmap-backed segment readers kept open. 0 disables the segment
    /// cache; small (non-mapped) segment readers are never cached.
    std::size_t segment_cache_slots{8};
};

class content_store {
  public:
    content_store() = default;

    /// Create/open a store rooted at `root`: creates `<root>/content/blobs`
    /// (recursively) and returns the handle. Fails with `invalid_argument` for
    /// an empty root and `io_error` when the directories cannot be created.
    ///
    /// Copies of a handle share the store root and the bounded read caches;
    /// both caches are internally synchronized, so concurrent reads through
    /// copies (or through one handle) are safe.
    [[nodiscard]] static auto open(std::string_view root)
        -> result<content_store>;

    /// `open(root)` with explicit cache options.
    [[nodiscard]] static auto open(std::string_view root,
                                   const content_store_options &options)
        -> result<content_store>;

    /// Open an *existing* store without creating anything on disk.
    ///
    /// Read-only diagnostics (`refrain-tools content where`) must not
    /// materialize `<root>/content/blobs` just by looking: this handle has the
    /// same read API as `open()`, but instead of creating the directory it
    /// reports `io_error` when `root` is missing, is not a directory or cannot
    /// be listed. An empty root is `invalid_argument`.
    [[nodiscard]] static auto open_existing(std::string_view root)
        -> result<content_store>;

    /// True when this handle references a store root (default-constructed
    /// handles are inert and every operation fails).
    [[nodiscard]] auto valid() const noexcept -> bool { return !root_.empty(); }

    /// Caller-provided store root (without the `content/blobs` suffix).
    [[nodiscard]] auto root() const noexcept -> std::string_view {
        return root_;
    }

    /// Absolute/relative `<root>/content/blobs` directory (diagnostics).
    [[nodiscard]] auto blobs_root() const -> std::string;

    /// Store `data` under its BLAKE3 digest and return the digest. Blobs up to
    /// 64 KiB are stored raw, larger ones packed; the choice is purely a
    /// function of `data.size()`, so a given input always produces the same
    /// physical blob within this format version. If the blob
    /// already exists it is verified and the same digest is returned (dedupe);
    /// an existing blob whose content does not match its name is `parse_error`
    /// and is left untouched (GC/repair decides what to do with it).
    [[nodiscard]] auto put(std::span<const std::byte> data) const
        -> result<digest32>;

    /// Read `path` and store its bytes (same semantics as `put`).
    [[nodiscard]] auto put_file(std::string_view path) const
        -> result<digest32>;

    /// True when a blob with this digest exists (existence only; `read()`
    /// verifies the content).
    [[nodiscard]] auto contains(const digest32 &digest) const -> bool;

    /// Load a blob from its own file and verify its logical BLAKE3 digest.
    ///
    /// This is the legacy "one file per blob" path: raw blobs are returned
    /// as-is and packed blobs are decoded (all blocks). A blob that lives in a
    /// content segment has no such file, so this overload reports `not_found`
    /// with a hint to use `read(const blob_location&)` together with the RFBT
    /// record. A damaged file is `parse_error`.
    [[nodiscard]] auto read(const digest32 &digest) const
        -> result<std::vector<std::byte>>;

    /// Load a blob through its RFBT location: file-backed blobs behave like
    /// `read(digest)`, in-segment blobs read `stored_size` bytes at
    /// `segment_offset` from the sealed segment and decode/verify them (raw
    /// bytes are checked against `digest`; packed bytes are decoded by RFBL).
    /// A missing segment/blob file is `io_error`; damage (bad segment body,
    /// wrong digest or metadata size mismatch) is `parse_error`.
    [[nodiscard]] auto read(const blob_location &location) const
        -> result<std::vector<std::byte>>;

    /// Physical layout of an existing blob. Header/table integrity, block
    /// boundaries and every block hash are verified (`parse_error` on damage);
    /// raw blobs are checked against their digest. Missing blobs surface the
    /// underlying `io_error`.
    [[nodiscard]] auto layout(const digest32 &digest) const
        -> result<blob_layout>;

    /// Copy the logical byte range starting at `offset` into `out`, decoding
    /// only the packed blocks that overlap the range. Requires
    /// `offset <= logical_size` and `out.size() <= logical_size - offset`
    /// (otherwise `invalid_argument`) and returns the number of bytes copied
    /// (== `out.size()`, or 0 for an empty range at the end). Block hashes are
    /// not checked to keep the path cheap; structure, offsets, sizes and
    /// decompressed sizes are checked (`parse_error` on damage). Raw blobs are
    /// read whole and sliced (their digest is verified).
    [[nodiscard]] auto read_range(const digest32 &digest, std::uint64_t offset,
                                  std::span<std::byte> out) const
        -> result<std::size_t>;

    /// `read_range` through a `blob_location` (same range contract). In-segment
    /// raw blobs are read straight from the segment body; in-segment packed
    /// blobs decode only the covered blocks, like the file-backed path.
    [[nodiscard]] auto read_range(const blob_location &location,
                                  std::uint64_t offset,
                                  std::span<std::byte> out) const
        -> result<std::size_t>;

    /// Filesystem path of a blob: `<root>/content/blobs/<hh>/<hex>`.
    [[nodiscard]] auto path_for(const digest32 &digest) const -> std::string;

  private:
    /// Shared, bounded read caches (defined in content_store.cpp).
    struct cache_state;

    /// Cached or one-off reader for the sealed segment `segment_id`. Mapped
    /// readers are inserted into the shared cache (re-checked under the lock
    /// because opening happens outside it); ifstream readers are returned as a
    /// one-off handle because they are not safe for concurrent reads. Errors
    /// carry the `content segment '<path>': ...` prefix.
    [[nodiscard]] static auto open_segment(const content_store &store,
                                           std::uint64_t segment_id)
        -> result<std::shared_ptr<const segment_reader>>;

    /// Read `size` bytes at `offset` from the body of an in-segment blob,
    /// through the shared segment reader cache.
    [[nodiscard]] auto read_segment_bytes(const blob_location &location,
                                          std::uint32_t offset,
                                          std::uint32_t size) const
        -> result<std::vector<std::byte>>;

    /// Directory that holds the blob file (without the trailing file name).
    [[nodiscard]] auto blob_directory(const digest32 &digest) const
        -> std::string;

    std::string root_{};
    std::shared_ptr<cache_state> cache_{};
};

/// Staged multi-blob writer for one content-store publication.
///
/// `begin()` opens the store, snapshots the digests of `existing` (nullable,
/// usually a parsed RFBT) and creates `<store>/content/segments` lazily.
/// `put()`/`put_file()` classify by the deterministic size rule:
///
///  * <= 64 KiB (including zero-byte content) -> appended to the current
///    segment staging file (8-byte aligned, no fsync). When the segment is
///    about to exceed `segment_target_bytes` it is sealed and a new one starts,
///    so one batch can publish several segments;
///  * > 64 KiB -> the legacy one-file-per-blob `content_store::put()` path
///    (packed, atomically published, already durable).
///
/// Content already present in `existing` (any kind) or seen earlier in this
/// batch is not written again: the same digest is returned and `new_blobs()`
/// gets no duplicate record. `new_blobs()` is the authoritative list of blobs
/// this batch created; feed each record into a `blob_table_entry` and publish
/// RFBT/RFRX after `commit()`.
///
/// `commit()` seals the last segment (publishing it under its content hash) and
/// marks the batch immutable. Intermediate segments sealed while filling are
/// already visible; aborting a batch (destructor without `commit()`) removes
/// the open `.part` staging file but does not roll back segments that were
/// published for earlier chunks (append-only store; GC reclaims unreferenced
/// ones). A failed write leaves the batch unusable; `commit()` reports the
/// error instead of publishing a partial state.
class batch {
  public:
    batch() noexcept;
    batch(const batch &) = delete;
    auto operator=(const batch &) -> batch & = delete;
    batch(batch &&other) noexcept;
    auto operator=(batch &&other) noexcept -> batch &;
    ~batch();

    /// Start a batch. `store_root` must be non-empty; `existing` may be null;
    /// `segment_target_bytes` is a soft segment-size hint and must be non-zero.
    [[nodiscard]] static auto
    begin(std::string_view store_root, const blob_table_view *existing,
          std::uint32_t segment_target_bytes = 32U * 1024U * 1024U)
        -> result<batch>;

    [[nodiscard]] auto valid() const noexcept -> bool;
    [[nodiscard]] auto root() const noexcept -> std::string_view;

    /// Store `data`, deduplicated by content digest, and return its digest.
    [[nodiscard]] auto put(std::span<const std::byte> data) -> result<digest32>;
    /// Read `path` and store its bytes (same semantics as `put`); a missing
    /// source file is `io_error` and writes nothing.
    [[nodiscard]] auto put_file(std::string_view path) -> result<digest32>;

    /// Seal the current segment and make `new_blobs()` final. Idempotence is
    /// intentionally not allowed: a second call is `invalid_argument`.
    [[nodiscard]] auto commit() -> status;

    /// Blobs created by this batch (only meaningful after a successful
    /// `commit()`; before that, in-segment records may still lack their
    /// `segment_id`).
    [[nodiscard]] auto new_blobs() const -> std::span<const blob_metadata>;

  private:
    struct impl;
    explicit batch(std::unique_ptr<impl> state) noexcept;

    std::unique_ptr<impl> impl_{};
};

} // namespace refrain::data
