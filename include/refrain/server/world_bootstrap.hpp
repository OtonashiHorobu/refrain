// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/world/model.hpp>
#include <refrain/world/store.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

namespace refrain::server {

struct world_bootstrap_config {
    /// Datapack `data` directory the generator reads.
    std::string data_root{};
    /// Extracted structure templates; an empty root still generates
    /// procedural pieces and features.
    std::string templates_root{};
    std::int64_t seed{0};
    /// Chunk radius around the spawn chunk. Zero generates the spawn chunk
    /// only.
    std::int32_t radius{3};
    /// Generator workers; zero selects the hardware concurrency.
    std::size_t workers{0};
};

/// Generate the spawn area of a freshly created world.
///
/// Writes every chunk in the square around `center` that the store does not
/// already hold, so a new world can serve joins. Chunks that already exist are
/// left untouched, which makes the call safe to repeat after an interrupted
/// start.
[[nodiscard]] auto generate_spawn_area(refrain::world::world_store &store,
                                       const world_bootstrap_config &config,
                                       refrain::world::chunk_pos center)
    -> result<std::size_t>;

} // namespace refrain::server
