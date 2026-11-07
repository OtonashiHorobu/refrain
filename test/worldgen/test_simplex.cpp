// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Legacy `SimplexNoise` samples and the frozen-temperature / biome-info
// noises. Expected raw float bits come from `SimplexVectors.java`.

#include <refrain/worldgen/noise.hpp>

#include <boost/ut.hpp>

#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

namespace {

using namespace boost::ut;
using namespace refrain::worldgen;

auto bits(float value) -> std::uint32_t {
    return std::bit_cast<std::uint32_t>(value);
}

constexpr std::array<std::uint32_t, 4> expected_2d{
    0x00000000U,
    0x3ea0ac6bU,
    0x3f1c1e88U,
    0x3e0dd172U,
};
constexpr std::array<std::uint32_t, 3> expected_3d{
    0x00000000U,
    0xbe736decU,
    0xbef3816fU,
};
constexpr std::array<std::uint32_t, 2> expected_offset{
    0x3eca351aU,
    0xbe55e5c2U,
};
constexpr std::array<std::uint32_t, 3> expected_frozen{
    0x00000000U,
    0xbe9ff416U,
    0x3e93f300U,
};
constexpr std::array<std::uint32_t, 2> expected_info{
    0xbd8771efU,
    0xbebbdfecU,
};

} // namespace

using namespace boost::ut;

suite<"worldgen/simplex"> simplex_tests = [] {
    "simplex matches the reference samples"_test = [] {
        legacy_random_source random{1234};
        const simplex_noise noise{random, true};
        constexpr std::array<std::array<double, 2>, 4> points_2d{{
            {0.0, 0.0},
            {1.5, -2.25},
            {100.25, 33.5},
            {-7.75, 64.125},
        }};
        for (std::size_t i = 0; i < points_2d.size(); ++i) {
            expect(bits(noise.get(points_2d[i][0], points_2d[i][1])) ==
                   expected_2d[i]);
        }
        constexpr std::array<std::array<double, 3>, 3> points_3d{{
            {0.0, 0.0, 0.0},
            {1.5, -2.25, 3.125},
            {100.25, 33.5, -7.75},
        }};
        for (std::size_t i = 0; i < points_3d.size(); ++i) {
            expect(bits(noise.get(points_3d[i][0], points_3d[i][1],
                                  points_3d[i][2])) == expected_3d[i]);
        }

        legacy_random_source offset_random{42};
        const simplex_noise offset_noise{offset_random, false};
        expect(bits(offset_noise.get(1.5, -2.25)) == expected_offset[0]);
        expect(bits(offset_noise.get(1.5, -2.25, 3.125)) == expected_offset[1]);
    };

    "the frozen-temperature stack and biome-info noise match"_test = [] {
        legacy_random_source random{3456};
        std::vector<noise_stack::layer> layers;
        layers.push_back(
            {std::make_unique<simplex_noise>(random, true), 1.0, 0.14285715F});
        layers.push_back(
            {std::make_unique<simplex_noise>(random, true), 0.5, 0.2857143F});
        layers.push_back(
            {std::make_unique<simplex_noise>(random, true), 0.25, 0.5714286F});
        noise_stack stack{std::move(layers)};
        constexpr std::array<std::array<double, 3>, 3> points{{
            {0.0, 0.0, 0.0},
            {1.5, -2.25, 3.125},
            {100.25, 33.5, -7.75},
        }};
        for (std::size_t i = 0; i < points.size(); ++i) {
            expect(bits(stack.get(points[i][0], points[i][1], points[i][2])) ==
                   expected_frozen[i]);
        }

        legacy_random_source info_random{2345};
        const simplex_noise info{info_random, true};
        expect(bits(info.get(4.0, 7.0)) == expected_info[0]);
        expect(bits(info.get(-8.0, 3.0)) == expected_info[1]);
    };
};
