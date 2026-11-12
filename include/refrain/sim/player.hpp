// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::sim {

/// Process-local runtime entity id. Ids are assigned in spawn order and are
/// never persisted (the storage entity format is not implemented yet); the sim
/// needs a deterministic, replay-stable identifier for the entity table.
using entity_id = std::uint64_t;

/// Connection id reserved for the synthetic player that the console and replay
/// scripts drive. Network connections use positive ids, so the synthetic player
/// is never reachable by a client.
inline constexpr std::uint64_t synthetic_connection = 0;

/// One inventory stack: an item key plus a count, with no components or
/// metadata.
struct item_stack {
    std::string item{};
    std::uint32_t count{0};
};

/// Maximum stack size for the current item model.
///
/// TODO: every item shares this ceiling and stacks carry no components or
/// durability; per-item stack sizes and damage come from the item registry.
inline constexpr std::uint32_t max_stack_size = 64;

/// The one authoritative record of a player. The simulation owns the table of
/// these records; the session layer only projects a record onto the fields the
/// protocol needs. Every mutable gameplay field lives here, so the console and
/// the protocol never act on two different copies.
struct player_state {
    /// Entity id, unique across players and spawned entities.
    entity_id id{0};
    /// Network connection that owns the player, or `synthetic_connection` for
    /// the player driven by the console and replay scripts.
    std::uint64_t connection{synthetic_connection};
    /// Profile uuid in wire byte order; zero for the synthetic player.
    std::array<std::uint8_t, 16> profile_id{};
    std::string name{};
    std::string world{};
    double x{0.0};
    double y{0.0};
    double z{0.0};
    double vx{0.0};
    double vy{0.0};
    double vz{0.0};
    float yaw{0.0F};
    float pitch{0.0F};
    bool on_ground{false};
    std::uint32_t game_mode{0};
    float health{20.0F};
    std::int32_t view_distance{10};
    std::vector<item_stack> inventory{};
};

/// Table of player records, keyed and addressed by entity id.
///
/// The map orders records by ascending entity id, which is the canonical order
/// for the state hash, console selection and packet emission. Player and
/// non-player entity ids come from the same sequence so a client never receives
/// two entities with one id.
class player_table {
  public:
    /// Everything needed to create one player record.
    struct registration {
        std::uint64_t connection{synthetic_connection};
        std::array<std::uint8_t, 16> profile_id{};
        std::string name{};
        std::string world{};
        double x{0.0};
        double y{0.0};
        double z{0.0};
        std::uint32_t game_mode{0};
        /// Preallocated inventory slots. The synthetic player starts without
        /// slots so its inventory grows exactly like the scripted player did;
        /// a connected player starts with the vanilla slot count.
        std::uint32_t inventory_slots{0};
    };

    /// Register a player and allocate its entity id. Fails with
    /// `invalid_argument` for an empty name or a duplicate connection and with
    /// `invalid_argument` when the id space is exhausted.
    [[nodiscard]] auto add(registration record) -> result<entity_id>;

    /// Remove the player owned by `connection`; false when it is unknown.
    [[nodiscard]] auto remove(std::uint64_t connection) -> bool;

    [[nodiscard]] auto find(entity_id id) noexcept -> player_state *;
    [[nodiscard]] auto find(entity_id id) const noexcept
        -> const player_state *;
    [[nodiscard]] auto find_by_connection(std::uint64_t connection) noexcept
        -> player_state *;
    [[nodiscard]] auto
    find_by_connection(std::uint64_t connection) const noexcept
        -> const player_state *;

    /// First record with `name` in ascending entity id order; names are not
    /// required to be unique, so a duplicate resolves to the lowest id.
    [[nodiscard]] auto find_by_name(std::string_view name) noexcept
        -> player_state *;
    [[nodiscard]] auto find_by_name(std::string_view name) const noexcept
        -> const player_state *;

    /// First record in ascending entity id order, or nullptr for an empty
    /// table. This is the console's default selection.
    [[nodiscard]] auto first() noexcept -> player_state *;
    [[nodiscard]] auto first() const noexcept -> const player_state *;

    /// Records in ascending entity id order.
    [[nodiscard]] auto records() const noexcept
        -> const std::map<entity_id, player_state> & {
        return players_;
    }

    template <typename F> void for_each(F &&visit) {
        for (auto &[id, value] : players_) {
            static_cast<void>(id);
            visit(value);
        }
    }

    template <typename F> void for_each(F &&visit) const {
        for (const auto &[id, value] : players_) {
            static_cast<void>(id);
            visit(value);
        }
    }

    [[nodiscard]] auto size() const noexcept -> std::size_t {
        return players_.size();
    }

    /// Allocate the next process-local entity id. `world::spawn_entity()` draws
    /// from the same sequence as player registration.
    [[nodiscard]] auto allocate_entity_id() -> result<entity_id>;

  private:
    std::map<entity_id, player_state> players_{};
    entity_id next_entity_id_{1};
};

} // namespace refrain::sim
