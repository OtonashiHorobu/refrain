// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/sim/player.hpp>

#include <cstdint>
#include <map>
#include <string_view>
#include <vector>

namespace refrain::session {

/// The player-record operations `play_session` performs.
///
/// The session layer does not decide where a player record lives. The
/// first-party composition keeps the table in the simulation and the store
/// forwards every call to it without a copy; a composition reached through the
/// contracts keeps the table inside the provider, so the store keeps a
/// per-tick projection and turns the mutation calls into the provider's
/// connection-addressed player calls. Either way the session owns no
/// authoritative record and never holds a pointer into a provider's table.
///
/// A store that implements only a part of this interface returns false (or
/// null) from the operations with no provider surface and says so next to the
/// method; the contract slice currently covers registration, removal, the
/// movement transform, the teleport, the view distance and the relative move.
class player_store {
  public:
    player_store() = default;
    player_store(const player_store &) = delete;
    auto operator=(const player_store &) -> player_store & = delete;
    player_store(player_store &&) = delete;
    auto operator=(player_store &&) -> player_store & = delete;
    virtual ~player_store() = default;

    /// Register a player and allocate its entity id.
    [[nodiscard]] virtual auto add(sim::player_table::registration record)
        -> result<sim::entity_id> = 0;

    /// Remove the player owned by `connection`; false when it owns none.
    [[nodiscard]] virtual auto remove(std::uint64_t connection) -> bool = 0;

    /// Record owned by `connection`, or nullptr. This is the hot path: a
    /// packet names a connection, so the lookup must not cost a provider call.
    [[nodiscard]] virtual auto find(std::uint64_t connection) const noexcept
        -> const sim::player_state * = 0;

    /// Record with entity id `id`, or nullptr.
    [[nodiscard]] virtual auto find_entity(sim::entity_id id) const noexcept
        -> const sim::player_state * = 0;

    /// First record named `name` in ascending entity id order, or nullptr.
    [[nodiscard]] virtual auto find_name(std::string_view name) const noexcept
        -> const sim::player_state * = 0;

    /// First record in ascending entity id order, or nullptr.
    [[nodiscard]] virtual auto first() const noexcept
        -> const sim::player_state * = 0;

    /// Records in ascending entity id order.
    [[nodiscard]] virtual auto records() const noexcept
        -> const std::map<sim::entity_id, sim::player_state> & = 0;

    [[nodiscard]] virtual auto size() const noexcept -> std::size_t = 0;

    /// Set position and rotation in one call, leaving velocity untouched.
    [[nodiscard]] virtual auto update_position(std::uint64_t connection,
                                               double x, double y, double z,
                                               float yaw, float pitch)
        -> bool = 0;

    [[nodiscard]] virtual auto set_on_ground(std::uint64_t connection,
                                             bool value) -> bool = 0;
    [[nodiscard]] virtual auto set_view_distance(std::uint64_t connection,
                                                 std::int32_t distance)
        -> bool = 0;
    [[nodiscard]] virtual auto set_health(std::uint64_t connection,
                                          float health) -> bool = 0;
    [[nodiscard]] virtual auto set_game_mode(std::uint64_t connection,
                                             std::uint32_t mode) -> bool = 0;

    /// Mutable inventory of the player, or nullptr when it owns none or the
    /// store has no in-process record to expose.
    [[nodiscard]] virtual auto inventory_of(std::uint64_t connection)
        -> std::vector<sim::item_stack> * = 0;

    /// Re-read a provider-owned table into the store's cache. The in-process
    /// store has nothing to do; the runtime calls it once per tick and reports
    /// a failure instead of ticking over a stale view.
    [[nodiscard]] virtual auto refresh() -> status { return status_ok(); }
};

/// Store over an in-process `sim::player_table`. The table is the authority:
/// every call forwards to it, so a composition that runs the simulation in
/// this process pays no copy and no projection.
class table_player_store final : public player_store {
  public:
    explicit table_player_store(sim::player_table &players) noexcept
        : players_{&players} {}

    [[nodiscard]] auto add(sim::player_table::registration record)
        -> result<sim::entity_id> override;
    [[nodiscard]] auto remove(std::uint64_t connection) -> bool override;
    [[nodiscard]] auto find(std::uint64_t connection) const noexcept
        -> const sim::player_state * override;
    [[nodiscard]] auto find_entity(sim::entity_id id) const noexcept
        -> const sim::player_state * override;
    [[nodiscard]] auto find_name(std::string_view name) const noexcept
        -> const sim::player_state * override;
    [[nodiscard]] auto first() const noexcept
        -> const sim::player_state * override;
    [[nodiscard]] auto records() const noexcept
        -> const std::map<sim::entity_id, sim::player_state> & override;
    [[nodiscard]] auto size() const noexcept -> std::size_t override;
    [[nodiscard]] auto update_position(std::uint64_t connection, double x,
                                       double y, double z, float yaw,
                                       float pitch) -> bool override;
    [[nodiscard]] auto set_on_ground(std::uint64_t connection, bool value)
        -> bool override;
    [[nodiscard]] auto set_view_distance(std::uint64_t connection,
                                         std::int32_t distance)
        -> bool override;
    [[nodiscard]] auto set_health(std::uint64_t connection, float health)
        -> bool override;
    [[nodiscard]] auto set_game_mode(std::uint64_t connection,
                                     std::uint32_t mode) -> bool override;
    [[nodiscard]] auto inventory_of(std::uint64_t connection)
        -> std::vector<sim::item_stack> * override;

  private:
    sim::player_table *players_{nullptr};
};

} // namespace refrain::session
