// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/command/dispatcher.hpp>
#include <refrain/net/server.hpp>
#include <refrain/world/store.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace refrain::server {

/// Shared state between the runtime and the network command bridge. The
/// listener pointer is installed after the network server has started.
struct command_environment {
    refrain::command::dispatcher dispatcher{};
    refrain::net::server *listener{nullptr};
    refrain::world::world_store *world{nullptr};
    std::shared_ptr<const refrain::net::command_tree> tree_cache{};

    /// Development default until a permission service exists. It allows the
    /// built-in gameplay commands for every player.
    refrain::command::permission_level permission{
        refrain::command::level::gamemode};

    mutable std::mutex mode_mutex{};
    std::unordered_map<std::uint64_t, net::game_mode> modes{};
    std::unordered_map<std::uint64_t, std::uint64_t> key_to_connection{};
    net::game_mode default_mode{net::game_mode::creative};

    [[nodiscard]] auto join_state(std::uint64_t connection) const
        -> net::join_state;
    [[nodiscard]] auto mode_of(std::uint64_t connection) const
        -> net::game_mode;
    void remember_key(std::uint64_t key, std::uint64_t connection);
    [[nodiscard]] auto connection_for(std::uint64_t key) const
        -> std::optional<std::uint64_t>;
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
