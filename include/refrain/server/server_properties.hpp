// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

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
[[nodiscard]] auto
parse_server_properties(std::string_view text,
                        std::vector<std::string> *present_keys = nullptr)
    -> result<server_properties>;

/// How import_server_properties() treats keys already present in the target
/// TOML file.
enum class server_properties_import_mode : std::uint8_t {
    /// Fill only keys the target file does not already define.
    merge,
    /// Overwrite mapped keys; unrelated keys stay untouched.
    force,
};

struct server_properties_import_report {
    std::size_t applied{0};
    std::vector<std::string> unknown_keys{};
    std::vector<std::string> written_files{};
};

/// Import a Java `server.properties` document into the per-namespace TOML
/// files under `config_dir` (`network.toml`, `world.toml`).
///
/// Unknown keys are reported in `unknown_keys`; with `strict` the first one
/// aborts the import. Existing files are loaded first so unrelated keys are
/// preserved; `merge` skips keys already set, `force` overwrites them.
[[nodiscard]] auto
import_server_properties(std::string_view text, std::string_view config_dir,
                         server_properties_import_mode mode, bool strict)
    -> result<server_properties_import_report>;

} // namespace refrain::server
