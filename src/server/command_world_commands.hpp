// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/server/command_bridge.hpp>

#include <memory>

namespace refrain::server {

/// Register the world and player commands of the runtime tree: `/tp` (also
/// `/teleport`), `/setblock`, `/fill`, `/clone`, `/give`, `/me`, `/msg` (also
/// `/tell` and `/w`) and `/save-all`.
///
/// The registrations own no state of their own: they read the shared
/// `command_environment`, so the world provider and the network listener are
/// the ones the runtime installed.
void register_world_commands(std::shared_ptr<command_environment> environment);

} // namespace refrain::server
