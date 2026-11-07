// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/io.hpp>
#include <refrain/data/json.hpp>
#include <refrain/worldgen/chunk/block_state_codec.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/structure/generation.hpp>
#include <refrain/worldgen/structure/placement.hpp>
#include <refrain/worldgen/structure/placement_stage.hpp>
#include <refrain/worldgen/structure/stage.hpp>
#include <refrain/worldgen/structure/start.hpp>
#include <refrain/worldgen/structure/structure_data.hpp>
#include <refrain/worldgen/structure/structure_set.hpp>

#include <filesystem>

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
    "the reference data resolves structure biome tags"_test = [] {
        const char *root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
        if (root == nullptr) {
            return;
        }
        const auto data = structure::load_structure_data(root);
        expect(data.has_value()) << "cannot load structure data";
        if (!data) {
            return;
        }
        expect(data->sets.size() >= 20U)
            << "unexpected structure-set count: " << data->sets.size();
        const auto buried = data->structures.find("minecraft:buried_treasure");
        expect(buried != data->structures.end());
        if (buried == data->structures.end()) {
            return;
        }
        expect(data->allows_biome(buried->second, "minecraft:beach"))
            << "beach missing";
        expect(data->allows_biome(buried->second, "minecraft:snowy_beach"))
            << "snowy beach missing";
        expect(!data->allows_biome(buried->second, "minecraft:plains"))
            << "plains accepted";
        const auto set = data->sets.find("minecraft:buried_treasures");
        expect(set != data->sets.end());
        if (set != data->sets.end()) {
            expect(set->second.random_spread.has_value());
            if (set->second.random_spread) {
                expect(set->second.random_spread->frequency_method ==
                       structure::frequency_reduction_method::legacy_type_2);
                expect(
                    !set->second.random_spread->is_structure_chunk(5086, 0, 2));
                expect(
                    set->second.random_spread->is_structure_chunk(5086, 3, 13));
            }
        }
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

suite<"worldgen/structure-piece-placement"> structure_piece_placement_tests =
    [] {
        "buried treasure writes a chest into the ocean floor"_test = [] {
            block_state_registry registry;
            auto stone = registry.intern("minecraft:stone",
                                         {.air = false, .blocks_motion = true});
            auto sand = registry.intern("minecraft:sand",
                                        {.air = false, .blocks_motion = true});
            expect(stone.has_value() && sand.has_value());
            if (!stone || !sand) {
                return;
            }
            proto_chunk chunk{registry, 0, 0, 0, 32};
            chunk.set_status(chunk_status::biomes);
            for (jint z = 0; z < 16; ++z) {
                for (jint x = 0; x < 16; ++x) {
                    for (jint y = 0; y < 4; ++y) {
                        expect(chunk.set_state(x, y, z, *stone));
                    }
                    expect(chunk.set_state(x, 4, z, *sand));
                }
            }
            chunk.set_status(chunk_status::terrain);
            chunk.add_structure_start(::refrain::worldgen::structure_start{
                .structure = "minecraft:buried_treasure",
                .step = "underground_structures",
                .chunk_x = 0,
                .chunk_z = 0,
                .min_x = 9,
                .min_y = 90,
                .min_z = 9,
                .max_x = 9,
                .max_y = 90,
                .max_z = 9,
                .pieces = {{.type = "minecraft:btp",
                            .min_x = 9,
                            .min_y = 90,
                            .min_z = 9,
                            .max_x = 9,
                            .max_y = 90,
                            .max_z = 9,
                            .orientation = -1}}});
            chunk.add_structure_reference("minecraft:buried_treasure", 0, 0);
            chunk_region region{chunk, 0};
            const auto resolve = [&](std::string_view key) {
                if (const auto found = registry.find(key); found.has_value()) {
                    return *found;
                }
                const auto block = state_block_name(key);
                const bool air = is_air_block_name(block);
                const auto inserted =
                    registry.intern(key, {.air = air, .blocks_motion = !air});
                return inserted.value_or(no_block_state);
            };
            const structure::structure_place_context context{
                .region = &region,
                .chunk = &chunk,
                .states = &registry,
                .state = resolve,
            };
            auto random =
                worldgen_random{std::make_unique<legacy_random_source>(7)};
            structure::place_structures(chunk, context,
                                        "underground_structures", random);
            bool found_chest = false;
            for (jint y = 0; y < 32; ++y) {
                if (registry.name(chunk.state_at(9, y, 9))
                        .starts_with("minecraft:chest")) {
                    found_chest = true;
                }
            }
            expect(found_chest);
            const auto *entity =
                chunk.block_entity_at(block_pos{.x = 9, .y = 4, .z = 9});
            expect(entity != nullptr);
            if (entity != nullptr) {
                const auto id = entity->find("id");
                const auto table = entity->find("LootTable");
                const auto seed = entity->find("LootTableSeed");
                const auto components = entity->find("components");
                const auto keep_packed = entity->find("keepPacked");
                expect(id != entity->end() &&
                       id->second.as_string() == "minecraft:chest");
                expect(table != entity->end() &&
                       table->second.as_string() ==
                           "minecraft:chests/buried_treasure");
                expect(seed != entity->end() && seed->second.as_long());
                expect(components != entity->end() &&
                       components->second.as_compound());
                expect(keep_packed != entity->end() &&
                       keep_packed->second.as_byte() == 0);
            }
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

    "a structure area collects exactly the overlapping reference"_test = [] {
        structure::structure_data_set data;
        data.structures.emplace("minecraft:buried_treasure",
                                structure::structure_document{
                                    .id = "minecraft:buried_treasure",
                                    .type = "minecraft:buried_treasure",
                                    .step = "underground_structures",
                                    .biome_ids = {"minecraft:beach"},
                                });
        data.sets.emplace(
            "minecraft:buried_treasures",
            structure::structure_set{
                .id = "minecraft:buried_treasures",
                .structures = {{.structure = "minecraft:buried_treasure",
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
            .biome_name = [](std::uint16_t) { return "minecraft:beach"; },
            .height_at = [](jint, jint, heightmap_kind) { return 70; },
        };
        const auto area = structure::generate_structure_area(
            chunk_pos{.x = 0, .z = 0}, context, 8);
        expect(area.starts.size() == 289U);
        expect(area.references.size() == 1U);
        expect(area.references.front().chunk_x == 0);
        expect(area.references.front().chunk_z == 0);
        expect(area.references.front().count == 1);
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
        expect(chunk.structure_starts().front().pieces.size() == 1U);
        expect(chunk.structure_starts().front().pieces.front().type ==
               "minecraft:btp");
        expect(chunk.structure_starts().front().pieces.front().orientation ==
               -1);
        expect(chunk.structure_starts().front().chunk_x == 6);
        expect(chunk.structure_starts().front().chunk_z == -2);
        expect(chunk.structure_references().size() == result.references.size());
        expect(chunk.structure_references().front().count >= 1);
    };
};

suite<"worldgen/igloo-piece"> igloo_piece_tests = [] {
    "an igloo piece shifts to the entrance height and fills the chest"_test =
        [] {
            using namespace refrain;
            using namespace refrain::worldgen;
            block_state_registry registry;
            auto stone = registry.intern("minecraft:stone",
                                         {.air = false,
                                          .blocks_motion = true,
                                          .blocks_motion_no_leaves = true});
            auto chest = registry.intern(
                "minecraft:chest[facing=north,type=single,waterlogged=false]",
                {.air = false,
                 .blocks_motion = true,
                 .blocks_motion_no_leaves = true});
            auto marker =
                registry.intern("minecraft:structure_block[mode=data]",
                                {.air = false,
                                 .blocks_motion = true,
                                 .blocks_motion_no_leaves = true});
            expect(stone && chest && marker);
            if (!stone || !chest || !marker) {
                return;
            }
            const auto directory =
                std::filesystem::path{core::io::temp_directory()} /
                "refrain-igloo-piece";
            std::filesystem::remove_all(directory);
            std::filesystem::create_directories(directory / "minecraft" /
                                                "structure" / "igloo");

            data::nbt::compound root;
            auto size = data::nbt::make_list(data::nbt::tag_type::int_);
            for (const jint value : {3, 3, 3}) {
                (void)size.push(data::nbt::value{value});
            }
            root["size"] = std::move(size);
            auto palette = data::nbt::make_list(data::nbt::tag_type::compound);
            data::nbt::compound stone_entry;
            stone_entry["id"] =
                data::nbt::value{std::string{"minecraft:stone"}};
            (void)palette.push(data::nbt::value{stone_entry});
            data::nbt::compound chest_entry;
            chest_entry["id"] =
                data::nbt::value{std::string{"minecraft:chest"}};
            data::nbt::compound chest_properties;
            chest_properties["facing"] = data::nbt::value{std::string{"north"}};
            chest_properties["type"] = data::nbt::value{std::string{"single"}};
            chest_properties["waterlogged"] =
                data::nbt::value{std::string{"false"}};
            chest_entry["properties"] = data::nbt::value{chest_properties};
            (void)palette.push(data::nbt::value{chest_entry});
            data::nbt::compound marker_entry;
            marker_entry["id"] =
                data::nbt::value{std::string{"minecraft:structure_block"}};
            data::nbt::compound marker_properties;
            marker_properties["mode"] = data::nbt::value{std::string{"data"}};
            marker_entry["properties"] = data::nbt::value{marker_properties};
            (void)palette.push(data::nbt::value{marker_entry});
            root["palette"] = std::move(palette);

            auto blocks = data::nbt::make_list(data::nbt::tag_type::compound);
            const auto make_block = [&](std::initializer_list<jint> pos,
                                        jint state, data::nbt::compound nbt) {
                data::nbt::compound entry;
                auto pos_list = data::nbt::make_list(data::nbt::tag_type::int_);
                for (const jint value : pos) {
                    (void)pos_list.push(data::nbt::value{value});
                }
                entry["pos"] = std::move(pos_list);
                entry["state"] = data::nbt::value{state};
                if (!nbt.empty()) {
                    entry["nbt"] = data::nbt::value{std::move(nbt)};
                }
                return entry;
            };
            (void)blocks.push(data::nbt::value{make_block({0, 0, 0}, 0, {})});
            data::nbt::compound chest_nbt;
            chest_nbt["id"] = data::nbt::value{std::string{"minecraft:chest"}};
            (void)blocks.push(
                data::nbt::value{make_block({1, 0, 1}, 1, chest_nbt)});
            data::nbt::compound marker_nbt;
            marker_nbt["mode"] = data::nbt::value{std::string{"data"}};
            marker_nbt["metadata"] = data::nbt::value{std::string{"chest"}};
            (void)blocks.push(
                data::nbt::value{make_block({1, 1, 1}, 2, marker_nbt)});
            root["blocks"] = std::move(blocks);

            const auto written = data::nbt::write_file(
                (directory / "minecraft" / "structure" / "igloo" / "bottom.nbt")
                    .string(),
                data::nbt::value{root}, "", data::nbt::compression::gzip);
            expect(written.has_value());

            structure::structure_template_manager manager{directory.string(),
                                                          registry};
            proto_chunk chunk{registry, 0, 0, 0, 384};
            chunk.heightmap_at(heightmap_kind::world_surface_wg)
                .set_first_available(3, 2, 68);
            worldgen::structure_start start{
                .structure = "minecraft:igloo",
                .step = "surface_structures",
                .chunk_x = 0,
                .chunk_z = 0,
                .min_x = 0,
                .min_y = 0,
                .min_z = 0,
                .max_x = 15,
                .max_y = 383,
                .max_z = 15,
            };
            start.pieces.push_back(structure_piece_record{
                .type = "minecraft:igl",
                .min_x = 0,
                .min_y = 0,
                .min_z = 0,
                .max_x = 15,
                .max_y = 383,
                .max_z = 15,
                .template_id = "minecraft:igloo/bottom",
                .template_x = 0,
                .template_y = 90,
                .template_z = 0,
                .template_rotation = 0,
                .template_pivot_x = 3,
                .template_pivot_y = 6,
                .template_pivot_z = 7,
            });
            chunk.add_structure_start(start);
            chunk.add_structure_reference("minecraft:igloo", 0, 0);
            chunk_region region{chunk, 0};
            const auto resolve = [&](std::string_view key) {
                if (const auto found = registry.find(key); found.has_value()) {
                    return *found;
                }
                const auto block = state_block_name(key);
                const bool air = is_air_block_name(block);
                const auto inserted =
                    registry.intern(key, {.air = air, .blocks_motion = !air});
                return inserted.value_or(no_block_state);
            };
            const structure::structure_place_context context{
                .region = &region,
                .chunk = &chunk,
                .states = &registry,
                .state = resolve,
                .templates = &manager,
                .registry = &registry,
            };
            auto random =
                worldgen_random{std::make_unique<legacy_random_source>(7)};
            structure::place_structures(chunk, context, "surface_structures",
                                        random);

            // The entrance at (3, 2) reports height 68, so the y=90 template
            // shifts to y=67.
            expect(chunk.state_at(0, 67, 0) == *stone);
            expect(chunk.state_at(1, 67, 1) == *chest);
            expect(chunk.state_at(1, 68, 1) == registry.air());
            const auto *entity =
                chunk.block_entity_at(block_pos{.x = 1, .y = 67, .z = 1});
            expect(entity != nullptr);
            if (entity != nullptr) {
                const auto loot = entity->find("LootTable");
                expect(loot != entity->end());
                if (loot != entity->end()) {
                    expect(loot->second.as_string() ==
                           "minecraft:chests/igloo");
                }
                expect(entity->contains("LootTableSeed"));
            }
            std::filesystem::remove_all(directory);
        };
};

suite<"worldgen/swamp-hut"> swamp_hut_tests = [] {
    "the hut moves to the average ground height"_test = [] {
        using namespace refrain;
        using namespace refrain::worldgen;
        block_state_registry registry;
        auto stone = registry.intern("minecraft:stone",
                                     {.air = false,
                                      .blocks_motion = true,
                                      .blocks_motion_no_leaves = true});
        expect(stone.has_value());
        if (!stone) {
            return;
        }
        proto_chunk chunk{registry, 0, 0, 0, 128};
        for (jint y = 0; y <= 60; ++y) {
            for (jint z = 0; z < 16; ++z) {
                for (jint x = 0; x < 16; ++x) {
                    chunk.set_state(x, y, z, *stone);
                }
            }
        }
        chunk.heightmap_at(heightmap_kind::motion_blocking_no_leaves)
            .set_first_available(0, 0, 61);
        for (jint z = 0; z < 16; ++z) {
            for (jint x = 0; x < 16; ++x) {
                chunk.heightmap_at(heightmap_kind::motion_blocking_no_leaves)
                    .set_first_available(x, z, 61);
            }
        }
        auto start = structure::swamp_hut_start(chunk_pos{.x = 0, .z = 0}, 1);
        structure_start record{
            .structure = "minecraft:swamp_hut",
            .step = "surface_structures",
            .chunk_x = 0,
            .chunk_z = 0,
            .min_x = start.pieces.front().bounds.min_x,
            .min_y = start.pieces.front().bounds.min_y,
            .min_z = start.pieces.front().bounds.min_z,
            .max_x = start.pieces.front().bounds.max_x,
            .max_y = start.pieces.front().bounds.max_y,
            .max_z = start.pieces.front().bounds.max_z,
        };
        record.pieces.push_back(structure_piece_record{
            .type = "minecraft:tesh",
            .min_x = start.pieces.front().bounds.min_x,
            .min_y = start.pieces.front().bounds.min_y,
            .min_z = start.pieces.front().bounds.min_z,
            .max_x = start.pieces.front().bounds.max_x,
            .max_y = start.pieces.front().bounds.max_y,
            .max_z = start.pieces.front().bounds.max_z,
            .orientation = start.pieces.front().orientation,
            .height_position = -1,
        });
        chunk.add_structure_start(std::move(record));
        chunk.add_structure_reference("minecraft:swamp_hut", 0, 0);
        chunk_region region{chunk, 0};
        const auto resolve = [&](std::string_view key) -> block_state_id {
            if (const auto found = registry.find(key); found.has_value()) {
                return *found;
            }
            const auto block = state_block_name(key);
            const bool air = is_air_block_name(block);
            const auto inserted =
                registry.intern(key, {.air = air, .blocks_motion = !air});
            return inserted.has_value() ? *inserted : no_block_state;
        };
        const structure::structure_place_context context{
            .region = &region,
            .chunk = &chunk,
            .states = &registry,
            .state = resolve,
        };
        auto random =
            worldgen_random{std::make_unique<legacy_random_source>(7)};
        structure::place_structures(chunk, context, "surface_structures",
                                    random);
        jint planks_min_y = 128;
        jint planks = 0;
        for (jint y = 0; y < 128; ++y) {
            for (jint z = 0; z < 16; ++z) {
                for (jint x = 0; x < 16; ++x) {
                    if (registry.name(chunk.state_at(x, y, z)) ==
                        "minecraft:spruce_planks") {
                        planks_min_y = std::min(planks_min_y, y);
                        ++planks;
                    }
                }
            }
        }
        expect(planks > 0);
        // HPos is the average height (61), so the lowest planks land at 62.
        expect(planks_min_y == 62);
    };
};

suite<"worldgen/piece-coordinates"> piece_coordinate_tests = [] {
    "local coordinates follow the piece orientation"_test = [] {
        using namespace refrain;
        using namespace refrain::worldgen;
        structure_piece_record piece{
            .min_x = 10,
            .min_y = 64,
            .min_z = 20,
            .max_x = 16,
            .max_y = 78,
            .max_z = 26,
        };
        piece.orientation = 2; // north
        auto pos = structure::piece_local_to_world(piece, 64, 2, 3, 4);
        expect(pos.x == 12 && pos.y == 67 && pos.z == 22);
        piece.orientation = 0; // south
        pos = structure::piece_local_to_world(piece, 64, 2, 3, 4);
        expect(pos.x == 12 && pos.y == 67 && pos.z == 24);
        piece.orientation = 1; // west
        pos = structure::piece_local_to_world(piece, 64, 2, 3, 4);
        expect(pos.x == 12 && pos.y == 67 && pos.z == 22);
        piece.orientation = 3; // east
        pos = structure::piece_local_to_world(piece, 64, 2, 3, 4);
        expect(pos.x == 14 && pos.y == 67 && pos.z == 22);
    };
};

suite<"worldgen/ocean-ruin-height"> ocean_ruin_height_tests = [] {
    "the floor scan lowers the ruin onto a wide drop"_test = [] {
        using namespace refrain;
        using namespace refrain::worldgen;
        block_state_registry registry;
        const auto air = registry.intern("minecraft:air", {.air = true});
        const auto stone = registry.intern("minecraft:stone",
                                           {.air = false,
                                            .blocks_motion = true,
                                            .blocks_motion_no_leaves = true});
        const auto water = registry.intern(
            "minecraft:water", {.air = false, .fluid = true, .water = true});
        expect(air && stone && water);
        if (!air || !stone || !water) {
            return;
        }
        proto_chunk chunk{registry, 0, 0, 0, 32};
        for (jint y = 0; y < 9; ++y) {
            for (jint x = 0; x < 8; ++x) {
                for (jint z = 0; z < 8; ++z) {
                    chunk.set_state(x, y, z, *air);
                }
            }
        }
        for (jint x = 0; x <= 4; ++x) {
            for (jint z = 0; z <= 4; ++z) {
                chunk.set_state(x, 4, z, *stone);
                for (jint y = 5; y <= 8; ++y) {
                    chunk.set_state(x, y, z, *water);
                }
            }
        }
        chunk_region region{chunk, 0};
        expect(structure::ocean_ruin_floor_height(
                   region, registry, 0, block_pos{.x = 0, .y = 9, .z = 0},
                   block_pos{.x = 4, .y = 9, .z = 4}) == 5);
    };

    "a shallow floor keeps the piece height"_test = [] {
        using namespace refrain;
        using namespace refrain::worldgen;
        block_state_registry registry;
        const auto air = registry.intern("minecraft:air", {.air = true});
        const auto stone = registry.intern("minecraft:stone",
                                           {.air = false,
                                            .blocks_motion = true,
                                            .blocks_motion_no_leaves = true});
        expect(air && stone);
        if (!air || !stone) {
            return;
        }
        proto_chunk chunk{registry, 0, 0, 0, 32};
        for (jint x = 0; x < 8; ++x) {
            for (jint z = 0; z < 8; ++z) {
                for (jint y = 0; y < 9; ++y) {
                    chunk.set_state(x, y, z, *air);
                }
                chunk.set_state(x, 7, z, *stone);
            }
        }
        chunk_region region{chunk, 0};
        expect(structure::ocean_ruin_floor_height(
                   region, registry, 0, block_pos{.x = 0, .y = 9, .z = 0},
                   block_pos{.x = 4, .y = 9, .z = 4}) == 9);
    };
};
