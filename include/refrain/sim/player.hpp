// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/sim/item.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace refrain::sim {

/// Process-local runtime entity id. Ids are assigned in spawn order and are
/// never persisted (the storage entity format is not implemented yet); the sim
/// needs a deterministic, replay-stable identifier for the entity table. Ids
/// stay within the signed 32-bit range because they are written to the wire as
/// a VarInt entity id.
///
/// TODO: entity ids are not persisted yet. When the storage entity format
/// lands, a world reload must restore the stored id of every entity before
/// allocating new ones, so replay and the state hash stay stable across a
/// restart.
using entity_id = std::uint64_t;

/// Connection id reserved for the synthetic player that the console and replay
/// scripts drive. Network connections use positive ids, so the synthetic player
/// is never reachable by a client.
inline constexpr std::uint64_t synthetic_connection = 0;

/// Profile uuid size in wire byte order.
inline constexpr std::size_t profile_uuid_size = 16;

/// Player movement input bits. A client reports the keys it holds once per
/// tick (`player_input` report); the simulation consumes the same bits for the
/// console-driven player and for the server-side movement prediction, so the
/// two paths share one representation. The values match the wire bit layout.
enum class player_input : std::uint8_t {
    none = 0,
    forward = 1U << 0,
    backward = 1U << 1,
    left = 1U << 2,
    right = 1U << 3,
    jump = 1U << 4,
    shift = 1U << 5,
    sprint = 1U << 6,
};

[[nodiscard]] constexpr auto operator|(player_input left, player_input right)
    -> player_input {
    return static_cast<player_input>(static_cast<std::uint8_t>(left) |
                                     static_cast<std::uint8_t>(right));
}

[[nodiscard]] constexpr auto operator&(player_input left, player_input right)
    -> player_input {
    return static_cast<player_input>(static_cast<std::uint8_t>(left) &
                                     static_cast<std::uint8_t>(right));
}

[[nodiscard]] constexpr auto has_input(player_input value, player_input flag)
    -> bool {
    return (value & flag) != player_input::none;
}

/// How the last damage that killed (or hurt) a player was dealt. The
/// simulation records the source so the runtime can choose the death message
/// and the client's damage feedback without guessing from the health drop.
enum class damage_source : std::uint8_t {
    generic = 0,
    fall,
    drown,
    starve,
    out_of_world,
    in_wall,
    player_attack,
    mob_attack,
};

/// Player game mode, as the protocol numbers it.
inline constexpr std::uint32_t game_mode_survival = 0;
inline constexpr std::uint32_t game_mode_creative = 1;
inline constexpr std::uint32_t game_mode_adventure = 2;
inline constexpr std::uint32_t game_mode_spectator = 3;

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
    /// True when a network client owns this record and reports its own
    /// position. The simulation only applies validated movement requests to
    /// such a record and never predicts its motion between reports: the client
    /// is the authority for where the body is, the server is the authority for
    /// whether that position is reachable. Console and replay players have no
    /// client and are simulated from their input.
    bool client_driven{false};
    /// Profile uuid in wire byte order; zero for the synthetic player.
    std::array<std::uint8_t, profile_uuid_size> profile_id{};
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
    std::int32_t view_distance{10};
    std::vector<item_stack> inventory{};
    /// Selected hotbar slot, `0..8`. The held stack is
    /// `inventory[selected_slot]` when the inventory has that many slots; the
    /// client reports the slot with `set_carried_item` and the server's break
    /// rules read the held tool from it.
    std::uint8_t selected_slot{0};

    // --- movement input and prediction -------------------------------------

    /// Keys held this tick. The simulation reads them in `tick()`; a client
    /// report overwrites them once per tick.
    player_input input{player_input::none};
    /// Creative or spectator flight. The client toggles it with the abilities
    /// packet; the server only honours the toggle while the mode allows it.
    bool flying{false};
    /// Feet position of the last accepted ground contact. Jump validation uses
    /// it as the ceiling a player may reach without flight or fluid.
    double ground_y{0.0};

    // --- survival state ----------------------------------------------------

    /// Health points, 0..20. A record at 0 is dead until respawned.
    float health{20.0F};
    /// Hunger points, 0..20.
    std::int32_t food_level{20};
    /// Saturation points, 0..food_level.
    float saturation{5.0F};
    /// Food exhaustion, 0..40. Movement and damage add to it and the food tick
    /// consumes 4 points per reduction.
    float exhaustion{0.0F};
    /// Ticks the natural-regeneration/starvation timer has accumulated.
    std::int32_t food_timer{0};
    /// Air supply, 300 at the surface and negative while drowning.
    std::int32_t air_supply{300};
    /// Distance fallen since the last ground contact, in blocks.
    double fall_distance{0.0};
    /// Ticks of damage immunity left; a second hit inside the window only
    /// counts when it is stronger than the first.
    std::int32_t invulnerable_time{0};
    /// Damage of the hit that opened the current immunity window.
    float last_hurt{0.0F};
    /// Experience state, matching the `set_experience` packet fields.
    float experience_progress{0.0F};
    std::int32_t experience_level{0};
    std::int32_t total_experience{0};
    /// True once health reaches zero. The record keeps its position and
    /// inventory until the respawn request arrives.
    bool dead{false};
    /// Ticks spent dead; the runtime sends the death screen once and the
    /// simulation keeps the record for the death animation window.
    std::int32_t death_time{0};
    /// Damage source of the last hit, kept for the death message.
    damage_source last_damage{damage_source::generic};
    /// Connection of the player that dealt the last damaging hit, or zero for
    /// environmental damage. The death message and the kill experience read
    /// it, and identity fields are not part of the state hash.
    std::uint64_t last_attacker{0};
    /// Ticks this record has existed; periodic survival effects (peaceful
    /// regeneration) key off it so the result does not depend on wall time.
    std::int32_t age{0};

    // --- pending validated movement ---------------------------------------

    /// A movement request the validation accepted for the next `tick()`. The
    /// position is the authoritative target; `tick()` applies it after the
    /// prediction step so fall distance and ground contact see one movement
    /// per tick.
    bool has_move_request{false};
    double move_x{0.0};
    double move_y{0.0};
    double move_z{0.0};
    bool move_on_ground{false};
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
        /// See `player_state::client_driven`: a network registration is driven
        /// by the client's movement reports, a console or replay registration
        /// is simulated from its input.
        bool client_driven{false};
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
    /// O(1) lookup by connection; the record map is keyed by entity id.
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

    /// Record at `index` in ascending entity id order, or nullptr past the
    /// end. This is the indexed form of `records()` used by the contract's
    /// player projection: the projection walks the table once per tick, so the
    /// lookup is O(1) instead of advancing the map iterator per index.
    [[nodiscard]] auto nth(std::size_t index) const noexcept
        -> const player_state *;

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
    /// from the same sequence as player registration. Fails when the signed
    /// 32-bit wire range is exhausted.
    [[nodiscard]] auto allocate_entity_id() -> result<entity_id>;

  private:
    std::map<entity_id, player_state> players_{};
    /// Entity ids in ascending order, which is the order of the map. A new
    /// player always receives the highest id, so an addition appends and a
    /// removal erases; `nth()` indexes this vector.
    std::vector<entity_id> order_{};
    /// Connection to entity id. A record's connection never changes, so the
    /// index only follows `add()` and `remove()`; no ordering decision reads
    /// it.
    std::unordered_map<std::uint64_t, entity_id> by_connection_{};
    entity_id next_entity_id_{1};
};

} // namespace refrain::sim
