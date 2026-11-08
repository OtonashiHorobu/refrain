// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/biome/nether_biome_source.hpp>

namespace refrain::worldgen {

namespace {

auto point(float value) -> climate::parameter {
    return climate::point(climate::quantize(value));
}

auto parameters(float temperature, float humidity, float continentalness,
                float erosion, float depth, float weirdness, float offset,
                const char *biome) -> biome_parameter {
    return biome_parameter{
        .parameters =
            climate::parameter_point{
                .temperature = point(temperature),
                .humidity = point(humidity),
                .continentalness = point(continentalness),
                .erosion = point(erosion),
                .depth = point(depth),
                .weirdness = point(weirdness),
                .offset = climate::quantize(offset),
            },
        .biome = biome,
    };
}

} // namespace

auto nether_biome_parameters() -> std::vector<biome_parameter> {
    return {
        parameters(0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F,
                   "minecraft:nether_wastes"),
        parameters(0.0F, -0.5F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F,
                   "minecraft:soul_sand_valley"),
        parameters(0.4F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F,
                   "minecraft:crimson_forest"),
        parameters(0.0F, 0.5F, 0.0F, 0.0F, 0.0F, 0.375F, 0.0F,
                   "minecraft:warped_forest"),
        parameters(-0.5F, 0.0F, 0.0F, 0.0F, 0.0F, 0.175F, 0.0F,
                   "minecraft:basalt_deltas"),
    };
}

} // namespace refrain::worldgen
