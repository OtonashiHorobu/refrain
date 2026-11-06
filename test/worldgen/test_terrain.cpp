// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/density.hpp>
#include <refrain/worldgen/terrain.hpp>

#include <boost/ut.hpp>

#include <array>
#include <cstdint>

namespace {

using namespace boost::ut;
using namespace refrain::worldgen;
using namespace refrain::worldgen::density;
using namespace refrain::worldgen::terrain;

struct fill_settings : fill_options {
    fill_settings() {
        min_y = -64;
        height = 384;
        sea_level = 63;
        blocks = block_ids{
            .air = 0, .default_block = 10, .default_fluid = 20, .lava = 30};
    }
};

auto count(const chunk_blocks &chunk, std::uint16_t state) -> std::size_t {
    std::size_t total = 0;
    for (const auto state_id : chunk.states) {
        if (state_id == state) {
            ++total;
        }
    }
    return total;
}

} // namespace

suite<"worldgen/terrain-fill"> terrain_fill_tests = [] {
    "positive density fills the whole chunk with the default block"_test = [] {
        const fill_settings settings;
        const sampler_context context;
        disabled_aquifer aquifer{fluid_picker{.sea_level = settings.sea_level,
                                              .default_fluid = 20,
                                              .lava = 30,
                                              .air = 0}};
        auto value = density::constant(1.0F);
        const auto chunk = fill_chunk(settings, *value, context, 0, 0, aquifer);
        expect(chunk.states.size() == 384U * 256U);
        expect(count(chunk, 10) == chunk.states.size());
        expect(chunk.world_surface[0] == 319);
        expect(chunk.ocean_floor[0] == 319);
        expect(chunk.at(0, 319, 0) == 10);
        expect(chunk.at(15, -64, 15) == 10);
    };

    "negative density leaves water below sea level and air above"_test = [] {
        const fill_settings settings;
        const sampler_context context;
        disabled_aquifer aquifer{fluid_picker{.sea_level = settings.sea_level,
                                              .default_fluid = 20,
                                              .lava = 30,
                                              .air = 0}};
        auto value = density::constant(-1.0F);
        const auto chunk = fill_chunk(settings, *value, context, 0, 0, aquifer);
        expect(count(chunk, 20) == 117U * 256U);
        expect(count(chunk, 30) == 10U * 256U);
        expect(count(chunk, 10) == 0U);
        expect(chunk.at(0, 62, 0) == 20);
        expect(chunk.at(0, 63, 0) == 0);
        expect(chunk.world_surface[0] == 62);
        expect(chunk.ocean_floor[0] == -65);
    };

    "the ocean-floor heightmap stops at the highest motion-blocking block"_test =
        [] {
            const fill_settings settings;
            const sampler_context context;
            disabled_aquifer aquifer{
                fluid_picker{.sea_level = settings.sea_level,
                             .default_fluid = 20,
                             .lava = 30,
                             .air = 0}};
            auto value = gradient_y(-64, 64, 1.0F, -1.0F);
            const auto chunk =
                fill_chunk(settings, *value, context, 0, 0, aquifer);
            expect(chunk.at(0, -1, 0) == 10);
            expect(chunk.at(0, 0, 0) == 20);
            expect(chunk.at(0, 62, 0) == 20);
            expect(chunk.at(0, 63, 0) == 0);
            expect(chunk.ocean_floor[0] == -1);
            expect(chunk.world_surface[0] == 62);
        };

    "an empty column reports the bottom of the world minus one"_test = [] {
        const fill_settings settings;
        const sampler_context context;
        disabled_aquifer aquifer{fluid_picker{
            .sea_level = -100, .default_fluid = 20, .lava = 30, .air = 0}};
        auto value = density::constant(-1.0F);
        const auto chunk = fill_chunk(settings, *value, context, 0, 0, aquifer);
        expect(count(chunk, 0) == chunk.states.size());
        expect(chunk.ocean_floor[0] == -65);
        expect(chunk.world_surface[0] == -65);
    };

    "the density zero crossing splits stone from air"_test = [] {
        const fill_settings settings;
        const sampler_context context;
        disabled_aquifer aquifer{fluid_picker{.sea_level = settings.sea_level,
                                              .default_fluid = 20,
                                              .lava = 30,
                                              .air = 0}};
        auto value = gradient_y(-64, 320, 1.0F, -1.0F);
        const auto chunk =
            fill_chunk(settings, *value, context, 3, -2, aquifer);
        expect(count(chunk, 10) == 192U * 256U);
        expect(count(chunk, 20) == 0U);
        expect(chunk.at(0, 127, 0) == 10);
        expect(chunk.at(0, 128, 0) == 0);
        expect(chunk.world_surface[0] == 127);
    };

    "the lava column replaces the fluid below -54"_test = [] {
        const fill_settings settings;
        const sampler_context context;
        disabled_aquifer aquifer{fluid_picker{.sea_level = settings.sea_level,
                                              .default_fluid = 20,
                                              .lava = 30,
                                              .air = 0}};
        auto value = density::constant(-1.0F);
        const auto chunk = fill_chunk(settings, *value, context, 0, 0, aquifer);
        expect(chunk.at(0, -55, 0) == 30);
        expect(chunk.at(0, -54, 0) == 20);
        expect(chunk.at(0, -60, 0) == 30);
    };
};
