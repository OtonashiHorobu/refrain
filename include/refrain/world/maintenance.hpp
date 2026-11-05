// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Quiesced in-place palette maintenance: rebuild a closed RWF world into a
// deterministic sibling directory and swap it in under a crash marker.
//
// Contract:
// * The world must be closed: no `world_store`/`sim::world` handle in this
//   process and no other process reading or writing it. Callers quiesce first
//   (sim thread: drain autosave, `flush()`, release the store) and reopen
//   afterwards.
// * The swap is crash safe: `<root>.palette-maintenance.json` records the
//   phase and the sibling scratch paths. `recover_palette_maintenance` finishes
//   or rolls back an interrupted swap, is idempotent, and is called by
//   `world_store::open` before any region is opened.
// * A corrupt/foreign marker or an ambiguous on-disk state fails closed
//   (`parse_error`/`io_error`); it is never silently ignored.
// * Online dual-generation rewriting is not implemented.

#pragma once

#include <refrain/core/error.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace refrain::world {

/// Inputs of `rebuild_palette_in_place`.
struct palette_maintenance_options {
    /// Rebuild worker count: 0 = auto, 1 = serial, N = N workers (forwarded to
    /// `anvil::rebuild_palette`; output bytes do not depend on it).
    unsigned threads{1};
    /// zstd level of the rebuilt region records (1..19).
    int zstd_level{3};
};

/// Outcome of a completed in-place rebuild.
struct palette_maintenance_report {
    /// World palette table entries before/after the rebuild; `entries_after`
    /// drops entries no chunk references.
    std::size_t entries_before{0};
    std::size_t entries_after{0};
    /// Chunks and regions copied into the rebuilt world.
    std::uint64_t chunks{0};
    std::uint64_t regions{0};
    /// World directory bytes before/after (same accounting as
    /// `world_store::stats()`; the marker and sibling scratch are not part of
    /// either world).
    std::uint64_t bytes_before{0};
    std::uint64_t bytes_after{0};
    /// True when a crash marker left by an earlier attempt was resolved before
    /// this rebuild started.
    bool recovered{false};
};

/// Absolute marker path: `<absolute root>.palette-maintenance.json`.
/// Returns an empty string for an empty/invalid root (no filename component).
[[nodiscard]] auto palette_maintenance_marker_path(std::string_view world_root)
    -> std::string;

/// Deterministic sibling scratch path
/// `<parent>/.<basename>.palette-maintenance-<pid>-<attempt>.tmp`.
/// Returns an empty string for an invalid root.
[[nodiscard]] auto palette_maintenance_temp_path(std::string_view world_root,
                                                 unsigned attempt)
    -> std::string;

/// Backup sibling path with the same stem and a `.bak` suffix.
[[nodiscard]] auto palette_maintenance_backup_path(std::string_view world_root,
                                                   unsigned attempt)
    -> std::string;

/// Rebuild the world palette in place through the Anvil round-trip and swap the
/// rebuilt world into `world_root`.
///
/// Preconditions: `world_root` is a closed RWF world directory (not a symlink),
/// `options.zstd_level` is in [1, 19], and no other process is using it.
///
/// Steps: recover any leftover marker → allocate a free scratch stem
/// (`attempt` 0..15, pre-existing paths are never deleted) → write the marker
/// (`planned`, then `rebuilding`) → `anvil::rebuild_palette` into the temp
/// directory → fsync the temp tree → marker `swapping` → rename root→backup,
/// temp→root, fsync the parent → validate the new root opens → remove the
/// backup and the marker.
///
/// On failure before the swap the scratch is removed and the world is
/// untouched. A failure during the swap restores the backup when possible;
/// otherwise the marker is kept so `recover_palette_maintenance` (and therefore
/// the next `world_store::open`) finishes or rolls back. Any delete/fsync
/// failure returns `io_error` and keeps the marker rather than reporting a
/// half-cleaned success.
///
/// `recovered` in the report is set when a marker from an earlier attempt was
/// resolved at the start. Repeated successful calls are idempotent (the second
/// call finds no unreferenced entries).
[[nodiscard]] auto
rebuild_palette_in_place(std::string_view world_root,
                         const palette_maintenance_options &options = {})
    -> result<palette_maintenance_report>;

/// Finish or roll back a swap interrupted by a crash.
///
/// Returns `true` when a marker was present and the on-disk state was made
/// consistent (roll-forward, rollback, or marker-only cleanup), `false` when
/// there is no marker (a successful no-op) or when this process is currently
/// rebuilding that root (see `rebuild_palette_in_place`).
///
/// Fail-closed rules (no filesystem mutation, marker kept):
/// * unreadable/damaged/foreign-schema marker, unknown phase, unknown keys, or
///   scratch paths that are not direct siblings carrying this root's
///   `.palette-maintenance-<pid>-<attempt>` prefix (`parse_error`);
/// * `world`, `backup` and `temp` all present, or no recoverable world at all
///   (`io_error`);
/// * any rename/remove/fsync failure (`io_error`, marker kept).
///
/// Idempotent: a second call after a successful recovery returns `false`.
[[nodiscard]] auto recover_palette_maintenance(std::string_view world_root)
    -> result<bool>;

} // namespace refrain::world
