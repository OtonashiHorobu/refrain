// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/json.hpp>
#include <refrain/worldgen/structure/placement.hpp>
#include <refrain/worldgen/structure/start.hpp>
#include <refrain/worldgen/structure/structure_set.hpp>

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

suite<"worldgen/structure-set"> structure_set_tests = [] {
    "a structure set decodes entries and a random spread placement"_test = [] {
        auto document = refrain::data::json::value::parse(
            R"({"structures":[{"structure":"minecraft:village_plains","weight":2},{"structure":"minecraft:village_taiga","weight":1}],"placement":{"type":"minecraft:random_spread","spacing":34,"separation":8,"salt":10387312,"spread_type":"linear"}})");
        expect(document.has_value());
        if (!document) {
            return;
        }
        const auto set =
            structure::decode_structure_set("minecraft:villages", *document);
        expect(set.has_value());
        if (!set) {
            return;
        }
        expect(set->structures.size() == 2U);
        expect(set->structures[0].structure == "minecraft:village_plains");
        expect(set->structures[0].weight == 2);
        expect(set->random_spread.has_value());
        if (set->random_spread) {
            expect(set->random_spread->spacing == 34);
            expect(set->random_spread->separation == 8);
        }
    };
};

suite<"worldgen/structure-start"> structure_start_tests = [] {
    "bbox intersection, chunk proximity and reference caps"_test = [] {
        const structure::structure_bbox bounds{.min_x = 10,
                                               .min_y = 20,
                                               .min_z = 30,
                                               .max_x = 40,
                                               .max_y = 50,
                                               .max_z = 60};
        expect(bounds.intersects(structure::structure_bbox{.min_x = 39,
                                                           .min_y = 49,
                                                           .min_z = 59,
                                                           .max_x = 70,
                                                           .max_y = 80,
                                                           .max_z = 90}));
        expect(!bounds.intersects(structure::structure_bbox{.min_x = 41,
                                                            .min_y = 20,
                                                            .min_z = 30,
                                                            .max_x = 60,
                                                            .max_y = 50,
                                                            .max_z = 60}));
        expect(bounds.is_close_to(chunk_pos{.x = 0, .z = 1}, 16));
        expect(!bounds.is_close_to(chunk_pos{.x = 8, .z = 8}, 0));

        structure::structure_start start{.structure = "minecraft:test",
                                         .chunk = {.x = 0, .z = 0},
                                         .references = 0,
                                         .max_references = 1,
                                         .bounds = bounds};
        expect(start.can_add_reference());
        start.add_reference();
        expect(!start.can_add_reference());
        expect(start.references == 1);
    };
};
