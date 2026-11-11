// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/entity_broadcast.hpp>

#include <cmath>

namespace refrain::server {
namespace {

[[nodiscard]] auto scaled_delta(double delta) -> std::int64_t {
    return std::llround(delta * entity_move_scale);
}

} // namespace

auto choose_entity_move(const net::movement_event &previous,
                        const net::movement_event &current) -> entity_move {
    const auto dx{scaled_delta(current.x - previous.x)};
    const auto dy{scaled_delta(current.y - previous.y)};
    const auto dz{scaled_delta(current.z - previous.z)};
    const auto moved{dx != 0 || dy != 0 || dz != 0};
    const auto turned{current.yaw != previous.yaw ||
                      current.pitch != previous.pitch};
    if (!moved && !turned) {
        return {};
    }
    const auto fits{dx >= -entity_move_limit && dx <= entity_move_limit &&
                    dy >= -entity_move_limit && dy <= entity_move_limit &&
                    dz >= -entity_move_limit && dz <= entity_move_limit};
    if (!fits) {
        return {.kind = entity_move_kind::teleport};
    }
    entity_move move{};
    move.dx = static_cast<std::int16_t>(dx);
    move.dy = static_cast<std::int16_t>(dy);
    move.dz = static_cast<std::int16_t>(dz);
    if (moved && turned) {
        move.kind = entity_move_kind::position_rotation;
    } else if (moved) {
        move.kind = entity_move_kind::position;
    } else {
        move.kind = entity_move_kind::rotation;
    }
    return move;
}

auto within_tracking_range(const net::movement_event &viewer,
                           const net::movement_event &current, double range)
    -> bool {
    const auto dx{current.x - viewer.x};
    const auto dy{current.y - viewer.y};
    const auto dz{current.z - viewer.z};
    return dx * dx + dy * dy + dz * dz <= range * range;
}

} // namespace refrain::server
