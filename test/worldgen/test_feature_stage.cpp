// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/json.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/feature/feature_stage.hpp>

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
using refrain::worldgen::feature::biome_feature_list;
using refrain::worldgen::feature::block_blob_feature;
using refrain::worldgen::feature::decode_block_predicate;
using refrain::worldgen::feature::decode_placement;
using refrain::worldgen::feature::decorate;
using refrain::worldgen::feature::feature_context;
using refrain::worldgen::feature::feature_sorter;
using refrain::worldgen::feature::placed_feature_runtime;
using refrain::worldgen::feature::placement_context;
using refrain::worldgen::feature::simple_block_feature;
using refrain::worldgen::feature::spring_feature;
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
                auto modifier = decode_placement(entry);
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
