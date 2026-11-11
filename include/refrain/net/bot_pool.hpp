// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/net/transport.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace refrain::net {

/// Options for a load client that keeps many protocol bots in play.
struct bot_pool_options {
    endpoint target{};
    std::size_t bots{100};
    /// Time each bot stays in play after it reaches the play state.
    std::chrono::milliseconds duration{std::chrono::seconds{30}};
    /// Time from connect to the play state before a bot counts as failed.
    std::chrono::milliseconds timeout{std::chrono::seconds{15}};
    /// Radius of the circle a bot walks in play; zero disables walking.
    double walk_radius{0.0};
    /// Interval between move packets; values below one millisecond are
    /// clamped to one millisecond.
    std::chrono::milliseconds walk_period{std::chrono::milliseconds{100}};
    std::string username_prefix{"Bot"};
    std::uint8_t view_distance{8};
};

/// Result of one pool run. `failed` counts bots that never reached the play
/// state; a bot that reaches play and is then disconnected still counts as
/// joined. `errors` holds at most the first 16 failure messages.
struct bot_pool_report {
    std::size_t requested{0};
    std::size_t joined{0};
    std::size_t failed{0};
    std::uint64_t packets{0};
    std::uint64_t chunks{0};
    std::vector<std::string> errors{};
    /// Join latency (connect to the play state) over the joined bots.
    std::chrono::milliseconds join_p50{0};
    std::chrono::milliseconds join_p99{0};
};

/// Run `options.bots` offline-mode clients over one shared transport with four
/// I/O threads. Every bot sends the handshake and login start, acknowledges
/// the login, finishes configuration, enters play, counts serverbound and
/// clientbound traffic, answers keep-alives, optionally walks, stays for
/// `options.duration` and then closes. The call blocks until every bot is done
/// and never waits on one bot while another makes progress. Protocol and
/// network failures are reported through `bot_pool_report`; the function does
/// not throw for them.
[[nodiscard]] auto run_bot_pool(const bot_pool_options &options)
    -> bot_pool_report;

} // namespace refrain::net
