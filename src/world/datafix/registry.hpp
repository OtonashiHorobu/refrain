// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Internal fix table of the world DataFixer.
//
// A fix is registered by the reference schema version that introduced it
// (the version a record must be *older* than for the fix to run), the record
// types the reference game applies it to, a provenance name, and a pure
// transformation of an NBT document. The table is sorted by schema version;
// `fix_document` runs the entries in that order, so every record is upgraded
// step by step and the result does not depend on the walk order.

#pragma once

#include <refrain/data/nbt.hpp>
#include <refrain/world/datafix.hpp>

#include <cstdint>
#include <span>
#include <string_view>

namespace refrain::world::datafix_detail {

/// Record-type bitset for `fix_step::types`.
[[nodiscard]] constexpr auto type_bit(datafix_type type) noexcept
    -> std::uint32_t {
    return 1U << static_cast<std::uint32_t>(type);
}

constexpr auto level_type = type_bit(datafix_type::level);
constexpr auto player_type = type_bit(datafix_type::player);
constexpr auto chunk_type = type_bit(datafix_type::chunk);
constexpr auto entity_chunk_type = type_bit(datafix_type::entity_chunk);
constexpr auto poi_chunk_type = type_bit(datafix_type::poi_chunk);
constexpr auto world_gen_settings_type =
    type_bit(datafix_type::world_gen_settings);
constexpr auto saved_data_type_bit = type_bit(datafix_type::saved_data);
constexpr auto structure_type = type_bit(datafix_type::structure);
constexpr auto advancements_type = type_bit(datafix_type::advancements);
constexpr auto stats_type = type_bit(datafix_type::stats);

/// Types whose records can carry item stacks; the item-component fixes apply
/// there. Chunk records carry block-entity items and entity chunks carry the
/// items an entity holds (item frames, armor stands, dropped items).
constexpr auto item_record_types = player_type | chunk_type | entity_chunk_type;

struct fix_step {
    /// Reference schema version that introduced the change. The step runs for
    /// a source version lower than this one.
    std::int32_t schema_version{0};
    /// Record types the reference fix applies to.
    std::uint32_t types{0};
    /// Provenance: the reference fix class this transformation reproduces.
    std::string_view name{};
    /// Pure in-place transformation; returns true when it changed `document`.
    bool (*apply)(data::nbt::value &document){nullptr};
};

/// The fix table, ascending by `schema_version`.
[[nodiscard]] auto fix_steps() noexcept -> std::span<const fix_step>;

} // namespace refrain::world::datafix_detail
