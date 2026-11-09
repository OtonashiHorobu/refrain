// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/net/base.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace refrain::session {

/// Gameplay-side player record. Entity ids are assigned by the session, not by
/// the network connection id.
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

/// Session-owned player table and visibility model. The network layer reports
/// connection-scoped events; this object decides which entities exist and who
/// can see them.
class play_session {
  public:
    /// Register a joined player and allocate a process-local entity id.
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

    [[nodiscard]] auto all() const -> std::vector<player>;

    [[nodiscard]] auto size() const -> std::size_t;

  private:
    std::unordered_map<std::uint64_t, player> players_{};
    std::uint32_t next_entity_id_{1};
};

} // namespace refrain::session
