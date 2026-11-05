// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/data/checksum.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::data {

// TOC double index of the content-addressed store:
//
// Both index layouts are defined by this project rather than borrowed from a
// third party, so the hand-written codecs are deliberate.
//
//  * `blobs.idx` (RFBT) is the global blob table: one 80-byte record per
//    logical blob, sorted by digest, carrying physical metadata (including the
//    content-segment location), the reference count and the content kind. It
//    never maps locations to blobs.
//  * `<composition>.idx` (RFRX) is one resolution index per composition: the
//    `location -> blob_index` map with source and priority, sorted by
//    `(location_hash, priority desc, source_index, path)`. `resolve()` picks
//    the winner deterministically (highest priority, then smallest
//    `blob_index`, then smallest source), so legacy indexes whose entries tie
//    on priority still do not depend on file order.
//    `blob_index` addresses the digest-sorted RFBT table; the RFRX header also
//    stores the first 8 bytes of BLAKE3(blobs.idx) so a stale index/table pair
//    fails closed instead of resolving to the wrong blob.
//
// Both formats are little-endian, have a 128-byte header with a CRC32C over
// the header prefix and a BLAKE3 over everything after the header, and are
// byte-deterministic functions of their logical input (no timestamps, no
// composition id, no host endianness). Builders validate their input and
// report `invalid_argument`; parsers are fail-closed and report `parse_error`
// (or `version_mismatch` for a foreign format version). The builders return
// `result` rather than a bare byte vector because the project is
// exception-free and duplicate/conflicting entries must surface as a value.
//
// Whole indexes are kept in memory through `parse_blob_table()` /
// `parse_resolution_index()`. `mapped_blob_table` and
// `mapped_resolution_index` below expose the same validated bytes straight
// from a read-only `platform::mapped_file`, decoding each record by value on
// access; the wire format is unchanged.

/// Relative names under `<store>/content`; shared by tools and tests.
inline constexpr std::string_view content_blob_table_filename{"blobs.idx"};
inline constexpr std::string_view content_resolutions_directory{
    "content/resolutions"};

/// Fixed on-disk sizes.
inline constexpr std::size_t blob_table_header_bytes{128};
inline constexpr std::size_t blob_table_entry_bytes{80};
inline constexpr std::size_t resolution_index_header_bytes{128};
inline constexpr std::size_t resolution_index_entry_bytes{32};

/// RFBT entry `flags` bits. The remaining bits are reserved and must be zero.
inline constexpr std::uint32_t blob_flags_packed{0x0001};
inline constexpr std::uint32_t blob_flags_all_raw_blocks{0x0002};
/// The blob bytes live in a content segment instead of their own
/// `content/blobs/<hh>/<digest>` file. Such a blob is addressed
/// through `content_store::read(const blob_location&)`, not by digest alone.
inline constexpr std::uint32_t blob_flags_in_segment{0x0004};

/// RFBT entry `kind` values.
inline constexpr std::uint16_t blob_kind_content{0};
inline constexpr std::uint16_t blob_kind_cook{1};
inline constexpr std::uint16_t blob_kind_world{2};
inline constexpr std::uint16_t blob_kind_builtin{3};

/// RFRX per-entry string limits.
inline constexpr std::size_t max_resolution_location_bytes{1024};
inline constexpr std::size_t max_resolution_source_bytes{256};

/// One record of the global blob table (`<store>/content/blobs.idx`).
///
/// `logical_size`/`flags`/`block_count`/`stored_size` describe the physical
/// blob (raw or packed), `refcount` is the number of references from all
/// published resolution indexes, and `kind` is one of the `blob_kind_*`
/// values. Defaults describe a zero-byte raw content blob.
///
/// When `flags & blob_flags_in_segment` is set, `segment_id` names the sealed
/// `<store>/content/segments/<16-hex>.rfseg` file and `segment_offset` is the
/// blob's start relative to the segment body (8-byte aligned). A zero-byte raw
/// blob is the only in-segment record allowed to have `stored_size == 0`;
/// otherwise both segment fields must be zero.
struct blob_table_entry {
    digest32 digest{};
    std::uint64_t logical_size{0};
    std::uint32_t flags{0};
    std::uint32_t refcount{0};
    std::uint16_t kind{0};
    std::uint32_t block_count{0};
    /// Physical blob size in bytes (`logical_size` for raw blobs).
    std::uint32_t stored_size{0};
    /// Sealed content-segment id (`0` when the blob has its own file).
    std::uint64_t segment_id{0};
    /// Blob start relative to the segment body (`0` when not in a segment).
    std::uint32_t segment_offset{0};
};

/// Parsed view of a global blob table. `entries` points into storage owned by
/// the view (copied out of the input bytes), so the view is copyable and the
/// span stays valid for the view's lifetime. Records are sorted by digest and
/// unique. `blob_table_blake3_prefix` is the first 8 bytes of BLAKE3 over the
/// entire `blobs.idx` file and is what an RFRX index binds itself to.
struct blob_table_view {
    blob_table_view() = default;
    blob_table_view(const blob_table_view &other) : storage_{other.storage_} {
        entries = storage_;
        blob_table_blake3_prefix = other.blob_table_blake3_prefix;
    }
    auto operator=(const blob_table_view &other) -> blob_table_view & {
        if (this != &other) {
            storage_ = other.storage_;
            entries = storage_;
            blob_table_blake3_prefix = other.blob_table_blake3_prefix;
        }
        return *this;
    }
    blob_table_view(blob_table_view &&other) noexcept
        : storage_{std::move(other.storage_)} {
        entries = storage_;
        blob_table_blake3_prefix = other.blob_table_blake3_prefix;
    }
    auto operator=(blob_table_view &&other) noexcept -> blob_table_view & {
        if (this != &other) {
            storage_ = std::move(other.storage_);
            entries = storage_;
            blob_table_blake3_prefix = other.blob_table_blake3_prefix;
        }
        return *this;
    }
    ~blob_table_view() = default;

    std::span<const blob_table_entry> entries{};
    std::array<std::byte, 8> blob_table_blake3_prefix{};

  private:
    std::vector<blob_table_entry> storage_{};

    friend auto parse_blob_table(std::span<const std::byte> bytes)
        -> result<blob_table_view>;
};

/// Read-only mmap-backed view of a global blob table
/// (`<store>/content/blobs.idx`, RFBT v1).
///
/// `open()` maps the whole file with `platform::mapped_file` and runs the same
/// fail-closed validation as `parse_blob_table()` (magic/version/flags, entry
/// size, offsets/counts/bounds, reserved header and record bytes, header
/// CRC32C, file BLAKE3, per-record metadata rules and strictly increasing,
/// unique digests) before returning. Records are little-endian decoded on
/// access and returned by value: no pointer or reference into the mapping is
/// ever exposed. The mapping is owned by the view, which is move-only.
///
/// The view is immutable after `open()`, so its const accessors may be called
/// from several threads at once.
class mapped_blob_table {
  public:
    mapped_blob_table() noexcept;
    mapped_blob_table(const mapped_blob_table &) = delete;
    auto operator=(const mapped_blob_table &) -> mapped_blob_table & = delete;
    mapped_blob_table(mapped_blob_table &&other) noexcept;
    auto operator=(mapped_blob_table &&other) noexcept -> mapped_blob_table &;
    ~mapped_blob_table();

    /// Map and fully validate `path`. An empty path is `invalid_argument`; a
    /// missing, unreadable, non-regular or unmappable file keeps the
    /// `platform::mapped_file` error (`io_error`); a truncated, corrupt or
    /// foreign table is `parse_error` (`version_mismatch` for an unsupported
    /// format version), exactly like `parse_blob_table()`.
    [[nodiscard]] static auto open(std::string_view path)
        -> result<mapped_blob_table>;

    /// True when this handle owns a successfully validated mapping.
    [[nodiscard]] auto valid() const noexcept -> bool;

    /// Number of records in the table.
    [[nodiscard]] auto size() const noexcept -> std::size_t;

    /// Record at `index`. Precondition: `index < size()`. A violation trips an
    /// assertion in debug builds and yields a default record in release
    /// builds; callers that cannot prove the bound should use `find()`.
    [[nodiscard]] auto at(std::size_t index) const -> blob_table_entry;

    /// Record whose digest is `digest`, or `std::nullopt` when absent. Binary
    /// search over the digest-sorted table, decoding each probed record.
    [[nodiscard]] auto find(const digest32 &digest) const
        -> std::optional<blob_table_entry>;

    /// First 8 bytes of BLAKE3 over the entire mapped file — the value an
    /// RFRX index binds itself to.
    [[nodiscard]] auto blake3_prefix() const noexcept
        -> std::array<std::byte, 8>;

  private:
    struct impl;
    explicit mapped_blob_table(std::unique_ptr<impl> state) noexcept;

    std::unique_ptr<impl> impl_{};
};

/// Serialize a global blob table. Entries are validated (reserved flags/fields
/// zero, known kind, packed/block-count/stored-size consistency and the
/// in-segment field rules) and sorted by digest; duplicate digests are
/// `invalid_argument`. The result is deterministic for the same entries
/// (independent of input order).
[[nodiscard]] auto build_blob_table(std::span<const blob_table_entry> entries)
    -> result<std::vector<std::byte>>;

/// Parse and fully validate a global blob table: magic/version/flags/entry
/// size/reserved fields, header CRC32C, file BLAKE3, record count/offsets,
/// per-record reserved bits and metadata (including the in-segment id/offset
/// fields), and strictly increasing unique digests.
[[nodiscard]] auto parse_blob_table(std::span<const std::byte> bytes)
    -> result<blob_table_view>;

/// One `location -> blob digest` candidate fed to `build_resolution_index`.
///
/// `location` is a safe `'/'`-separated UTF-8 path (at most 1024 bytes),
/// `source` a non-empty UTF-8 source id (at most 256 bytes), `priority` the
/// override priority (higher wins), and `flags` is reserved (must be zero).
struct resolution_input {
    std::string location{};
    std::string source{};
    std::int32_t priority{0};
    digest32 blob{};
    std::uint16_t flags{0};
};

/// One parsed record of a resolution index. `blob_index` addresses the
/// digest-sorted global blob table; use `blob_digest()` to recover the digest.
struct resolution_entry {
    std::string location{};
    std::string source{};
    std::int32_t priority{0};
    std::uint32_t blob_index{0};
    std::uint16_t flags{0};
};

/// Parsed resolution index. Entries keep the on-disk order and own their
/// strings.
struct resolution_index_view {
    std::vector<resolution_entry> entries{};
};

/// `resolve` result: `winner` is the highest-priority entry for the location
/// (ties broken by smallest `blob_index`, then smallest source) and `fallback`
/// the remaining entries in `(priority desc, blob_index asc, source asc)`
/// order. Both point into the owning `resolution_index_view`.
struct resolution {
    const resolution_entry *winner{nullptr};
    std::vector<const resolution_entry *> fallback{};
};

/// Read-only mmap-backed view of a per-composition resolution index
/// (`<store>/content/resolutions/<composition>.idx`, RFRX v1).
///
/// `open()` maps the whole file with `platform::mapped_file` and runs the same
/// fail-closed validation as `parse_resolution_index()` against the supplied
/// blob table: header magic/version/flags/reserved bytes, counts/offsets/arena
/// bounds, the `blob_table_blake3_prefix` binding, header CRC32C, file BLAKE3,
/// sorted unique source and location tables, per-entry reserved bits,
/// path/source references, recomputed location hashes, in-range `blob_index`
/// values and the canonical entry order (legacy same-primary-key ties allowed,
/// exact duplicates rejected). Nothing is skipped or deferred to access time.
///
/// Records are decoded on demand and returned by value; location/source strings
/// are copied out of the mapped arena, so `at()`/`find()` never expose a
/// pointer into the mapping. The mapping is owned by the move-only view. The
/// view is immutable after `open()`, so its const accessors may be called from
/// several threads at once.
class mapped_resolution_index {
  public:
    mapped_resolution_index() noexcept;
    mapped_resolution_index(const mapped_resolution_index &) = delete;
    auto operator=(const mapped_resolution_index &)
        -> mapped_resolution_index & = delete;
    mapped_resolution_index(mapped_resolution_index &&other) noexcept;
    auto operator=(mapped_resolution_index &&other) noexcept
        -> mapped_resolution_index &;
    ~mapped_resolution_index();

    /// Map and validate `path` against a parsed `blob_table` (the prefix and
    /// record count bind the index to that exact table). An empty path is
    /// `invalid_argument`; file-level failures keep the
    /// `platform::mapped_file` error; a truncated/corrupt index, a stale table
    /// binding or a table that cannot satisfy a `blob_index` is `parse_error`
    /// (`version_mismatch` for an unsupported format version), exactly like
    /// `parse_resolution_index()`.
    [[nodiscard]] static auto open(std::string_view path,
                                   const blob_table_view &blob_table)
        -> result<mapped_resolution_index>;

    /// Same, against an already mapped blob table (avoids decoding the table
    /// twice when both indexes are opened from disk). An invalid/default
    /// `blob_table` is `invalid_argument`.
    [[nodiscard]] static auto open(std::string_view path,
                                   const mapped_blob_table &blob_table)
        -> result<mapped_resolution_index>;

    /// True when this handle owns a successfully validated mapping.
    [[nodiscard]] auto valid() const noexcept -> bool;

    /// Number of records in the index.
    [[nodiscard]] auto size() const noexcept -> std::size_t;

    /// Record at `index` in on-disk order. Precondition: `index < size()`. A
    /// violation trips an assertion in debug builds and yields a default
    /// record in release builds; callers that cannot prove the bound should
    /// use `find()`.
    [[nodiscard]] auto at(std::size_t index) const -> resolution_entry;

    /// Every entry for `location`, in winner order (`resolution_precedes()`:
    /// highest priority, then smallest `blob_index`, then smallest `source`);
    /// `front()` is therefore the same winner `resolve()` selects. Empty when
    /// the location is absent. The lookup hashes `location`, binary-searches
    /// the `location_hash` range and compares the arena path bytes exactly.
    [[nodiscard]] auto find(std::string_view location) const
        -> std::vector<resolution_entry>;

    /// Every entry whose `location_hash` is `location_hash`, in on-disk
    /// (canonical) order. A hash collision between different locations is
    /// reported as one group; use the string overload for an exact lookup.
    [[nodiscard]] auto find(std::uint64_t location_hash) const
        -> std::vector<resolution_entry>;

    /// The 8-byte `blob_table_blake3_prefix` stored in the header, already
    /// verified to equal the bound table's prefix at `open()`.
    [[nodiscard]] auto blob_table_prefix() const noexcept
        -> std::array<std::byte, 8>;

  private:
    static auto open_bound(std::string_view path,
                           const std::array<std::byte, 8> &table_prefix,
                           std::size_t blob_count)
        -> result<mapped_resolution_index>;

    struct impl;
    explicit mapped_resolution_index(std::unique_ptr<impl> state) noexcept;

    std::unique_ptr<impl> impl_{};
};

/// True when `candidate` outranks `current` for the same location: higher
/// priority, then smaller blob_index, then lexicographically smaller source.
/// Single winner order shared by resolve(), content_pack, content_stream and
/// the builtin image.
[[nodiscard]] auto resolution_precedes(const resolution_entry &candidate,
                                       const resolution_entry &current) noexcept
    -> bool;

/// Serialize a resolution index bound to `blob_table`. Every input digest must
/// be present in the table (otherwise `invalid_argument`) because it is
/// stored as a positional index. Validates locations (safe relative path,
/// UTF-8, non-empty, length limits) and sources, rejects *any* two entries with
/// the same `(location, priority)` (regardless of source or blob; conflicting
/// sources are named in the error), then sorts entries by
/// `(BLAKE3(location)[0:8] LE, priority desc, source_index, path)` and builds
/// the source/location arena. There is no same-priority fallback chain: layers
/// must give every source a distinct priority (the startup path derives it from
/// the gamepack `[mods].list` layer order). No composition id is stored, so the
/// same input and table always yield the same bytes.
[[nodiscard]] auto
build_resolution_index(std::span<const resolution_input> entries,
                       const blob_table_view &blob_table)
    -> result<std::vector<std::byte>>;

/// Parse and fully validate a resolution index against `blob_table`: header
/// magic/version/flags/reserved fields, the `blob_table_blake3_prefix` binding,
/// CRC32C, file BLAKE3, offsets/counts/arena bounds, sorted unique source and
/// location tables, per-entry reserved bits, path/source references,
/// recomputed location hashes, in-range `blob_index` values and the canonical
/// entry order. Entries that tie on the documented sort key may carry
/// different blobs and are accepted in either relative order: such indexes are
/// legacy/manual (`build_resolution_index` no longer creates them and
/// `resolve()` ignores their file order). An exact duplicate record is
/// `parse_error`.
[[nodiscard]] auto parse_resolution_index(std::span<const std::byte> bytes,
                                          const blob_table_view &blob_table)
    -> result<resolution_index_view>;

/// Resolve `location` against a parsed index. The winner is the highest
/// priority entry; ties are broken by smallest `blob_index`, then
/// lexicographically smallest `source` (independent of the on-disk order).
/// The remaining entries are fallbacks, sorted by
/// `(priority desc, blob_index asc, source asc)`. Returns `not_found` when the
/// location is absent.
[[nodiscard]] auto resolve(const resolution_index_view &index,
                           std::string_view location) -> result<resolution>;

/// Digest of a parsed record through the blob table. `invalid_argument` for an
/// out-of-range `blob_index` (a valid parse cannot produce one).
[[nodiscard]] auto blob_digest(const resolution_entry &entry,
                               const blob_table_view &blob_table)
    -> result<digest32>;

/// Digest of the winner of a `resolve()` result (`not_found` when the
/// resolution has no winner).
[[nodiscard]] auto resolved_blob(const resolution &resolved,
                                 const blob_table_view &blob_table)
    -> result<digest32>;

} // namespace refrain::data
