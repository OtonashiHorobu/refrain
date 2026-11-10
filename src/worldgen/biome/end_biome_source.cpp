// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/biome/end_biome_source.hpp>

namespace refrain::worldgen {

namespace {

constexpr jint central_island_radius = 4096;
constexpr float highlands_threshold = 0.25F;
constexpr float midlands_threshold = -0.0625F;
constexpr float islands_threshold = -0.21875F;

} // namespace

the_end_biome_source::the_end_biome_source()
    : biomes_{"minecraft:the_end", "minecraft:end_highlands",
              "minecraft:end_midlands", "minecraft:small_end_islands",
              "minecraft:end_barrens"} {}

auto the_end_biome_source::noise_biome(const climate_sampler &sampler,
                                       jint quart_x, jint quart_y,
                                       jint quart_z) const -> std::uint16_t {
    const jint block_x = quart_x * 4;
    const jint block_y = quart_y * 4;
    const jint block_z = quart_z * 4;
    const jint chunk_x = block_x >> 4;
    const jint chunk_z = block_z >> 4;
    if (static_cast<jlong>(chunk_x) * chunk_x +
            static_cast<jlong>(chunk_z) * chunk_z <=
        central_island_radius) {
        return 0;
    }
    const jint weird_x = (chunk_x * 2 + 1) * 8;
    const jint weird_z = (chunk_z * 2 + 1) * 8;
    const float erosion = sampler.samplers()[3]->sample_value(
        sampler.context(), weird_x, block_y, weird_z);
    if (erosion > highlands_threshold) {
        return 1;
    }
    if (erosion >= midlands_threshold) {
        return 2;
    }
    return erosion < islands_threshold ? 3 : 4;
}

} // namespace refrain::worldgen
