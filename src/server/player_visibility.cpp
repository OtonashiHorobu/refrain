// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/player_visibility.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string_view>
#include <vector>

namespace refrain::server {
namespace {

[[nodiscard]] auto chunk_coordinate(double value) -> std::int64_t {
    return static_cast<std::int64_t>(std::floor(value / 16.0));
}

} // namespace

auto effective_tracking_chunks(std::int32_t tracking_chunks,
                               std::int32_t view_distance) -> std::int32_t {
    return std::max(0, std::min(tracking_chunks, view_distance));
}

auto player_visible(const visibility_query &viewer,
                    std::string_view entity_world, double entity_x,
                    double entity_z, std::int32_t tracking_chunks) -> bool {
    if (viewer.world != entity_world) {
        return false;
    }
    const auto chunks{
        effective_tracking_chunks(tracking_chunks, viewer.view_distance)};
    const auto range{static_cast<double>(chunks) * 16.0};
    const auto dx{entity_x - viewer.x};
    const auto dz{entity_z - viewer.z};
    if (dx * dx + dz * dz > range * range) {
        return false;
    }
    // The entity must also sit inside the viewer's chunk viewport, which is
    // the same square the chunk stream fills.
    const auto viewer_chunk_x{chunk_coordinate(viewer.x)};
    const auto viewer_chunk_z{chunk_coordinate(viewer.z)};
    const auto entity_chunk_x{chunk_coordinate(entity_x)};
    const auto entity_chunk_z{chunk_coordinate(entity_z)};
    return std::llabs(entity_chunk_x - viewer_chunk_x) <= chunks &&
           std::llabs(entity_chunk_z - viewer_chunk_z) <= chunks;
}

player_visibility_index::player_visibility_index(std::int32_t tracking_chunks)
    : tracking_chunks_{tracking_chunks} {}

void player_visibility_index::rebuild(
    std::span<const visibility_entity> entities) {
    worlds_.clear();
    max_chunks_ = 0;
    for (std::size_t position{0}; position < entities.size(); ++position) {
        const auto &entity{entities[position]};
        const auto chunks{
            effective_tracking_chunks(tracking_chunks_, entity.view_distance)};
        max_chunks_ = std::max(max_chunks_, chunks);
        const auto chunk_x{chunk_coordinate(entity.x)};
        const auto chunk_z{chunk_coordinate(entity.z)};
        worlds_[entity.world][chunk_position{.z = chunk_z, .x = chunk_x}]
            .push_back(position);
    }
}

void player_visibility_index::candidates(const visibility_query &viewer,
                                         std::vector<std::size_t> &out) const {
    out.clear();
    const auto world{worlds_.find(viewer.world)};
    if (world == worlds_.end()) {
        return;
    }
    const auto chunks{
        effective_tracking_chunks(tracking_chunks_, viewer.view_distance)};
    const auto centre_x{chunk_coordinate(viewer.x)};
    const auto centre_z{chunk_coordinate(viewer.z)};
    const auto x_min{centre_x - chunks};
    const auto x_max{centre_x + chunks};
    const auto z_max{centre_z + chunks};
    for (auto row{centre_z - chunks}; row <= z_max; ++row) {
        const auto &buckets{world->second};
        auto bucket{buckets.lower_bound(chunk_position{.z = row, .x = x_min})};
        for (; bucket != buckets.end() && bucket->first.z == row &&
               bucket->first.x <= x_max;
             ++bucket) {
            out.insert(out.end(), bucket->second.begin(), bucket->second.end());
        }
    }
    std::sort(out.begin(), out.end());
}

void player_visibility_index::viewers(std::string_view world_name, double x,
                                      double z,
                                      std::vector<std::size_t> &out) const {
    out.clear();
    const auto world{worlds_.find(world_name)};
    if (world == worlds_.end()) {
        return;
    }
    const auto centre_x{chunk_coordinate(x)};
    const auto centre_z{chunk_coordinate(z)};
    const auto x_min{centre_x - max_chunks_};
    const auto x_max{centre_x + max_chunks_};
    const auto z_max{centre_z + max_chunks_};
    for (auto row{centre_z - max_chunks_}; row <= z_max; ++row) {
        const auto &buckets{world->second};
        auto bucket{buckets.lower_bound(chunk_position{.z = row, .x = x_min})};
        for (; bucket != buckets.end() && bucket->first.z == row &&
               bucket->first.x <= x_max;
             ++bucket) {
            out.insert(out.end(), bucket->second.begin(), bucket->second.end());
        }
    }
    std::sort(out.begin(), out.end());
}

} // namespace refrain::server
