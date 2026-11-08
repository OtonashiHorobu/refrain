// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The generation driver. It owns the status order and the heightmap
// transitions; the stage bodies are supplied by the caller so the driver can
// hold the single-threaded skeleton before the concrete stages are wired.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/worldgen/chunk/chunk_region.hpp>
#include <refrain/worldgen/chunk/proto_chunk.hpp>

#include <functional>
#include <memory>

namespace refrain::worldgen {

struct generator_stages {
    /// Fills the 4x4x4 quart biome grid before any blocks are written; surface
    /// material rules read biomes back from the chunk.
    std::function<status(proto_chunk &)> biomes{};
    std::function<void(proto_chunk &)> fill{};
    std::function<status(proto_chunk &)> surface{};
    std::function<status(proto_chunk &)> carve{};
    /// Structure placement for the target chunk, run before features.
    std::function<status(proto_chunk &)> structures{};
    /// Feature decoration for the target chunk.
    std::function<status(proto_chunk &)> features{};
};

class generator {
  public:
    generator(const block_state_table &states, jint min_y, jint height,
              generator_stages stages);

    /// Generates one chunk to `terrain`: biomes, fill, surface, carve, then
    /// prime the four final heightmaps.
    [[nodiscard]] auto generate_terrain(const chunk_pos &pos)
        -> result<std::unique_ptr<proto_chunk>>;

    /// Status-driven entry point. Only `terrain` is implemented while the
    /// later stages are being wired.
    [[nodiscard]] auto generate(const chunk_pos &pos, chunk_status target)
        -> result<std::unique_ptr<proto_chunk>>;

    [[nodiscard]] auto min_y() const noexcept -> jint { return min_y_; }
    [[nodiscard]] auto height() const noexcept -> jint { return height_; }

  private:
    const block_state_table *states_{nullptr};
    jint min_y_{0};
    jint height_{0};
    generator_stages stages_{};
};

} // namespace refrain::worldgen
