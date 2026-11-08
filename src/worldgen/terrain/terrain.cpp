// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/terrain/terrain.hpp>

#include <refrain/worldgen/terrain/aquifer.hpp>

#include <algorithm>
#include <memory>
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
    chunk.post_processing.resize(
        static_cast<std::size_t>((options.height + 15) / 16));

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
                if (aquifer.should_schedule_fluid_update() &&
                    (*state == options.blocks.default_fluid ||
                     *state == options.blocks.lava)) {
                    const jint local_y = block_y - options.min_y;
                    const auto section = static_cast<std::size_t>(local_y >> 4);
                    const auto packed = static_cast<std::uint16_t>(
                        (x & 15) | ((local_y & 15) << 4) | ((z & 15) << 8));
                    chunk.post_processing[section].push_back(packed);
                }
            }
        }
    }
    return chunk;
}

auto first_occupied_height(const column_height_options &options,
                           const density::sampler &final_density,
                           const density::sampler_context &context,
                           jint block_x, jint block_z,
                           const std::function<bool(std::uint16_t)> &predicate)
    -> jint {
    const volume vol{1, options.height, 1, block_x, options.min_y, block_z};
    buffer density_buffer{vol.size()};
    final_density.sample_volume(context, density_buffer, vol);

    const fluid_picker picker{.sea_level = options.sea_level,
                              .default_fluid = options.blocks.default_fluid,
                              .lava = options.blocks.lava,
                              .air = options.blocks.air};
    std::unique_ptr<aquifer> fluids;
    if (options.aquifer != nullptr) {
        if (options.factory == nullptr) {
            throw std::invalid_argument{
                "a noise aquifer requires a positional random factory"};
        }
        fluids = std::make_unique<noise_aquifer>(*options.aquifer,
                                                 *options.factory, vol, picker,
                                                 options.blocks, context);
    } else {
        fluids = std::make_unique<disabled_aquifer>(picker);
    }

    for (jint y = vol.size_y() - 1; y >= 0; --y) {
        const jint block_y = vol.block_y(y);
        const float density = density_buffer.get(vol.index_unchecked(0, y, 0));
        auto state = fluids->compute_substance(block_x, block_y, block_z,
                                               static_cast<double>(density));
        if (!state.has_value()) {
            state = options.blocks.default_block;
        }
        if (predicate(*state)) {
            return block_y;
        }
    }
    return options.min_y - 1;
}

void fill_proto_chunk(proto_chunk &chunk, const proto_fill_options &options,
                      const density::sampler &final_density,
                      const density::sampler_context &context,
                      aquifer &aquifer) {
    fill_proto_chunk(
        chunk, options,
        proto_fill_volume{.min_y = chunk.min_y(), .height = chunk.height()},
        final_density, context, aquifer);
}

void fill_proto_chunk(proto_chunk &chunk, const proto_fill_options &options,
                      const proto_fill_volume &volume_settings,
                      const density::sampler &final_density,
                      const density::sampler_context &context,
                      aquifer &aquifer) {
    const volume vol{chunk_width,           volume_settings.height,
                     chunk_depth,           chunk.chunk_x() * 16,
                     volume_settings.min_y, chunk.chunk_z() * 16};
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
                    state = options.default_block;
                }
                if (*state == options.air) {
                    continue;
                }
                chunk.set_state(x, block_y, z, *state);
                if (aquifer.should_schedule_fluid_update() &&
                    chunk.table().traits(*state).fluid) {
                    chunk.mark_post_processing(x, block_y, z);
                }
            }
        }
    }
}

} // namespace refrain::worldgen::terrain
