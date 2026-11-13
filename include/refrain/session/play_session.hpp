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
    std::uint32_t entity_id{0};
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
    std::uint32_t entity_id{0};
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
///
/// Thread ownership: the simulation thread is the only caller. The network
/// shell observes connection lifecycle on its I/O threads, so it queues join,
/// leave and ready events and the runtime delivers them from the tick loop
/// instead of calling this object from the transport. Every member, including
/// the projection cache, is unsynchronised and relies on that single caller.
class play_session {
  public:
    /// Standalone session with its own player table, for callers that are not
    /// attached to a running simulation (tests and tooling). The server always
    /// passes the world's table so the console and the protocol share one
    /// record.
    play_session();
    explicit play_session(sim::player_table &players);
    play_session(const play_session &) = delete;
    auto operator=(const play_session &) -> play_session & = delete;
    play_session(play_session &&other) noexcept;
    auto operator=(play_session &&other) noexcept -> play_session &;

    /// Register the player owned by `connection` and allocate its entity id.
    /// The record lives in the simulation table; this layer only attaches it to
    /// the connection.
    [[nodiscard]] auto add_player(std::uint64_t connection, net::uuid id,
                                  std::string name, std::string world, double x,
                                  double y, double z, std::uint32_t game_mode)
        -> result<std::uint32_t>;

    /// Remove a player and release its entity id.
    auto remove_player(std::uint64_t connection) -> bool;

    [[nodiscard]] auto player_of(std::uint64_t connection) const
        -> const session::player *;
    [[nodiscard]] auto player_by_entity(std::uint32_t entity_id) const
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
    /// copying the projection. The pointers stay valid until the next call and
    /// observe mutations made through this object (the cached projection is
    /// refreshed in place).
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
    void refresh_if_cached(const sim::player_state &record) const;
    void refresh_all() const;

    /// Table owned by the standalone constructor; unused when the server
    /// attaches the world's table.
    sim::player_table own_players_{};
    sim::player_table *players_{nullptr};
    /// Projection cache keyed by entity id so pointers stay valid while the
    /// network shell holds them for one tick. Only the simulation thread reads
    /// or writes it; the class comment states where the other threads hand
    /// their events over instead of touching this map.
    mutable std::map<sim::entity_id, player> views_{};
};

} // namespace refrain::session
