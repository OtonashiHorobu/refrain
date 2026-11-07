// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The first structure-start generators. Each entry emits the same piece
// geometry and reference cap as the reference structure type.

#pragma once

#include <refrain/worldgen/structure/start.hpp>
#include <refrain/worldgen/structure/template.hpp>

namespace refrain::worldgen::structure {

/// `BuriedTreasureStructure.findGenerationPoint`: a single 1x1x1 piece at the
/// chunk centre column, y=90.
[[nodiscard]] auto buried_treasure_start(const chunk_pos &chunk)
    -> structure_start;

class structure_template_manager;

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
