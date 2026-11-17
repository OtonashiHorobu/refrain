// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/entity_broadcast.hpp>

#include <cmath>
#include <optional>

namespace refrain::server {
namespace {

[[nodiscard]] auto scaled_delta(double delta) -> std::optional<std::int64_t> {
    const auto scaled{delta * entity_move_scale};
    if (!std::isfinite(scaled)) {
        return std::nullopt;
    }
    return std::llround(scaled);
}

} // namespace

auto finite_position(double x, double y, double z, float yaw, float pitch)
    -> bool {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
           std::isfinite(yaw) && std::isfinite(pitch);
}

auto addressable_position(double x, double y, double z) -> bool {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
           std::fabs(x) <= world_horizontal_limit &&
           std::fabs(y) <= world_vertical_limit &&
           std::fabs(z) <= world_horizontal_limit;
}

auto choose_entity_move(const net::movement_event &previous,
                        const net::movement_event &current) -> entity_move {
    // A reported position that is not finite has no packet representation: the
    // delta conversion and the rotation byte are undefined for it, and a
    // teleport carrying NaN would only move the invalid value to the client.
    // The record itself never holds such a position; this is the last guard
    // for a caller that bypassed the movement and teleport boundaries.
    if (!finite_position(current.x, current.y, current.z, current.yaw,
                         current.pitch)) {
        return {};
    }
    // The current position is representable but the previous one is not, so
    // there is no delta to encode.
    if (!finite_position(previous.x, previous.y, previous.z, previous.yaw,
                         previous.pitch)) {
        return {.kind = entity_move_kind::teleport};
    }
    const auto dx{scaled_delta(current.x - previous.x)};
    const auto dy{scaled_delta(current.y - previous.y)};
    const auto dz{scaled_delta(current.z - previous.z)};
    // A pair of finite positions can still be too far apart for the scaled
    // delta: opposite extremes of the double range overflow on subtraction or
    // on the scale multiplication, and the conversion of that value is
    // undefined. Like a delta wider than the 16-bit field, it becomes a
    // teleport to the current position.
    if (!dx || !dy || !dz) {
        return {.kind = entity_move_kind::teleport};
    }
    const auto moved{*dx != 0 || *dy != 0 || *dz != 0};
    const auto turned{current.yaw != previous.yaw ||
                      current.pitch != previous.pitch};
    if (!moved && !turned) {
        return {};
    }
    const auto fits{*dx >= -entity_move_limit && *dx <= entity_move_limit &&
                    *dy >= -entity_move_limit && *dy <= entity_move_limit &&
                    *dz >= -entity_move_limit && *dz <= entity_move_limit};
    if (!fits) {
        return {.kind = entity_move_kind::teleport};
    }
    entity_move move{};
    move.dx = static_cast<std::int16_t>(*dx);
    move.dy = static_cast<std::int16_t>(*dy);
    move.dz = static_cast<std::int16_t>(*dz);
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
