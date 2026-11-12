// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/session/play_session.hpp>

#include <utility>

namespace refrain::session {

play_session::play_session() : players_{&own_players_} {}

play_session::play_session(sim::player_table &players) : players_{&players} {}

play_session::play_session(play_session &&other) noexcept
    : own_players_{std::move(other.own_players_)},
      players_{other.players_ == &other.own_players_ ? &own_players_
                                                     : other.players_},
      views_{std::move(other.views_)} {}

auto play_session::operator=(play_session &&other) noexcept -> play_session & {
    if (this == &other) {
        return *this;
    }
    own_players_ = std::move(other.own_players_);
    players_ =
        other.players_ == &other.own_players_ ? &own_players_ : other.players_;
    views_ = std::move(other.views_);
    return *this;
}

auto play_session::add_player(std::uint64_t connection, net::uuid id,
                              std::string name, std::string world, double x,
                              double y, double z, std::uint32_t game_mode)
    -> result<std::uint32_t> {
    if (connection == sim::synthetic_connection) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            "a network player needs a connection; use the world table for the "
            "synthetic player")};
    }
    auto added{players_->add(sim::player_table::registration{
        .connection = connection,
        .profile_id = id.bytes,
        .name = std::move(name),
        .world = std::move(world),
        .x = x,
        .y = y,
        .z = z,
        .game_mode = game_mode,
        .inventory_slots = 36,
    })};
    if (!added) {
        return std::unexpected{std::move(added.error())};
    }
    // The table caps ids at the signed 32-bit wire range.
    return static_cast<std::uint32_t>(*added);
}

auto play_session::remove_player(std::uint64_t connection) -> bool {
    const auto *record{find_connected(connection)};
    if (record == nullptr) {
        return false;
    }
    views_.erase(record->id);
    return players_->remove(connection);
}

auto play_session::player_of(std::uint64_t connection) const
    -> const session::player * {
    const auto *record{find_connected(connection)};
    if (record == nullptr) {
        return nullptr;
    }
    refresh(*record);
    return &views_.at(record->id);
}

auto play_session::player_by_entity(std::uint32_t entity_id) const
    -> const session::player * {
    const auto *record{players_->find(entity_id)};
    if (record == nullptr || record->connection == sim::synthetic_connection) {
        return nullptr;
    }
    refresh(*record);
    return &views_.at(record->id);
}

auto play_session::inventory_of(std::uint64_t connection)
    -> std::vector<sim::item_stack> * {
    auto *record{players_->find_by_connection(connection)};
    if (record == nullptr || record->connection == sim::synthetic_connection) {
        return nullptr;
    }
    return &record->inventory;
}

auto play_session::update_position(std::uint64_t connection, double x, double y,
                                   double z, float yaw, float pitch) -> bool {
    auto *record{players_->find_by_connection(connection)};
    if (record == nullptr || record->connection == sim::synthetic_connection) {
        return false;
    }
    record->x = x;
    record->y = y;
    record->z = z;
    record->yaw = yaw;
    record->pitch = pitch;
    return true;
}

auto play_session::set_on_ground(std::uint64_t connection, bool value) -> bool {
    auto *record{players_->find_by_connection(connection)};
    if (record == nullptr || record->connection == sim::synthetic_connection) {
        return false;
    }
    record->on_ground = value;
    return true;
}

auto play_session::set_view_distance(std::uint64_t connection,
                                     std::int32_t distance) -> bool {
    auto *record{players_->find_by_connection(connection)};
    if (record == nullptr || record->connection == sim::synthetic_connection ||
        distance < 0) {
        return false;
    }
    record->view_distance = distance;
    return true;
}

auto play_session::set_health(std::uint64_t connection, float health) -> bool {
    auto *record{players_->find_by_connection(connection)};
    if (record == nullptr || record->connection == sim::synthetic_connection) {
        return false;
    }
    record->health = health;
    return true;
}

auto play_session::set_game_mode(std::uint64_t connection, std::uint32_t mode)
    -> bool {
    auto *record{players_->find_by_connection(connection)};
    if (record == nullptr || record->connection == sim::synthetic_connection) {
        return false;
    }
    record->game_mode = mode;
    return true;
}

auto play_session::visible_to(std::uint64_t observer, double radius) const
    -> std::vector<visibility_entry> {
    std::vector<visibility_entry> out;
    const auto *source{find_connected(observer)};
    if (source == nullptr || radius < 0.0) {
        return out;
    }
    const auto squared_radius{radius * radius};
    for (const auto &[id, value] : players_->records()) {
        static_cast<void>(id);
        if (value.connection == sim::synthetic_connection ||
            value.connection == observer || value.world != source->world) {
            continue;
        }
        const auto dx{value.x - source->x};
        const auto dy{value.y - source->y};
        const auto dz{value.z - source->z};
        if (dx * dx + dy * dy + dz * dz <= squared_radius) {
            out.push_back({.connection = value.connection,
                           .entity_id = static_cast<std::uint32_t>(value.id),
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
    refresh_all();
    std::vector<player> out;
    out.reserve(views_.size());
    for (const auto &[id, value] : views_) {
        static_cast<void>(id);
        out.push_back(value);
    }
    return out;
}

auto play_session::size() const -> std::size_t {
    std::size_t count{0};
    for (const auto &[id, value] : players_->records()) {
        static_cast<void>(id);
        if (value.connection != sim::synthetic_connection) {
            ++count;
        }
    }
    return count;
}

auto play_session::find_connected(std::uint64_t connection) const noexcept
    -> const sim::player_state * {
    const auto *record{players_->find_by_connection(connection)};
    if (record == nullptr || record->connection == sim::synthetic_connection) {
        return nullptr;
    }
    return record;
}

auto play_session::project(const sim::player_state &record) -> player {
    return player{
        .connection = record.connection,
        .id = net::uuid{.bytes = record.profile_id},
        .name = record.name,
        .world = record.world,
        .entity_id = static_cast<std::uint32_t>(record.id),
        .x = record.x,
        .y = record.y,
        .z = record.z,
        .yaw = record.yaw,
        .pitch = record.pitch,
        .on_ground = record.on_ground,
        .game_mode = record.game_mode,
        .health = record.health,
        .view_distance = record.view_distance,
        .inventory = record.inventory,
    };
}

void play_session::refresh(const sim::player_state &record) const {
    // Assign into the existing node so pointers handed out by player_of() and
    // for_each() stay valid for the duration of one tick.
    views_[record.id] = project(record);
}

void play_session::refresh_all() const {
    for (const auto &[id, record] : players_->records()) {
        static_cast<void>(id);
        if (record.connection != sim::synthetic_connection) {
            refresh(record);
        }
    }
    for (auto iterator{views_.begin()}; iterator != views_.end();) {
        const auto *record{players_->find(iterator->first)};
        if (record == nullptr ||
            record->connection == sim::synthetic_connection) {
            iterator = views_.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

} // namespace refrain::session
