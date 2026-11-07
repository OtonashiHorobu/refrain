// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/structure/stage.hpp>

#include <refrain/worldgen/structure/generation.hpp>

namespace refrain::worldgen::structure {

void write_structure_stage(proto_chunk &chunk,
                           const buried_starts_result &result) {
    for (const auto &start : result.starts) {
        if (start.chunk.x != chunk.chunk_x() ||
            start.chunk.z != chunk.chunk_z()) {
            continue;
        }
        chunk.add_structure_start(
            ::refrain::worldgen::structure_start{.structure = start.structure,
                                                 .chunk_x = start.chunk.x,
                                                 .chunk_z = start.chunk.z,
                                                 .min_x = start.bounds.min_x,
                                                 .min_y = start.bounds.min_y,
                                                 .min_z = start.bounds.min_z,
                                                 .max_x = start.bounds.max_x,
                                                 .max_y = start.bounds.max_y,
                                                 .max_z = start.bounds.max_z});
    }
    for (const auto &reference : result.references) {
        chunk.add_structure_reference(reference.structure, reference.chunk_x,
                                      reference.chunk_z, reference.count);
    }
}

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
