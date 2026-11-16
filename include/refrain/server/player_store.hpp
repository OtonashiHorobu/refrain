// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/server/world_port.hpp>
#include <refrain/session/player_store.hpp>

#include <memory>

namespace refrain::server {

/// Build the player store a session uses over a contract world port.
///
/// The provider owns the player table: the store keeps a value projection of
/// it, refreshed by one bulk read per tick, and forwards the mutation calls the
/// contract exposes. The session then serves a packet's connection lookup from
/// the projection instead of calling the provider once per packet.
///
/// The returned store borrows `world`, so the port must outlive it.
[[nodiscard]] auto make_world_port_player_store(world_port &world)
    -> std::unique_ptr<session::player_store>;

} // namespace refrain::server
