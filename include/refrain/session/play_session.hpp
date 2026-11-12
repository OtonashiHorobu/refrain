// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/net/base.hpp>
#include <refrain/sim/player.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::session {

/// Protocol-facing projection of one simulation player record.
///
/// The simulation owns the record (`sim::player_state`); this struct only
/// carries the fields the network shell encodes. It is refreshed from the
/// authoritative record on access and is never written back.
struct player {
    std::uint64_t connection{0};
    net::uuid id{};
    std::string name{};
    std::string world{};
    sim::entity_id entity_id{0};
    double x{0.0};
    double y{0.0};
    double z{0.0};
    float yaw{0.0F};
    float pitch{0.0F};
    bool on_ground{false};
    std::uint32_t game_mode{0};
    float health{20.0F};
    std::int32_t view_distance{10};
    std::vector<sim::item_stack> inventory{};
};

struct visibility_entry {
    std::uint64_t connection{0};
    sim::entity_id entity_id{0};
    double x{0.0};
    double y{0.0};
    double z{0.0};
    float yaw{0.0F};
    float pitch{0.0F};
};

/// Gameplay-side adapter over the simulation's player table. The network layer
/// reports connection-scoped events; this object resolves them to the one
/// authoritative player record and decides which entities a connection can see.
///
/// It owns no player data: mutations go straight to `sim::player_table`, and
/// the projection cache only holds the last encoded view of each record. The
/// synthetic player that the console and replay scripts drive has no network
/// connection, so it is excluded from the connection-scoped views.
class play_session {
  public:
    explicit play_session(sim::player_table &players);

    /// Register the player owned by `connection` and allocate its entity id.
    /// The record lives in the simulation table; this layer only attaches it to
    /// the connection.
    [[nodiscard]] auto add_player(std::uint64_t connection, net::uuid id,
                                  std::string name, std::string world, double x,
                                  double y, double z, std::uint32_t game_mode)
        -> result<sim::entity_id>;

    /// Remove a player and release its entity id.
    auto remove_player(std::uint64_t connection) -> bool;

    [[nodiscard]] auto player_of(std::uint64_t connection) const
        -> const session::player *;
    [[nodiscard]] auto player_by_entity(sim::entity_id entity_id) const
        -> const session::player *;

    /// Mutable inventory owned by one player. The simulation thread is the
    /// only writer; the pointer is invalidated by `remove_player()`.
    [[nodiscard]] auto inventory_of(std::uint64_t connection)
        -> std::vector<sim::item_stack> *;

    [[nodiscard]] auto update_position(std::uint64_t connection, double x,
                                       double y, double z, float yaw,
                                       float pitch) -> bool;

    [[nodiscard]] auto set_on_ground(std::uint64_t connection, bool value)
        -> bool;

    [[nodiscard]] auto set_view_distance(std::uint64_t connection,
                                         std::int32_t distance) -> bool;

    [[nodiscard]] auto set_health(std::uint64_t connection, float health)
        -> bool;

    [[nodiscard]] auto set_game_mode(std::uint64_t connection,
                                     std::uint32_t mode) -> bool;

    /// Players within `radius` blocks of `observer`, excluding the observer.
    [[nodiscard]] auto visible_to(std::uint64_t observer, double radius) const
        -> std::vector<visibility_entry>;

    /// Connected players in ascending entity id order.
    [[nodiscard]] auto all() const -> std::vector<player>;

    /// Visit every connected player in ascending entity id order without
    /// copying the projection. The simulation thread is the only writer, so
    /// this is the cheap path for per-tick iteration.
    template <typename F> void for_each(F &&visit) const {
        refresh_all();
        for (const auto &[id, value] : views_) {
            static_cast<void>(id);
            visit(value);
        }
    }

    [[nodiscard]] auto size() const -> std::size_t;

  private:
    [[nodiscard]] auto find_connected(std::uint64_t connection) const noexcept
        -> const sim::player_state *;
    [[nodiscard]] static auto project(const sim::player_state &record)
        -> player;
    void refresh(const sim::player_state &record) const;
    void refresh_all() const;

    sim::player_table *players_{nullptr};
    /// Projection cache keyed by entity id so pointers stay valid while the
    /// network shell holds them for one tick.
    mutable std::map<sim::entity_id, player> views_{};
};

} // namespace refrain::session
