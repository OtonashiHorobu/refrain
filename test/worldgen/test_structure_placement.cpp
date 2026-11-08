// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/io.hpp>
#include <refrain/data/json.hpp>
#include <refrain/data/nbt.hpp>
#include <refrain/worldgen/chunk/block_state_codec.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/structure/generation.hpp>
#include <refrain/worldgen/structure/mansion_layout.hpp>
#include <refrain/worldgen/structure/placement.hpp>
#include <refrain/worldgen/structure/placement_stage.hpp>
#include <refrain/worldgen/structure/stage.hpp>
#include <refrain/worldgen/structure/start.hpp>
#include <refrain/worldgen/structure/structure_data.hpp>
#include <refrain/worldgen/structure/structure_set.hpp>

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <span>

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
        check(frequency_reduction_method::default_method, false);
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
            .fixed_biome = true,
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
            .fixed_biome = true,
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
            .fixed_biome = true,
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

            refrain::data::nbt::compound root;
            auto size = refrain::data::nbt::make_list(
                refrain::data::nbt::tag_type::int_);
            for (const jint value : {3, 3, 3}) {
                (void)size.push(refrain::data::nbt::value{value});
            }
            root["size"] = std::move(size);
            auto palette = refrain::data::nbt::make_list(
                refrain::data::nbt::tag_type::compound);
            refrain::data::nbt::compound stone_entry;
            stone_entry["id"] =
                refrain::data::nbt::value{std::string{"minecraft:stone"}};
            (void)palette.push(refrain::data::nbt::value{stone_entry});
            refrain::data::nbt::compound chest_entry;
            chest_entry["id"] =
                refrain::data::nbt::value{std::string{"minecraft:chest"}};
            refrain::data::nbt::compound chest_properties;
            chest_properties["facing"] =
                refrain::data::nbt::value{std::string{"north"}};
            chest_properties["type"] =
                refrain::data::nbt::value{std::string{"single"}};
            chest_properties["waterlogged"] =
                refrain::data::nbt::value{std::string{"false"}};
            chest_entry["properties"] =
                refrain::data::nbt::value{chest_properties};
            (void)palette.push(refrain::data::nbt::value{chest_entry});
            refrain::data::nbt::compound marker_entry;
            marker_entry["id"] = refrain::data::nbt::value{
                std::string{"minecraft:structure_block"}};
            refrain::data::nbt::compound marker_properties;
            marker_properties["mode"] =
                refrain::data::nbt::value{std::string{"data"}};
            marker_entry["properties"] =
                refrain::data::nbt::value{marker_properties};
            (void)palette.push(refrain::data::nbt::value{marker_entry});
            root["palette"] = std::move(palette);

            auto blocks = refrain::data::nbt::make_list(
                refrain::data::nbt::tag_type::compound);
            const auto make_block = [&](std::initializer_list<jint> pos,
                                        jint state,
                                        refrain::data::nbt::compound nbt) {
                refrain::data::nbt::compound entry;
                auto pos_list = refrain::data::nbt::make_list(
                    refrain::data::nbt::tag_type::int_);
                for (const jint value : pos) {
                    (void)pos_list.push(refrain::data::nbt::value{value});
                }
                entry["pos"] = std::move(pos_list);
                entry["state"] = refrain::data::nbt::value{state};
                if (!nbt.empty()) {
                    entry["nbt"] = refrain::data::nbt::value{std::move(nbt)};
                }
                return entry;
            };
            (void)blocks.push(
                refrain::data::nbt::value{make_block({0, 0, 0}, 0, {})});
            refrain::data::nbt::compound chest_nbt;
            chest_nbt["id"] =
                refrain::data::nbt::value{std::string{"minecraft:chest"}};
            (void)blocks.push(
                refrain::data::nbt::value{make_block({1, 0, 1}, 1, chest_nbt)});
            refrain::data::nbt::compound marker_nbt;
            marker_nbt["mode"] = refrain::data::nbt::value{std::string{"data"}};
            marker_nbt["metadata"] =
                refrain::data::nbt::value{std::string{"chest"}};
            (void)blocks.push(refrain::data::nbt::value{
                make_block({1, 1, 1}, 2, marker_nbt)});
            root["blocks"] = std::move(blocks);

            const auto written = refrain::data::nbt::write_file(
                (directory / "minecraft" / "structure" / "igloo" / "bottom.nbt")
                    .string(),
                refrain::data::nbt::value{root}, "",
                refrain::data::nbt::compression::gzip);
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

suite<"worldgen/structure-data-index"> structure_data_index_tests = [] {
    "the feature seed index follows the sorted step order"_test = [] {
        using namespace refrain;
        using namespace refrain::worldgen;
        structure::structure_data_set data;
        for (const auto *id : {"test:b", "test:a", "test:c"}) {
            data.structures.emplace(id, structure::structure_document{
                                            .id = id,
                                            .type = "minecraft:test",
                                            .step = "surface_structures"});
        }
        data.structures.emplace(
            "test:other",
            structure::structure_document{.id = "test:other",
                                          .type = "minecraft:test",
                                          .step = "underground_structures"});
        expect(structure::structure_step_index(data, "test:a") == 0);
        expect(structure::structure_step_index(data, "test:b") == 1);
        expect(structure::structure_step_index(data, "test:c") == 2);
        expect(structure::structure_step_index(data, "test:other") == 0);
        expect(
            !structure::structure_step_index(data, "test:missing").has_value());
    };
};

suite<"worldgen/structure-locate"> structure_locate_tests = [] {
    auto make_data = [] {
        structure::structure_data_set data;
        data.structures.emplace(
            "test:hut",
            structure::structure_document{.id = "test:hut",
                                          .type = "minecraft:buried_treasure",
                                          .step = "underground_structures",
                                          .biome_ids = {"minecraft:beach"}});
        data.sets.emplace(
            "test:huts",
            structure::structure_set{
                .id = "test:huts",
                .structures = {{.structure = "test:hut", .weight = 1}},
                .random_spread =
                    structure::random_spread_placement{
                        .spacing = 1,
                        .separation = 0,
                        .salt = 0,
                        .spread = structure::spread_type::linear,
                        .frequency = 1.0F},
            });
        return data;
    };

    "a structure is found in the origin chunk"_test = [&] {
        const auto data = make_data();
        const structure::structure_start_context context{
            .seed = 1,
            .data = &data,
            .fixed_biome = true,
            .biome_at = [](jint, jint, jint) { return std::uint16_t{0}; },
            .biome_name = [](std::uint16_t) -> std::string_view {
                return "minecraft:beach";
            },
            .height_at = [](jint, jint, heightmap_kind) { return jint{70}; },
        };
        const auto found = structure::locate_structure(
            "test:hut", chunk_pos{.x = 0, .z = 0}, 2, context);
        expect(found.has_value());
        expect(found && found->chunk.x == 0 && found->chunk.z == 0);
        expect(found && found->structure == "test:hut");
    };

    "an unknown id and a disabled radius find nothing"_test = [&] {
        const auto data = make_data();
        const structure::structure_start_context context{
            .seed = 1,
            .data = &data,
            .fixed_biome = true,
            .biome_at = [](jint, jint, jint) { return std::uint16_t{0}; },
            .biome_name = [](std::uint16_t) -> std::string_view {
                return "minecraft:beach";
            },
            .height_at = [](jint, jint, heightmap_kind) { return jint{70}; },
        };
        expect(!structure::locate_structure(
                    "test:missing", chunk_pos{.x = 0, .z = 0}, 2, context)
                    .has_value());
        expect(!structure::locate_structure(
                    "test:hut", chunk_pos{.x = 0, .z = 0}, -1, context)
                    .has_value());
    };
};

namespace {

auto structure_region_payload(std::span<const std::byte> region, jint x, jint z)
    -> std::optional<std::span<const std::byte>> {
    if (region.size() < 8192) {
        return std::nullopt;
    }
    const auto index = static_cast<std::size_t>((x & 31) + (z & 31) * 32);
    const auto *entry = region.data() + index * 4U;
    const auto offset = (static_cast<std::size_t>(entry[0]) << 16U) |
                        (static_cast<std::size_t>(entry[1]) << 8U) |
                        static_cast<std::size_t>(entry[2]);
    if (offset == 0) {
        return std::nullopt;
    }
    const auto start = offset * 4096U;
    if (start + 5U > region.size()) {
        return std::nullopt;
    }
    const auto length = (static_cast<std::size_t>(region[start]) << 24U) |
                        (static_cast<std::size_t>(region[start + 1]) << 16U) |
                        (static_cast<std::size_t>(region[start + 2]) << 8U) |
                        static_cast<std::size_t>(region[start + 3]);
    if (length < 1U || start + 4U + length > region.size()) {
        return std::nullopt;
    }
    if (static_cast<std::uint8_t>(region[start + 4U]) != 2U) {
        return std::nullopt;
    }
    return region.subspan(start + 5U, length - 1U);
}

auto field(const refrain::data::nbt::compound *root, std::string_view key)
    -> const refrain::data::nbt::value * {
    if (root == nullptr) {
        return nullptr;
    }
    const auto found = root->find(key);
    return found == root->end() ? nullptr : &found->second;
}

} // namespace

suite<"worldgen/locate-oracle"> locate_oracle_tests = [] {
    "locate finds the jungle pyramid in the reference world"_test = [] {
        const char *region_path = std::getenv("REFRAIN_FULL_STAGE_REGION");
        const char *data_root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
        const char *templates_root = std::getenv("REFRAIN_VANILLA_STRUCTURES");
        if (region_path == nullptr || data_root == nullptr ||
            templates_root == nullptr ||
            std::getenv("REFRAIN_LOCATE_JUNGLE") == nullptr) {
            return;
        }
        const auto bytes = refrain::core::io::read_file(region_path);
        expect(bytes.has_value());
        if (!bytes) {
            return;
        }
        std::map<std::pair<jint, jint>, std::array<jint, 256>> cache;
        const auto height_at = [&](jint x, jint z) -> jint {
            const jint chunk_x = x >> 4;
            const jint chunk_z = z >> 4;
            const auto key = std::make_pair(chunk_x, chunk_z);
            auto found = cache.find(key);
            if (found == cache.end()) {
                std::array<jint, 256> heights{};
                heights.fill(-64);
                const auto payload =
                    structure_region_payload(*bytes, chunk_x, chunk_z);
                if (payload) {
                    const auto document = refrain::data::nbt::read(
                        *payload, refrain::data::nbt::compression::zlib);
                    if (document) {
                        const auto *maps =
                            field(document->as_compound(), "Heightmaps");
                        const auto *surface =
                            maps == nullptr
                                ? nullptr
                                : field(maps->as_compound(), "WORLD_SURFACE");
                        const auto longs = surface == nullptr
                                               ? std::nullopt
                                               : surface->as_long_array();
                        if (longs) {
                            for (std::size_t i = 0; i < 256; ++i) {
                                const auto word = i / 7;
                                const auto shift = (i % 7) * 9;
                                if (word < longs->size()) {
                                    heights[i] = static_cast<jint>(
                                                     ((*longs)[word] >> shift) &
                                                     0x1FFU) +
                                                 (-64);
                                }
                            }
                        }
                    }
                }
                found = cache.emplace(key, heights).first;
            }
            return found
                ->second[static_cast<std::size_t>((z & 15) * 16 + (x & 15))];
        };
        auto data = structure::load_structure_data(data_root);
        expect(data.has_value());
        if (!data) {
            return;
        }
        const auto env_int = [](const char *name, jint fallback) {
            const char *value = std::getenv(name);
            return value == nullptr ? fallback : std::atoi(value);
        };
        const std::string structure_id =
            std::getenv("REFRAIN_LOCATE_STRUCTURE") == nullptr
                ? "minecraft:jungle_pyramid"
                : std::getenv("REFRAIN_LOCATE_STRUCTURE");
        const std::string biome = std::getenv("REFRAIN_LOCATE_BIOME") == nullptr
                                      ? "minecraft:jungle"
                                      : std::getenv("REFRAIN_LOCATE_BIOME");
        const jlong locate_seed = env_int("REFRAIN_LOCATE_SEED", 2);
        const jint radius = env_int("REFRAIN_LOCATE_RADIUS", 12);
        const jint expected_x = env_int("REFRAIN_LOCATE_EXPECT_X", 9);
        const jint expected_z = env_int("REFRAIN_LOCATE_EXPECT_Z", 2);
        block_state_registry registry;
        structure::structure_template_manager manager{templates_root, registry};
        const structure::structure_start_context context{
            .seed = locate_seed,
            .data = &*data,
            .min_y = -64,
            .world_height = 384,
            .sea_level = 63,
            .fixed_biome = true,
            .biome_at = [](jint, jint, jint) { return std::uint16_t{0}; },
            .biome_name = [&biome](std::uint16_t) -> std::string_view {
                return biome;
            },
            .height_at = [&](jint x, jint z,
                             heightmap_kind) { return height_at(x, z); },
            .templates = &manager,
        };
        const auto found = structure::locate_structure(
            structure_id, chunk_pos{.x = 0, .z = 0}, radius, context);
        expect(found.has_value());
        if (found) {
            expect(found->chunk.x == expected_x &&
                   found->chunk.z == expected_z);
        }
    };
};

suite<"worldgen/structure-content-index"> structure_content_index_tests = [] {
    "the content step order matches the reference indices"_test = [] {
        const char *data_root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
        if (data_root == nullptr) {
            return;
        }
        const auto data = structure::load_structure_data(data_root);
        expect(data.has_value());
        if (!data) {
            return;
        }
        const auto index = [&](std::string_view id) {
            return structure::structure_step_index(*data, id);
        };
        expect(index("minecraft:buried_treasure") == 0);
        expect(index("minecraft:mineshaft") == 1);
        expect(index("minecraft:jungle_pyramid") == 22);
        expect(!index("minecraft:missing_structure").has_value());
    };
};

suite<"worldgen/ruined-portal"> ruined_portal_tests = [] {
    "the portal start carries its setup and template"_test = [&] {
        structure::structure_data_set data;
        data.structures.emplace(
            "test:portal",
            structure::structure_document{
                .id = "test:portal",
                .type = "minecraft:ruined_portal",
                .step = "surface_structures",
                .biome_ids = {"minecraft:plains"},
                .config = refrain::data::json::value::parse(R"({"setups":[{
                    "placement":"on_land_surface",
                    "air_pocket_probability":0.0,
                    "mossiness":0.5,
                    "overgrown":true,
                    "vines":false,
                    "can_be_cold":false,
                    "replace_with_blackstone":true,
                    "weight":1.0}]})")
                              .value(),
            });
        data.sets.emplace(
            "test:portals",
            structure::structure_set{
                .id = "test:portals",
                .structures = {{.structure = "test:portal", .weight = 1}},
                .random_spread =
                    structure::random_spread_placement{
                        .spacing = 1,
                        .separation = 0,
                        .salt = 0,
                        .spread = structure::spread_type::linear,
                        .frequency = 1.0F},
            });
        block_state_registry registry;
        structure::structure_template_manager manager{"/nonexistent", registry};
        const structure::structure_start_context context{
            .seed = 4,
            .data = &data,
            .min_y = -64,
            .world_height = 384,
            .sea_level = 63,
            .fixed_biome = true,
            .biome_at = [](jint, jint, jint) { return std::uint16_t{0}; },
            .biome_name = [](std::uint16_t) -> std::string_view {
                return "minecraft:plains";
            },
            .height_at = [](jint, jint, heightmap_kind) { return jint{70}; },
            .opaque_at = [](jint, jint y, jint) { return y < 70; },
            .templates = &manager,
        };
        const auto starts = structure::generate_structure_starts(
            chunk_pos{.x = 0, .z = 0}, context);
        expect(starts.size() == 1U);
        if (starts.empty() || starts.front().pieces.empty()) {
            return;
        }
        const auto &piece = starts.front().pieces.front();
        expect(piece.type == "minecraft:ruined_portal");
        expect(piece.portal.has_value());
        if (piece.portal) {
            expect(piece.portal->placement == "on_land_surface");
            expect(piece.portal->mossiness == 0.5F);
            expect(piece.portal->overgrown);
            expect(piece.portal->replace_with_blackstone);
        }
        expect(piece.template_id.starts_with("minecraft:ruined_portal/"));
    };

    "the reference portal start selects its template and transform"_test = [] {
        const char *data_root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
        const char *templates_root = std::getenv("REFRAIN_VANILLA_STRUCTURES");
        if (data_root == nullptr || templates_root == nullptr ||
            std::getenv("REFRAIN_PORTAL_START_ORACLE") == nullptr) {
            return;
        }
        const auto data = structure::load_structure_data(data_root);
        expect(data.has_value());
        if (!data) {
            return;
        }
        block_state_registry registry;
        structure::structure_template_manager manager{templates_root, registry};
        const structure::structure_start_context context{
            .seed = 1,
            .data = &*data,
            .min_y = -64,
            .world_height = 384,
            .sea_level = 63,
            .fixed_biome = true,
            .biome_at = [](jint, jint, jint) { return std::uint16_t{0}; },
            .biome_name = [](std::uint16_t) -> std::string_view {
                return "minecraft:plains";
            },
            .height_at = [](jint, jint, heightmap_kind) { return jint{63}; },
            .opaque_at = [](jint, jint y, jint) { return y < 63; },
            .templates = &manager,
        };
        const auto starts = structure::generate_structure_starts(
            chunk_pos{.x = -16, .z = -36}, context);
        expect(starts.size() == 1U);
        if (starts.empty() || starts.front().pieces.empty()) {
            return;
        }
        const auto &piece = starts.front().pieces.front();
        expect(piece.template_id == "minecraft:ruined_portal/portal_2");
        expect(piece.template_rotation == 3);
        expect(piece.template_mirror == 0);
        expect(piece.portal.has_value());
        if (piece.portal) {
            expect(piece.portal->placement == "on_land_surface");
        }
    };

    "the underground reference portal start matches its transform"_test = [] {
        const char *data_root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
        const char *templates_root = std::getenv("REFRAIN_VANILLA_STRUCTURES");
        if (data_root == nullptr || templates_root == nullptr ||
            std::getenv("REFRAIN_PORTAL_START_ORACLE") == nullptr) {
            return;
        }
        const auto data = structure::load_structure_data(data_root);
        expect(data.has_value());
        if (!data) {
            return;
        }
        block_state_registry registry;
        structure::structure_template_manager manager{templates_root, registry};
        const structure::structure_start_context context{
            .seed = 1,
            .data = &*data,
            .min_y = -64,
            .world_height = 384,
            .sea_level = 63,
            .fixed_biome = true,
            .biome_at = [](jint, jint, jint) { return std::uint16_t{0}; },
            .biome_name = [](std::uint16_t) -> std::string_view {
                return "minecraft:plains";
            },
            .height_at = [](jint, jint, heightmap_kind) { return jint{63}; },
            .opaque_at = [](jint, jint y, jint) { return y < 63; },
            .templates = &manager,
        };
        const auto starts = structure::generate_structure_starts(
            chunk_pos{.x = 15, .z = -40}, context);
        expect(starts.size() == 1U);
        if (starts.empty() || starts.front().pieces.empty()) {
            return;
        }
        const auto &piece = starts.front().pieces.front();
        expect(piece.template_id == "minecraft:ruined_portal/portal_9");
        expect(piece.template_rotation == 3);
        expect(piece.template_mirror == 2);
        expect(piece.template_origin.y == 52);
        expect(piece.portal.has_value());
        if (piece.portal) {
            expect(piece.portal->placement == "underground");
        }
    };
};

suite<"worldgen/mansion-grid"> mansion_grid_tests = [] {
    "the mansion grid keeps an outside value and edits ranges"_test = [] {
        structure::mansion_simple_grid grid{11, 11, 5};
        expect(grid.get(-1, 0) == 5);
        expect(grid.get(11, 11) == 5);
        grid.set(2, 3, 4, 5, 7);
        expect(grid.get(2, 3) == 7);
        expect(grid.get(4, 5) == 7);
        expect(grid.get(1, 3) == 0);
        grid.set_if(2, 3, 0, 9);
        expect(grid.get(2, 3) == 7);
        grid.set_if(2, 3, 7, 9);
        expect(grid.get(2, 3) == 9);
        expect(grid.edges_to(3, 3, 9));
        expect(!grid.edges_to(8, 8, 9));
    };

    "the mansion grid is deterministic and has an entrance"_test = [] {
        auto random_a =
            worldgen_random{std::make_unique<legacy_random_source>(1)};
        auto random_b =
            worldgen_random{std::make_unique<legacy_random_source>(1)};
        structure::mansion_grid first{random_a};
        structure::mansion_grid second{random_b};
        expect(first.entrance_x() == 7 && first.entrance_y() == 4);
        expect(structure::mansion_grid::is_house(first.base_grid(), 7, 4));
        expect(first.base_grid().get(7, 4) == 3);
        bool has_room = false;
        bool has_third = false;
        for (jint y = 0; y < 11; ++y) {
            for (jint x = 0; x < 11; ++x) {
                expect(first.base_grid().get(x, y) ==
                       second.base_grid().get(x, y));
                has_room = has_room || first.floor_rooms(0).get(x, y) != 0;
                has_third = has_third || structure::mansion_grid::is_house(
                                             first.third_floor_grid(), x, y);
            }
        }
        expect(has_room);
        expect(has_third);
    };

    "mansion room names come from the floor collection"_test = [] {
        auto random =
            worldgen_random{std::make_unique<legacy_random_source>(3)};
        const structure::mansion_room_collection first{0};
        const structure::mansion_room_collection second{1};
        const auto a = first.room_1x1(random);
        expect(a.starts_with("1x1_a") && a.size() == 6);
        const auto b = second.room_1x1(random);
        expect(b.starts_with("1x1_b") && b.size() == 6);
        expect(second.room_1x2_side(random, true) == "1x2_c_stairs");
        expect(second.room_1x2_front(random, true) == "1x2_d_stairs");
        expect(first.room_2x2_secret(random) == "2x2_s1");
        expect(second.room_2x2_secret(random) == "2x2_s1");
    };

    "the mansion placer emits deterministic walls and roofs"_test = [] {
        auto first_random =
            worldgen_random{std::make_unique<legacy_random_source>(2)};
        structure::mansion_grid first_grid{first_random};
        const auto first = structure::generate_mansion(
            first_random, block_pos{.x = 0, .y = 60, .z = 0},
            structure::template_rotation::none, first_grid);
        auto second_random =
            worldgen_random{std::make_unique<legacy_random_source>(2)};
        structure::mansion_grid second_grid{second_random};
        const auto second = structure::generate_mansion(
            second_random, block_pos{.x = 0, .y = 60, .z = 0},
            structure::template_rotation::none, second_grid);
        expect(first.size() == second.size());
        expect(!first.empty());
        if (first.size() != second.size() || first.empty()) {
            return;
        }
        bool has_entrance = false;
        bool has_roof = false;
        bool has_wall = false;
        bool has_corridor = false;
        bool has_room = false;
        bool has_indoor_wall = false;
        for (std::size_t i = 0; i < first.size(); ++i) {
            expect(first[i].template_name == second[i].template_name);
            expect(first[i].origin.x == second[i].origin.x);
            expect(first[i].origin.y == second[i].origin.y);
            expect(first[i].origin.z == second[i].origin.z);
            expect(first[i].rotation == second[i].rotation);
            has_entrance = has_entrance || first[i].template_name == "entrance";
            has_roof = has_roof || first[i].template_name == "roof";
            has_wall = has_wall || first[i].template_name == "wall_flat";
            has_corridor =
                has_corridor || first[i].template_name == "corridor_floor";
            has_room = has_room || first[i].template_name.starts_with("1x1_") ||
                       first[i].template_name.starts_with("1x2_") ||
                       first[i].template_name.starts_with("2x2_");
            has_indoor_wall =
                has_indoor_wall ||
                first[i].template_name.starts_with("indoors_wall");
        }
        expect(has_entrance);
        expect(has_roof);
        expect(has_wall);
        expect(has_corridor);
        expect(has_room);
        expect(has_indoor_wall);
    };
};

suite<"worldgen/mansion-reference"> mansion_reference_tests = [] {
    "the mansion piece list matches the reference digest"_test = [] {
        const char *templates_root = std::getenv("REFRAIN_VANILLA_STRUCTURES");
        if (templates_root == nullptr) {
            return;
        }
        block_state_registry registry;
        structure::structure_template_manager manager{templates_root, registry};
        const auto start = structure::woodland_mansion_start(
            chunk_pos{.x = 46, .z = 5}, 1, manager,
            [](jint, jint, heightmap_kind) { return jint{63}; });
        expect(start.pieces.size() == 484U);
        std::uint64_t hash = 1469598103934665603ULL;
        const auto mix = [&](std::string_view text) {
            for (const char ch : text) {
                hash ^=
                    static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
                hash *= 1099511628211ULL;
            }
        };
        constexpr std::string_view prefix{"minecraft:woodland_mansion/"};
        for (const auto &piece : start.pieces) {
            std::string_view name{piece.template_id};
            if (name.starts_with(prefix)) {
                name.remove_prefix(prefix.size());
            }
            const std::string line =
                std::string{name} + "," +
                std::to_string(piece.template_origin.x) + "," +
                std::to_string(piece.template_origin.y) + "," +
                std::to_string(piece.template_origin.z) + "," +
                std::to_string(piece.template_rotation) + "," +
                std::to_string(piece.template_mirror) + ";";
            mix(line);
        }
        expect(hash == 0x9e4d7e3609cb264eULL);
    };

    "the mansion zero position follows size and rotation"_test = [] {
        const block_pos zero{.x = 1, .y = 0, .z = 0};
        const auto none = structure::mansion_zero_position(
            zero, structure::template_mirror::none,
            structure::template_rotation::none, 7, 7);
        expect(none.x == 1 && none.z == 0);
        const auto half = structure::mansion_zero_position(
            zero, structure::template_mirror::none,
            structure::template_rotation::clockwise_180, 7, 7);
        expect(half.x == 7 && half.z == 6);
        const auto cw = structure::mansion_zero_position(
            zero, structure::template_mirror::none,
            structure::template_rotation::clockwise_90, 7, 7);
        expect(cw.x == 7 && cw.z == 0);
        const auto ccw = structure::mansion_zero_position(
            zero, structure::template_mirror::none,
            structure::template_rotation::counterclockwise_90, 7, 7);
        expect(ccw.x == 1 && ccw.z == 6);
        const auto mirrored = structure::mansion_zero_position(
            zero, structure::template_mirror::front_back,
            structure::template_rotation::none, 7, 7);
        expect(mirrored.x == 7 && mirrored.z == 0);
    };
};
