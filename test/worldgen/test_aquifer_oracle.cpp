// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Synthetic aquifer decisions generated from the real 26.3 class with
// `tools/worldgen/vectors/run.sh <server.jar> AquiferOracle`.
// Codes: -1 default block, 0 air, 2 water, 3 lava, 8 other.

#include <refrain/worldgen/aquifer.hpp>
#include <refrain/worldgen/density.hpp>
#include <refrain/worldgen/rng.hpp>
#include <refrain/worldgen/terrain.hpp>

#include <boost/ut.hpp>

#include <array>
#include <cstdint>
#include <memory>

namespace {

using namespace boost::ut;
using namespace refrain::worldgen;
using namespace refrain::worldgen::density;
using namespace refrain::worldgen::terrain;

auto code_of(const std::optional<std::uint16_t> &state) -> int {
    if (!state.has_value()) {
        return -1;
    }
    if (*state == 0) {
        return 0;
    }
    if (*state == 2) {
        return 2;
    }
    if (*state == 3) {
        return 3;
    }
    return 8;
}

} // namespace

suite<"worldgen/aquifer-oracle"> aquifer_oracle_tests = [] {
    "synthetic aquifer decisions match the reference"_test = [] {
        aquifer_config config;
        config.barrier = gradient_y(-64, 320, -1.0F, 1.0F);
        config.fluid_level_floodedness = gradient_y(-64, 320, 1.0F, -1.0F);
        config.fluid_level_spread = gradient_y(-64, 320, 0.5F, -0.5F);
        config.lava = density::constant(0.5F);
        config.exclusion = density::constant(-1.0F);
        config.surface_level = density::constant(30.0F);

        auto factory = new_positional_factory(algorithm::xoroshiro, 12345);
        const volume vol{16, 384, 16, 0, -64, 0};
        const fluid_picker picker{
            .sea_level = 63, .default_fluid = 2, .lava = 3, .air = 0};
        const block_ids blocks{
            .air = 0, .default_block = 10, .default_fluid = 2, .lava = 3};
        const sampler_context context;
        noise_aquifer aquifer{config, *factory, vol, picker, blocks, context};

        constexpr std::array<int, 3> xs{0, 8, 15};
        constexpr std::array<int, 3> zs{0, 8, 15};
        constexpr std::array<int, 5> ys{-60, -32, 0, 40, 70};
        constexpr std::array<double, 3> densities{-1.0, 0.0, 0.5};
        constexpr std::array<int, 135> expected_codes{
            3,  3,  -1, 2,  2,  -1, 2,  2,  -1, 2,  2,  -1, 0,  0,  -1, 3,  3,
            -1, 2,  2,  -1, 2,  2,  -1, 2,  2,  -1, 0,  0,  -1, 3,  3,  -1, 2,
            2,  -1, 2,  2,  -1, 2,  2,  -1, 0,  0,  -1, 3,  3,  -1, 2,  2,  -1,
            2,  2,  -1, 2,  2,  -1, 0,  0,  -1, 3,  3,  -1, 2,  2,  -1, 2,  2,
            -1, 2,  2,  -1, 0,  0,  -1, 3,  3,  -1, 2,  2,  -1, 2,  2,  -1, 2,
            2,  -1, 0,  0,  -1, 3,  3,  -1, 2,  2,  -1, 2,  2,  -1, 2,  2,  -1,
            0,  0,  -1, 3,  3,  -1, 2,  2,  -1, 2,  2,  -1, 2,  2,  -1, 0,  0,
            -1, 3,  3,  -1, 2,  2,  -1, 2,  2,  -1, 2,  2,  -1, 0,  0,  -1};
        constexpr std::array<int, 135> expected_flags{
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

        std::size_t index = 0;
        for (const int x : xs) {
            for (const int z : zs) {
                for (const int y : ys) {
                    for (const double density : densities) {
                        const auto state =
                            aquifer.compute_substance(x, y, z, density);
                        expect(code_of(state) == expected_codes[index])
                            << "point " << index;
                        expect(
                            (aquifer.should_schedule_fluid_update() ? 1 : 0) ==
                            expected_flags[index])
                            << "flag " << index;
                        ++index;
                    }
                }
            }
        }
    };
};
