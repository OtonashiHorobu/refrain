// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/session/play_session.hpp>

#include <cmath>
#include <limits>
#include <utility>

namespace refrain::session {

auto play_session::add_player(std::uint64_t connection, net::uuid id,
                              std::string name, std::string world, double x,
                              double y, double z, std::uint32_t game_mode)
    -> result<std::uint32_t> {
    if (connection == 0 || name.empty() || players_.contains(connection)) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "session player is invalid")};
    }
    const auto entity_id{next_entity_id_++};
    players_.emplace(connection, player{.connection = connection,
                                        .id = id,
                                        .name = std::move(name),
                                        .world = std::move(world),
                                        .entity_id = entity_id,
                                        .x = x,
                                        .y = y,
                                        .z = z,
                                        .game_mode = game_mode});
    return entity_id;
}

auto play_session::remove_player(std::uint64_t connection) -> bool {
    return players_.erase(connection) != 0;
}

auto play_session::player_of(std::uint64_t connection) const
    -> const session::player * {
    const auto found{players_.find(connection)};
    return found == players_.end() ? nullptr : &found->second;
}

auto play_session::update_position(std::uint64_t connection, double x, double y,
                                   double z, float yaw, float pitch) -> bool {
    const auto found{players_.find(connection)};
    if (found == players_.end()) {
        return false;
    }
    found->second.x = x;
    found->second.y = y;
    found->second.z = z;
    found->second.yaw = yaw;
    found->second.pitch = pitch;
    return true;
}

auto play_session::set_health(std::uint64_t connection, float health) -> bool {
    const auto found{players_.find(connection)};
    if (found == players_.end()) {
        return false;
    }
    found->second.health = health;
    return true;
}

auto play_session::set_game_mode(std::uint64_t connection, std::uint32_t mode)
    -> bool {
    const auto found{players_.find(connection)};
    if (found == players_.end()) {
        return false;
    }
    found->second.game_mode = mode;
    return true;
}

auto play_session::visible_to(std::uint64_t observer, double radius) const
    -> std::vector<visibility_entry> {
    std::vector<visibility_entry> out;
    const auto source{players_.find(observer)};
    if (source == players_.end() || radius < 0.0) {
        return out;
    }
    const auto squared_radius{radius * radius};
    for (const auto &[connection, value] : players_) {
        if (connection == observer || value.world != source->second.world) {
            continue;
        }
        const auto dx{value.x - source->second.x};
        const auto dy{value.y - source->second.y};
        const auto dz{value.z - source->second.z};
        if (dx * dx + dy * dy + dz * dz <= squared_radius) {
            out.push_back({.connection = connection,
                           .entity_id = value.entity_id,
                           .x = value.x,
                           .y = value.y,
                           .z = value.z,
                           .yaw = value.yaw,
                           .pitch = value.pitch});
        }
    }
    return out;
}

auto play_session::all() const -> std::vector<player> {
    std::vector<player> out;
    out.reserve(players_.size());
    for (const auto &[connection, value] : players_) {
        static_cast<void>(connection);
        out.push_back(value);
    }
    return out;
}

auto play_session::size() const -> std::size_t { return players_.size(); }

} // namespace refrain::session
