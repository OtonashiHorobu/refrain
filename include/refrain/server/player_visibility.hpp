// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Player visibility for the entity broadcast. Mirrors the vanilla tracker:
// a player sees another player when both share a world, the horizontal
// distance is inside the effective tracking range, and the entity is inside
// the viewer's chunk viewport.

#pragma once

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace refrain::server {

/// Vanilla `EntityTypes.PLAYER` tracking range, in chunks.
inline constexpr std::int32_t player_tracking_chunks = 32;

struct visibility_query {
    std::string_view world{};
    double x{0.0};
    double z{0.0};
    /// Viewer view distance in chunks; the tracker clamps the entity type
    /// range with it.
    std::int32_t view_distance{10};
};

/// Vanilla `ChunkMap.TrackedEntity.updatePlayer`: the horizontal distance must
/// not exceed `min(tracking_chunks, view_distance) * 16`, and the chunk of the
/// entity must be inside the viewer's viewport.
[[nodiscard]] auto player_visible(const visibility_query &viewer,
                                  std::string_view entity_world,
                                  double entity_x, double entity_z,
                                  std::int32_t tracking_chunks) -> bool;

} // namespace refrain::server
