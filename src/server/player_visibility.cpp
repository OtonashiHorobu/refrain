// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/player_visibility.hpp>

#include <cmath>

namespace refrain::server {

auto player_visible(const visibility_query &viewer,
                    std::string_view entity_world, double entity_x,
                    double entity_z, std::int32_t tracking_chunks) -> bool {
    if (viewer.world != entity_world) {
        return false;
    }
    const auto chunks{
        std::max(0, std::min(tracking_chunks, viewer.view_distance))};
    const auto range{static_cast<double>(chunks) * 16.0};
    const auto dx{entity_x - viewer.x};
    const auto dz{entity_z - viewer.z};
    if (dx * dx + dz * dz > range * range) {
        return false;
    }
    // The entity must also sit inside the viewer's chunk viewport, which is
    // the same square the chunk stream fills.
    const auto viewer_chunk_x{
        static_cast<std::int64_t>(std::floor(viewer.x / 16.0))};
    const auto viewer_chunk_z{
        static_cast<std::int64_t>(std::floor(viewer.z / 16.0))};
    const auto entity_chunk_x{
        static_cast<std::int64_t>(std::floor(entity_x / 16.0))};
    const auto entity_chunk_z{
        static_cast<std::int64_t>(std::floor(entity_z / 16.0))};
    return std::llabs(entity_chunk_x - viewer_chunk_x) <= chunks &&
           std::llabs(entity_chunk_z - viewer_chunk_z) <= chunks;
}

} // namespace refrain::server
