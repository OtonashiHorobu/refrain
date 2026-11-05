// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::data {

// Read-only layered content VFS.
//
// A `vfs` is an immutable, priority-ordered stack of packs: vanilla jar data
// (zip), world/server datapacks (directory or zip), compositions of the
// content-addressed store (RFBT/RFRX, mounted through `open_content_pack()`),
// and the embedded built-in content image (RFSI v1, memory). Later mounts have
// *higher* priority:
// when two packs contain the same virtual path the winner is the pack mounted
// last, and `list*`/`find` expose that merged view. Mount calls happen during
// boot; after `freeze()` the snapshot is read-only and the read API is safe to
// call from multiple threads (all returned data is owned copies; no pointers
// into pack storage escape except `find()`'s pack handle identity).
//
// Virtual paths are UTF-8 archive-relative strings validated by
// <refrain/data/path.hpp>: '/'-only, no leading '/', no '\', no control bytes,
// no empty/"."/".." segments. Unsafe or duplicate-within-one-pack entry paths
// make a pack open fail; cross-pack duplicates are the normal override
// mechanism (highest priority wins).

/// Storage container backing one mounted pack.
///
/// The numeric values are part of the diagnostic/API surface, so no value is
/// ever reused for a different format.
enum class pack_format : std::uint8_t {
    directory = 0, ///< a directory tree (development/world datapacks)
    zip = 1,       ///< a vanilla jar / resource pack / zip datapack
    // 2 is reserved for a retired pack format and is never reused.
    memory = 3,  ///< an RFSI image borrowed from caller-owned memory (built-in)
    content = 4, ///< one composition of the content-addressed store (RFBT/RFRX)
    rfs = 5,     ///< an RFS1 container produced by `data cook`
};

/// Description of a pack to mount. `label` is diagnostic only (empty = derive
/// from `path`); it never participates in path resolution.
struct pack_source {
    std::string path{};
    pack_format format{pack_format::directory};
    std::string label{};
};

/// One file of the merged view (path is the archive-relative virtual path).
struct pack_file_info {
    std::string path{};
    std::uint64_t size{0};
};

/// Upper bound for the number of files in one pack and in a whole mounted
/// stack. Together with the per-entry size caps it bounds memory on hostile
/// input (zip-bomb / giant-index defence).
inline constexpr std::size_t vfs_max_files{4U * 1024U * 1024U};

/// Immutable, move-only view over one read-only container.
///
/// Implementations are created through the `open_*_pack()` factories (or a
/// custom subclass for tests/plugins). Paths passed to `find()`/`read()` are
/// virtual paths; implementations reject unsafe paths and unknown files with
/// error values instead of throwing.
class pack {
  public:
    pack() = default;
    pack(const pack &) = delete;
    auto operator=(const pack &) -> pack & = delete;
    pack(pack &&) noexcept = default;
    auto operator=(pack &&) noexcept -> pack & = default;
    virtual ~pack() = default;

    [[nodiscard]] virtual auto format() const noexcept -> pack_format = 0;

    /// Diagnostic name shown in `vfs::mounted()`/logs.
    [[nodiscard]] virtual auto label() const noexcept -> std::string_view = 0;

    [[nodiscard]] virtual auto file_count() const noexcept -> std::size_t = 0;

    /// Entries sorted by `path` bytes ascending (deterministic, stable order).
    [[nodiscard]] virtual auto entries() const noexcept
        -> std::span<const pack_file_info> = 0;

    /// Lookup by exact virtual path; nullptr when the pack has no such file.
    [[nodiscard]] virtual auto find(std::string_view path) const noexcept
        -> const pack_file_info * = 0;

    /// Read one file. Returns `not_found` for unknown paths, `parse_error` for
    /// corrupt container data and `io_error` for host filesystem failures.
    [[nodiscard]] virtual auto read(std::string_view path) const
        -> result<std::vector<std::byte>> = 0;

    /// Convenience wrapper around `read()` (UTF-8 text; NUL bytes are kept).
    [[nodiscard]] auto read_text(std::string_view path) const
        -> result<std::string>;
};

/// Open a directory tree as a pack. Entries are discovered recursively; the
/// virtual path is the path relative to `root` with '/' separators. Symlinks
/// are skipped (they could escape the root and make the snapshot ambiguous).
/// `label` defaults to `root`.
[[nodiscard]] auto open_dir_pack(std::string_view root,
                                 std::string_view label = {})
    -> result<std::unique_ptr<pack>>;

/// Open a zip/jar as a pack (libzip-backed). Directory entries are ignored;
/// names are validated with the portable-name rules of
/// `refrain::archive::safe_relative_path()`, so zip-slip and Windows
/// device names are rejected before any read. `label` defaults to `path`.
[[nodiscard]] auto open_zip_pack(std::string_view path,
                                 std::string_view label = {})
    -> result<std::unique_ptr<pack>>;

/// Open an RFS1 container (the output of `refrain-tools data cook`) as a pack.
/// Record keys are virtual paths; every key is validated with the same safe
/// relative-path rules as zip/dir packs.
[[nodiscard]] auto open_rfs_pack(std::string_view path,
                                 std::string_view label = {})
    -> result<std::unique_ptr<pack>>;

/// One mounted pack, for diagnostics.
struct mounted_pack_info {
    std::string label{};
    pack_format format{pack_format::directory};
    std::size_t file_count{0};
};

/// Immutable (after freeze) layered pack stack. Copies share the same snapshot:
/// mounting/freezing through one copy is visible through the others.
///
/// Thread safety: mount()/freeze() must not race with any other call. After
/// `freeze()` all query/read methods are safe from multiple threads; the merged
/// index is never mutated again, and packs are immutable. `find()` returns the
/// winning `pack*`, whose lifetime is the vfs snapshot's lifetime.
class vfs {
  public:
    vfs();
    vfs(const vfs &) = default;
    auto operator=(const vfs &) -> vfs & = default;
    vfs(vfs &&) noexcept = default;
    auto operator=(vfs &&) noexcept -> vfs & = default;
    ~vfs() = default;

    /// Mount a pack on top of the current stack (highest priority = last one).
    ///
    /// Rejects (all with `invalid_argument`): frozen stack, null pack, an empty
    /// pack, a pack whose entries are unsorted/duplicated/unsafe, and a stack
    /// that would exceed `vfs_max_files`. The pack is not consumed on failure.
    [[nodiscard]] auto mount(std::unique_ptr<pack> source) -> status;

    /// Open `source` through the matching `open_*_pack()` factory and mount it.
    /// `pack_format::memory` cannot be opened from a path (it needs caller
    /// bytes: `data::open_builtin_image_pack()`), and `pack_format::content`
    /// is not a single path (it needs a store root plus a composition): mount
    /// both through their factories + `mount(unique_ptr)`.
    [[nodiscard]] auto mount(const pack_source &source) -> status;

    [[nodiscard]] auto pack_count() const noexcept -> std::size_t;

    /// Number of distinct virtual paths in the merged view (after overrides).
    [[nodiscard]] auto file_count() const noexcept -> std::size_t;

    [[nodiscard]] auto mounted() const noexcept
        -> std::span<const mounted_pack_info>;

    /// Individual layers in mount order (lowest priority first, highest last).
    ///
    /// Layer-aware consumers need the physical stack rather than the merged
    /// view: datapack tag merging (`datapack_set::load(vfs)`) must read every
    /// contributing layer, including entries hidden by a higher-priority pack.
    /// The returned pointers stay valid for the lifetime of this snapshot (and
    /// all copies sharing it).
    [[nodiscard]] auto packs() const -> std::vector<const pack *>;

    /// Winning pack for `path`, or nullptr when no pack contains it.
    [[nodiscard]] auto find(std::string_view path) const noexcept
        -> const pack *;

    [[nodiscard]] auto exists(std::string_view path) const noexcept -> bool;

    [[nodiscard]] auto size(std::string_view path) const
        -> result<std::uint64_t>;

    /// Read from the highest-priority pack that contains `path`.
    [[nodiscard]] auto read(std::string_view path) const
        -> result<std::vector<std::byte>>;

    [[nodiscard]] auto read_text(std::string_view path) const
        -> result<std::string>;

    /// Merged view: distinct paths sorted ascending, sizes from the winner.
    [[nodiscard]] auto list() const noexcept
        -> const std::vector<pack_file_info> &;

    /// Merged entries whose path starts with `prefix` (sorted ascending).
    [[nodiscard]] auto list_prefix(std::string_view prefix) const
        -> std::vector<pack_file_info>;

    /// Merged direct children of `directory` ("" = stack root): file names
    /// without suffix and directory names with a trailing '/', sorted,
    /// de-duplicated. Unsafe `directory` input yields an empty result.
    [[nodiscard]] auto list_directory(std::string_view directory) const
        -> std::vector<std::string>;

    /// Seal the snapshot: from now on mount() fails with `invalid_argument`
    /// and the read API is thread-safe. Idempotent.
    void freeze();

    [[nodiscard]] auto is_frozen() const noexcept -> bool;

  private:
    struct impl;
    std::shared_ptr<impl> impl_;
};

} // namespace refrain::data
