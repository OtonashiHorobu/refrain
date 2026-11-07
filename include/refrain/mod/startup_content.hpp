// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/mod/mod_ref.hpp>
#include <refrain/mod/rfmod.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::mod {

struct mod_lock;

// Server startup content bootstrap.
//
// One call runs the whole startup content pipeline against one content store,
// so `refrain-server` does not have to shell out to `refrain-tools`:
//
//   0  lock   `<store>/content/.lock` (directory + `owner`, stale reclaim)
//   1  scan   `<inbox>/*.rfmod` (sorted), parse each package manifest
//   2  install packages whose receipt is missing or names a different
//              archive digest (optional; `install_rfmod`, one batch each)
//   3  index  publish `<store>/content/resolutions/<composition>.idx` from
//              the installed receipts (`data::publish_resolution`; every
//              receipt `files[]` entry except the package `manifest.toml`,
//              which is metadata rather than a VFS location)
//   4  gc     mark-sweep with every receipt file as an extra root
//              (`data::collect_garbage`; off / dry-run / apply, with an
//              optional deletion-phase budget: deferred deletions stay on disk
//              and the next mark-sweep retries them)
//
// The CLI (`refrain-tools mods install` / `content build|gc`) remains the
// debugging/operations entry point; both share the same libraries.
//
// Failure policy: a bad package (unreadable archive, bad manifest, failed
// signature/hash verification) is counted and moved to `inbox/rejected/`, and
// the remaining packages are still processed. A corrupt *install receipt* is
// never skipped silently: receipts are loaded strictly before the index is
// published or GC runs, so a damaged receipt aborts with the offending path
// and leaves the existing index/table untouched (fail closed). A missing
// `blobs.idx` (a store that never published one) only skips GC.
//
// The store lock is a directory (atomic `std::filesystem::create_directory`)
// containing an `owner` file with the pid and start time. A lock is reclaimed
// when its owner pid is no longer alive, or — when no usable pid can be read —
// when the owner record is older than one hour (portable fallback); every
// reclaim is logged. A lock that cannot be created because the store is
// read-only is reported as `permission_denied`;
// in that case the caller may run with `auto_install = false` and a
// non-applying GC to scan/report without writing (the index is not published).

/// Options of `run_startup_content`.
struct startup_content_options {
    /// Content store root. Required; an empty value is `invalid_argument`
    /// (callers that have no store skip the call entirely).
    std::string store_root{};
    /// Directory scanned for `*.rfmod` packages. Empty selects
    /// `<store_root>/mods/inbox`.
    std::string inbox_dir{};
    /// Composition whose resolution index is published, e.g. `gamepack-<id>`.
    /// Required; same safe single path segment rules as `content build`
    /// (non-empty, at most 200 UTF-8 bytes, no '/', '\\' or control bytes).
    std::string composition{};
    /// Priority of the *highest* layer. Layer rank `r` (0-based, in
    /// `layer_order`) is published with `priority = base_priority - r`, so
    /// every selected layer gets a distinct priority and the lowest layer gets
    /// `base_priority - (selected_layers - 1)`. Must fit the RFRX `int32`
    /// field (<= INT32_MAX) and be >= the number of selected layers, otherwise
    /// `invalid_argument` (there must be room for every rank).
    ///
    /// `mods.lock` stays id-sorted and its fingerprint is unchanged: layer
    /// order is configuration, not lock identity. Reordering changes content
    /// winners but not the lock fingerprint.
    std::uint32_t base_priority{1000};
    /// Exact version pins: mod id -> installed version. Callers build this
    /// with `merge_pinned_versions()`: `mods.lock` is the baseline and explicit
    /// gamepack `[mods].list` entries override it; when auto_scan is disabled
    /// the caller passes a null lock and the gamepack list is the sole source.
    /// A pinned id that has receipts must use exactly this version (missing
    /// version is a startup error); ids without any receipt (builtin mods) are
    /// ignored. Unpinned ids fall back to highest origin then highest SemVer.
    std::map<std::string, std::string> pinned_versions{};
    /// Layer order, i.e. the priority list: the first entry has the highest
    /// priority. Entries are mod ids from the gamepack `[mods].list`
    /// declaration order. Selected receipts whose id is not listed are
    /// appended after the listed ones, sorted by id, with a warning; listed
    /// ids without an install receipt are warned and ignored (builtin mods are
    /// commonly declared in the gamepack but are not receipt layers); duplicate
    /// ids are `invalid_argument`. Builtin is not part of the receipt layer:
    /// the RFSI image is mounted separately and is always lowest in the VFS.
    std::vector<std::string> layer_order{};
    /// Install packages into the CAS. `false` skips only the install/quarantine
    /// step (nothing is moved or deleted): the pipeline still scans, still
    /// publishes the resolution index from the receipts when the store is
    /// writable, and still runs GC according to `gc`. Only the read-only
    /// downgrade skips index publication.
    bool auto_install{false};
    /// After a successful install move the package to `inbox/installed/`
    /// (collision-suffixed); `false` deletes it instead.
    bool keep_installed{true};
    /// Host-observed origin recorded by installs performed by this run
    /// (forwarded to `install_rfmod`; the inbox is user-supplied by default).
    mod_origin install_origin{mod_origin::user};
    /// Require a signature from one of `trusted_keys` (forwarded to
    /// `install_rfmod`).
    bool require_signature{false};
    /// Trust anchors matched by manifest `key_id`.
    std::vector<rfmod_trusted_key> trusted_keys{};
    /// Garbage-collection policy.
    enum class gc_mode { off, dry_run, apply };
    /// Default is a dry run: the startup path never deletes without an
    /// explicit `apply` (the server CLI defaults to `apply`).
    gc_mode gc{gc_mode::dry_run};
    /// Keep unreferenced blobs/segments newer than this many days (0 = no
    /// grace window).
    int gc_keep_days{7};
    /// Wall-time budget in milliseconds for the GC physical deletion phase
    /// (0 = unlimited, the library default). Forwarded to
    /// `data::gc_options::budget_ms`: planning/marking/re-encoding and the
    /// table/index publication are never bounded, only the unlinks after the
    /// new table is published. Deletions that do not fit are reported as
    /// deferred and retried by the next mark-sweep (the server has no periodic
    /// GC loop yet, so in practice that is the next startup).
    std::uint64_t gc_budget_ms{0};
    /// How long to wait for `<store>/content/.lock` before failing with
    /// `io_error` (0 = single attempt).
    int lock_timeout_ms{5000};
    /// Optional per-stage log sink. Each call receives one complete line
    /// without the trailing newline (same contract as
    /// `data::collect_garbage`); lines are stable, prefixed with
    /// `startup-content: ` and report outcomes or warnings, not per-file
    /// progress.
    std::function<void(std::string_view)> log{};
};

/// Merge content version pins from the two sources:
/// `mods.lock` is the baseline (the resolved composition), explicit gamepack
/// `[mods].list` entries override it for the same id. A null `lock` means the
/// baseline is absent (first boot) or auto_scan is disabled; in the latter
/// case the caller must not pass a lock at all, so the gamepack list is the
/// sole source. Overrides with an empty version are ignored (the gamepack
/// parser already validates non-empty refs). The returned map is sorted by id.
[[nodiscard]] auto
merge_pinned_versions(const mod_lock *lock,
                      std::span<const mod::mod_ref> overrides)
    -> std::map<std::string, std::string>;

/// Result of one `run_startup_content` call.
struct startup_content_report {
    /// `*.rfmod` files found in the inbox.
    std::size_t scanned{0};
    /// Packages installed by this run.
    std::size_t installed{0};
    /// Packages left untouched: already installed (receipt archive digest
    /// matches) or needing install while `auto_install` is false.
    std::size_t skipped{0};
    /// Packages that failed to open/parse/verify/install.
    std::size_t failed{0};
    /// `files[]` records read from every install receipt.
    std::size_t receipt_files{0};
    /// Entries written to the published resolution index.
    std::size_t published_entries{0};
    /// True when this run wrote `<composition>.idx`.
    bool index_published{false};
    /// Physical deletions performed by an applying GC.
    std::uint64_t gc_deleted_blobs{0};
    std::uint64_t gc_deleted_segments{0};
    /// Planned deletions left on disk because `gc_budget_ms` expired (apply
    /// only); the next mark-sweep re-plans them.
    std::uint64_t gc_deferred_blobs{0};
    std::uint64_t gc_deferred_segments{0};
    /// True when the store lock was acquired (false for a read-only store).
    bool lock_acquired{false};
    /// True when the store could not be locked for writing and the pipeline
    /// ran in scan-only mode (never with `auto_install`/`gc = apply`).
    bool read_only{false};
    /// GC plan: unreferenced standalone blobs / orphan segments (both modes;
    /// for `apply` these are the pre-deletion counts).
    std::uint64_t gc_planned_blobs{0};
    std::uint64_t gc_planned_segments{0};
};

/// Run the startup content pipeline (see the file comment for the stages and
/// failure policy).
///
/// Receipts are the single source of truth for the index and the GC roots. The
/// index uses one version per mod id: when several versions of the same mod
/// are installed (update without uninstall) the highest SemVer wins and the
/// choice is logged: an explicit gamepack pin first, else the highest origin
/// (`user` > `first_party` > `builtin`) and then the highest SemVer. Layer
/// order comes from `startup_content_options::layer_order` (the gamepack
/// `[mods].list` declaration order): each selected receipt gets a distinct
/// priority (`base_priority - rank`), so a location shipped by several mods is
/// resolved by layer order rather than by source id. The RFRX builder rejects
/// any remaining same-`(location, priority)` pair, so this path must always
/// assign distinct priorities (it does). Cross-layer overrides are legal
/// because their priorities differ.
[[nodiscard]] auto run_startup_content(const startup_content_options &options)
    -> result<startup_content_report>;

} // namespace refrain::mod
