// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Player visibility and entity broadcast. The broadcaster owns the per-viewer
// tracking sets and the last movement it sent for each player; the gateway owns
// the connections and the encoding output.

#pragma once

#include <refrain/net/server.hpp>
#include <refrain/server/play_gateway.hpp>
#include <refrain/session/play_session.hpp>

#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace refrain::server {

/// Mirrors the vanilla `ChunkMap.TrackedEntity` pass for player entities.
///
/// One `tick()` emits the visibility transitions (spawn/remove) and the
/// movement packets of that tick. Lifecycle hooks (`joined`, `left`, `attack`,
/// `animation`, `play_ready`, `respawn`) emit the one-off session packets so
/// the tick loop does not need to know their wire shape.
class entity_broadcaster {
  public:
    /// `tracking_chunks` is the effective entity tracking range, already
    /// converted from blocks to chunks by the caller.
    entity_broadcaster(play_gateway &gateway, std::int32_t tracking_chunks);

    /// Send the visibility transitions and the movement updates of one tick.
    void tick(const refrain::session::play_session &players);

    /// Send the tab-list add entries and announce the new player to the
    /// connections already in play.
    void joined(const refrain::session::play_session &players,
                std::uint64_t connection);

    /// Drop one connection's tracking state, remove its entity from every
    /// viewer and announce the leave.
    void left(const refrain::session::play_session &players,
              const refrain::session::player &value);

    void attack(refrain::session::play_session &players,
                const refrain::net::attack_event &event);

    void animation(const refrain::session::play_session &players,
                   std::uint64_t source, std::int8_t action);

    void play_ready(const refrain::session::play_session &players,
                    std::uint64_t connection);

    void respawn(refrain::session::play_session &players,
                 std::uint64_t connection);

  private:
    play_gateway &gateway_;
    std::int32_t tracking_chunks_{0};
    std::unordered_map<std::uint64_t, refrain::net::movement_event>
        last_moves_{};
    std::unordered_map<std::uint64_t, std::unordered_set<std::uint64_t>>
        visible_{};
};

} // namespace refrain::server
