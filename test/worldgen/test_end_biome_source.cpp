// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/biome/end_biome_source.hpp>

#include <refrain/worldgen/density/density.hpp>

#include <boost/ut.hpp>
#include <memory>

namespace {
using namespace boost::ut;
using namespace refrain::worldgen;

auto make_sampler(float erosion, std::array<density::sampler_ptr, 5> &others,
                  density::sampler_ptr &erosion_sampler)
    -> std::unique_ptr<climate_sampler> {
    for (auto &value : others) {
        value = density::constant(0.0F);
    }
    erosion_sampler = density::constant(erosion);
    return std::make_unique<climate_sampler>(
        *others[0], *others[1], *others[2], *erosion_sampler, *others[3],
        *others[4], density::sampler_context{});
}

suite<"worldgen/end-biome-source"> end_biome_source_tests = [] {
    "the central island uses the end biome"_test = [] {
        const the_end_biome_source source;
        std::array<density::sampler_ptr, 5> others;
        density::sampler_ptr erosion;
        auto sampler = make_sampler(0.9F, others, erosion);
        expect(source.noise_biome(*sampler, 0, 16, 0) == 0);
        expect(source.biome(0) == "minecraft:the_end");
    };

    "the outer bands follow the erosion thresholds"_test = [] {
        const the_end_biome_source source;
        std::array<density::sampler_ptr, 5> others;
        density::sampler_ptr erosion;
        auto high = make_sampler(0.5F, others, erosion);
        expect(source.noise_biome(*high, 400, 16, 0) == 1);
        auto mid = make_sampler(0.0F, others, erosion);
        expect(source.noise_biome(*mid, 400, 16, 0) == 2);
        auto islands = make_sampler(-0.5F, others, erosion);
        expect(source.noise_biome(*islands, 400, 16, 0) == 3);
        auto barrens = make_sampler(-0.1F, others, erosion);
        expect(source.noise_biome(*barrens, 400, 16, 0) == 4);
    };

    "the possible biomes keep the reference order"_test = [] {
        const the_end_biome_source source;
        expect(source.biomes().size() == 5);
        expect(source.biome(1) == "minecraft:end_highlands");
        expect(source.biome(2) == "minecraft:end_midlands");
        expect(source.biome(3) == "minecraft:small_end_islands");
        expect(source.biome(4) == "minecraft:end_barrens");
    };
};
} // namespace
