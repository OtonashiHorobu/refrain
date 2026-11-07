// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/structure/generation.hpp>

namespace refrain::worldgen::structure {

auto buried_treasure_start(const chunk_pos &chunk) -> structure_start {
    const structure_bbox bounds{.min_x = chunk.x * 16 + 9,
                                .min_y = 90,
                                .min_z = chunk.z * 16 + 9,
                                .max_x = chunk.x * 16 + 9,
                                .max_y = 90,
                                .max_z = chunk.z * 16 + 9};
    structure_start start{.structure = "minecraft:buried_treasure",
                          .chunk = chunk,
                          .references = 0,
                          .max_references = 1,
                          .bounds = bounds};
    start.pieces.push_back(structure_piece{
        .type = "minecraft:buried_treasure_piece", .bounds = bounds});
    return start;
}

} // namespace refrain::worldgen::structure
