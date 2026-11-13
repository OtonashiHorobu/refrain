// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/net/transport.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace refrain::net {

struct bot_options {
    endpoint target{};
    std::string username{"Bot"};
    std::chrono::milliseconds timeout{10'000};
    std::uint8_t view_distance{8};
    /// Finish only after this many `level_chunk_with_light` packets; 0 stops
    /// at the play login.
    std::size_t wait_chunks{0};
    /// Keep the connection in play for this long after the join condition is
    /// met (useful for multi-client tests).
    std::chrono::milliseconds linger{0};
    /// Optional play command (leading slash optional) sent after the play
    /// login.
    std::string command{};
    /// Finish only after this many system_chat messages.
    std::size_t wait_messages{0};
    /// Send a play move packet after the join (offsets from the spawn
    /// position sent by the server).
    double move_dx{0.0};
    double move_dy{0.0};
    double move_dz{0.0};
    std::chrono::milliseconds move_delay{0};
    std::chrono::milliseconds command_delay{0};
    /// Walk in a circle of this radius around the spawn position until the
    /// connection ends. Zero keeps the single `move_dx/dy/dz` step.
    double walk_radius{0.0};
    /// Time between walk steps.
    std::chrono::milliseconds walk_period{100};
    /// Walk around this offset from the spawn position, so a load test can
    /// spread its bots over the world instead of stacking them.
    double walk_origin_x{0.0};
    double walk_origin_z{0.0};
    /// Stall the read loop by this long before each received batch. A client
    /// that stops draining its socket is what makes the server's per-connection
    /// write queue grow, and this bot reproduces that by refusing to consume:
    /// the kernel receive buffer fills, then the server's socket stops
    /// accepting, and the tick sees the backlog a slow client produces.
    std::chrono::milliseconds read_delay{0};
    /// Record decoded packet bodies for capture tooling.
    bool capture{false};
};

struct captured_packet {
    std::uint8_t state{0};
    std::uint32_t id{0};
    std::vector<std::byte> body{};
};

struct bot_report {
    std::size_t packets{0};
    std::size_t chunks{0};
    std::string name{};
    std::vector<std::string> messages{};
    std::vector<captured_packet> capture{};
};

/// Offline-mode protocol bot: handshake -> login -> configuration -> play.
/// Online-mode servers report the encryption request as unsupported.
[[nodiscard]] auto run_bot(const bot_options &options) -> result<bot_report>;

} // namespace refrain::net
