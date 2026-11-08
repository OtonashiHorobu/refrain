// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/command/dispatcher.hpp>
#include <refrain/net/server.hpp>

#include <memory>

namespace refrain::server {

/// Shared state between the runtime and the network command bridge. The
/// listener pointer is installed after the network server has started.
struct command_environment {
    refrain::command::dispatcher dispatcher{};
    refrain::net::server *listener{nullptr};
    std::shared_ptr<const refrain::net::command_tree> tree_cache{};
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
