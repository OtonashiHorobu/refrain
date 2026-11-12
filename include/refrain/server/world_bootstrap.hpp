// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/server/world_port.hpp>
#include <refrain/world/model.hpp>

#include <cstddef>

namespace refrain::server {

/// Generate the spawn area of a freshly created world.
///
/// Writes every chunk in the square around `center` that the port's store does
/// not already hold, so a new world can serve joins. Chunks that already exist
/// are left untouched, which makes the call safe to repeat after an interrupted
/// start.
[[nodiscard]] auto generate_spawn_area(world_port &world,
                                       const world_bootstrap_config &config,
                                       refrain::world::chunk_pos center)
    -> result<std::size_t>;

} // namespace refrain::server
