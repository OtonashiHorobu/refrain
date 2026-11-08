// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The first structure-start generators. Each entry emits the same piece
// geometry and reference cap as the reference structure type.

#pragma once

#include <refrain/worldgen/structure/start.hpp>
#include <refrain/worldgen/structure/structure_data.hpp>
#include <refrain/worldgen/structure/template.hpp>

#include <functional>
#include <optional>

namespace refrain::worldgen::structure {

struct structure_start_context;

/// `BuriedTreasureStructure.findGenerationPoint`: a single 1x1x1 piece at the
/// chunk centre column, y=90.
[[nodiscard]] auto buried_treasure_start(const chunk_pos &chunk)
    -> structure_start;

class structure_template_manager;

/// `ShipwreckStructure`: one shipwreck template piece, rotated, optionally
/// moved to the beached/ocean floor height when the template is too large.
[[nodiscard]] auto shipwreck_start(
    const chunk_pos &chunk, jlong seed, structure_template_manager &templates,
    bool beached,
    const std::function<jint(jint, jint, heightmap_kind)> &height_at)
    -> structure_start;

/// `JungleTempleStructure`: one 12x10x15 scattered piece on the surface,
/// rejected when its corner heights are below sea level.
[[nodiscard]] auto jungle_temple_start(
    const chunk_pos &chunk, jlong seed, jint sea_level,
    const std::function<jint(jint, jint, heightmap_kind)> &height_at)
    -> std::optional<structure_start>;

/// `MineshaftStructure`: the recursive room/corridor/crossing/stairs set,
/// moved below sea level after generation.
[[nodiscard]] auto mineshaft_start(const chunk_pos &chunk, jlong seed,
                                   jint min_y, jint sea_level, bool mesa)
    -> structure_start;

/// `OceanRuinStructure`: the main ruin piece plus an optional cluster of
/// small ruins. `warm` selects the warm or cold template pools.
[[nodiscard]] auto woodland_mansion_start(
    const chunk_pos &chunk, jlong seed, structure_template_manager &templates,
    const std::function<jint(jint, jint, heightmap_kind)> &height_at)
    -> structure_start;

/// `OceanMonumentStructure`: one building piece whose generated room graph
/// yields entry, core, fitted rooms and the two wings plus the penthouse. The
/// building starts 29 blocks west and north of the chunk.
[[nodiscard]] auto ocean_monument_start(const chunk_pos &chunk, jlong seed)
    -> structure_start;

/// `StrongholdStructure.findGenerationPoint`: one start stairs-down piece plus
/// the recursively fitted stronghold rooms, moved below sea level.
[[nodiscard]] auto stronghold_start(const chunk_pos &chunk, jlong seed,
                                    jint min_y, jint sea_level,
                                    jint max_tries = 10000) -> structure_start;

/// Candidate chunks of a `concentric_rings` placement: the ring geometry and
/// per-ring biome search, including the fixed-biome search shape.
[[nodiscard]] auto concentric_ring_positions(
    jlong seed, const concentric_rings_placement &placement,
    const structure_start_context &context) -> std::vector<chunk_pos>;

[[nodiscard]] auto ruined_portal_start(
    const chunk_pos &chunk, jlong seed, const structure_document &structure,
    structure_template_manager &templates,
    const std::function<jint(jint, jint, heightmap_kind)> &height_at,
    const std::function<bool(jint, jint, jint)> &opaque_at,
    const std::function<bool(jint, jint, jint)> &biome_cold, jint min_y)
    -> structure_start;

[[nodiscard]] auto ocean_ruin_start(const chunk_pos &chunk, jlong seed,
                                    structure_template_manager &templates,
                                    bool warm, float large_probability,
                                    float cluster_probability)
    -> structure_start;

/// `OceanRuinPiece.getHeight`: lowers the ruin onto the seabed when the
/// scanned columns expose a wide enough drop.
[[nodiscard]] auto ocean_ruin_floor_height(chunk_region &region,
                                           const block_state_registry &states,
                                           jint min_y,
                                           const block_pos &position,
                                           const block_pos &corner) -> jint;

/// `DesertPyramidStructure`: one 21x21x15 single piece at generation height
/// 64, below the lowest surface corner of the plot.
[[nodiscard]] auto desert_pyramid_start(const chunk_pos &chunk, jlong seed)
    -> structure_start;

/// `SwampHutStructure`: one 7x7x9 scattered hut piece at y=64. The piece keeps
/// the unshifted box; placement moves it to the average ground height.
[[nodiscard]] auto swamp_hut_start(const chunk_pos &chunk, jlong seed)
    -> structure_start;

/// `IglooPieces.addPieces`: the top template at y=90 and, for half the start
/// seeds, a ladder column and a basement below it. The pieces carry the
/// template id, rotation and pivot; their final y is resolved when the piece
/// is placed in a chunk.
[[nodiscard]] auto igloo_start(const chunk_pos &chunk, jlong seed,
                               structure_template_manager &templates)
    -> structure_start;

/// The world access `minecraft:jigsaw` start generation needs. Pools come from
/// the structure data set; template sizes and jigsaw blocks come from the
/// template manager. Heights are the unbearded `getFirstFreeHeight` values the
/// reference queries while expanding terrain-matched pieces.
struct jigsaw_generation_context {
    const structure_document *structure{nullptr};
    const structure_data_set *data{nullptr};
    structure_template_manager *templates{nullptr};
    jlong seed{0};
    jint min_y{-64};
    jint world_height{384};
    jint sea_level{63};
    std::function<jint(jint, jint, heightmap_kind)> height_at{};
    /// `GenerationContext.isValidBiome` at the generated start anchor.
    std::function<bool(const block_pos &)> valid_biome{};
    /// `GenerationContext.couldStructureExistInColumn` for the projected
    /// start column.
    std::function<bool(jint, jint)> column_allowed{};
};

/// `JigsawStructure.findGenerationPoint`: chooses the centre piece, projects it
/// to the start heightmap and expands the pool graph through its connectors.
[[nodiscard]] auto jigsaw_start(const chunk_pos &chunk,
                                const jigsaw_generation_context &context)
    -> std::optional<structure_start>;

} // namespace refrain::worldgen::structure
