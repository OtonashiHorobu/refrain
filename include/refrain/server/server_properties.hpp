// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace refrain::server {

/// The subset of vanilla `server.properties` the headless server consumes.
///
/// Keys are vanilla-compatible (Java `.properties` syntax: `key=value` or
/// `key: value`, `#`/`!` comments, backslash escapes). Unknown keys are
/// ignored so a server.properties produced by vanilla still loads.
struct server_properties {
    std::string level_name{"world"};
    std::string motd{"A Refrain Server"};
    std::int32_t max_players{20};
    std::int32_t view_distance{10};
    std::int32_t simulation_distance{10};
    bool online_mode{true};
    bool allow_nether{true};
    bool enable_status{true};
    std::string server_ip{"0.0.0.0"};
    std::int32_t server_port{25565};
    std::int32_t network_compression_threshold{256};
    bool enforce_secure_profile{true};
    bool prevent_proxy_connections{false};
    std::int32_t rate_limit{0};
    std::int32_t player_idle_timeout{0};
};

/// Parse `server.properties` text.
///
/// Fails only on a malformed assignment or an out-of-range typed value; missing
/// keys keep their defaults. Limits: 1 MiB and 4096 lines are enforced by the
/// caller's `read_text` options and this parser's line bound.
[[nodiscard]] auto parse_server_properties(std::string_view text)
    -> result<server_properties>;

} // namespace refrain::server
