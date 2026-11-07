// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/structure/start.hpp>

#include <algorithm>

namespace refrain::worldgen::structure {

auto structure_bbox::intersects(const structure_bbox &other) const -> bool {
    return min_x <= other.max_x && max_x >= other.min_x &&
           min_y <= other.max_y && max_y >= other.min_y &&
           min_z <= other.max_z && max_z >= other.min_z;
}

auto structure_bbox::is_close_to(const chunk_pos &chunk, jint radius) const
    -> bool {
    const jint chunk_min_x = chunk.x * 16;
    const jint chunk_min_z = chunk.z * 16;
    return max_x >= chunk_min_x - radius &&
           min_x <= chunk_min_x + 15 + radius &&
           max_z >= chunk_min_z - radius && min_z <= chunk_min_z + 15 + radius;
}

auto starts_for_chunk(const std::vector<structure_start> &starts,
                      const chunk_pos &chunk, jint radius)
    -> std::vector<const structure_start *> {
    std::vector<const structure_start *> result;
    for (const auto &start : starts) {
        if (start.bounds.is_close_to(chunk, radius)) {
            result.push_back(&start);
        }
    }
    return result;
}

auto collect_references(std::vector<structure_start> &starts,
                        const chunk_pos &chunk, jint radius)
    -> std::vector<structure_reference> {
    std::vector<structure_reference> result;
    const auto add = [&](std::string_view structure, const chunk_pos &source) {
        for (auto &reference : result) {
            if (reference.structure == structure &&
                reference.chunk_x == source.x &&
                reference.chunk_z == source.z) {
                ++reference.count;
                return;
            }
        }
        result.push_back(
            structure_reference{.structure = std::string{structure},
                                .chunk_x = source.x,
                                .chunk_z = source.z,
                                .count = 1});
    };
    for (auto &start : starts) {
        if (!start.bounds.is_close_to(chunk, radius) ||
            !start.can_add_reference()) {
            continue;
        }
        start.add_reference();
        add(start.structure, start.chunk);
    }
    return result;
}

} // namespace refrain::worldgen::structure
