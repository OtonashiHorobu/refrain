// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Vectors generated from the real 26.3 classes with
// `tools/worldgen/vectors/run.sh <server.jar> AquiferVectors`.

#include <refrain/worldgen/aquifer.hpp>
#include <refrain/worldgen/rng.hpp>

#include <boost/ut.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>

namespace {

using namespace boost::ut;
using namespace refrain::worldgen;
using namespace refrain::worldgen::terrain;

auto bits(double value) -> std::uint64_t {
    std::uint64_t out = 0;
    std::memcpy(&out, &value, sizeof(out));
    return out;
}

} // namespace

suite<"worldgen/aquifer-grid"> aquifer_grid_tests = [] {
    "grid conversions use shift and floor division"_test = [] {
        expect(aquifer_grid::grid_x(0) == 0);
        expect(aquifer_grid::grid_x(15) == 0);
        expect(aquifer_grid::grid_x(16) == 1);
        expect(aquifer_grid::grid_x(-1) == -1);
        expect(aquifer_grid::grid_y(0) == 0);
        expect(aquifer_grid::grid_y(11) == 0);
        expect(aquifer_grid::grid_y(12) == 1);
        expect(aquifer_grid::grid_y(-1) == -1);
        expect(aquifer_grid::grid_y(-12) == -1);
        expect(aquifer_grid::grid_y(-13) == -2);
        expect(aquifer_grid::from_grid_x(-3, 8) == -40);
        expect(aquifer_grid::from_grid_y(-60, 4) == -716);
        expect(aquifer_grid::from_grid_z(100, 2) == 1602);
    };

    "cell centers match the positional streams"_test = [] {
        auto factory = new_positional_factory(algorithm::xoroshiro, 12345);
        constexpr std::array<std::array<jint, 3>, 4> cells{{
            {{0, 0, 0}},
            {{1, -1, 2}},
            {{-3, 5, -7}},
            {{100, -60, -100}},
        }};
        constexpr std::array<std::array<jint, 3>, 4> expected{{
            {{2, 8, 5}},
            {{18, -8, 33}},
            {{-40, 66, -106}},
            {{1601, -716, -1598}},
        }};
        for (std::size_t i = 0; i < cells.size(); ++i) {
            const auto center = aquifer_grid::center(*factory, cells[i][0],
                                                     cells[i][1], cells[i][2]);
            expect(center.x == expected[i][0]) << "cell " << i << " x";
            expect(center.y == expected[i][1]) << "cell " << i << " y";
            expect(center.z == expected[i][2]) << "cell " << i << " z";
        }
    };

    "similarity matches the reference formula"_test = [] {
        constexpr std::array<std::array<jint, 2>, 4> inputs{{
            {{3, 4}},
            {{10, 16}},
            {{17, 28}},
            {{24, 40}},
        }};
        constexpr std::array<std::uint64_t, 4> expected{{
            0x3feeb851eb851eb8ULL,
            0x3fe851eb851eb852ULL,
            0x3fe1eb851eb851ecULL,
            0x3fd70a3d70a3d70aULL,
        }};
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            expect(bits(aquifer_grid::similarity(inputs[i][0], inputs[i][1])) ==
                   expected[i])
                << "similarity " << i;
        }
    };
};
