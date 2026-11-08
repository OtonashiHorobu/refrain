// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/biome/nether_biome_source.hpp>

#include <boost/ut.hpp>

namespace {
using namespace boost::ut;
using namespace refrain::worldgen;

auto target(float temperature, float humidity, float weirdness)
    -> climate::target_point {
    return climate::target_point{
        .temperature = climate::quantize(temperature),
        .humidity = climate::quantize(humidity),
        .continentalness = 0,
        .erosion = 0,
        .depth = 0,
        .weirdness = climate::quantize(weirdness),
    };
}

suite<"worldgen/nether-biome-source"> nether_biome_source_tests = [] {
    "the five nether parameter points resolve by climate"_test = [] {
        multi_noise_biome_source source{nether_biome_parameters()};
        expect(source.biome(source.biome_at(target(0.0F, 0.0F, 0.0F))) ==
               "minecraft:nether_wastes");
        expect(source.biome(source.biome_at(target(0.0F, -0.5F, 0.0F))) ==
               "minecraft:soul_sand_valley");
        expect(source.biome(source.biome_at(target(0.4F, 0.0F, 0.0F))) ==
               "minecraft:crimson_forest");
        expect(source.biome(source.biome_at(target(0.0F, 0.5F, 0.375F))) ==
               "minecraft:warped_forest");
        expect(source.biome(source.biome_at(target(-0.5F, 0.0F, 0.175F))) ==
               "minecraft:basalt_deltas");
    };

    "the nether source interns five biomes in table order"_test = [] {
        multi_noise_biome_source source{nether_biome_parameters()};
        expect(source.biomes().size() == 5);
        expect(source.biomes()[0] == "minecraft:nether_wastes");
        expect(source.biomes()[4] == "minecraft:basalt_deltas");
    };
};
} // namespace
