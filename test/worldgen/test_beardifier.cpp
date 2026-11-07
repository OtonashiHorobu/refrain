// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Synthetic `Beardifier` samples and kernel entries. The expected raw float
// bits come from `BeardifierVectors.java`.

#include <refrain/worldgen/density/density.hpp>
#include <refrain/worldgen/terrain/beardifier.hpp>

#include <boost/ut.hpp>

#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <vector>

namespace {

using namespace boost::ut;
using namespace refrain::worldgen;

auto bits(float value) -> std::uint32_t {
    return std::bit_cast<std::uint32_t>(value);
}

auto make_beardifier() -> beardifier {
    return beardifier{
        std::vector<beardifier_rigid>{
            beardifier_rigid{.box = {0, 60, 0, 15, 70, 15},
                             .adjustment = terrain_adjustment::bury,
                             .ground_level_delta = 2},
            beardifier_rigid{.box = {-10, 50, -10, -1, 60, -1},
                             .adjustment = terrain_adjustment::beard_thin,
                             .ground_level_delta = 0},
            beardifier_rigid{.box = {20, 64, 20, 30, 74, 30},
                             .adjustment = terrain_adjustment::beard_box,
                             .ground_level_delta = 3},
            beardifier_rigid{.box = {5, 40, 5, 12, 55, 12},
                             .adjustment = terrain_adjustment::encapsulate,
                             .ground_level_delta = 0},
            beardifier_rigid{.box = {40, 60, 40, 50, 70, 50},
                             .adjustment = terrain_adjustment::none,
                             .ground_level_delta = 0}},
        std::vector<jigsaw_junction>{jigsaw_junction{8, 64, 8},
                                     jigsaw_junction{-8, 55, -8},
                                     jigsaw_junction{25, 70, 25}},
        std::optional<bounding_box>{
            bounding_box{0, 40, 0, 50, 74, 50}.inflated_by(24)}};
}

constexpr std::array<std::uint32_t, 10> expected_points{
    0x3f41441aU, 0x3f871a2fU, 0x3ea51971U, 0xbdc440f9U, 0xbf55a040U,
    0x3f4cccceU, 0x00000000U, 0xb39c90e3U, 0x00000000U, 0x00000000U,
};

constexpr std::array<std::uint32_t, 36> expected_volume{
    0xbc538f55U, 0xbb8ef98aU, 0xba6dcbbdU, 0xbce4577aU, 0xbc227e17U,
    0xbb076663U, 0xbd15a909U, 0xbc571f7cU, 0xbb32ab35U, 0xbce4577aU,
    0xbc227e17U, 0xbb076663U, 0xbce4577aU, 0xbc227e17U, 0xbb076663U,
    0xbd8b83b9U, 0xbcc16682U, 0xbb9c53f9U, 0xbdc2bebbU, 0xbd024f7fU,
    0xbbcf2d3dU, 0xbd8b83b9U, 0xbcc16682U, 0xbb9c53f9U, 0xbd15a909U,
    0xbc571f7cU, 0xbb32ab35U, 0xbdc2bebbU, 0xbd024f7fU, 0xbbcf2d3dU,
    0xbe0cd19dU, 0xbd30f7e7U, 0xbc0994f7U, 0xbdc2bebbU, 0xbd024f7fU,
    0xbbcf2d3dU,
};

constexpr std::array<std::uint32_t, 5> expected_kernel{
    0x2c89d3acU, 0x3f7c07f5U, 0x3387902bU, 0x3406ccbdU, 0x3bf64ed8U,
};

} // namespace

suite<"worldgen/beardifier"> beardifier_tests = [] {
    "sample values and kernel entries match the reference"_test = [] {
        const auto beard = make_beardifier();
        const density::sampler_context context;
        constexpr std::array<std::array<jint, 3>, 10> points{{
            {8, 64, 8},
            {0, 60, 0},
            {15, 70, 15},
            {-5, 55, -5},
            {25, 70, 25},
            {8, 40, 8},
            {45, 65, 45},
            {-20, 50, -20},
            {30, 90, 30},
            {12, 0, 12},
        }};
        for (std::size_t i = 0; i < points.size(); ++i) {
            const auto value = beard.sample_value(context, points[i][0],
                                                  points[i][1], points[i][2]);
            if (bits(value) != expected_points[i]) {
                std::fprintf(stderr, "point %zu: got %08x want %08x\n", i,
                             bits(value), expected_points[i]);
            }
            expect(bits(value) == expected_points[i]);
        }

        const volume vol{4, 3, 3, -12, 58, -12, 2, 2, 2};
        buffer output{vol.size()};
        output.fill(0.0F);
        beard.sample_volume(context, output, vol);
        for (std::size_t i = 0; i < expected_volume.size(); ++i) {
            if (bits(output.get(i)) != expected_volume[i]) {
                std::fprintf(stderr, "volume %zu: got %08x want %08x\n", i,
                             bits(output.get(i)), expected_volume[i]);
            }
            expect(bits(output.get(i)) == expected_volume[i]);
        }

        constexpr std::array<std::array<jint, 3>, 5> entries{{
            {0, 0, 0},
            {12, 12, 12},
            {0, 12, 23},
            {23, 0, 11},
            {5, 7, 9},
        }};
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const auto value =
                beard.kernel_entry(entries[i][0], entries[i][1], entries[i][2]);
            expect(bits(value) == expected_kernel[i]);
        }
    };

    "the empty beardifier contributes zero"_test = [] {
        const auto beard = beardifier::empty();
        const density::sampler_context context;
        expect(beard->sample_value(context, 8, 64, 8) == 0.0F);
        const volume vol{2, 2, 2, 0, 0, 0};
        buffer output{vol.size()};
        output.fill(1.0F);
        beard->sample_volume(context, output, vol);
        for (std::size_t i = 0; i < output.size(); ++i) {
            expect(output.get(i) == 0.0F);
        }
    };

    "the context binds a beardifier sampler"_test = [] {
        const auto beard = make_beardifier();
        const auto sampler = density::beardifier();
        density::sampler_context context;
        expect(sampler->sample_value(context, 8, 64, 8) == 0.0F);
        context.set_field(density::context_key::beardifier, &beard);
        expect(bits(sampler->sample_value(context, 8, 64, 8)) ==
               expected_points[0]);
    };
};
