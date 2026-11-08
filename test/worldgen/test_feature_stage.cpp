// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/json.hpp>
#include <refrain/worldgen/chunk/block_state_codec.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/feature/feature_data.hpp>
#include <refrain/worldgen/feature/feature_registry.hpp>
#include <refrain/worldgen/feature/feature_stage.hpp>
#include <refrain/worldgen/feature/tree.hpp>

#include <boost/ut.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using refrain::worldgen::block_state_registry;
using refrain::worldgen::chunk_region;
using refrain::worldgen::chunk_status;
using refrain::worldgen::heightmap_kind;
using refrain::worldgen::proto_chunk;
using refrain::worldgen::feature::attached_to_leaves_decorator;
using refrain::worldgen::feature::attached_to_logs_decorator;
using refrain::worldgen::feature::bamboo_feature;
using refrain::worldgen::feature::biome_feature_list;
using refrain::worldgen::feature::block_blob_feature;
using refrain::worldgen::feature::boolean_selector_feature;
using refrain::worldgen::feature::configured_feature_document;
using refrain::worldgen::feature::decode_block_predicate;
using refrain::worldgen::feature::decode_placement;
using refrain::worldgen::feature::decode_state_provider;
using refrain::worldgen::feature::decorate;
using refrain::worldgen::feature::delta_feature;
using refrain::worldgen::feature::end_platform_feature;
using refrain::worldgen::feature::feature_context;
using refrain::worldgen::feature::feature_data_set;
using refrain::worldgen::feature::feature_registry;
using refrain::worldgen::feature::feature_sorter;
using refrain::worldgen::feature::foliage_kind;
using refrain::worldgen::feature::freeze_top_layer_feature;
using refrain::worldgen::feature::geode_feature;
using refrain::worldgen::feature::geode_parameters;
using refrain::worldgen::feature::huge_fungus_feature;
using refrain::worldgen::feature::huge_mushroom_feature;
using refrain::worldgen::feature::iceberg_feature;
using refrain::worldgen::feature::lake_feature;
using refrain::worldgen::feature::large_dripstone_feature;
using refrain::worldgen::feature::leave_vine_decorator;
using refrain::worldgen::feature::monster_room_feature;
using refrain::worldgen::feature::multiface_growth_feature;
using refrain::worldgen::feature::place_on_ground_decorator;
using refrain::worldgen::feature::placed_feature_document;
using refrain::worldgen::feature::placed_feature_runtime;
using refrain::worldgen::feature::placement_context;
using refrain::worldgen::feature::random_neighbor_spread_feature;
using refrain::worldgen::feature::replace_blobs_feature;
using refrain::worldgen::feature::sequence_feature;
using refrain::worldgen::feature::shelf_mushroom_decorator;
using refrain::worldgen::feature::simple_block_feature;
using refrain::worldgen::feature::simple_state;
using refrain::worldgen::feature::speleothem_cluster_feature;
using refrain::worldgen::feature::spring_feature;
using refrain::worldgen::feature::tree_decorator_context;
using refrain::worldgen::feature::tree_feature;
using refrain::worldgen::feature::trunk_kind;
using refrain::worldgen::feature::trunk_vine_decorator;
using refrain::worldgen::feature::underwater_magma_feature;
using refrain::worldgen::feature::vines_feature;
using refrain::worldgen::feature::void_start_platform_feature;
using refrain::worldgen::provider::generation_context;

auto parse(std::string_view text) -> refrain::data::json::value {
    auto parsed = refrain::data::json::value::parse(text);
    return parsed ? std::move(*parsed) : refrain::data::json::value{};
}

} // namespace

using namespace boost::ut;
using namespace refrain::worldgen;

suite<"worldgen/feature-stage"> feature_stage_tests = [] {
    "a simple block feature places through count, in_square and heightmap"_test =
        [] {
            block_state_registry registry;
            auto stone = registry.intern("minecraft:stone",
                                         {.air = false,
                                          .blocks_motion = true,
                                          .blocks_motion_no_leaves = true});
            expect(stone.has_value());
            if (!stone) {
                return;
            }

            proto_chunk chunk{registry, 0, 0, 0, 32};
            chunk.set_status(chunk_status::biomes);
            for (jint x = 0; x < 16; ++x) {
                for (jint z = 0; z < 16; ++z) {
                    expect(chunk.set_state(x, 4, z, *stone));
                }
            }
            chunk_region region{chunk, 1};

            const std::vector<biome_feature_list> lists{{
                .biome = "oracle:test",
                .steps = {{}, {}, {}, {}, {}, {}, {}, {}, {}, {"oracle:stone"}},
            }};
            const auto plan = feature_sorter::build({"oracle:test"}, lists);
            expect(plan.has_value());
            if (!plan) {
                return;
            }

            placed_feature_runtime placed;
            placed.id = "oracle:stone";
            const auto feature = simple_block_feature{*stone, false};
            placed.configured = &feature;
            const auto placement_document = parse(
                R"([{"type":"minecraft:count","count":2},
                    {"type":"minecraft:in_square"},
                    {"type":"minecraft:heightmap","heightmap":"WORLD_SURFACE_WG"}])");
            if (!placement_document.is_array()) {
                return;
            }
            for (const auto &entry : *placement_document.as_array()) {
                auto modifier = decode_placement(entry, registry);
                expect(modifier.has_value());
                if (modifier) {
                    placed.placement.push_back(std::move(*modifier));
                }
            }
            expect(placed.placement.size() == 3U);

            std::unordered_map<std::string, placed_feature_runtime> runtime;
            runtime.emplace("oracle:stone", std::move(placed));

            placement_context placement{
                .region = &region,
                .chunk = &chunk,
                .states = &registry,
                .generation = generation_context{.min_y = 0,
                                                 .height = 32,
                                                 .sea_level = 8},
            };
            feature_context context{.placement = placement};
            decorate(context, *plan, runtime, {"oracle:test"}, lists, 12345);

            std::size_t placed_count = 0;
            for (jint x = 0; x < 16; ++x) {
                for (jint z = 0; z < 16; ++z) {
                    if (chunk.state_at(x, 5, z) == *stone) {
                        ++placed_count;
                    }
                }
            }
            expect(placed_count == 2U);
        };

    "a sequence runs nested placed features in order and stops on failure"_test =
        [] {
            block_state_registry registry;
            auto stone = registry.intern("minecraft:stone", {.air = false});
            expect(stone.has_value());
            if (!stone) {
                return;
            }
            proto_chunk chunk{registry, 0, 0, 0, 32};
            chunk.set_status(chunk_status::biomes);
            chunk_region region{chunk, 0};
            placement_context placement{
                .region = &region,
                .chunk = &chunk,
                .states = &registry,
                .generation = generation_context{.min_y = 0,
                                                 .height = 32,
                                                 .sea_level = 8},
            };
            feature_context context{.placement = placement};
            std::vector<std::string> order;
            context.place_by_id = [&](std::string_view id, worldgen_random &,
                                      const block_pos &) {
                order.emplace_back(id);
                return id != "oracle:stop";
            };
            const sequence_feature sequence{{"oracle:first", "oracle:second"}};
            auto random =
                worldgen_random{std::make_unique<legacy_random_source>(1)};
            expect(sequence.place(context, random,
                                  block_pos{.x = 1, .y = 2, .z = 3}));
            expect(order ==
                   std::vector<std::string>({"oracle:first", "oracle:second"}));
            order.clear();
            const sequence_feature failing{
                {"oracle:first", "oracle:stop", "oracle:third"}};
            expect(!failing.place(context, random,
                                  block_pos{.x = 1, .y = 2, .z = 3}));
            expect(order ==
                   std::vector<std::string>({"oracle:first", "oracle:stop"}));
        };

    "replace blobs replaces a manhattan ball around the target block"_test =
        [] {
            block_state_registry registry;
            auto stone = registry.intern("minecraft:stone", {.air = false});
            auto gold = registry.intern("minecraft:gold_block", {.air = false});
            expect(stone.has_value() && gold.has_value());
            if (!stone || !gold) {
                return;
            }
            proto_chunk chunk{registry, 0, 0, 0, 32};
            chunk.set_status(chunk_status::biomes);
            for (jint z = 0; z < 16; ++z) {
                for (jint x = 0; x < 16; ++x) {
                    for (jint y = 4; y <= 8; ++y) {
                        expect(chunk.set_state(x, y, z, *stone));
                    }
                }
            }
            chunk_region region{chunk, 0};
            placement_context placement{
                .region = &region,
                .chunk = &chunk,
                .states = &registry,
                .generation = generation_context{.min_y = 0,
                                                 .height = 32,
                                                 .sea_level = 8},
            };
            feature_context context{.placement = placement};
            auto radius =
                refrain::worldgen::provider::int_provider::decode(parse("1"));
            expect(radius.has_value());
            if (!radius) {
                return;
            }
            const replace_blobs_feature feature{*stone, *gold,
                                                std::move(*radius)};
            auto random =
                worldgen_random{std::make_unique<legacy_random_source>(1)};
            expect(feature.place(context, random,
                                 block_pos{.x = 8, .y = 10, .z = 8}));
            expect(chunk.state_at(8, 8, 8) == *gold);
            expect(chunk.state_at(8, 7, 8) == *gold);
            expect(chunk.state_at(7, 8, 8) == *gold);
            expect(chunk.state_at(8, 6, 8) == *stone);
        };

    "random neighbor spread grows next to exactly one accepted block"_test = [] {
        block_state_registry registry;
        auto stone = registry.intern("minecraft:stone", {.air = false});
        auto glowstone = registry.intern("minecraft:glowstone", {.air = false});
        expect(stone.has_value() && glowstone.has_value());
        if (!stone || !glowstone) {
            return;
        }
        proto_chunk chunk{registry, 0, 0, 0, 32};
        chunk.set_status(chunk_status::biomes);
        for (jint z = 0; z < 16; ++z) {
            for (jint x = 0; x < 16; ++x) {
                expect(chunk.set_state(x, 4, z, *stone));
            }
        }
        chunk_region region{chunk, 0};
        placement_context placement{
            .region = &region,
            .chunk = &chunk,
            .states = &registry,
            .generation =
                generation_context{.min_y = 0, .height = 32, .sea_level = 8},
        };
        feature_context context{.placement = placement};
        auto can_replace = decode_block_predicate(
            parse(
                R"({"type":"minecraft:matching_blocks","blocks":["minecraft:air"]})"),
            registry);
        auto attempts =
            refrain::worldgen::provider::int_provider::decode(parse("64"));
        auto offset = refrain::worldgen::provider::int_provider::decode(parse(
            R"({"type":"minecraft:uniform","min_inclusive":-1,"max_inclusive":1})"));
        auto y_offset = refrain::worldgen::provider::int_provider::decode(parse(
            R"({"type":"minecraft:uniform","min_inclusive":-1,"max_inclusive":1})"));
        expect(can_replace.has_value() && attempts.has_value() &&
               offset.has_value() && y_offset.has_value());
        if (!can_replace || !attempts || !offset || !y_offset) {
            return;
        }
        auto block = simple_state(*glowstone);
        const random_neighbor_spread_feature feature{
            std::move(block),        {"minecraft:glowstone"},
            std::move(*can_replace), std::move(*attempts),
            std::move(*offset),      std::move(*y_offset)};
        auto random =
            worldgen_random{std::make_unique<legacy_random_source>(7)};
        expect(
            feature.place(context, random, block_pos{.x = 8, .y = 5, .z = 8}));
        expect(chunk.state_at(8, 5, 8) == *glowstone);
        std::size_t count = 0;
        for (jint z = 0; z < 16; ++z) {
            for (jint x = 0; x < 16; ++x) {
                for (jint y = 0; y < 32; ++y) {
                    if (chunk.state_at(x, y, z) == *glowstone) {
                        ++count;
                    }
                }
            }
        }
        expect(count > 1U);
    };

    "bamboo grows a stalk and replaces podzol beneath it"_test = [] {
        block_state_registry registry;
        auto dirt = registry.intern("minecraft:dirt", {.air = false});
        auto bamboo = registry.intern(
            "minecraft:bamboo[age=1,leaves=none,stage=0]", {.air = false});
        auto bamboo_large = registry.intern(
            "minecraft:bamboo[age=1,leaves=large,stage=1]", {.air = false});
        auto bamboo_top = registry.intern(
            "minecraft:bamboo[age=1,leaves=large,stage=0]", {.air = false});
        auto bamboo_small = registry.intern(
            "minecraft:bamboo[age=1,leaves=small,stage=0]", {.air = false});
        auto podzol = registry.intern("minecraft:podzol", {.air = false});
        expect(dirt.has_value() && bamboo.has_value() &&
               bamboo_large.has_value() && bamboo_top.has_value() &&
               bamboo_small.has_value() && podzol.has_value());
        if (!dirt || !bamboo || !bamboo_large || !bamboo_top || !bamboo_small ||
            !podzol) {
            return;
        }
        const std::array<block_state_id, 1> dirt_tag{*dirt};
        registry.add_tag("minecraft:supports_bamboo", dirt_tag);
        registry.add_tag("minecraft:beneath_bamboo_podzol_replaceable",
                         dirt_tag);
        proto_chunk chunk{registry, 0, 0, 0, 32};
        chunk.set_status(chunk_status::biomes);
        for (jint z = 0; z < 16; ++z) {
            for (jint x = 0; x < 16; ++x) {
                expect(chunk.set_state(x, 4, z, *dirt));
            }
        }
        chunk.set_status(chunk_status::terrain);
        chunk_region region{chunk, 0};
        placement_context placement{
            .region = &region,
            .chunk = &chunk,
            .states = &registry,
            .generation =
                generation_context{.min_y = 0, .height = 32, .sea_level = 8},
        };
        feature_context context{.placement = placement};
        const bamboo_feature feature{1.0F,        *bamboo,       *bamboo_large,
                                     *bamboo_top, *bamboo_small, *podzol};
        auto random =
            worldgen_random{std::make_unique<legacy_random_source>(3)};
        expect(
            feature.place(context, random, block_pos{.x = 8, .y = 5, .z = 8}));
        expect(chunk.state_at(8, 5, 8) == *bamboo);
        expect(chunk.state_at(8, 4, 8) == *podzol);
        jint bamboo_count = 0;
        for (jint y = 5; y < 32; ++y) {
            const auto state = chunk.state_at(8, y, 8);
            if (state == *bamboo || state == *bamboo_large ||
                state == *bamboo_top || state == *bamboo_small) {
                ++bamboo_count;
            }
        }
        expect(bamboo_count >= 5);
    };

    "vines attach to the first acceptable neighbour"_test = [] {
        block_state_registry registry;
        auto stone = registry.intern("minecraft:stone",
                                     {.air = false, .blocks_motion = true});
        expect(stone.has_value());
        if (!stone) {
            return;
        }
        std::array<block_state_id, 6> vines_states{};
        constexpr std::array<std::string_view, 6> keys{{
            "minecraft:vine[east=false,north=false,south=false,up=false,west="
            "false]",
            "minecraft:vine[east=false,north=false,south=false,up=true,west="
            "false]",
            "minecraft:vine[east=false,north=true,south=false,up=false,west="
            "false]",
            "minecraft:vine[east=false,north=false,south=true,up=false,west="
            "false]",
            "minecraft:vine[east=false,north=false,south=false,up=false,west="
            "true]",
            "minecraft:vine[east=true,north=false,south=false,up=false,west="
            "false]",
        }};
        for (std::size_t i = 0; i < keys.size(); ++i) {
            const auto state = registry.intern(keys[i], {.air = false});
            expect(state.has_value());
            if (!state) {
                return;
            }
            vines_states[i] = *state;
        }
        proto_chunk chunk{registry, 0, 0, 0, 32};
        chunk.set_status(chunk_status::biomes);
        expect(chunk.set_state(8, 5, 7, *stone));
        chunk_region region{chunk, 0};
        placement_context placement{
            .region = &region,
            .chunk = &chunk,
            .states = &registry,
            .generation =
                generation_context{.min_y = 0, .height = 32, .sea_level = 8},
        };
        feature_context context{.placement = placement};
        const vines_feature feature{vines_states};
        auto random =
            worldgen_random{std::make_unique<legacy_random_source>(1)};
        expect(
            feature.place(context, random, block_pos{.x = 8, .y = 5, .z = 8}));
        expect(chunk.state_at(8, 5, 8) == vines_states[2]);
    };

    "a planted huge fungus builds a stem and a hat"_test = [] {
        block_state_registry registry;
        auto nylium = registry.intern("minecraft:crimson_nylium",
                                      {.air = false, .blocks_motion = true});
        auto stem = registry.intern("minecraft:crimson_stem",
                                    {.air = false, .blocks_motion = true});
        auto hat = registry.intern("minecraft:nether_wart_block",
                                   {.air = false, .blocks_motion = true});
        auto decor = registry.intern("minecraft:shroomlight",
                                     {.air = false, .blocks_motion = true});
        std::array<block_state_id, 3> heads{};
        for (jint age = 0; age < 3; ++age) {
            const auto state = registry.intern(
                "minecraft:weeping_vines[age=" + std::to_string(23 + age) + "]",
                {.air = false});
            expect(state.has_value());
            if (!state) {
                return;
            }
            heads[static_cast<std::size_t>(age)] = *state;
        }
        auto plant =
            registry.intern("minecraft:weeping_vines_plant", {.air = false});
        expect(nylium.has_value() && stem.has_value() && hat.has_value() &&
               decor.has_value() && plant.has_value());
        if (!nylium || !stem || !hat || !decor || !plant) {
            return;
        }
        proto_chunk chunk{registry, 0, 0, 0, 32};
        chunk.set_status(chunk_status::biomes);
        for (jint z = 0; z < 16; ++z) {
            for (jint x = 0; x < 16; ++x) {
                expect(chunk.set_state(x, 4, z, *nylium));
            }
        }
        chunk_region region{chunk, 0};
        placement_context placement{
            .region = &region,
            .chunk = &chunk,
            .states = &registry,
            .generation =
                generation_context{.min_y = 0, .height = 32, .sea_level = 8},
        };
        feature_context context{.placement = placement};
        auto replaceable = decode_block_predicate(
            parse(
                R"({"type":"minecraft:matching_blocks","blocks":["minecraft:air"]})"),
            registry);
        expect(replaceable.has_value());
        if (!replaceable) {
            return;
        }
        const huge_fungus_feature feature{*nylium,
                                          *stem,
                                          *hat,
                                          *decor,
                                          heads,
                                          *plant,
                                          std::move(*replaceable),
                                          true,
                                          true};
        auto random =
            worldgen_random{std::make_unique<legacy_random_source>(11)};
        expect(
            feature.place(context, random, block_pos{.x = 8, .y = 5, .z = 8}));
        expect(chunk.state_at(8, 5, 8) == *stem);
        jint hat_count = 0;
        for (jint y = 5; y < 32; ++y) {
            for (jint z = 0; z < 16; ++z) {
                for (jint x = 0; x < 16; ++x) {
                    if (chunk.state_at(x, y, z) == *hat) {
                        ++hat_count;
                    }
                }
            }
        }
        expect(hat_count > 0);
    };

    "a huge mushroom builds a stem and a shaped cap"_test = [] {
        block_state_registry registry;
        auto dirt = registry.intern("minecraft:dirt",
                                    {.air = false, .blocks_motion = true});
        auto cap = registry.intern(
            "minecraft:brown_mushroom_block[down=true,east=true,north=true,"
            "south=true,up=true,west=true]",
            {.air = false, .blocks_motion = true});
        auto stem = registry.intern(
            "minecraft:mushroom_stem[down=false,east=true,north=true,"
            "south=true,up=false,west=true]",
            {.air = false, .blocks_motion = true});
        expect(dirt.has_value() && cap.has_value() && stem.has_value());
        if (!dirt || !cap || !stem) {
            return;
        }
        const std::array<block_state_id, 1> dirt_tag{*dirt};
        registry.add_tag("minecraft:huge_brown_mushroom_can_place_on",
                         dirt_tag);
        proto_chunk chunk{registry, 0, 0, 0, 32};
        chunk.set_status(chunk_status::biomes);
        for (jint z = 0; z < 16; ++z) {
            for (jint x = 0; x < 16; ++x) {
                expect(chunk.set_state(x, 4, z, *dirt));
            }
        }
        chunk_region region{chunk, 0};
        const auto resolve = [&registry](std::string_view key) {
            if (const auto found = registry.find(key); found) {
                return *found;
            }
            const auto block = state_block_name(key);
            const bool air = is_air_block_name(block);
            return registry.intern(key, {.air = air, .blocks_motion = !air})
                .value_or(no_block_state);
        };
        placement_context placement{
            .region = &region,
            .chunk = &chunk,
            .states = &registry,
            .generation =
                generation_context{.min_y = 0, .height = 32, .sea_level = 8},
            .resolve_state = resolve,
        };
        feature_context context{.placement = placement};
        auto predicate = decode_block_predicate(
            parse(
                R"({"type":"minecraft:matching_block_tag","tag":"minecraft:huge_brown_mushroom_can_place_on"})"),
            registry);
        expect(predicate.has_value());
        if (!predicate) {
            return;
        }
        const huge_mushroom_feature feature{false, simple_state(*cap),
                                            simple_state(*stem), 2,
                                            std::move(*predicate)};
        auto random =
            worldgen_random{std::make_unique<legacy_random_source>(5)};
        expect(
            feature.place(context, random, block_pos{.x = 8, .y = 5, .z = 8}));
        expect(chunk.state_at(8, 5, 8) == *stem);
        bool cap_found = false;
        for (jint y = 5; y < 32; ++y) {
            for (jint z = 0; z < 16; ++z) {
                for (jint x = 0; x < 16; ++x) {
                    if (state_block_name(registry.name(chunk.state_at(
                            x, y, z))) == "minecraft:brown_mushroom_block") {
                        cap_found = true;
                    }
                }
            }
        }
        expect(cap_found);
    };

    "an iceberg grows ice above and below sea level"_test = [] {
        block_state_registry registry;
        auto water = registry.intern(
            "minecraft:water", {.air = false, .fluid = true, .water = true});
        auto blue = registry.intern("minecraft:blue_ice",
                                    {.air = false, .blocks_motion = true});
        auto packed = registry.intern("minecraft:packed_ice",
                                      {.air = false, .blocks_motion = true});
        auto ice = registry.intern("minecraft:ice",
                                   {.air = false, .blocks_motion = true});
        auto snow_block = registry.intern(
            "minecraft:snow_block", {.air = false, .blocks_motion = true});
        auto snow_layer =
            registry.intern("minecraft:snow[layers=1]", {.air = false});
        expect(water.has_value() && blue.has_value() && packed.has_value() &&
               ice.has_value() && snow_block.has_value() &&
               snow_layer.has_value());
        if (!water || !blue || !packed || !ice || !snow_block || !snow_layer) {
            return;
        }
        proto_chunk chunk{registry, 0, 0, 0, 32};
        chunk.set_status(chunk_status::biomes);
        for (jint z = 0; z < 16; ++z) {
            for (jint x = 0; x < 16; ++x) {
                for (jint y = 0; y <= 8; ++y) {
                    expect(chunk.set_state(x, y, z, *water));
                }
            }
        }
        chunk_region region{chunk, 0};
        placement_context placement{
            .region = &region,
            .chunk = &chunk,
            .states = &registry,
            .generation =
                generation_context{.min_y = 0, .height = 32, .sea_level = 8},
        };
        feature_context context{.placement = placement};
        const iceberg_feature feature{*blue,   *snow_block, *snow_layer, *ice,
                                      *packed, *blue,       *water};
        auto random =
            worldgen_random{std::make_unique<legacy_random_source>(13)};
        expect(
            feature.place(context, random, block_pos{.x = 8, .y = 8, .z = 8}));
        jint ice_count = 0;
        for (jint y = 0; y < 32; ++y) {
            for (jint z = 0; z < 16; ++z) {
                for (jint x = 0; x < 16; ++x) {
                    const auto state = chunk.state_at(x, y, z);
                    if (state == *blue || state == *packed || state == *ice ||
                        state == *snow_block || state == *snow_layer) {
                        ++ice_count;
                    }
                }
            }
        }
        expect(ice_count > 0);
    };

    "a geode writes its layered shell into stone"_test = [] {
        block_state_registry registry;
        auto stone = registry.intern("minecraft:stone",
                                     {.air = false, .blocks_motion = true});
        auto air = registry.intern("minecraft:air", {.air = true});
        auto amethyst = registry.intern("minecraft:amethyst_block",
                                        {.air = false, .blocks_motion = true});
        auto budding = registry.intern("minecraft:budding_amethyst",
                                       {.air = false, .blocks_motion = true});
        auto calcite = registry.intern("minecraft:calcite",
                                       {.air = false, .blocks_motion = true});
        auto basalt = registry.intern("minecraft:smooth_basalt",
                                      {.air = false, .blocks_motion = true});
        expect(stone.has_value() && air.has_value() && amethyst.has_value() &&
               budding.has_value() && calcite.has_value() &&
               basalt.has_value());
        if (!stone || !air || !amethyst || !budding || !calcite || !basalt) {
            return;
        }
        proto_chunk chunk{registry, 0, 0, 0, 32};
        chunk.set_status(chunk_status::biomes);
        for (jint z = 0; z < 16; ++z) {
            for (jint x = 0; x < 16; ++x) {
                for (jint y = 0; y < 32; ++y) {
                    expect(chunk.set_state(x, y, z, *stone));
                }
            }
        }
        chunk_region region{chunk, 0};
        placement_context placement{
            .region = &region,
            .chunk = &chunk,
            .states = &registry,
            .generation =
                generation_context{
                    .min_y = 0, .height = 32, .sea_level = 8, .world_seed = 1},
            .resolve_state =
                [&registry](std::string_view key) {
                    return registry.find(key).value_or(no_block_state);
                },
        };
        feature_context context{.placement = placement};
        geode_parameters parameters{
            .filling = simple_state(*air),
            .inner = simple_state(*amethyst),
            .alternate_inner = simple_state(*budding),
            .middle = simple_state(*calcite),
            .outer = simple_state(*basalt),
            .cannot_replace_tag = "",
            .invalid_blocks_tag = "",
            .crack_chance = 0.0,
            .outer_wall_min = 2,
            .outer_wall_max = 3,
            .distribution_min = 2,
            .distribution_max = 3,
            .point_offset_min = 1,
            .point_offset_max = 1,
            .min_gen_offset = -8,
            .max_gen_offset = 8,
        };
        const geode_feature feature{std::move(parameters)};
        auto random =
            worldgen_random{std::make_unique<legacy_random_source>(17)};
        expect(
            feature.place(context, random, block_pos{.x = 8, .y = 16, .z = 8}));
        jint changed = 0;
        for (jint y = 0; y < 32; ++y) {
            for (jint z = 0; z < 16; ++z) {
                for (jint x = 0; x < 16; ++x) {
                    if (chunk.state_at(x, y, z) != *stone) {
                        ++changed;
                    }
                }
            }
        }
        expect(changed > 0);
    };

    "freeze top layer ices the floor and snows the top"_test = [] {
        block_state_registry registry;
        auto grass = registry.intern("minecraft:grass_block[snowy=false]",
                                     {.air = false, .blocks_motion = true});
        auto ice = registry.intern("minecraft:ice",
                                   {.air = false, .blocks_motion = true});
        auto snow = registry.intern("minecraft:snow[layers=1]", {.air = false});
        expect(grass.has_value() && ice.has_value() && snow.has_value());
        if (!grass || !ice || !snow) {
            return;
        }
        proto_chunk chunk{registry, 0, 0, 0, 32};
        chunk.set_status(chunk_status::biomes);
        for (jint z = 0; z < 16; ++z) {
            for (jint x = 0; x < 16; ++x) {
                expect(chunk.set_state(x, 4, z, *grass));
            }
        }
        chunk_region region{chunk, 0};
        placement_context placement{
            .region = &region,
            .chunk = &chunk,
            .states = &registry,
            .generation =
                generation_context{.min_y = 0, .height = 32, .sea_level = 8},
            .height = [](heightmap_kind, jint, jint) { return jint{5}; },
            .resolve_state =
                [&registry](std::string_view key) {
                    return registry.find(key).value_or(no_block_state);
                },
            .should_freeze = [](const block_pos &) { return true; },
            .should_snow = [](const block_pos &) { return true; },
        };
        feature_context context{.placement = placement};
        const freeze_top_layer_feature feature{*ice, *snow};
        auto random =
            worldgen_random{std::make_unique<legacy_random_source>(1)};
        expect(
            feature.place(context, random, block_pos{.x = 0, .y = 0, .z = 0}));
        expect(chunk.state_at(0, 4, 0) == *ice);
        expect(chunk.state_at(0, 5, 0) == *snow);
    };

    "freeze marks a bare grass block as snowy"_test = [] {
        block_state_registry registry;
        auto grass = registry.intern("minecraft:grass_block",
                                     {.air = false, .blocks_motion = true});
        auto grass_snowy =
            registry.intern("minecraft:grass_block[snowy=true]",
                            {.air = false, .blocks_motion = true});
        auto ice = registry.intern("minecraft:ice",
                                   {.air = false, .blocks_motion = true});
        auto snow = registry.intern("minecraft:snow[layers=1]", {.air = false});
        expect(grass.has_value() && grass_snowy.has_value() &&
               ice.has_value() && snow.has_value());
        if (!grass || !grass_snowy || !ice || !snow) {
            return;
        }
        proto_chunk chunk{registry, 0, 0, 0, 32};
        chunk.set_status(chunk_status::biomes);
        for (jint z = 0; z < 16; ++z) {
            for (jint x = 0; x < 16; ++x) {
                expect(chunk.set_state(x, 4, z, *grass));
                expect(chunk.set_state(x, 5, z, registry.air()));
            }
        }
        chunk_region region{chunk, 0};
        placement_context placement{
            .region = &region,
            .chunk = &chunk,
            .states = &registry,
            .generation =
                generation_context{.min_y = 0, .height = 32, .sea_level = 8},
            .height = [](heightmap_kind, jint, jint) { return jint{5}; },
            .resolve_state =
                [&registry](std::string_view key) {
                    return registry.find(key).value_or(no_block_state);
                },
            .should_freeze = [](const block_pos &) { return false; },
            .should_snow = [](const block_pos &) { return true; },
        };
        feature_context context{.placement = placement};
        const freeze_top_layer_feature feature{*ice, *snow};
        auto random =
            worldgen_random{std::make_unique<legacy_random_source>(1)};
        expect(
            feature.place(context, random, block_pos{.x = 0, .y = 0, .z = 0}));
        expect(chunk.state_at(0, 5, 0) == *snow);
        expect(chunk.state_at(0, 4, 0) == *grass_snowy);
    };

    "a delta writes lava into a flat cave floor"_test = [] {
        block_state_registry registry;
        auto stone = registry.intern("minecraft:stone",
                                     {.air = false, .blocks_motion = true});
        auto lava =
            registry.intern("minecraft:lava", {.air = false, .fluid = true});
        auto magma = registry.intern("minecraft:magma_block",
                                     {.air = false, .blocks_motion = true});
        expect(stone.has_value() && lava.has_value() && magma.has_value());
        if (!stone || !lava || !magma) {
            return;
        }
        proto_chunk chunk{registry, 0, 0, 0, 32};
        chunk.set_status(chunk_status::biomes);
        for (jint z = 0; z < 16; ++z) {
            for (jint x = 0; x < 16; ++x) {
                for (jint y = 0; y < 16; ++y) {
                    expect(chunk.set_state(x, y, z, *stone));
                }
            }
        }
        chunk_region region{chunk, 0};
        placement_context placement{
            .region = &region,
            .chunk = &chunk,
            .states = &registry,
            .generation =
                generation_context{.min_y = 0, .height = 32, .sea_level = 8},
        };
        feature_context context{.placement = placement};
        auto size =
            refrain::worldgen::provider::int_provider::decode(parse("3"));
        auto rim =
            refrain::worldgen::provider::int_provider::decode(parse("1"));
        expect(size.has_value() && rim.has_value());
        if (!size || !rim) {
            return;
        }
        const delta_feature feature{*lava, *magma, std::move(*size),
                                    std::move(*rim)};
        auto random =
            worldgen_random{std::make_unique<legacy_random_source>(29)};
        expect(
            feature.place(context, random, block_pos{.x = 8, .y = 15, .z = 8}));
        jint lava_count = 0;
        for (jint y = 0; y < 16; ++y) {
            for (jint z = 0; z < 16; ++z) {
                for (jint x = 0; x < 16; ++x) {
                    if (chunk.state_at(x, y, z) == *lava) {
                        ++lava_count;
                    }
                }
            }
        }
        expect(lava_count > 0);
    };

    "large dripstone grows between a cave floor and ceiling"_test = [] {
        block_state_registry registry;
        auto stone = registry.intern("minecraft:stone",
                                     {.air = false, .blocks_motion = true});
        auto dripstone = registry.intern("minecraft:dripstone_block",
                                         {.air = false, .blocks_motion = true});
        expect(stone.has_value() && dripstone.has_value());
        if (!stone || !dripstone) {
            return;
        }
        const std::array<block_state_id, 1> stone_tag{*stone};
        registry.add_tag("minecraft:base_stone_overworld", stone_tag);
        registry.add_tag("minecraft:dripstone_replaceable_blocks", stone_tag);
        proto_chunk chunk{registry, 0, 0, 0, 32};
        chunk.set_status(chunk_status::biomes);
        for (jint z = 0; z < 16; ++z) {
            for (jint x = 0; x < 16; ++x) {
                for (jint y = 0; y < 32; ++y) {
                    expect(chunk.set_state(x, y, z, *stone));
                }
            }
        }
        for (jint y = 10; y <= 18; ++y) {
            expect(chunk.set_state(8, y, 8, registry.air()));
        }
        chunk_region region{chunk, 0};
        placement_context placement{
            .region = &region,
            .chunk = &chunk,
            .states = &registry,
            .generation =
                generation_context{.min_y = 0, .height = 32, .sea_level = 8},
            .resolve_state =
                [&registry](std::string_view key) {
                    return registry.find(key).value_or(no_block_state);
                },
        };
        feature_context context{.placement = placement};
        auto radius = refrain::worldgen::provider::int_provider::decode(parse(
            R"({"type":"minecraft:uniform","min_inclusive":4,"max_inclusive":8})"));
        auto scale = refrain::worldgen::provider::float_provider::decode(parse(
            R"({"type":"minecraft:uniform","min_inclusive":1.0,"max_exclusive":2.0})"));
        auto blunt = refrain::worldgen::provider::float_provider::decode(parse(
            R"({"type":"minecraft:uniform","min_inclusive":0.3,"max_exclusive":0.9})"));
        auto blunt2 = refrain::worldgen::provider::float_provider::decode(parse(
            R"({"type":"minecraft:uniform","min_inclusive":0.3,"max_exclusive":0.9})"));
        auto wind = refrain::worldgen::provider::float_provider::decode(parse(
            R"({"type":"minecraft:uniform","min_inclusive":0.0,"max_exclusive":0.3})"));
        expect(radius.has_value() && scale.has_value() && blunt.has_value() &&
               blunt2.has_value() && wind.has_value());
        if (!radius || !scale || !blunt || !blunt2 || !wind) {
            return;
        }
        const large_dripstone_feature feature{
            "minecraft:dripstone_replaceable_blocks",
            30,
            std::move(*radius),
            4,
            8,
            std::move(*scale),
            0.33F,
            std::move(*blunt),
            std::move(*blunt2),
            std::move(*wind),
            4,
            0.6F};
        auto random =
            worldgen_random{std::make_unique<legacy_random_source>(31)};
        expect(
            feature.place(context, random, block_pos{.x = 8, .y = 14, .z = 8}));
        jint dripstone_count = 0;
        for (jint y = 0; y < 32; ++y) {
            for (jint z = 0; z < 16; ++z) {
                for (jint x = 0; x < 16; ++x) {
                    if (chunk.state_at(x, y, z) == *dripstone) {
                        ++dripstone_count;
                    }
                }
            }
        }
        expect(dripstone_count > 0);
    };
};

suite<"worldgen/tree"> tree_tests = [] {
    "a straight tree places logs, leaves and the below-trunk block"_test = [] {
        block_state_registry registry;
        auto dirt = registry.intern("minecraft:dirt",
                                    {.air = false,
                                     .blocks_motion = true,
                                     .blocks_motion_no_leaves = true});
        auto log = registry.intern("minecraft:oak_log[axis=y]",
                                   {.air = false,
                                    .blocks_motion = true,
                                    .blocks_motion_no_leaves = true});
        auto leaves = registry.intern("minecraft:oak_leaves[distance=7,"
                                      "persistent=false,waterlogged=false]",
                                      {.air = false,
                                       .blocks_motion = true,
                                       .blocks_motion_no_leaves = true});
        expect(dirt.has_value() && log.has_value() && leaves.has_value());
        if (!dirt || !log || !leaves) {
            return;
        }
        auto radius = provider::int_provider::decode(parse("2"));
        auto offset = provider::int_provider::decode(parse("0"));
        expect(radius.has_value() && offset.has_value());
        if (!radius || !offset) {
            return;
        }
        tree_feature tree{
            registry,
            trunk_kind::straight,
            foliage_kind::blob,
            simple_state(*log),
            simple_state(*leaves),
            simple_state(*dirt),
            4,
            2,
            0,
            std::move(*radius),
            std::move(*offset),
            nullptr,
            nullptr,
            3,
            1,
            0,
            1,
            false,
            1,
            1,
            false,
            0,
            true,
            [] {
                std::vector<
                    std::unique_ptr<refrain::worldgen::feature::tree_decorator>>
                    out;
                out.push_back(std::make_unique<trunk_vine_decorator>());
                return out;
            }()};
        proto_chunk chunk{registry, 0, 0, 0, 32};
        chunk.set_status(chunk_status::terrain);
        chunk_region region{chunk, 1};
        feature_context context{.placement = placement_context{
                                    .region = &region,
                                    .chunk = &chunk,
                                    .states = &registry,
                                    .generation =
                                        generation_context{
                                            .min_y = 0,
                                            .height = 32,
                                            .sea_level = 8,
                                        },
                                }};
        auto random =
            worldgen_random{std::make_unique<xoroshiro_random_source>(12345)};
        expect(tree.place(context, random, block_pos{.x = 8, .y = 10, .z = 8}));
        expect(chunk.state_at(8, 9, 8) == *dirt);
        std::size_t logs = 0;
        std::size_t leaf_blocks = 0;
        for (jint y = 0; y < 32; ++y) {
            for (jint x = 0; x < 16; ++x) {
                for (jint z = 0; z < 16; ++z) {
                    const auto name = registry.name(chunk.state_at(x, y, z));
                    logs += name.starts_with("minecraft:oak_log");
                    leaf_blocks += name.starts_with("minecraft:oak_leaves");
                }
            }
        }
        expect(logs > 0U);
        expect(leaf_blocks > 0U);
        std::size_t vines = 0;
        for (jint y = 0; y < 32; ++y) {
            for (jint x = 0; x < 16; ++x) {
                for (jint z = 0; z < 16; ++z) {
                    vines += registry.name(chunk.state_at(x, y, z))
                                 .starts_with("minecraft:vine");
                }
            }
        }
        expect(vines > 0U);
    };

    "an acacia foliage placer creates the three flat rows"_test = [] {
        block_state_registry registry;
        auto dirt = registry.intern("minecraft:dirt",
                                    {.air = false,
                                     .blocks_motion = true,
                                     .blocks_motion_no_leaves = true});
        auto log = registry.intern("minecraft:acacia_log[axis=y]",
                                   {.air = false,
                                    .blocks_motion = true,
                                    .blocks_motion_no_leaves = true});
        auto leaves = registry.intern("minecraft:acacia_leaves[distance=7,"
                                      "persistent=false,waterlogged=false]",
                                      {.air = false,
                                       .blocks_motion = true,
                                       .blocks_motion_no_leaves = true});
        expect(dirt.has_value() && log.has_value() && leaves.has_value());
        if (!dirt || !log || !leaves) {
            return;
        }
        auto radius = provider::int_provider::decode(parse("2"));
        auto offset = provider::int_provider::decode(parse("0"));
        if (!radius || !offset) {
            return;
        }
        tree_feature tree{registry,
                          trunk_kind::straight,
                          foliage_kind::acacia,
                          simple_state(*log),
                          simple_state(*leaves),
                          simple_state(*dirt),
                          5,
                          2,
                          2,
                          std::move(*radius),
                          std::move(*offset),
                          nullptr,
                          nullptr,
                          0,
                          1,
                          0,
                          2,
                          false,
                          1,
                          1,
                          false,
                          0,
                          true};
        proto_chunk chunk{registry, 0, 0, 0, 32};
        chunk.set_status(chunk_status::terrain);
        chunk_region region{chunk, 1};
        feature_context context{.placement = placement_context{
                                    .region = &region,
                                    .chunk = &chunk,
                                    .states = &registry,
                                    .generation =
                                        generation_context{
                                            .min_y = 0,
                                            .height = 32,
                                            .sea_level = 8,
                                        },
                                }};
        auto random =
            worldgen_random{std::make_unique<xoroshiro_random_source>(12345)};
        expect(tree.place(context, random, block_pos{.x = 8, .y = 10, .z = 8}));
        std::size_t leaf_blocks = 0;
        for (jint y = 0; y < 32; ++y) {
            for (jint x = 0; x < 16; ++x) {
                for (jint z = 0; z < 16; ++z) {
                    leaf_blocks += registry.name(chunk.state_at(x, y, z))
                                       .starts_with("minecraft:acacia_leaves");
                }
            }
        }
        expect(leaf_blocks > 0U);
    };
};

suite<"worldgen/snow-layer"> snow_layer_tests = [] {
    "a one-layer snow block has no collision"_test = [] {
        block_state_registry registry;
        auto provider = decode_state_provider(
            parse(R"({"id":"minecraft:snow","properties":{"layers":"1"}})"),
            registry);
        expect(provider.has_value());
        if (!provider) {
            return;
        }
        auto random =
            worldgen_random{std::make_unique<xoroshiro_random_source>(12345)};
        const placement_context context{};
        const auto state = (*provider)->get(context, random,
                                            block_pos{.x = 0, .y = 0, .z = 0});
        expect(!registry.traits(state).air);
        expect(!registry.traits(state).blocks_motion);
        expect(!registry.traits(state).blocks_motion_no_leaves);
    };
};

suite<"worldgen/underwater-magma"> underwater_magma_tests = [] {
    "magma fills buried floor positions under a water column"_test = [] {
        block_state_registry registry;
        auto stone = registry.intern("minecraft:stone",
                                     {.air = false,
                                      .blocks_motion = true,
                                      .blocks_motion_no_leaves = true});
        auto water = registry.intern(
            "minecraft:water", {.air = false, .fluid = true, .water = true});
        auto magma = registry.intern("minecraft:magma_block",
                                     {.air = false,
                                      .blocks_motion = true,
                                      .blocks_motion_no_leaves = true});
        expect(stone && water && magma);
        if (!stone || !water || !magma) {
            return;
        }

        proto_chunk chunk{registry, 0, 0, 0, 64};
        chunk.set_status(chunk_status::terrain);
        for (jint y = 0; y < 64; ++y) {
            for (jint z = 0; z < 16; ++z) {
                for (jint x = 0; x < 16; ++x) {
                    chunk.set_state(x, y, z, *stone);
                }
            }
        }
        for (jint y = 20; y <= 23; ++y) {
            chunk.set_state(8, y, 8, *water);
        }
        chunk_region region{chunk, 0};
        feature_context context{.placement = placement_context{
                                    .region = &region,
                                    .chunk = &chunk,
                                    .states = &registry,
                                    .generation =
                                        generation_context{
                                            .min_y = 0,
                                            .height = 64,
                                            .sea_level = 8,
                                        },
                                }};
        auto random =
            worldgen_random{std::make_unique<xoroshiro_random_source>(12345)};
        underwater_magma_feature feature{5, 1, 1.0F, *magma};

        expect(!feature.place(context, random,
                              block_pos{.x = 8, .y = 40, .z = 8}));
        expect(
            feature.place(context, random, block_pos{.x = 8, .y = 23, .z = 8}));
        expect(chunk.state_at(8, 19, 8) == *magma);
    };
};

suite<"worldgen/speleothem"> speleothem_tests = [] {
    "a cave floor and ceiling grow pointed dripstone columns"_test = [] {
        block_state_registry registry;
        auto stone = registry.intern("minecraft:stone",
                                     {.air = false,
                                      .blocks_motion = true,
                                      .blocks_motion_no_leaves = true});
        auto dripstone = registry.intern("minecraft:dripstone_block",
                                         {.air = false,
                                          .blocks_motion = true,
                                          .blocks_motion_no_leaves = true});
        auto pointed =
            registry.intern("minecraft:pointed_dripstone[thickness=tip,tip_"
                            "direction=up,waterlogged=false]",
                            {.air = false,
                             .blocks_motion = false,
                             .blocks_motion_no_leaves = false});
        auto cave_air = registry.intern("minecraft:cave_air", {.air = true});
        auto water =
            registry.intern("minecraft:water", {.fluid = true, .water = true});
        expect(stone && dripstone && pointed && cave_air && water);
        if (!stone || !dripstone || !pointed || !cave_air || !water) {
            return;
        }
        registry.add_tag("minecraft:dripstone_replaceable_blocks",
                         std::span<const block_state_id>{&*stone, 1});
        registry.add_tag("minecraft:base_stone_overworld",
                         std::span<const block_state_id>{&*stone, 1});
        proto_chunk chunk{registry, 0, 0, 0, 64};
        for (jint y = 0; y < 64; ++y) {
            for (jint z = 0; z < 16; ++z) {
                for (jint x = 0; x < 16; ++x) {
                    chunk.set_state(x, y, z, *stone);
                }
            }
        }
        for (jint y = 30; y <= 33; ++y) {
            for (jint z = 2; z <= 14; ++z) {
                for (jint x = 2; x <= 14; ++x) {
                    chunk.set_state(x, y, z, *cave_air);
                }
            }
        }
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
        feature_context context{.placement = placement_context{
                                    .region = &region,
                                    .chunk = &chunk,
                                    .states = &registry,
                                    .generation =
                                        generation_context{
                                            .min_y = 0,
                                            .height = 64,
                                            .sea_level = 8,
                                        },
                                    .resolve_state = resolve,
                                }};
        auto height = provider::int_provider::decode(
            *refrain::data::json::value::parse("3"));
        auto radius = provider::int_provider::decode(
            *refrain::data::json::value::parse("2"));
        auto layer = provider::int_provider::decode(
            *refrain::data::json::value::parse("2"));
        expect(height && radius && layer);
        if (!height || !radius || !layer) {
            return;
        }
        speleothem_cluster_feature feature{
            *dripstone,
            *pointed,
            "minecraft:dripstone_replaceable_blocks",
            12,
            std::move(*height),
            std::move(*radius),
            1,
            3,
            std::move(*layer),
            provider::make_constant_float(1.0F),
            provider::make_constant_float(0.0F),
            1.0F,
            3,
            8,
        };
        auto random =
            worldgen_random{std::make_unique<xoroshiro_random_source>(12345)};
        expect(
            feature.place(context, random, block_pos{.x = 8, .y = 31, .z = 8}));
        jint pointed_count = 0;
        jint base_count = 0;
        for (jint y = 24; y <= 40; ++y) {
            for (jint z = 4; z <= 12; ++z) {
                for (jint x = 4; x <= 12; ++x) {
                    const auto name = registry.name(chunk.state_at(x, y, z));
                    if (name.starts_with("minecraft:pointed_dripstone")) {
                        ++pointed_count;
                    }
                    if (name == "minecraft:dripstone_block") {
                        ++base_count;
                    }
                }
            }
        }
        expect(pointed_count > 0);
        expect(base_count > 0);
    };
};

suite<"worldgen/multiface"> multiface_tests = [] {
    "glow lichen attaches to the face of a support block"_test = [] {
        block_state_registry registry;
        auto stone = registry.intern("minecraft:stone",
                                     {.air = false,
                                      .blocks_motion = true,
                                      .blocks_motion_no_leaves = true});
        expect(stone.has_value());
        if (!stone) {
            return;
        }
        proto_chunk chunk{registry, 0, 0, 0, 16};
        for (jint z = 0; z < 16; ++z) {
            for (jint x = 0; x < 16; ++x) {
                chunk.set_state(x, 0, z, *stone);
            }
        }
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
        feature_context context{.placement = placement_context{
                                    .region = &region,
                                    .chunk = &chunk,
                                    .states = &registry,
                                    .resolve_state = resolve,
                                }};
        auto random =
            worldgen_random{std::make_unique<xoroshiro_random_source>(12345)};
        multiface_growth_feature feature{
            "minecraft:glow_lichen", 10, true, true, true, 0.5F,
            {"minecraft:stone"}};
        expect(
            feature.place(context, random, block_pos{.x = 8, .y = 1, .z = 8}));
        const auto name = registry.name(chunk.state_at(8, 1, 8));
        expect(name.starts_with("minecraft:glow_lichen["));
        expect(name.find("down=true") != std::string_view::npos);
        jint count = 0;
        for (jint y = 1; y < 4; ++y) {
            for (jint z = 0; z < 16; ++z) {
                for (jint x = 0; x < 16; ++x) {
                    if (registry.name(chunk.state_at(x, y, z))
                            .starts_with("minecraft:glow_lichen")) {
                        ++count;
                    }
                }
            }
        }
        expect(count >= 1);

        auto second =
            worldgen_random{std::make_unique<xoroshiro_random_source>(7)};
        expect(
            !feature.place(context, second, block_pos{.x = 8, .y = 6, .z = 8}));
    };
};

suite<"worldgen/platform"> platform_tests = [] {
    "the void platform fills the origin chunk and skips far chunks"_test = [] {
        block_state_registry registry;
        auto stone = registry.intern("minecraft:stone",
                                     {.air = false,
                                      .blocks_motion = true,
                                      .blocks_motion_no_leaves = true});
        auto cobblestone = registry.intern("minecraft:cobblestone",
                                           {.air = false,
                                            .blocks_motion = true,
                                            .blocks_motion_no_leaves = true});
        expect(stone && cobblestone);
        if (!stone || !cobblestone) {
            return;
        }
        proto_chunk chunk{registry, 0, 0, 0, 128};
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
        feature_context context{.placement = placement_context{
                                    .region = &region,
                                    .chunk = &chunk,
                                    .states = &registry,
                                    .resolve_state = resolve,
                                }};
        auto random =
            worldgen_random{std::make_unique<xoroshiro_random_source>(7)};
        void_start_platform_feature platform;
        expect(platform.place(context, random,
                              block_pos{.x = 8, .y = 50, .z = 8}));
        expect(chunk.state_at(0, 53, 0) == *stone);
        expect(chunk.state_at(8, 53, 8) == *cobblestone);
        expect(chunk.state_at(15, 53, 15) == *stone);

        proto_chunk far_chunk{registry, 2, 0, 0, 128};
        chunk_region far_region{far_chunk, 0};
        feature_context far_context{.placement = placement_context{
                                        .region = &far_region,
                                        .chunk = &far_chunk,
                                        .states = &registry,
                                        .resolve_state = resolve,
                                    }};
        expect(platform.place(far_context, random,
                              block_pos{.x = 40, .y = 50, .z = 8}));
        expect(far_chunk.state_at(0, 53, 0) == registry.air());
    };

    "the end platform clears the interior and lays obsidian"_test = [] {
        block_state_registry registry;
        auto obsidian = registry.intern("minecraft:obsidian",
                                        {.air = false,
                                         .blocks_motion = true,
                                         .blocks_motion_no_leaves = true});
        auto stone = registry.intern("minecraft:stone",
                                     {.air = false,
                                      .blocks_motion = true,
                                      .blocks_motion_no_leaves = true});
        expect(obsidian && stone);
        if (!obsidian || !stone) {
            return;
        }
        proto_chunk chunk{registry, 0, 0, 0, 64};
        for (jint z = 4; z <= 12; ++z) {
            for (jint x = 4; x <= 12; ++x) {
                for (jint y = 8; y <= 12; ++y) {
                    chunk.set_state(x, y, z, *stone);
                }
            }
        }
        chunk_region region{chunk, 0};
        const auto resolve = [&](std::string_view key) -> block_state_id {
            if (const auto found = registry.find(key); found.has_value()) {
                return *found;
            }
            const auto block = state_block_name(key);
            const bool is_air = is_air_block_name(block);
            const auto inserted =
                registry.intern(key, {.air = is_air, .blocks_motion = !is_air});
            return inserted.has_value() ? *inserted : no_block_state;
        };
        feature_context context{.placement = placement_context{
                                    .region = &region,
                                    .chunk = &chunk,
                                    .states = &registry,
                                    .resolve_state = resolve,
                                }};
        auto random =
            worldgen_random{std::make_unique<xoroshiro_random_source>(7)};
        end_platform_feature platform;
        expect(platform.place(context, random,
                              block_pos{.x = 8, .y = 10, .z = 8}));
        expect(chunk.state_at(8, 9, 8) == *obsidian);
        expect(registry.name(chunk.state_at(8, 10, 8)) == "minecraft:air");
        expect(chunk.state_at(6, 9, 6) == *obsidian);
        expect(registry.name(chunk.state_at(3, 9, 3)) == "minecraft:air");
    };
};

suite<"worldgen/monster-room"> monster_room_tests = [] {
    "a solid shell with one wall hole becomes a spawner room"_test = [] {
        block_state_registry registry;
        auto stone = registry.intern("minecraft:stone",
                                     {.air = false,
                                      .blocks_motion = true,
                                      .blocks_motion_no_leaves = true});
        auto cave_air = registry.intern("minecraft:cave_air", {.air = true});
        auto cobblestone = registry.intern("minecraft:cobblestone",
                                           {.air = false,
                                            .blocks_motion = true,
                                            .blocks_motion_no_leaves = true});
        auto mossy = registry.intern("minecraft:mossy_cobblestone",
                                     {.air = false,
                                      .blocks_motion = true,
                                      .blocks_motion_no_leaves = true});
        auto spawner = registry.intern("minecraft:spawner",
                                       {.air = false,
                                        .blocks_motion = true,
                                        .blocks_motion_no_leaves = true});
        auto chest = registry.intern("minecraft:chest[facing=north]",
                                     {.air = false,
                                      .blocks_motion = true,
                                      .blocks_motion_no_leaves = true});
        expect(stone && cave_air && cobblestone && mossy && spawner && chest);
        if (!stone || !cave_air || !cobblestone || !mossy || !spawner ||
            !chest) {
            return;
        }
        proto_chunk chunk{registry, 0, 0, 0, 64};
        for (jint y = 24; y <= 36; ++y) {
            for (jint z = 2; z <= 14; ++z) {
                for (jint x = 2; x <= 14; ++x) {
                    chunk.set_state(x, y, z, *stone);
                }
            }
        }
        // Two vertical shafts; exactly one of them lies on the room wall for
        // either random room size.
        for (jint y = 30; y <= 31; ++y) {
            chunk.set_state(5, y, 8, *cave_air);
            chunk.set_state(4, y, 8, *cave_air);
        }
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
        feature_context context{.placement = placement_context{
                                    .region = &region,
                                    .chunk = &chunk,
                                    .states = &registry,
                                    .generation =
                                        generation_context{
                                            .min_y = 0,
                                            .height = 64,
                                            .sea_level = 8,
                                        },
                                    .resolve_state = resolve,
                                }};
        auto random =
            worldgen_random{std::make_unique<xoroshiro_random_source>(12345)};
        monster_room_feature feature;
        expect(
            feature.place(context, random, block_pos{.x = 8, .y = 30, .z = 8}));
        expect(chunk.state_at(8, 30, 8) == *spawner);
        const auto *entity =
            chunk.block_entity_at(block_pos{.x = 8, .y = 30, .z = 8});
        expect(entity != nullptr);
        if (entity != nullptr) {
            const auto spawn_data = entity->find("SpawnData");
            expect(spawn_data != entity->end());
            if (spawn_data != entity->end()) {
                const auto *compound = spawn_data->second.as_compound();
                expect(compound != nullptr);
                if (compound != nullptr) {
                    const auto id = compound->find("id");
                    expect(id != compound->end());
                }
            }
        }
        const auto wall_below = chunk.state_at(8, 29, 8);
        expect(wall_below == *cobblestone || wall_below == *mossy);
        bool found_wall = false;
        for (jint y = 29; y <= 34 && !found_wall; ++y) {
            for (jint z = 4; z <= 12 && !found_wall; ++z) {
                for (jint x = 4; x <= 12; ++x) {
                    const auto state = chunk.state_at(x, y, z);
                    if (state == *cobblestone || state == *mossy) {
                        found_wall = true;
                        break;
                    }
                }
            }
        }
        expect(found_wall);
    };

    "a fully solid block has no hole and is rejected"_test = [] {
        block_state_registry registry;
        auto stone = registry.intern("minecraft:stone",
                                     {.air = false,
                                      .blocks_motion = true,
                                      .blocks_motion_no_leaves = true});
        expect(stone.has_value());
        if (!stone) {
            return;
        }
        proto_chunk chunk{registry, 0, 0, 0, 64};
        for (jint y = 24; y <= 36; ++y) {
            for (jint z = 2; z <= 14; ++z) {
                for (jint x = 2; x <= 14; ++x) {
                    chunk.set_state(x, y, z, *stone);
                }
            }
        }
        chunk_region region{chunk, 0};
        feature_context context{.placement = placement_context{
                                    .region = &region,
                                    .chunk = &chunk,
                                    .states = &registry,
                                    .generation =
                                        generation_context{
                                            .min_y = 0,
                                            .height = 64,
                                            .sea_level = 8,
                                        },
                                }};
        auto random =
            worldgen_random{std::make_unique<xoroshiro_random_source>(12345)};
        monster_room_feature feature;
        expect(!feature.place(context, random,
                              block_pos{.x = 8, .y = 30, .z = 8}));
        expect(chunk.state_at(8, 30, 8) == *stone);
    };
};

suite<"worldgen/lake"> lake_tests = [] {
    "a lava lake carves a cavity in stone"_test = [] {
        block_state_registry registry;
        auto stone = registry.intern("minecraft:stone",
                                     {.air = false,
                                      .blocks_motion = true,
                                      .blocks_motion_no_leaves = true});
        auto lava = registry.intern("minecraft:lava[level=0]",
                                    {.air = false, .fluid = true});
        auto cave_air = registry.intern("minecraft:cave_air", {.air = true});
        auto ice =
            registry.intern("minecraft:ice", {.air = false,
                                              .blocks_motion = true,
                                              .blocks_motion_no_leaves = true});
        expect(stone && lava && cave_air && ice);
        if (!stone || !lava || !cave_air || !ice) {
            return;
        }

        const auto decode = [&](std::string_view text) {
            auto document = parse(text);
            return decode_block_predicate(document, registry);
        };
        auto can_place = decode(R"({"type":"minecraft:true"})");
        auto can_replace = decode(
            R"({"type":"minecraft:not","predicate":{"type":"minecraft:matching_block_tag","tag":"minecraft:features_cannot_replace"}})");
        auto can_replace_barrier = decode(
            R"({"type":"minecraft:not","predicate":{"type":"minecraft:matching_block_tag","tag":"minecraft:lava_pool_stone_cannot_replace"}})");
        expect(can_place.has_value() && can_replace.has_value() &&
               can_replace_barrier.has_value());
        if (!can_place || !can_replace || !can_replace_barrier) {
            return;
        }

        proto_chunk chunk{registry, 0, 0, 0, 64};
        chunk.set_status(chunk_status::terrain);
        for (jint y = 0; y < 64; ++y) {
            for (jint z = 0; z < 16; ++z) {
                for (jint x = 0; x < 16; ++x) {
                    chunk.set_state(x, y, z, *stone);
                }
            }
        }
        chunk_region region{chunk, 0};
        feature_context context{.placement = placement_context{
                                    .region = &region,
                                    .chunk = &chunk,
                                    .states = &registry,
                                    .generation =
                                        generation_context{
                                            .min_y = 0,
                                            .height = 64,
                                            .sea_level = 8,
                                        },
                                }};
        auto random =
            worldgen_random{std::make_unique<xoroshiro_random_source>(12345)};
        lake_feature lake{simple_state(*lava),
                          simple_state(*stone),
                          std::move(*can_place),
                          std::move(*can_replace),
                          std::move(*can_replace_barrier),
                          *cave_air,
                          *ice};

        expect(!lake.place(context, random, block_pos{.x = 8, .y = 3, .z = 8}));
        expect(lake.place(context, random, block_pos{.x = 8, .y = 20, .z = 8}));

        std::size_t lava_blocks = 0;
        std::size_t air_blocks = 0;
        for (jint y = 0; y < 64; ++y) {
            for (jint z = 0; z < 16; ++z) {
                for (jint x = 0; x < 16; ++x) {
                    const auto state = chunk.state_at(x, y, z);
                    lava_blocks += state == *lava;
                    air_blocks += state == *cave_air;
                }
            }
        }
        expect(lava_blocks > 0U);
        expect(air_blocks > 0U);
    };
};

suite<"worldgen/block-predicate"> block_predicate_tests = [] {
    "solid and matching fluids predicates inspect traits"_test = [] {
        block_state_registry registry;
        auto stone = registry.intern("minecraft:stone",
                                     {.air = false,
                                      .blocks_motion = true,
                                      .blocks_motion_no_leaves = true});
        auto water = registry.intern(
            "minecraft:water", {.air = false, .fluid = true, .water = true});
        expect(stone.has_value() && water.has_value());
        if (!stone || !water) {
            return;
        }
        proto_chunk chunk{registry, 0, 0, 0, 32};
        chunk.set_status(chunk_status::terrain);
        (void)chunk.set_state(1, 2, 3, *stone);
        (void)chunk.set_state(2, 2, 3, *water);
        chunk_region region{chunk, 1};
        auto solid = decode_block_predicate(
            parse(R"({"type":"minecraft:solid"})"), registry);
        auto fluid = decode_block_predicate(
            parse(
                R"({"type":"minecraft:matching_fluids","fluids":["minecraft:water"]})"),
            registry);
        expect(solid.has_value() && fluid.has_value());
        if (!solid || !fluid) {
            return;
        }
        expect((*solid)->test(region, registry,
                              block_pos{.x = 1, .y = 2, .z = 3}));
        expect(!(*solid)->test(region, registry,
                               block_pos{.x = 2, .y = 2, .z = 3}));
        expect((*fluid)->test(region, registry,
                              block_pos{.x = 2, .y = 2, .z = 3}));

        auto sturdy = decode_block_predicate(
            parse(
                R"({"type":"minecraft:has_sturdy_face","direction":"up","offset":[0,-1,0]})"),
            registry);
        expect(sturdy.has_value());
        if (sturdy) {
            expect((*sturdy)->test(region, registry,
                                   block_pos{.x = 1, .y = 3, .z = 3}));
        }
    };

    "random chance keeps positions by probability"_test = [] {
        block_state_registry registry;
        proto_chunk chunk{registry, 0, 0, 0, 32};
        chunk_region region{chunk, 1};
        const placement_context context{
            .region = &region, .chunk = &chunk, .states = &registry};
        auto always = decode_placement(
            parse(R"({"type":"minecraft:random_chance","chance":1.0})"),
            registry);
        auto never = decode_placement(
            parse(R"({"type":"minecraft:random_chance","chance":0.0})"),
            registry);
        expect(always.has_value() && never.has_value());
        if (!always || !never) {
            return;
        }
        auto random =
            worldgen_random{std::make_unique<xoroshiro_random_source>(12345)};
        std::vector<block_pos> out;
        (*always)->modify(context, random, block_pos{.x = 0, .y = 0, .z = 0},
                          out);
        expect(out.size() == 1U);
        out.clear();
        (*never)->modify(context, random, block_pos{.x = 0, .y = 0, .z = 0},
                         out);
        expect(out.empty());
    };
};

suite<"worldgen/placement"> placement_tests = [] {
    "the block predicate filter keeps only matching positions"_test = [] {
        block_state_registry registry;
        auto stone = registry.intern("minecraft:stone",
                                     {.air = false,
                                      .blocks_motion = true,
                                      .blocks_motion_no_leaves = true});
        expect(stone.has_value());
        if (!stone) {
            return;
        }
        proto_chunk chunk{registry, 0, 0, 0, 32};
        chunk.set_status(chunk_status::terrain);
        (void)chunk.set_state(1, 2, 3, *stone);
        chunk_region region{chunk, 1};
        const placement_context context{
            .region = &region, .chunk = &chunk, .states = &registry};
        auto modifier = decode_placement(
            parse(
                R"({"type":"minecraft:block_predicate_filter","predicate":{"type":"minecraft:matching_blocks","blocks":["minecraft:stone"]}})"),
            registry);
        expect(modifier.has_value());
        if (!modifier) {
            return;
        }
        auto random =
            worldgen_random{std::make_unique<xoroshiro_random_source>(12345)};
        std::vector<block_pos> out;
        (*modifier)->modify(context, random, block_pos{.x = 1, .y = 2, .z = 3},
                            out);
        expect(out.size() == 1U);
        out.clear();
        (*modifier)->modify(context, random, block_pos{.x = 0, .y = 0, .z = 0},
                            out);
        expect(out.empty());
    };
};

suite<"worldgen/placement-scan"> placement_scan_tests = [] {
    "environment scan finds the target along a direction"_test = [] {
        block_state_registry registry;
        auto stone = registry.intern("minecraft:stone",
                                     {.air = false,
                                      .blocks_motion = true,
                                      .blocks_motion_no_leaves = true});
        expect(stone.has_value());
        if (!stone) {
            return;
        }
        proto_chunk chunk{registry, 0, 0, 0, 32};
        chunk.set_status(chunk_status::terrain);
        (void)chunk.set_state(0, 5, 0, *stone);
        chunk_region region{chunk, 1};
        const placement_context context{
            .region = &region, .chunk = &chunk, .states = &registry};
        auto modifier = decode_placement(
            parse(
                R"({"type":"minecraft:environment_scan","direction_of_search":"down","target_condition":{"type":"minecraft:matching_blocks","blocks":["minecraft:stone"]},"max_steps":6})"),
            registry);
        expect(modifier.has_value());
        if (!modifier) {
            return;
        }
        auto random =
            worldgen_random{std::make_unique<xoroshiro_random_source>(12345)};
        std::vector<block_pos> out;
        (*modifier)->modify(context, random, block_pos{.x = 0, .y = 10, .z = 0},
                            out);
        expect(out.size() == 1U);
        if (!out.empty()) {
            expect(out[0].y == 5);
        }
    };
};

suite<"worldgen/placement-layers"> placement_layer_tests = [] {
    "count_on_every_layer walks each ground layer"_test = [] {
        block_state_registry registry;
        auto stone = registry.intern("minecraft:stone",
                                     {.air = false,
                                      .blocks_motion = true,
                                      .blocks_motion_no_leaves = true});
        expect(stone.has_value());
        if (!stone) {
            return;
        }
        proto_chunk chunk{registry, 0, 0, 0, 32};
        for (jint x = 0; x < 16; ++x) {
            for (jint z = 0; z < 16; ++z) {
                (void)chunk.set_state(x, 5, z, *stone);
                (void)chunk.set_state(x, 10, z, *stone);
            }
        }
        chunk.set_status(chunk_status::terrain);
        chunk_region region{chunk, 1};
        const placement_context context{
            .region = &region, .chunk = &chunk, .states = &registry};
        auto modifier = decode_placement(
            parse(R"({"type":"minecraft:count_on_every_layer","count":1})"),
            registry);
        expect(modifier.has_value());
        if (!modifier) {
            return;
        }
        auto random =
            worldgen_random{std::make_unique<xoroshiro_random_source>(12345)};
        std::vector<block_pos> out;
        (*modifier)->modify(context, random, block_pos{.x = 0, .y = 0, .z = 0},
                            out);
        expect(out.size() == 2U);
        for (const auto &position : out) {
            expect(position.y == 6 || position.y == 11);
        }
    };
};

suite<"worldgen/placement-water"> placement_water_tests = [] {
    "the surface water depth filter compares final heightmaps"_test = [] {
        block_state_registry registry;
        auto stone = registry.intern("minecraft:stone",
                                     {.air = false,
                                      .blocks_motion = true,
                                      .blocks_motion_no_leaves = true});
        auto water = registry.intern(
            "minecraft:water", {.air = false, .fluid = true, .water = true});
        expect(stone.has_value() && water.has_value());
        if (!stone || !water) {
            return;
        }
        proto_chunk chunk{registry, 0, 0, 0, 32};
        for (jint x = 0; x < 16; ++x) {
            for (jint z = 0; z < 16; ++z) {
                (void)chunk.set_state(x, 5, z, *stone);
                for (jint y = 6; y <= 8; ++y) {
                    (void)chunk.set_state(x, y, z, *water);
                }
            }
        }
        chunk.set_status(chunk_status::terrain);
        chunk_region region{chunk, 1};
        const placement_context context{
            .region = &region, .chunk = &chunk, .states = &registry};
        auto random =
            worldgen_random{std::make_unique<xoroshiro_random_source>(12345)};
        auto filter = decode_placement(
            parse(
                R"({"type":"minecraft:surface_water_depth_filter","max_water_depth":3})"),
            registry);
        expect(filter.has_value());
        if (!filter) {
            return;
        }
        std::vector<block_pos> out;
        (*filter)->modify(context, random, block_pos{.x = 0, .y = 0, .z = 0},
                          out);
        expect(out.size() == 1U);
        out.clear();
        auto shallow = decode_placement(
            parse(
                R"({"type":"minecraft:surface_water_depth_filter","max_water_depth":2})"),
            registry);
        if (shallow) {
            (*shallow)->modify(context, random,
                               block_pos{.x = 0, .y = 0, .z = 0}, out);
            expect(out.empty());
        }
    };
};

suite<"worldgen/shelf-mushroom"> shelf_mushroom_tests = [] {
    "shelf mushrooms attach to standing trunk sides"_test = [] {
        block_state_registry registry;
        proto_chunk chunk{registry, 0, 0, 0, 32};
        chunk.set_status(chunk_status::terrain);
        chunk_region region{chunk, 1};
        feature_context context{.placement = placement_context{
                                    .region = &region,
                                    .chunk = &chunk,
                                    .states = &registry,
                                    .generation =
                                        generation_context{
                                            .min_y = 0,
                                            .height = 32,
                                            .sea_level = 8,
                                        },
                                }};
        auto random =
            worldgen_random{std::make_unique<xoroshiro_random_source>(12345)};
        const std::vector<block_pos> logs{{.x = 8, .y = 5, .z = 8},
                                          {.x = 8, .y = 6, .z = 8},
                                          {.x = 8, .y = 7, .z = 8}};
        const std::vector<block_pos> leaves{};
        shelf_mushroom_decorator decorator{1.0F};
        const tree_decorator_context decorator_context{.context = context,
                                                       .states = registry,
                                                       .random = random,
                                                       .logs = logs,
                                                       .leaves = leaves};
        decorator.place(decorator_context);
        std::size_t mushrooms = 0;
        for (jint y = 0; y < 32; ++y) {
            for (jint x = 0; x < 16; ++x) {
                for (jint z = 0; z < 16; ++z) {
                    mushrooms += registry.name(chunk.state_at(x, y, z))
                                     .starts_with("minecraft:shelf_mushroom");
                }
            }
        }
        expect(mushrooms > 0U);
    };
};

suite<"worldgen/attached-logs-decorator"> attached_logs_tests = [] {
    "attached_to_logs places blocks beside logs"_test = [] {
        block_state_registry registry;
        auto litter = registry.intern("minecraft:leaf_litter",
                                      {.air = false,
                                       .blocks_motion = true,
                                       .blocks_motion_no_leaves = true});
        expect(litter.has_value());
        if (!litter) {
            return;
        }
        proto_chunk chunk{registry, 0, 0, 0, 32};
        chunk.set_status(chunk_status::terrain);
        chunk_region region{chunk, 1};
        feature_context context{.placement = placement_context{
                                    .region = &region,
                                    .chunk = &chunk,
                                    .states = &registry,
                                    .generation =
                                        generation_context{
                                            .min_y = 0,
                                            .height = 32,
                                            .sea_level = 8,
                                        },
                                }};
        auto random =
            worldgen_random{std::make_unique<xoroshiro_random_source>(12345)};
        const std::vector<block_pos> logs{{.x = 8, .y = 5, .z = 8}};
        const std::vector<block_pos> leaves{};
        attached_to_logs_decorator decorator{
            1.0F, simple_state(*litter), {"down"}};
        const tree_decorator_context decorator_context{.context = context,
                                                       .states = registry,
                                                       .random = random,
                                                       .logs = logs,
                                                       .leaves = leaves};
        decorator.place(decorator_context);
        expect(registry.name(chunk.state_at(8, 4, 8)) ==
               "minecraft:leaf_litter");
    };
};

suite<"worldgen/attached-decorator"> attached_decorator_tests = [] {
    "attached_to_leaves hangs blocks below leaves"_test = [] {
        block_state_registry registry;
        auto litter = registry.intern("minecraft:leaf_litter",
                                      {.air = false,
                                       .blocks_motion = true,
                                       .blocks_motion_no_leaves = true});
        expect(litter.has_value());
        if (!litter) {
            return;
        }
        proto_chunk chunk{registry, 0, 0, 0, 32};
        chunk.set_status(chunk_status::terrain);
        chunk_region region{chunk, 1};
        feature_context context{.placement = placement_context{
                                    .region = &region,
                                    .chunk = &chunk,
                                    .states = &registry,
                                    .generation =
                                        generation_context{
                                            .min_y = 0,
                                            .height = 32,
                                            .sea_level = 8,
                                        },
                                }};
        auto random =
            worldgen_random{std::make_unique<xoroshiro_random_source>(12345)};
        const std::vector<block_pos> logs{};
        const std::vector<block_pos> leaves{{.x = 8, .y = 5, .z = 8},
                                            {.x = 8, .y = 5, .z = 9}};
        attached_to_leaves_decorator decorator{
            1.0F, 0, 0, simple_state(*litter), 1, {"down"}};
        const tree_decorator_context decorator_context{.context = context,
                                                       .states = registry,
                                                       .random = random,
                                                       .logs = logs,
                                                       .leaves = leaves};
        decorator.place(decorator_context);
        expect(registry.name(chunk.state_at(8, 4, 8)) ==
               "minecraft:leaf_litter");
        expect(registry.name(chunk.state_at(8, 4, 9)) ==
               "minecraft:leaf_litter");
    };
};

suite<"worldgen/ground-decorator"> ground_decorator_tests = [] {
    "place_on_ground puts blocks above solid ground"_test = [] {
        block_state_registry registry;
        auto stone = registry.intern("minecraft:stone",
                                     {.air = false,
                                      .blocks_motion = true,
                                      .blocks_motion_no_leaves = true});
        auto litter = registry.intern("minecraft:leaf_litter",
                                      {.air = false,
                                       .blocks_motion = true,
                                       .blocks_motion_no_leaves = true});
        expect(stone.has_value() && litter.has_value());
        if (!stone || !litter) {
            return;
        }
        proto_chunk chunk{registry, 0, 0, 0, 32};
        for (jint x = 0; x < 16; ++x) {
            for (jint z = 0; z < 16; ++z) {
                for (jint y = 0; y <= 5; ++y) {
                    (void)chunk.set_state(x, y, z, *stone);
                }
            }
        }
        (void)chunk.set_state(8, 7, 8, *stone);
        (void)chunk.set_state(8, 8, 8, *stone);
        chunk.set_status(chunk_status::terrain);
        chunk_region region{chunk, 1};
        feature_context context{.placement = placement_context{
                                    .region = &region,
                                    .chunk = &chunk,
                                    .states = &registry,
                                    .generation =
                                        generation_context{
                                            .min_y = 0,
                                            .height = 32,
                                            .sea_level = 8,
                                        },
                                }};
        auto random =
            worldgen_random{std::make_unique<xoroshiro_random_source>(12345)};
        const std::vector<block_pos> logs{{.x = 8, .y = 7, .z = 8},
                                          {.x = 8, .y = 8, .z = 8}};
        const std::vector<block_pos> leaves{};
        place_on_ground_decorator decorator{100, 0, 1, simple_state(*litter)};
        const tree_decorator_context decorator_context{.context = context,
                                                       .states = registry,
                                                       .random = random,
                                                       .logs = logs,
                                                       .leaves = leaves};
        decorator.place(decorator_context);
        std::size_t placed = 0;
        for (jint y = 6; y < 12; ++y) {
            for (jint x = 0; x < 16; ++x) {
                for (jint z = 0; z < 16; ++z) {
                    placed += registry.name(chunk.state_at(x, y, z)) ==
                              "minecraft:leaf_litter";
                }
            }
        }
        expect(placed > 0U) << placed;
    };
};

suite<"worldgen/state-provider"> state_provider_tests = [] {
    "weighted providers sample both entries"_test = [] {
        block_state_registry registry;
        auto stone = registry.intern("minecraft:stone",
                                     {.air = false,
                                      .blocks_motion = true,
                                      .blocks_motion_no_leaves = true});
        auto moss = registry.intern("minecraft:mossy_cobblestone",
                                    {.air = false,
                                     .blocks_motion = true,
                                     .blocks_motion_no_leaves = true});
        expect(stone.has_value() && moss.has_value());
        if (!stone || !moss) {
            return;
        }
        auto provider = decode_state_provider(
            parse(
                R"({"type":"minecraft:weighted","entries":[{"data":"minecraft:stone","weight":1},{"data":"minecraft:mossy_cobblestone","weight":1}]})"),
            registry);
        expect(provider.has_value());
        if (!provider) {
            return;
        }
        proto_chunk chunk{registry, 0, 0, 0, 32};
        chunk_region region{chunk, 1};
        const placement_context context{
            .region = &region, .chunk = &chunk, .states = &registry};
        std::array<bool, 2> seen{};
        auto random =
            worldgen_random{std::make_unique<xoroshiro_random_source>(12345)};
        for (int i = 0; i < 50; ++i) {
            const auto state = (*provider)->get(
                context, random, block_pos{.x = 0, .y = 0, .z = 0});
            expect(state == *stone || state == *moss);
            seen[state == *stone ? 0 : 1] = true;
        }
        expect(seen[0] && seen[1]);
    };

    "rotated and randomized providers edit the state key"_test = [] {
        block_state_registry registry;
        (void)registry.intern("minecraft:oak_log[axis=y]",
                              {.air = false,
                               .blocks_motion = true,
                               .blocks_motion_no_leaves = true});
        (void)registry.intern("minecraft:water[level=0]",
                              {.air = false, .fluid = true, .water = true});
        proto_chunk chunk{registry, 0, 0, 0, 32};
        chunk_region region{chunk, 1};
        const placement_context context{
            .region = &region, .chunk = &chunk, .states = &registry};
        auto random =
            worldgen_random{std::make_unique<xoroshiro_random_source>(12345)};

        auto rotated = decode_state_provider(
            parse(
                R"({"type":"minecraft:rotated","state":{"id":"minecraft:oak_log","properties":{"axis":"y"}},"direction":"east"})"),
            registry);
        expect(rotated.has_value());
        if (rotated) {
            const auto state = (*rotated)->get(
                context, random, block_pos{.x = 0, .y = 0, .z = 0});
            expect(registry.name(state) == "minecraft:oak_log[axis=x]");
        }

        auto randomized = decode_state_provider(
            parse(
                R"({"type":"minecraft:randomized_int","source":{"id":"minecraft:water","properties":{"level":"0"}},"property":"level","values":2})"),
            registry);
        expect(randomized.has_value());
        if (randomized) {
            const auto state =
                (*randomized)
                    ->get(context, random, block_pos{.x = 0, .y = 0, .z = 0});
            expect(registry.name(state) == "minecraft:water[level=2]");
        }
    };
};

suite<"worldgen/boolean-selector"> boolean_selector_tests = [] {
    "the boolean selector forwards to one nested feature"_test = [] {
        feature_context context;
        std::string chosen;
        context.place_by_id = [&chosen](std::string_view id, worldgen_random &,
                                        const block_pos &) {
            chosen = std::string{id};
            return true;
        };
        boolean_selector_feature selector{"oracle:true", "oracle:false"};
        auto random =
            worldgen_random{std::make_unique<xoroshiro_random_source>(12345)};
        expect(
            selector.place(context, random, block_pos{.x = 0, .y = 0, .z = 0}));
        expect(chosen == "oracle:true" || chosen == "oracle:false");
    };
};

suite<"worldgen/block-blob"> block_blob_tests = [] {
    "a blob places around its anchor"_test = [] {
        block_state_registry registry;
        auto stone = registry.intern("minecraft:stone",
                                     {.air = false,
                                      .blocks_motion = true,
                                      .blocks_motion_no_leaves = true});
        auto moss = registry.intern("minecraft:mossy_cobblestone",
                                    {.air = false,
                                     .blocks_motion = true,
                                     .blocks_motion_no_leaves = true});
        expect(stone.has_value());
        expect(moss.has_value());
        if (!stone || !moss) {
            return;
        }

        const auto predicate_document = parse(
            R"({"type":"minecraft:matching_blocks","blocks":["minecraft:stone"]})");
        auto predicate = decode_block_predicate(predicate_document, registry);
        expect(predicate.has_value());
        if (!predicate) {
            return;
        }

        proto_chunk chunk{registry, 0, 0, 0, 32};
        chunk.set_status(chunk_status::biomes);
        for (jint y = 0; y < 20; ++y) {
            for (jint x = 0; x < 16; ++x) {
                for (jint z = 0; z < 16; ++z) {
                    (void)chunk.set_state(x, y, z, *stone);
                }
            }
        }
        chunk_region region{chunk, 1};
        feature_context context{.placement = placement_context{
                                    .region = &region,
                                    .chunk = &chunk,
                                    .states = &registry,
                                    .generation =
                                        generation_context{
                                            .min_y = 0,
                                            .height = 32,
                                            .sea_level = 8,
                                        },
                                }};
        block_blob_feature blob{*moss, std::move(*predicate)};
        auto random =
            worldgen_random{std::make_unique<xoroshiro_random_source>(12345)};
        expect(blob.place(context, random, block_pos{.x = 8, .y = 10, .z = 8}));
        std::size_t moss_count = 0;
        for (jint y = 0; y < 20; ++y) {
            for (jint x = 0; x < 16; ++x) {
                for (jint z = 0; z < 16; ++z) {
                    moss_count += chunk.state_at(x, y, z) == *moss;
                }
            }
        }
        expect(moss_count > 0U);
    };
};

suite<"worldgen/spring"> spring_tests = [] {
    "a spring fills its matching rock and hole pattern"_test = [] {
        block_state_registry registry;
        auto stone = registry.intern("minecraft:stone",
                                     {.air = false,
                                      .blocks_motion = true,
                                      .blocks_motion_no_leaves = true});
        auto water =
            registry.intern("minecraft:water[falling=true]",
                            {.air = false, .fluid = true, .water = true});
        expect(stone.has_value());
        expect(water.has_value());
        if (!stone || !water) {
            return;
        }

        proto_chunk chunk{registry, 0, 0, 0, 32};
        chunk.set_status(chunk_status::biomes);
        for (jint x = 0; x < 16; ++x) {
            for (jint z = 0; z < 16; ++z) {
                expect(chunk.set_state(x, 9, z, *stone));
            }
        }
        expect(chunk.set_state(7, 10, 8, *stone));
        expect(chunk.set_state(9, 10, 8, *stone));
        expect(chunk.set_state(8, 10, 7, *stone));
        expect(chunk.set_state(8, 10, 9, *stone));
        expect(chunk.set_state(8, 11, 8, *stone));

        chunk_region region{chunk, 1};
        feature_context context{.placement = placement_context{
                                    .region = &region,
                                    .chunk = &chunk,
                                    .states = &registry,
                                    .generation =
                                        generation_context{
                                            .min_y = 0,
                                            .height = 32,
                                            .sea_level = 8,
                                        },
                                }};
        spring_feature spring{*water, true, 5, 0, {"minecraft:stone"}};
        auto random =
            worldgen_random{std::make_unique<legacy_random_source>(12345)};
        expect(
            spring.place(context, random, block_pos{.x = 8, .y = 10, .z = 8}));
        expect(chunk.state_at(8, 10, 8) == *water);
    };
};

suite<"worldgen/overlay"> overlay_tests = [] {
    "an overlay places its inline and referenced features"_test = [] {
        block_state_registry registry;
        auto stone = registry.intern("minecraft:stone",
                                     {.air = false,
                                      .blocks_motion = true,
                                      .blocks_motion_no_leaves = true});
        auto moss = registry.intern("minecraft:mossy_cobblestone",
                                    {.air = false,
                                     .blocks_motion = true,
                                     .blocks_motion_no_leaves = true});
        expect(stone.has_value() && moss.has_value());
        if (!stone || !moss) {
            return;
        }
        feature_data_set data;
        data.features.emplace(
            "oracle:moss",
            configured_feature_document{
                .id = "oracle:moss",
                .document = parse(
                    R"({"type":"minecraft:overlay","features":[{"feature":{"type":"minecraft:simple_block","to_place":{"id":"minecraft:mossy_cobblestone"}},"placement":[]}]})")});
        data.placed.emplace(
            "oracle:p",
            placed_feature_document{
                .id = "oracle:p",
                .feature = "oracle:moss",
                .placement = parse(
                    R"([{"type":"minecraft:fixed_placement","positions":[[8,5,8]]}])")});
        data.biomes.push_back(biome_feature_list{.biome = "oracle:test",
                                                 .steps = {{"oracle:p"}}});
        feature_registry features{registry};
        const auto loaded = features.load(data);
        expect(loaded.has_value()) << loaded.error().message;
        if (!loaded) {
            return;
        }
        const auto plan = feature_sorter::build({"oracle:test"}, data.biomes);
        expect(plan.has_value());
        if (!plan) {
            return;
        }
        proto_chunk chunk{registry, 0, 0, 0, 32};
        chunk.set_status(chunk_status::terrain);
        chunk_region region{chunk, 1};
        feature_context context{.placement = placement_context{
                                    .region = &region,
                                    .chunk = &chunk,
                                    .states = &registry,
                                    .generation =
                                        generation_context{
                                            .min_y = 0,
                                            .height = 32,
                                            .sea_level = 8,
                                        },
                                }};
        decorate(context, *plan, features.placed(), {"oracle:test"},
                 data.biomes, 12345);
        expect(chunk.state_at(8, 5, 8) == *moss);
    };
};

suite<"worldgen/selector"> selector_tests = [] {
    "a random selector places the first matching nested feature"_test = [] {
        block_state_registry registry;
        auto stone = registry.intern("minecraft:stone",
                                     {.air = false,
                                      .blocks_motion = true,
                                      .blocks_motion_no_leaves = true});
        auto moss = registry.intern("minecraft:mossy_cobblestone",
                                    {.air = false,
                                     .blocks_motion = true,
                                     .blocks_motion_no_leaves = true});
        expect(stone.has_value() && moss.has_value());
        if (!stone || !moss) {
            return;
        }

        feature_data_set data;
        data.features.emplace(
            "oracle:a",
            configured_feature_document{
                .id = "oracle:a",
                .document = parse(
                    R"({"type":"minecraft:simple_block","to_place":{"id":"minecraft:stone"}})")});
        data.features.emplace(
            "oracle:b",
            configured_feature_document{
                .id = "oracle:b",
                .document = parse(
                    R"({"type":"minecraft:simple_block","to_place":{"id":"minecraft:mossy_cobblestone"}})")});
        data.features.emplace(
            "oracle:selector",
            configured_feature_document{
                .id = "oracle:selector",
                .document = parse(
                    R"({"type":"minecraft:random_selector","default":{"feature":"oracle:b","placement":[]},"features":[{"chance":1.0,"feature":{"feature":"oracle:a","placement":[]}}]})")});
        const auto placed_entry = [](std::string id, std::string feature,
                                     std::string placement) {
            return std::pair{
                id, placed_feature_document{.id = id,
                                            .feature = std::move(feature),
                                            .placement = parse(placement)}};
        };
        data.placed.emplace("oracle:pa",
                            placed_entry("oracle:pa", "oracle:a", "[]").second);
        data.placed.emplace("oracle:pb",
                            placed_entry("oracle:pb", "oracle:b", "[]").second);
        data.placed.emplace(
            "oracle:ps",
            placed_entry(
                "oracle:ps", "oracle:selector",
                R"([{"type":"minecraft:fixed_placement","positions":[[8,5,8]]}])")
                .second);
        data.biomes.push_back(biome_feature_list{.biome = "oracle:test",
                                                 .steps = {{"oracle:ps"}}});

        feature_registry features{registry};
        const auto loaded = features.load(data);
        expect(loaded.has_value()) << loaded.error().message;
        const auto plan = feature_sorter::build({"oracle:test"}, data.biomes);
        expect(plan.has_value());
        if (!plan) {
            return;
        }

        proto_chunk chunk{registry, 0, 0, 0, 32};
        chunk.set_status(chunk_status::terrain);
        chunk_region region{chunk, 1};
        feature_context context{.placement = placement_context{
                                    .region = &region,
                                    .chunk = &chunk,
                                    .states = &registry,
                                    .generation =
                                        generation_context{
                                            .min_y = 0,
                                            .height = 32,
                                            .sea_level = 8,
                                        },
                                }};
        decorate(context, *plan, features.placed(), {"oracle:test"},
                 data.biomes, 12345);
        expect(chunk.state_at(8, 5, 8) == *stone);
    };
};

suite<"worldgen/feature/end-gateway"> end_gateway_tests = [] {
    "the gateway frame is bedrock around one gateway block"_test = [] {
        using namespace refrain::worldgen;
        block_state_registry states;
        (void)states.intern("minecraft:air", {.air = true});
        (void)states.intern("minecraft:bedrock",
                            {.air = false,
                             .blocks_motion = true,
                             .blocks_motion_no_leaves = true});
        const auto gateway = states.intern(
            "minecraft:end_gateway", {.air = false, .blocks_motion = true});
        const auto bedrock = states.find("minecraft:bedrock");
        expect(gateway.has_value() && bedrock.has_value());
        if (!gateway || !bedrock) {
            return;
        }
        proto_chunk chunk{states, 0, 0, 0, 32};
        chunk_region region{chunk, 0};
        feature::placement_context placement{
            .region = &region,
            .chunk = &chunk,
            .states = &states,
            .generation = provider::generation_context{.min_y = 0,
                                                       .height = 32,
                                                       .sea_level = 8},
        };
        feature::feature_context context{.placement = placement};
        const feature::end_gateway_feature feature{
            *gateway, *bedrock, block_pos{.x = 0, .y = 64, .z = 0}, true};
        worldgen_random random{std::make_unique<legacy_random_source>(1)};
        expect(
            feature.place(context, random, block_pos{.x = 8, .y = 8, .z = 8}));
        expect(chunk.state_at(8, 8, 8) == *gateway);
        expect(chunk.state_at(8, 10, 8) == *bedrock);
        const auto *entity = chunk.block_entity_at(
            refrain::worldgen::block_pos{.x = 8, .y = 8, .z = 8});
        expect(entity != nullptr &&
               entity->find("ExitPortal") != entity->end());
        expect(chunk.state_at(7, 8, 8) == *states.find("minecraft:air"));
        expect(chunk.state_at(9, 8, 8) == *states.find("minecraft:air"));
    };
};

suite<"worldgen/feature/end-island"> end_island_tests = [] {
    "the island places end stone below the origin"_test = [] {
        using namespace refrain::worldgen;
        block_state_registry states;
        (void)states.intern("minecraft:air", {.air = true});
        const auto end_stone = states.intern("minecraft:end_stone",
                                             {.air = false,
                                              .blocks_motion = true,
                                              .blocks_motion_no_leaves = true});
        expect(end_stone.has_value());
        if (!end_stone) {
            return;
        }
        proto_chunk chunk{states, 0, 0, 0, 32};
        chunk_region region{chunk, 0};
        feature::placement_context placement{
            .region = &region,
            .chunk = &chunk,
            .states = &states,
            .generation = provider::generation_context{.min_y = 0,
                                                       .height = 32,
                                                       .sea_level = 8},
        };
        feature::feature_context context{.placement = placement};
        const feature::end_island_feature feature{*end_stone};
        worldgen_random random{std::make_unique<legacy_random_source>(1)};
        expect(
            feature.place(context, random, block_pos{.x = 8, .y = 16, .z = 8}));
        expect(chunk.state_at(8, 16, 8) == *end_stone);
        expect(chunk.state_at(8, 17, 8) == *states.find("minecraft:air"));
        std::size_t count = 0;
        for (jint y = 0; y < 32; ++y) {
            for (jint z = 0; z < 16; ++z) {
                for (jint x = 0; x < 16; ++x) {
                    if (chunk.state_at(x, y, z) == *end_stone) {
                        ++count;
                    }
                }
            }
        }
        expect(count > 1);
    };
};

suite<"worldgen/feature/end-podium"> end_podium_tests = [] {
    "the active podium writes the portal, pillar and torches"_test = [] {
        using namespace refrain::worldgen;
        block_state_registry states;
        (void)states.intern("minecraft:air", {.air = true});
        (void)states.intern("minecraft:bedrock",
                            {.air = false,
                             .blocks_motion = true,
                             .blocks_motion_no_leaves = true});
        const auto end_stone = states.intern("minecraft:end_stone",
                                             {.air = false,
                                              .blocks_motion = true,
                                              .blocks_motion_no_leaves = true});
        const auto portal = states.intern(
            "minecraft:end_portal", {.air = false, .blocks_motion = true});
        std::array<block_state_id, 4> torches{};
        bool torches_ok = true;
        for (std::size_t i = 0; i < 4; ++i) {
            const std::array<std::string, 4> keys{
                "minecraft:wall_torch[facing=south]",
                "minecraft:wall_torch[facing=north]",
                "minecraft:wall_torch[facing=east]",
                "minecraft:wall_torch[facing=west]"};
            const auto torch = states.intern(keys[i], {.air = false});
            torches_ok = torches_ok && torch.has_value();
            torches[i] = torch.value_or(no_block_state);
        }
        const auto bedrock = states.find("minecraft:bedrock");
        expect(end_stone.has_value() && portal.has_value() && bedrock &&
               torches_ok);
        if (!end_stone || !portal || !bedrock || !torches_ok) {
            return;
        }
        proto_chunk chunk{states, 0, 0, 0, 32};
        chunk_region region{chunk, 0};
        feature::placement_context placement{
            .region = &region,
            .chunk = &chunk,
            .states = &states,
            .generation = provider::generation_context{.min_y = 0,
                                                       .height = 32,
                                                       .sea_level = 8},
        };
        feature::feature_context context{.placement = placement};
        const feature::end_podium_feature feature{true, *bedrock, *end_stone,
                                                  *portal, torches};
        worldgen_random random{std::make_unique<legacy_random_source>(1)};
        expect(
            feature.place(context, random, block_pos{.x = 8, .y = 8, .z = 8}));
        expect(chunk.state_at(8, 8, 8) == *bedrock);
        expect(chunk.state_at(8, 8, 9) == *portal);
        expect(chunk.state_at(8, 10, 8) == *bedrock);
        expect(chunk.state_at(8, 7, 8) == *bedrock);
        expect(chunk.state_at(8, 10, 9) == torches[0]);
    };
};

suite<"worldgen/feature/chorus-plant"> chorus_plant_tests = [] {
    "the chorus plant grows upwards from a supported origin"_test = [] {
        using namespace refrain::worldgen;
        block_state_registry states;
        (void)states.intern("minecraft:air", {.air = true});
        const auto end_stone = states.intern("minecraft:end_stone",
                                             {.air = false,
                                              .blocks_motion = true,
                                              .blocks_motion_no_leaves = true});
        const auto plant =
            states.intern("minecraft:chorus_plant", {.air = false});
        const auto flower =
            states.intern("minecraft:chorus_flower[age=5]", {.air = false});
        expect(end_stone.has_value() && plant.has_value() &&
               flower.has_value());
        if (!end_stone || !plant || !flower) {
            return;
        }
        proto_chunk chunk{states, 0, 0, 0, 32};
        chunk.set_state(8, 4, 8, *end_stone);
        chunk_region region{chunk, 0};
        const auto resolve = [&](std::string_view key) {
            if (const auto found = states.find(key); found) {
                return *found;
            }
            return states
                .intern(key, {.air = false,
                              .blocks_motion = false,
                              .blocks_motion_no_leaves = false})
                .value_or(no_block_state);
        };
        feature::placement_context placement{
            .region = &region,
            .chunk = &chunk,
            .states = &states,
            .generation = provider::generation_context{.min_y = 0,
                                                       .height = 32,
                                                       .sea_level = 8},
            .resolve_state = resolve,
        };
        feature::feature_context context{.placement = placement};
        const feature::chorus_plant_feature feature{*plant, *flower};
        worldgen_random random{std::make_unique<legacy_random_source>(1)};
        expect(
            feature.place(context, random, block_pos{.x = 8, .y = 5, .z = 8}));
        expect(states.name(chunk.state_at(8, 5, 8))
                   .starts_with("minecraft:chorus_plant"));
        std::size_t flowers = 0;
        for (jint y = 0; y < 32; ++y) {
            for (jint z = 0; z < 16; ++z) {
                for (jint x = 0; x < 16; ++x) {
                    if (states.name(chunk.state_at(x, y, z)) ==
                        "minecraft:chorus_flower[age=5]") {
                        ++flowers;
                    }
                }
            }
        }
        expect(flowers > 0);
    };
};

suite<"worldgen/feature/bonus-chest"> bonus_chest_tests = [] {
    "the bonus chest picks a surface cell and lights it"_test = [] {
        using namespace refrain::worldgen;
        block_state_registry states;
        (void)states.intern("minecraft:air", {.air = true});
        const auto stone =
            states.intern("minecraft:stone", {.air = false,
                                              .blocks_motion = true,
                                              .blocks_motion_no_leaves = true});
        const auto chest = states.intern("minecraft:chest", {.air = false});
        const auto torch = states.intern("minecraft:torch", {.air = false});
        expect(stone && chest && torch);
        if (!stone || !chest || !torch) {
            return;
        }
        proto_chunk chunk{states, 0, 0, 0, 32};
        for (jint z = 0; z < 16; ++z) {
            for (jint x = 0; x < 16; ++x) {
                for (jint y = 0; y < 5; ++y) {
                    chunk.set_state(x, y, z, *stone);
                }
            }
        }
        chunk.set_status(chunk_status::terrain);
        chunk_region region{chunk, 0};
        feature::placement_context placement{
            .region = &region,
            .chunk = &chunk,
            .states = &states,
            .generation = provider::generation_context{.min_y = 0,
                                                       .height = 32,
                                                       .sea_level = 8},
        };
        feature::feature_context context{.placement = placement};
        const feature::bonus_chest_feature feature{*chest, *torch};
        worldgen_random random{std::make_unique<legacy_random_source>(1)};
        expect(
            feature.place(context, random, block_pos{.x = 0, .y = 0, .z = 0}));
        std::size_t chests = 0;
        std::size_t torches = 0;
        for (jint z = 0; z < 16; ++z) {
            for (jint x = 0; x < 16; ++x) {
                if (chunk.state_at(x, 5, z) == *chest) {
                    ++chests;
                } else if (chunk.state_at(x, 5, z) == *torch) {
                    ++torches;
                }
            }
        }
        expect(chests == 1);
        expect(torches > 0);
    };
};

suite<"worldgen/feature/end-spike"> end_spike_tests = [] {
    "the spike writes obsidian, bedrock and fire"_test = [] {
        using namespace refrain::worldgen;
        block_state_registry states;
        (void)states.intern("minecraft:air", {.air = true});
        (void)states.intern("minecraft:obsidian",
                            {.air = false,
                             .blocks_motion = true,
                             .blocks_motion_no_leaves = true});
        (void)states.intern("minecraft:bedrock",
                            {.air = false,
                             .blocks_motion = true,
                             .blocks_motion_no_leaves = true});
        (void)states.intern("minecraft:fire", {.air = false});
        proto_chunk chunk{states, 0, 0, 0, 32};
        chunk_region region{chunk, 0};
        const auto resolve = [&](std::string_view key) {
            if (const auto found = states.find(key); found) {
                return *found;
            }
            return states
                .intern(key, {.air = false,
                              .blocks_motion = false,
                              .blocks_motion_no_leaves = false})
                .value_or(no_block_state);
        };
        feature::placement_context placement{
            .region = &region,
            .chunk = &chunk,
            .states = &states,
            .generation = provider::generation_context{.min_y = 0,
                                                       .height = 32,
                                                       .sea_level = 8},
            .resolve_state = resolve,
        };
        feature::feature_context context{.placement = placement};
        const feature::end_spike_feature feature{
            {feature::end_spike{.center_x = 8,
                                .center_z = 8,
                                .radius = 1,
                                .height = 10,
                                .guarded = false}},
            false};
        worldgen_random random{std::make_unique<legacy_random_source>(1)};
        expect(
            feature.place(context, random, block_pos{.x = 0, .y = 0, .z = 0}));
        expect(states.name(chunk.state_at(8, 9, 8)) == "minecraft:obsidian");
        expect(states.name(chunk.state_at(9, 9, 8)) == "minecraft:obsidian");
        expect(states.name(chunk.state_at(8, 10, 8)) == "minecraft:bedrock");
        expect(states.name(chunk.state_at(8, 11, 8)) == "minecraft:fire");
    };
};

suite<"worldgen/feature/vegetation-patch"> vegetation_patch_tests = [] {
    "a floor patch replaces the ground and reports its surface"_test = [] {
        using namespace refrain::worldgen;
        block_state_registry states;
        (void)states.intern("minecraft:air", {.air = true});
        const auto stone =
            states.intern("minecraft:stone", {.air = false,
                                              .blocks_motion = true,
                                              .blocks_motion_no_leaves = true});
        const auto moss = states.intern("minecraft:moss_block",
                                        {.air = false,
                                         .blocks_motion = true,
                                         .blocks_motion_no_leaves = true});
        expect(stone && moss);
        if (!stone || !moss) {
            return;
        }
        const std::array<block_state_id, 1> tag_states{*stone};
        states.add_tag("test:replaceable", tag_states);
        proto_chunk chunk{states, 0, 0, 0, 32};
        for (jint z = 0; z < 16; ++z) {
            for (jint x = 0; x < 16; ++x) {
                for (jint y = 0; y < 6; ++y) {
                    chunk.set_state(x, y, z, *stone);
                }
            }
        }
        chunk.set_status(chunk_status::terrain);
        chunk_region region{chunk, 0};
        auto depth = provider::int_provider::decode(parse("2"));
        auto radius = provider::int_provider::decode(parse("1"));
        expect(depth.has_value() && radius.has_value());
        if (!depth || !radius) {
            return;
        }
        feature::placement_context placement{
            .region = &region,
            .chunk = &chunk,
            .states = &states,
            .generation = provider::generation_context{.min_y = 0,
                                                       .height = 32,
                                                       .sea_level = 8},
        };
        bool nested_called = false;
        feature::feature_context context{
            .placement = placement,
            .place_by_id =
                [&](std::string_view, worldgen_random &, const block_pos &) {
                    nested_called = true;
                    return true;
                },
        };
        const feature::vegetation_patch_feature patch{
            "test:nested",
            feature::simple_state(*moss),
            "#test:replaceable",
            std::move(*depth),
            std::move(*radius),
            false,
            5,
            0.0F,
            0.0F,
            0.0F,
            false};
        worldgen_random random{std::make_unique<legacy_random_source>(1)};
        expect(patch.place(context, random, block_pos{.x = 8, .y = 6, .z = 8}));
        expect(states.name(chunk.state_at(8, 5, 8)) == "minecraft:moss_block");
        expect(states.name(chunk.state_at(8, 4, 8)) == "minecraft:moss_block");
        expect(!nested_called);
    };
};

suite<"worldgen/feature/root-system"> root_system_tests = [] {
    "the root system finds a ceiling pocket and delegates"_test = [] {
        using namespace refrain::worldgen;
        block_state_registry states;
        const auto air = states.intern("minecraft:air", {.air = true});
        const auto stone =
            states.intern("minecraft:stone", {.air = false,
                                              .blocks_motion = true,
                                              .blocks_motion_no_leaves = true});
        const auto rooted =
            states.intern("minecraft:rooted_dirt", {.air = false});
        const auto hanging =
            states.intern("minecraft:hanging_roots", {.air = false});
        expect(air && stone && rooted && hanging);
        if (!air || !stone || !rooted || !hanging) {
            return;
        }
        const std::array<block_state_id, 1> air_tag{*air};
        states.add_tag("test:air", air_tag);
        auto predicate = feature::decode_block_predicate(
            parse(
                R"({"type":"minecraft:matching_block_tag","tag":"test:air"})"),
            states);
        expect(predicate.has_value());
        if (!predicate) {
            return;
        }
        proto_chunk chunk{states, 0, 0, 0, 32};
        for (jint z = 0; z < 16; ++z) {
            for (jint x = 0; x < 16; ++x) {
                chunk.set_state(x, 10, z, *stone);
            }
        }
        chunk.set_status(chunk_status::terrain);
        chunk_region region{chunk, 0};
        feature::placement_context placement{
            .region = &region,
            .chunk = &chunk,
            .states = &states,
            .generation = provider::generation_context{.min_y = 0,
                                                       .height = 32,
                                                       .sea_level = 8},
        };
        bool nested_called = false;
        feature::feature_context context{
            .placement = placement,
            .place_by_id =
                [&](std::string_view, worldgen_random &, const block_pos &) {
                    nested_called = true;
                    return true;
                },
        };
        const feature::root_system_feature root{"test:nested",
                                                feature::simple_state(*rooted),
                                                feature::simple_state(*hanging),
                                                std::move(*predicate),
                                                "#test:air",
                                                2,
                                                0,
                                                0,
                                                2,
                                                1,
                                                3,
                                                2,
                                                2,
                                                1,
                                                1};
        worldgen_random random{std::make_unique<legacy_random_source>(1)};
        expect(root.place(context, random, block_pos{.x = 8, .y = 9, .z = 8}));
        expect(nested_called);
    };

    "a patch calls its nested feature for every surface cell"_test = [] {
        using namespace refrain::worldgen;
        block_state_registry states;
        (void)states.intern("minecraft:air", {.air = true});
        const auto stone =
            states.intern("minecraft:stone", {.air = false,
                                              .blocks_motion = true,
                                              .blocks_motion_no_leaves = true});
        const auto moss = states.intern("minecraft:moss_block", {.air = false});
        expect(stone && moss);
        if (!stone || !moss) {
            return;
        }
        const std::array<block_state_id, 1> tag_states{*stone};
        states.add_tag("test:replaceable", tag_states);
        proto_chunk chunk{states, 0, 0, 0, 32};
        for (jint z = 0; z < 16; ++z) {
            for (jint x = 0; x < 16; ++x) {
                for (jint y = 0; y < 6; ++y) {
                    chunk.set_state(x, y, z, *stone);
                }
            }
        }
        chunk.set_status(chunk_status::terrain);
        chunk_region region{chunk, 0};
        auto depth = provider::int_provider::decode(parse("1"));
        auto radius = provider::int_provider::decode(parse("1"));
        expect(depth.has_value() && radius.has_value());
        if (!depth || !radius) {
            return;
        }
        feature::placement_context placement{
            .region = &region,
            .chunk = &chunk,
            .states = &states,
            .generation = provider::generation_context{.min_y = 0,
                                                       .height = 32,
                                                       .sea_level = 8},
        };
        std::size_t calls = 0;
        feature::feature_context context{
            .placement = placement,
            .place_by_id =
                [&](std::string_view, worldgen_random &, const block_pos &) {
                    ++calls;
                    return true;
                },
        };
        const feature::vegetation_patch_feature patch{
            "test:nested",
            feature::simple_state(*moss),
            "#test:replaceable",
            std::move(*depth),
            std::move(*radius),
            false,
            5,
            1.0F,
            0.0F,
            0.0F,
            false};
        worldgen_random random{std::make_unique<legacy_random_source>(1)};
        expect(patch.place(context, random, block_pos{.x = 8, .y = 6, .z = 8}));
        expect(calls > 0);
    };

    "a waterlogged patch keeps exposed edges and floods the rest"_test = [] {
        using namespace refrain::worldgen;
        block_state_registry states;
        (void)states.intern("minecraft:air", {.air = true});
        const auto stone =
            states.intern("minecraft:stone", {.air = false,
                                              .blocks_motion = true,
                                              .blocks_motion_no_leaves = true});
        const auto moss = states.intern("minecraft:moss_block",
                                        {.air = false,
                                         .blocks_motion = true,
                                         .blocks_motion_no_leaves = true});
        const auto water = states.intern(
            "minecraft:water", {.air = false, .fluid = true, .water = true});
        expect(stone && moss && water);
        if (!stone || !moss || !water) {
            return;
        }
        const std::array<block_state_id, 1> tag_states{*stone};
        states.add_tag("test:replaceable", tag_states);
        proto_chunk chunk{states, 0, 0, 0, 32};
        for (jint z = 0; z < 16; ++z) {
            for (jint x = 0; x < 16; ++x) {
                for (jint y = 0; y < 6; ++y) {
                    chunk.set_state(x, y, z, *stone);
                }
            }
        }
        chunk.set_status(chunk_status::terrain);
        chunk_region region{chunk, 0};
        auto depth = provider::int_provider::decode(parse("1"));
        auto radius = provider::int_provider::decode(parse("1"));
        expect(depth.has_value() && radius.has_value());
        if (!depth || !radius) {
            return;
        }
        feature::placement_context placement{
            .region = &region,
            .chunk = &chunk,
            .states = &states,
            .generation = provider::generation_context{.min_y = 0,
                                                       .height = 32,
                                                       .sea_level = 8},
            .resolve_state =
                [&](std::string_view key) {
                    return states.find(key).value_or(no_block_state);
                },
        };
        feature::feature_context context{.placement = placement};
        const feature::vegetation_patch_feature patch{
            "test:nested",
            feature::simple_state(*moss),
            "#test:replaceable",
            std::move(*depth),
            std::move(*radius),
            false,
            5,
            0.0F,
            0.0F,
            0.0F,
            true};
        worldgen_random random{std::make_unique<legacy_random_source>(1)};
        expect(patch.place(context, random, block_pos{.x = 8, .y = 6, .z = 8}));
        expect(states.name(chunk.state_at(8, 5, 8)) == "minecraft:water");
    };
};
