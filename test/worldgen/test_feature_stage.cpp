// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/json.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/feature/feature_data.hpp>
#include <refrain/worldgen/feature/feature_registry.hpp>
#include <refrain/worldgen/feature/feature_stage.hpp>
#include <refrain/worldgen/feature/tree.hpp>

#include <boost/ut.hpp>

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
using refrain::worldgen::feature::biome_feature_list;
using refrain::worldgen::feature::block_blob_feature;
using refrain::worldgen::feature::boolean_selector_feature;
using refrain::worldgen::feature::configured_feature_document;
using refrain::worldgen::feature::decode_block_predicate;
using refrain::worldgen::feature::decode_placement;
using refrain::worldgen::feature::decode_state_provider;
using refrain::worldgen::feature::decorate;
using refrain::worldgen::feature::feature_context;
using refrain::worldgen::feature::feature_data_set;
using refrain::worldgen::feature::feature_registry;
using refrain::worldgen::feature::feature_sorter;
using refrain::worldgen::feature::foliage_kind;
using refrain::worldgen::feature::leave_vine_decorator;
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
using refrain::worldgen::feature::spring_feature;
using refrain::worldgen::feature::tree_decorator_context;
using refrain::worldgen::feature::tree_feature;
using refrain::worldgen::feature::trunk_kind;
using refrain::worldgen::feature::trunk_vine_decorator;
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
