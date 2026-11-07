// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/json.hpp>
#include <refrain/worldgen/structure/generation.hpp>
#include <refrain/worldgen/structure/placement.hpp>
#include <refrain/worldgen/structure/stage.hpp>
#include <refrain/worldgen/structure/start.hpp>
#include <refrain/worldgen/structure/structure_data.hpp>
#include <refrain/worldgen/structure/structure_set.hpp>

#include <boost/ut.hpp>

namespace {

class stage_table final : public refrain::worldgen::block_state_table {
  public:
    [[nodiscard]] auto air() const
        -> refrain::worldgen::block_state_id override {
        return 0;
    }
    [[nodiscard]] auto traits(refrain::worldgen::block_state_id) const
        -> refrain::worldgen::state_traits override {
        return {.air = true};
    }
    [[nodiscard]] auto name(refrain::worldgen::block_state_id) const
        -> std::string_view override {
        return "minecraft:air";
    }
    [[nodiscard]] auto in_tag(refrain::worldgen::block_state_id,
                              std::string_view) const -> bool override {
        return false;
    }
};

} // namespace

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
        const auto order =
            set->weighted_order(12345, chunk_pos{.x = 1, .z = 2});
        expect(order.size() == 2U);
        expect(order[0].structure != order[1].structure);
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

        auto collectable = start;
        collectable.references = 0;
        collectable.max_references = 2;
        std::vector<structure::structure_start> starts;
        starts.push_back(collectable);
        structure::structure_start far{.structure = "minecraft:far",
                                       .chunk = {.x = 20, .z = 20},
                                       .max_references = 1,
                                       .bounds = {.min_x = 3000,
                                                  .min_y = 0,
                                                  .min_z = 3000,
                                                  .max_x = 3000,
                                                  .max_y = 0,
                                                  .max_z = 3000}};
        starts.push_back(std::move(far));
        const auto references = structure::collect_references(
            starts, chunk_pos{.x = 0, .z = 1}, 16);
        expect(references.size() == 1U);
        expect(starts[0].references == 1);
    };
};

suite<"worldgen/structure-data"> structure_data_tests = [] {
    "a structure document decodes its type, step, biomes and adaptation"_test = [] {
        auto document = refrain::data::json::value::parse(
            R"({"type":"minecraft:jigsaw","biomes":"#minecraft:has_structure/village_plains","step":"surface_structures","terrain_adaptation":"beard_thin","spawn_overrides":{},"start_pool":"minecraft:village/plains/town_centers"})");
        expect(document.has_value());
        if (!document) {
            return;
        }
        const auto decoded = structure::decode_structure_document(
            "minecraft:village_plains", *document);
        expect(decoded.has_value());
        if (!decoded) {
            return;
        }
        expect(decoded->type == "minecraft:jigsaw");
        expect(decoded->step == "surface_structures");
        expect(decoded->adaptation ==
               structure::terrain_adaptation::beard_thin);
        expect(decoded->biome_tag == "minecraft:has_structure/village_plains");
        expect(decoded->biome_ids.empty());
    };
};

suite<"worldgen/buried-treasure"> buried_treasure_tests = [] {
    "the buried treasure start has one centre-column piece"_test = [] {
        const auto start =
            structure::buried_treasure_start(chunk_pos{.x = 2, .z = -3});
        expect(start.structure == "minecraft:buried_treasure");
        expect(start.pieces.size() == 1U);
        expect(start.bounds.min_x == 41);
        expect(start.bounds.max_x == 41);
        expect(start.bounds.min_z == -39);
        expect(start.bounds.min_y == 90);
        expect(start.max_references == 1);
    };
};

suite<"worldgen/structure-stage"> structure_stage_tests = [] {
    "buried starts are assembled around the target chunk"_test = [] {
        const std::vector<structure::selected_structure> structures{{
            .id = "oracle:buried",
            .placement = {.spacing = 1,
                          .separation = 0,
                          .salt = 0,
                          .spread = structure::spread_type::linear,
                          .frequency = 1.0F},
        }};
        const auto result = structure::generate_buried_starts(
            12345, chunk_pos{.x = 6, .z = -2}, structures, 8);
        expect(result.starts.size() == 289U);
        expect(!result.starts.empty());
        expect(result.starts.front().structure == "oracle:buried");
        expect(result.references.size() == 1U);
        expect(result.references.front().structure == "oracle:buried");
        expect(result.references.front().count >= 1);
    };

    "references are counted only for starts overlapping the target chunk"_test =
        [] {
            std::vector<structure::structure_start> starts{
                {.structure = "oracle:buried",
                 .chunk = {.x = 6, .z = -2},
                 .bounds = {.min_x = 105,
                            .min_y = 90,
                            .min_z = -23,
                            .max_x = 105,
                            .max_y = 90,
                            .max_z = -23}},
                {.structure = "oracle:buried",
                 .chunk = {.x = 5, .z = -2},
                 .bounds = {.min_x = 89,
                            .min_y = 90,
                            .min_z = -23,
                            .max_x = 89,
                            .max_y = 90,
                            .max_z = -23}},
            };
            const auto references = structure::collect_references(
                starts, chunk_pos{.x = 6, .z = -2}, 8);
            expect(references.size() == 1U);
            expect(references.front().chunk_x == 6);
            expect(references.front().count == 1);
        };

    "structure sets generate a surface start with its rotated piece"_test = [] {
        structure::structure_data_set data;
        data.structures.emplace("minecraft:desert_pyramid",
                                structure::structure_document{
                                    .id = "minecraft:desert_pyramid",
                                    .type = "minecraft:desert_pyramid",
                                    .step = "surface_structures",
                                    .biome_ids = {"minecraft:desert"},
                                });
        structure::structure_set set{
            .id = "minecraft:desert_pyramids",
            .structures = {{.structure = "minecraft:desert_pyramid",
                            .weight = 1}},
            .random_spread =
                random_spread_placement{.spacing = 1,
                                        .separation = 0,
                                        .salt = 0,
                                        .spread = spread_type::linear,
                                        .frequency = 1.0F},
        };
        data.sets.emplace("minecraft:desert_pyramids", set);
        const structure::structure_start_context context{
            .seed = 12345,
            .data = &data,
            .min_y = -64,
            .world_height = 384,
            .sea_level = 63,
            .biome_at = [](jint, jint, jint) { return std::uint16_t{0}; },
            .biome_name = [](std::uint16_t) { return "minecraft:desert"; },
            .height_at = [](jint, jint, heightmap_kind) { return 70; },
        };
        const auto starts = structure::generate_structure_starts(
            chunk_pos{.x = 0, .z = 0}, context);
        expect(starts.size() == 1U);
        if (starts.empty()) {
            return;
        }
        expect(starts.front().structure == "minecraft:desert_pyramid");
        expect(starts.front().pieces.size() == 1U);
        if (starts.front().pieces.empty()) {
            return;
        }
        const auto &piece = starts.front().pieces.front();
        expect(piece.bounds.min_y == 64 && piece.bounds.max_y == 78);
        const auto size_x = piece.bounds.max_x - piece.bounds.min_x + 1;
        const auto size_z = piece.bounds.max_z - piece.bounds.min_z + 1;
        expect((size_x == 21 && size_z == 21) ||
               (size_x == 15 && size_z == 21) ||
               (size_x == 21 && size_z == 15));
    };

    "a structure set rejects a chunk whose biome is not allowed"_test = [] {
        structure::structure_data_set data;
        data.structures.emplace("minecraft:swamp_hut",
                                structure::structure_document{
                                    .id = "minecraft:swamp_hut",
                                    .type = "minecraft:swamp_hut",
                                    .step = "surface_structures",
                                    .biome_ids = {"minecraft:swamp"},
                                });
        data.sets.emplace(
            "minecraft:swamp_huts",
            structure::structure_set{
                .id = "minecraft:swamp_huts",
                .structures = {{.structure = "minecraft:swamp_hut",
                                .weight = 1}},
                .random_spread =
                    random_spread_placement{.spacing = 1,
                                            .separation = 0,
                                            .salt = 0,
                                            .spread = spread_type::linear,
                                            .frequency = 1.0F},
            });
        const structure::structure_start_context context{
            .seed = 12345,
            .data = &data,
            .biome_at = [](jint, jint, jint) { return std::uint16_t{0}; },
            .biome_name = [](std::uint16_t) { return "minecraft:plains"; },
            .height_at = [](jint, jint, heightmap_kind) { return 70; },
        };
        expect(structure::generate_structure_starts(chunk_pos{.x = 0, .z = 0},
                                                    context)
                   .empty());
    };

    "the write stage keeps only starts owned by the chunk"_test = [] {
        const std::vector<structure::selected_structure> structures{{
            .id = "oracle:buried",
            .placement = {.spacing = 1,
                          .separation = 0,
                          .salt = 0,
                          .spread = structure::spread_type::linear,
                          .frequency = 1.0F},
        }};
        const auto result = structure::generate_buried_starts(
            12345, chunk_pos{.x = 6, .z = -2}, structures, 8);
        const stage_table table;
        proto_chunk chunk{table, 6, -2, -64, 384};
        structure::write_structure_stage(chunk, result);
        expect(chunk.structure_starts().size() == 1U);
        expect(chunk.structure_starts().front().structure == "oracle:buried");
        expect(chunk.structure_starts().front().chunk_x == 6);
        expect(chunk.structure_starts().front().chunk_z == -2);
        expect(chunk.structure_references().size() == result.references.size());
        expect(chunk.structure_references().front().count >= 1);
    };
};
