// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// In-place data version upgrade for RWF worlds, run before a store opens.
//
// A world records the game data version its records were written with. This
// build interprets one data version; a world recorded with an older one is
// upgraded by running one step per reference schema change and stamping the
// target version last. The steps are idempotent record transformations, so an
// interrupted upgrade is simply retried by the next open: the recorded version
// only changes after every record was updated, and a second run over an
// upgraded world is a no-op.
//
// Supported source window: `oldest_migratable_data_version` through
// `target_data_version`. The window covers the two reference schema changes
// after the merge of the intermediate terrain chunk statuses in 5013: schema
// 5014 removes the retired `minecraft:block_transformer` item component, and
// schema 5016 renames the buried-treasure map item name. No reference schema
// change exists after 5016, so nothing else separates those versions from 26.3.
// Older saves still need the chunk-status merge that the storage model already
// records as its own status set, so they are refused rather than rewritten;
// newer saves come from a newer build and are refused rather than downgraded.
//
// The upgrade touches the records a world persists: the level record, its
// saved-data files, every chunk's block-entity NBT and every player NBT record.
// Entity and item data that the storage model does not persist yet cannot
// appear in such a world, so there is nothing to migrate for it.

#pragma once

#include <refrain/core/error.hpp>

#include <cstdint>

namespace refrain::world {

/// Oldest source DataVersion this build can upgrade to `target_data_version`.
inline constexpr std::int32_t oldest_migratable_data_version = 5013;

/// Work a migration from `from_version` to `to_version` would run. `steps`
/// counts every upgrade step plus the final version stamp; zero means the
/// world already records `to_version`.
struct data_migration_plan {
    std::int32_t from_version{0};
    std::int32_t to_version{0};
    std::uint32_t steps{0};
};

struct data_migration_options {
    /// Validate the world and count the work without writing anything.
    bool dry_run{false};
};

struct data_migration_report {
    std::int32_t from_version{0};
    std::int32_t to_version{0};
    std::uint32_t steps{0};
    std::uint64_t chunks_scanned{0};
    std::uint64_t chunks_migrated{0};
    std::uint64_t player_records_scanned{0};
    std::uint64_t player_records_migrated{0};
    /// True when the world already recorded `to_version`, so nothing changed.
    bool no_op{false};
    /// True when the counts describe the work a real run would do.
    bool dry_run{false};
};

/// Validate that a world recorded at `from_version` can be upgraded to
/// `to_version` by this build and report the step count.
///
/// Fails with `unsupported` outside the supported window (the message names the
/// version and the window), with `invalid_argument` when `from_version` is
/// newer than `to_version`, and with `unsupported` when `to_version` is not the
/// target version this build writes.
[[nodiscard]] auto plan_data_migration(std::int32_t from_version,
                                       std::int32_t to_version)
    -> result<data_migration_plan>;

/// Upgrade the RWF world at `root` from `from_version` to `to_version`.
///
/// Preconditions: the world is closed (no store or server holds it) and
/// `from_version` equals the version its level record carries; a disagreement
/// fails closed instead of rewriting a different world. Chunk records are
/// written through the store, the version stamp through an atomic record
/// replace, and the level stamp is written last, so a failed migration leaves
/// the world recorded at its original version. The report counts the records
/// inspected and rewritten; with `options.dry_run` the same walk happens but
/// nothing is written and `report.dry_run` is set.
[[nodiscard]] auto
migrate_world_data(std::string_view root, std::int32_t from_version,
                   std::int32_t to_version,
                   const data_migration_options &options = {})
    -> result<data_migration_report>;

} // namespace refrain::world
