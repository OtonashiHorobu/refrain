// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Format-independent DataFixer for world records.
//
// A world record (level data, a chunk, a player, a saved-data file, ...) is
// stamped with the game data version that wrote it. Between the supported
// source window and this build's target version the reference game registers
// schema changes, and each change is a pure transformation of the records it
// touches. This library owns those transformations once: the RWF migration and
// the Anvil importer both decode a record, run the same fix chain for its type,
// and only then convert it to their own storage model or stamp the target
// version.
//
// The model is deliberately conservative:
//
// * A fix is a pure function of the NBT document. It has no clock, no random
//   source, no I/O and no dependence on the record's position in a region, so
//   the same input and window always produce the same output, and re-running a
//   fix chain over its own output changes nothing.
// * Fixes are selected by schema version: one whose reference schema version is
//   greater than the source and at most the target runs, in ascending schema
//   order. A fix applies only to the record types the reference game applies it
//   to, so a level record is not walked for item components it cannot contain.
// * Outside the declared window, or for a target version this build does not
//   implement, the call fails with `unsupported` and names the record type, the
//   version and the window. Nothing is interpreted as if it were the current
//   version.
//
// The fixer does not write the record's `DataVersion` field: a caller stamps
// the version only after every record it owns was fixed and durably written,
// which is what makes an interrupted upgrade retryable. `stamp_data_version`
// is provided for callers that upgraded a document in memory.
//
// The first fix table covers the two reference schema changes after 5013:
// schema 5014 removes the retired `minecraft:block_transformer` item component
// and schema 5016 renames the unfilled buried-treasure map item name. A source
// version is accepted when every schema change from it to the target is in the
// table; the declarations below make the window a property of the table rather
// than an independent constant, so widening the table widens the window.
//
// Scope limits of the current table: the records modelled here carry item
// stacks directly (player inventory, chunk block entities, entity chunks).
// Entity persistence, legacy pre-1.13/pre-1.18 chunk formats, structure
// templates, advancements and statistics are separate inputs with their own
// readers; their types are declared so callers can name them, but no fix of the
// current table applies to them yet.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/data/nbt.hpp>
#include <refrain/world/version.hpp>

#include <cstdint>
#include <string_view>

namespace refrain::world {

/// Record type of a fix chain, named after the reference data fixer types.
enum class datafix_type : std::uint8_t {
    level,              ///< `level.dat` `Data` compound / RWF meta `level`
    player,             ///< one player NBT record
    chunk,              ///< terrain chunk (block entities included)
    entity_chunk,       ///< entities region record for one chunk
    poi_chunk,          ///< point-of-interest region record for one chunk
    world_gen_settings, ///< `world_gen_settings.dat`
    saved_data,         ///< any other saved-data NBT record
    // Declared but not fixed by the current table: the importer copies these
    // records verbatim and the fixes registered so far do not apply to them.
    // TODO: give structure templates, advancement and statistics records a
    // type-specific fix chain once a schema change in the supported window
    // touches their fields.
    structure,
    advancements,
    stats,
};

/// Name of `type` for diagnostics, e.g. `"chunk"`.
[[nodiscard]] auto datafix_type_name(datafix_type type) noexcept
    -> std::string_view;

/// Type of a saved-data record selected by its file name: the generator
/// settings have their own reference type, every other saved-data file is
/// generic.
[[nodiscard]] auto saved_data_type(std::string_view file_name) noexcept
    -> datafix_type;

/// Oldest source DataVersion whose schema changes are in the fix table. A
/// record stamped with an older version must be brought forward by the game
/// (or a build with a wider table) before it can be read here.
inline constexpr std::int32_t oldest_migratable_data_version = 5013;

/// Target DataVersion the fix table reaches; this build writes it.
inline constexpr std::int32_t datafix_target_version = target_data_version;

/// Window and step count for one source-to-target request.
struct datafix_plan {
    std::int32_t from_version{0};
    std::int32_t to_version{0};
    /// Number of fix steps that run; the caller's version stamp is separate.
    std::uint32_t steps{0};
};

/// Validate that `from_version` can be fixed to `to_version` by this table.
///
/// Fails with `unsupported` when the source is outside
/// `[oldest_migratable_data_version, to_version]` or the target is not this
/// build's target; the message names both versions and the window. Fails with
/// `invalid_argument` when the request would downgrade a record.
[[nodiscard]] auto plan_datafix(std::int32_t from_version,
                                std::int32_t to_version)
    -> result<datafix_plan>;

/// Outcome of fixing one document.
struct datafix_result {
    /// True when at least one fix changed the document.
    bool changed{false};
    /// Number of fix steps that changed the document.
    std::uint32_t fixes_applied{0};
};

/// Run the fix chain for `type` over `document` from `from_version` to
/// `to_version`, in ascending schema version order.
///
/// `document` must be a compound; it is edited in place. On success
/// `result.changed` tells whether anything was rewritten, and the document is
/// the fixed record. The document's `DataVersion` field is not touched; stamp
/// it with `stamp_data_version` after the caller has persisted the record.
///
/// Fails closed with `unsupported` (message names the type, the version and the
/// window) outside the supported window and with `parse_error` when the
/// document is not a compound. The record's own `DataVersion` is checked by the
/// caller that read it, because a record can omit the field and fall back to
/// the version of the world that contains it.
[[nodiscard]] auto fix_document(data::nbt::value &document, datafix_type type,
                                std::int32_t from_version,
                                std::int32_t to_version)
    -> result<datafix_result>;

/// Stamp `document` with `data_version`; the last write of an upgrade.
[[nodiscard]] auto stamp_data_version(data::nbt::value &document,
                                      std::int32_t data_version) -> status;

/// Number of fix steps between two in-window versions (no window validation,
/// no version stamp).
[[nodiscard]] auto datafix_step_count(std::int32_t from_version,
                                      std::int32_t to_version) noexcept
    -> std::uint32_t;

} // namespace refrain::world
