// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/json.hpp>
#include <refrain/worldgen/structure/placement.hpp>

#include <boost/ut.hpp>

namespace {

using refrain::worldgen::structure::frequency_reduction_method;
using refrain::worldgen::structure::random_spread_placement;
using refrain::worldgen::structure::spread_type;

} // namespace

using namespace boost::ut;
using namespace refrain::worldgen;

suite<"worldgen/structure-placement"> structure_placement_tests = [] {
    "random spread potential chunks match the reference vectors"_test = [] {
        const random_spread_placement linear{.spacing = 32,
                                             .separation = 8,
                                             .salt = 12345,
                                             .spread = spread_type::linear};
        const auto check = [&](jlong seed, jint chunk_x, jint chunk_z,
                               jint expected_x, jint expected_z) {
            const auto chunk = linear.potential_chunk(seed, chunk_x, chunk_z);
            expect(chunk.x == expected_x);
            expect(chunk.z == expected_z);
        };
        check(12345, 0, 3, 20, 7);
        check(12345, 5, 13, 20, 7);
        check(12345, 31, 65, 7, 87);
        check(12345, 32, 67, 41, 81);
        check(12345, -1, 1, -13, 2);
        check(12345, -33, -63, -60, -62);
        check(-1, 0, 3, 9, 8);
        check(-1, 31, 65, 5, 79);

        const random_spread_placement triangular{.spacing = 32,
                                                 .separation = 8,
                                                 .salt = 777,
                                                 .spread =
                                                     spread_type::triangular};
        const auto chunk = triangular.potential_chunk(12345, 7, -9);
        expect(chunk.x == 12);
        expect(chunk.z == -14);
    };

    "frequency reduction methods match the reference vectors"_test = [] {
        const auto check = [](frequency_reduction_method method,
                              bool expected) {
            random_spread_placement placement{.spacing = 32,
                                              .separation = 8,
                                              .salt = 987,
                                              .frequency_method = method,
                                              .frequency = 0.5F};
            expect(placement.passes_frequency(12345, 5, -7) == expected);
        };
        check(frequency_reduction_method::default_method, true);
        check(frequency_reduction_method::legacy_type_1, false);
        check(frequency_reduction_method::legacy_type_2, true);
        check(frequency_reduction_method::legacy_type_3, false);
    };

    "structure chunks are exactly the potential chunk centers"_test = [] {
        const random_spread_placement linear{.spacing = 32,
                                             .separation = 8,
                                             .salt = 12345,
                                             .spread = spread_type::linear};
        expect(linear.is_structure_chunk(12345, 20, 7));
        expect(!linear.is_structure_chunk(12345, 21, 7));
        expect(linear.is_structure_chunk(-1, 9, 8));
    };
};
