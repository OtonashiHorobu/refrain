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

} // namespace refrain::worldgen::structure
