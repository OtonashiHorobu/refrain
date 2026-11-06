// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/terrain.hpp>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace refrain::worldgen::terrain {

auto fill_chunk(const fill_options &options,
                const density::sampler &final_density,
                const density::sampler_context &context, jint chunk_x,
                jint chunk_z, aquifer &aquifer) -> chunk_blocks {
    chunk_blocks chunk;
    chunk.min_y = options.min_y;
    chunk.height = options.height;
    chunk.states.assign(static_cast<std::size_t>(options.height) * 256U,
                        options.blocks.air);
    // An untouched heightmap reads `min_y` as its first available slot, so the
    // highest taken block of an empty column is one below the bottom.
    const jint empty_height = options.min_y - 1;
    chunk.ocean_floor.fill(empty_height);
    chunk.world_surface.fill(empty_height);

    const volume vol{chunk_width,  options.height, chunk_depth,
                     chunk_x * 16, options.min_y,  chunk_z * 16};
    buffer density_buffer{vol.size()};
    final_density.sample_volume(context, density_buffer, vol);

    for (jint z = 0; z < vol.size_z(); ++z) {
        const jint block_z = vol.block_z(z);
        for (jint x = 0; x < vol.size_x(); ++x) {
            const jint block_x = vol.block_x(x);
            for (jint y = vol.size_y() - 1; y >= 0; --y) {
                const jint block_y = vol.block_y(y);
                const float density =
                    density_buffer.get(vol.index_unchecked(x, y, z));
                auto state = aquifer.compute_substance(
                    block_x, block_y, block_z, static_cast<double>(density));
                if (!state.has_value()) {
                    state = options.blocks.default_block;
                }
                if (*state == options.blocks.air) {
                    continue;
                }
                chunk.set(x, block_y, z, *state);
                const auto column = static_cast<std::size_t>(z * 16 + x);
                if (*state != options.blocks.default_fluid &&
                    *state != options.blocks.lava) {
                    chunk.ocean_floor[column] =
                        std::max(chunk.ocean_floor[column], block_y);
                }
                chunk.world_surface[column] =
                    std::max(chunk.world_surface[column], block_y);
            }
        }
    }
    return chunk;
}

} // namespace refrain::worldgen::terrain
