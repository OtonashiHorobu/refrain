// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The first structure-start generators. Each entry emits the same piece
// geometry and reference cap as the reference structure type.

#pragma once

#include <refrain/worldgen/structure/start.hpp>
#include <refrain/worldgen/structure/template.hpp>

#include <functional>

namespace refrain::worldgen::structure {

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

/// `MineshaftStructure`: the recursive room/corridor/crossing/stairs set,
/// moved below sea level after generation.
[[nodiscard]] auto mineshaft_start(const chunk_pos &chunk, jlong seed,
                                   jint min_y, jint sea_level, bool mesa)
    -> structure_start;

/// `OceanRuinStructure`: the main ruin piece plus an optional cluster of
/// small ruins. `warm` selects the warm or cold template pools.
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

} // namespace refrain::worldgen::structure
