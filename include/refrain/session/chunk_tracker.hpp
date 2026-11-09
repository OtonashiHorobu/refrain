// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace refrain::session {

/// One viewport change the simulation should fill with chunk packets.
struct chunk_view_request {
    std::uint64_t connection{0};
    std::int32_t chunk_x{0};
    std::int32_t chunk_z{0};
    std::int32_t radius{0};
    std::int32_t seed_radius{0};
};

/// Session-owned chunk subscription tracking. The network layer no longer
/// decides when a player needs a new viewport; it only reports movement and
/// client view distance.
class chunk_tracker {
  public:
    /// Update one player's viewport. Returns true when the center or radius
    /// changed, or when the player has no view yet.
    auto update(std::uint64_t connection, std::int32_t chunk_x,
                std::int32_t chunk_z, std::int32_t radius) -> bool;

    /// Drain pending viewport requests. The first request for a player carries
    /// `seed_radius = 3` so spawn chunks are marked known.
    [[nodiscard]] auto take_requests() -> std::vector<chunk_view_request>;

    auto remove(std::uint64_t connection) -> bool;

  private:
    struct view {
        std::int32_t chunk_x{0};
        std::int32_t chunk_z{0};
        std::int32_t radius{0};
        bool initialized{false};
    };
    std::unordered_map<std::uint64_t, view> views_{};
    std::vector<chunk_view_request> pending_{};
};

} // namespace refrain::session
