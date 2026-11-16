// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/session/player_store.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::session {

auto table_player_store::add(sim::player_table::registration record)
    -> result<sim::entity_id> {
    return players_->add(std::move(record));
}

auto table_player_store::remove(std::uint64_t connection) -> bool {
    return players_->remove(connection);
}

auto table_player_store::find(std::uint64_t connection) const noexcept
    -> const sim::player_state * {
    return players_->find_by_connection(connection);
}

auto table_player_store::find_entity(sim::entity_id id) const noexcept
    -> const sim::player_state * {
    return players_->find(id);
}

auto table_player_store::find_name(std::string_view name) const noexcept
    -> const sim::player_state * {
    return players_->find_by_name(name);
}

auto table_player_store::first() const noexcept -> const sim::player_state * {
    return players_->first();
}

auto table_player_store::records() const noexcept
    -> const std::map<sim::entity_id, sim::player_state> & {
    return players_->records();
}

auto table_player_store::size() const noexcept -> std::size_t {
    return players_->size();
}

auto table_player_store::update_position(std::uint64_t connection, double x,
                                         double y, double z, float yaw,
                                         float pitch) -> bool {
    auto *record{players_->find_by_connection(connection)};
    if (record == nullptr) {
        return false;
    }
    record->x = x;
    record->y = y;
    record->z = z;
    record->yaw = yaw;
    record->pitch = pitch;
    return true;
}

auto table_player_store::set_on_ground(std::uint64_t connection, bool value)
    -> bool {
    auto *record{players_->find_by_connection(connection)};
    if (record == nullptr) {
        return false;
    }
    record->on_ground = value;
    return true;
}

auto table_player_store::set_view_distance(std::uint64_t connection,
                                           std::int32_t distance) -> bool {
    auto *record{players_->find_by_connection(connection)};
    if (record == nullptr || distance < 0) {
        return false;
    }
    record->view_distance = distance;
    return true;
}

auto table_player_store::set_health(std::uint64_t connection, float health)
    -> bool {
    auto *record{players_->find_by_connection(connection)};
    if (record == nullptr) {
        return false;
    }
    record->health = health;
    return true;
}

auto table_player_store::set_game_mode(std::uint64_t connection,
                                       std::uint32_t mode) -> bool {
    auto *record{players_->find_by_connection(connection)};
    if (record == nullptr) {
        return false;
    }
    record->game_mode = mode;
    return true;
}

auto table_player_store::inventory_of(std::uint64_t connection)
    -> std::vector<sim::item_stack> * {
    auto *record{players_->find_by_connection(connection)};
    return record == nullptr ? nullptr : &record->inventory;
}

} // namespace refrain::session
