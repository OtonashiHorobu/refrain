// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/structure/stage.hpp>

#include <refrain/worldgen/structure/generation.hpp>

namespace refrain::worldgen::structure {

auto generate_buried_starts(jlong seed, const chunk_pos &target,
                            const std::vector<selected_structure> &structures,
                            jint radius) -> buried_starts_result {
    buried_starts_result result;
    for (jint dx = -radius; dx <= radius; ++dx) {
        for (jint dz = -radius; dz <= radius; ++dz) {
            const chunk_pos chunk{.x = target.x + dx, .z = target.z + dz};
            for (const auto &structure : structures) {
                if (!structure.placement.is_structure_chunk(seed, chunk.x,
                                                            chunk.z)) {
                    continue;
                }
                auto start = buried_treasure_start(chunk);
                start.structure = structure.id;
                result.starts.push_back(std::move(start));
            }
        }
    }
    result.references = collect_references(result.starts, target, radius);
    return result;
}

} // namespace refrain::worldgen::structure
