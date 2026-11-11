// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Entity movement broadcast policy. The runtime decides which movement packet
// carries a player's update to the other players in range; the network layer
// only encodes what it is given.

#pragma once

#include <refrain/net/server.hpp>

#include <cstdint>

namespace refrain::server {

/// One movement step, encoded in 1/4096 blocks. The packet carries signed
/// 16-bit deltas, so a larger step becomes a teleport.
inline constexpr double entity_move_scale = 4096.0;
inline constexpr std::int64_t entity_move_limit = 32767;

/// Default tracking radius in blocks; vanilla tracks players at 48 blocks and
/// the value is configurable through `network.entity_tracking_range`.
inline constexpr double entity_tracking_range_blocks = 64.0;

enum class entity_move_kind {
    none,
    teleport,
    position,
    position_rotation,
    rotation,
};

struct entity_move {
    entity_move_kind kind{entity_move_kind::none};
    std::int16_t dx{0};
    std::int16_t dy{0};
    std::int16_t dz{0};
};

/// Chooses the cheapest packet that reproduces the step from `previous` to
/// `current`. A step that does not fit the delta packets falls back to a
/// teleport.
[[nodiscard]] auto choose_entity_move(const net::movement_event &previous,
                                      const net::movement_event &current)
    -> entity_move;

/// True when `current` is inside the tracking radius around `viewer`.
[[nodiscard]] auto within_tracking_range(const net::movement_event &viewer,
                                         const net::movement_event &current,
                                         double range) -> bool;

} // namespace refrain::server
