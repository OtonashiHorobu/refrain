// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/command/dispatcher.hpp>
#include <refrain/net/server.hpp>
#include <refrain/server/world_port.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace refrain::app {
class mod_management;
} // namespace refrain::app

namespace refrain::server {

/// Stable permission key derived from a player profile uuid. The command
/// bridge uses it as the command source identity, and the in-game mod command
/// compares it against the configured operator list.
[[nodiscard]] auto player_identity_key(const net::uuid &id) -> std::uint64_t;

/// Shared state between the runtime and the network command bridge. The
/// listener pointer is installed after the network server has started.
struct command_environment {
    refrain::command::dispatcher dispatcher{};
    refrain::net::server *listener{nullptr};
    world_port *world{nullptr};

    /// Wire-tree snapshots keyed by dispatcher revision, permission level and
    /// mod-admin grant. A tree is filtered by permission, so players with
    /// different grants must never share one snapshot.
    mutable std::mutex tree_cache_mutex{};
    std::unordered_map<std::uint64_t,
                       std::shared_ptr<const refrain::net::command_tree>>
        tree_cache{};

    /// Mod management application service shared with the server console. The
    /// `/mods` tree calls it so both frontends report the same results; null
    /// disables the in-game tree.
    refrain::app::mod_management *mods{nullptr};

    /// Permission gate of the in-game mod command tree. The console command
    /// dispatcher does not use it. Empty means no player may use `/mods`.
    std::function<bool(const refrain::command::source &)> mod_admin{};

    /// Development default until a permission service exists. It allows the
    /// built-in gameplay commands for every player.
    refrain::command::permission_level permission{
        refrain::command::level::admin};

    mutable std::mutex mode_mutex{};
    std::unordered_map<std::uint64_t, net::game_mode> modes{};
    std::unordered_map<std::uint64_t, std::uint64_t> key_to_connection{};
    std::unordered_map<std::uint64_t, std::string> identity_uuids{};
    net::game_mode default_mode{net::game_mode::creative};

    [[nodiscard]] auto join_state(std::uint64_t connection) const
        -> net::join_state;
    [[nodiscard]] auto mode_of(std::uint64_t connection) const
        -> net::game_mode;
    void remember_key(std::uint64_t key, std::uint64_t connection,
                      std::string uuid);
    [[nodiscard]] auto connection_for(std::uint64_t key) const
        -> std::optional<std::uint64_t>;
    /// Canonical uuid of the player this identity key was first resolved for;
    /// `nullopt` for a source the bridge never saw join.
    [[nodiscard]] auto uuid_for(std::uint64_t key) const
        -> std::optional<std::string>;
};

/// Register the commands implemented by the server runtime.
void register_runtime_commands(
    std::shared_ptr<command_environment> environment);

/// Build the network bridge that forwards parsed commands to the runtime
/// dispatcher and converts its immutable tree/suggestions to wire types.
[[nodiscard]] auto
make_command_bridge(std::shared_ptr<command_environment> environment)
    -> refrain::net::command_bridge;

} // namespace refrain::server
