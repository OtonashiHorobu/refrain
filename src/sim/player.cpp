// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/sim/player.hpp>

#include <fmt/format.h>

#include <limits>
#include <utility>

namespace refrain::sim {

auto player_table::add(registration record) -> result<entity_id> {
    if (record.name.empty()) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "player name must not be empty")};
    }
    for (const auto &[id, existing] : players_) {
        static_cast<void>(id);
        if (existing.connection == record.connection) {
            return std::unexpected{
                make_error(error_code::invalid_argument,
                           fmt::format("connection {} already owns a player",
                                       record.connection))};
        }
    }
    auto id{allocate_entity_id()};
    if (!id) {
        return std::unexpected{std::move(id.error())};
    }
    player_state value{};
    value.id = *id;
    value.connection = record.connection;
    value.profile_id = record.profile_id;
    value.name = std::move(record.name);
    value.world = std::move(record.world);
    value.x = record.x;
    value.y = record.y;
    value.z = record.z;
    value.game_mode = record.game_mode;
    value.inventory.resize(record.inventory_slots);
    players_.emplace(*id, std::move(value));
    return *id;
}

auto player_table::remove(std::uint64_t connection) -> bool {
    const auto found{find_by_connection(connection)};
    if (found == nullptr) {
        return false;
    }
    return players_.erase(found->id) != 0U;
}

auto player_table::find(entity_id id) noexcept -> player_state * {
    const auto found{players_.find(id)};
    return found == players_.end() ? nullptr : &found->second;
}

auto player_table::find(entity_id id) const noexcept -> const player_state * {
    const auto found{players_.find(id)};
    return found == players_.end() ? nullptr : &found->second;
}

auto player_table::find_by_connection(std::uint64_t connection) noexcept
    -> player_state * {
    for (auto &[id, value] : players_) {
        static_cast<void>(id);
        if (value.connection == connection) {
            return &value;
        }
    }
    return nullptr;
}

auto player_table::find_by_connection(std::uint64_t connection) const noexcept
    -> const player_state * {
    for (const auto &[id, value] : players_) {
        static_cast<void>(id);
        if (value.connection == connection) {
            return &value;
        }
    }
    return nullptr;
}

auto player_table::find_by_name(std::string_view name) noexcept
    -> player_state * {
    for (auto &[id, value] : players_) {
        static_cast<void>(id);
        if (value.name == name) {
            return &value;
        }
    }
    return nullptr;
}

auto player_table::find_by_name(std::string_view name) const noexcept
    -> const player_state * {
    for (const auto &[id, value] : players_) {
        static_cast<void>(id);
        if (value.name == name) {
            return &value;
        }
    }
    return nullptr;
}

auto player_table::first() noexcept -> player_state * {
    return players_.empty() ? nullptr : &players_.begin()->second;
}

auto player_table::first() const noexcept -> const player_state * {
    return players_.empty() ? nullptr : &players_.begin()->second;
}

auto player_table::allocate_entity_id() -> result<entity_id> {
    // Entity ids travel as VarInts, so the representable ceiling is the signed
    // 32-bit maximum rather than the width of entity_id.
    if (next_entity_id_ >
        static_cast<entity_id>(std::numeric_limits<std::int32_t>::max())) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "entity id space exhausted")};
    }
    return next_entity_id_++;
}

} // namespace refrain::sim
