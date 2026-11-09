// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Decoding and placement behaviour of the biome, noise-count and
// surface-relative placement modifiers, and of the weighted random template
// feature they are combined with.

#include <refrain/core/io.hpp>
#include <refrain/data/json.hpp>
#include <refrain/data/nbt.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/chunk/chunk_region.hpp>
#include <refrain/worldgen/chunk/proto_chunk.hpp>
#include <refrain/worldgen/feature/feature_data.hpp>
#include <refrain/worldgen/feature/feature_registry.hpp>
#include <refrain/worldgen/feature/feature_stage.hpp>
#include <refrain/worldgen/noise/noise.hpp>
#include <refrain/worldgen/structure/template.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace boost::ut;
using namespace refrain;
using namespace refrain::worldgen;
using namespace refrain::worldgen::feature;

auto parse(std::string_view text) -> data::json::value {
    auto parsed = data::json::value::parse(text);
    expect(parsed.has_value()) << parsed.error().message;
    return parsed ? std::move(*parsed) : data::json::value{};
}

auto make_random(std::uint64_t seed) -> worldgen_random {
    return worldgen_random{std::make_unique<xoroshiro_random_source>(seed)};
}

} // namespace

suite<"worldgen/placement-noise"> placement_noise_tests = [] {
    "noise threshold count emits the configured count for each side"_test = [] {
        block_state_registry registry;
        auto modifier =
            decode_placement(parse(R"({"type":"minecraft:noise_threshold_count",
                          "noise_level":2.0,
                          "below_noise":3,
                          "above_noise":7})"),
                             registry);
        expect(modifier.has_value()) << modifier.error().message;
        if (!modifier) {
            return;
        }
        const placement_context context{};
        auto random = make_random(1);
        std::vector<block_pos> out;
        (*modifier)->modify(context, random, block_pos{.x = 9, .y = 4, .z = 7},
                            out);
        expect(out.size() == 3U);
        for (const auto &position : out) {
            expect(position.x == 9 && position.y == 4 && position.z == 7);
        }
    };

    "a high noise level selects the above-noise count"_test = [] {
        block_state_registry registry;
        auto modifier =
            decode_placement(parse(R"({"type":"minecraft:noise_threshold_count",
                      "noise_level":-2.0,
                      "below_noise":3,
                      "above_noise":7})"),
                             registry);
        expect(modifier.has_value()) << modifier.error().message;
        if (!modifier) {
            return;
        }
        const placement_context context{};
        auto random = make_random(1);
        std::vector<block_pos> out;
        (*modifier)->modify(context, random, block_pos{.x = 9, .y = 4, .z = 7},
                            out);
        expect(out.size() == 7U);
    };

    "noise threshold count rejects missing and non-numeric fields"_test = [] {
        block_state_registry registry;
        expect(!decode_placement(
                    parse(R"({"type":"minecraft:noise_threshold_count",
                                          "below_noise":1,
                                          "above_noise":2})"),
                    registry)
                    .has_value());
        expect(!decode_placement(
                    parse(R"({"type":"minecraft:noise_threshold_count",
                                          "noise_level":"high",
                                          "below_noise":1,
                                          "above_noise":2})"),
                    registry)
                    .has_value());
        expect(!decode_placement(
                    parse(R"({"type":"minecraft:noise_threshold_count",
                                          "noise_level":0.0,
                                          "below_noise":1.5,
                                          "above_noise":2})"),
                    registry)
                    .has_value());
    };

    "noise based count rounds the scaled biome noise up"_test = [] {
        block_state_registry registry;
        auto modifier =
            decode_placement(parse(R"({"type":"minecraft:noise_based_count",
                      "noise_to_count_ratio":7,
                      "noise_factor":2.0,
                      "noise_offset":0.25})"),
                             registry);
        expect(modifier.has_value()) << modifier.error().message;
        if (!modifier) {
            return;
        }
        const placement_context context{};
        auto random = make_random(1);
        std::vector<block_pos> out;
        const block_pos origin{.x = 10, .y = 0, .z = -40};
        (*modifier)->modify(context, random, origin, out);
        const double noise = static_cast<double>(
            biome_info_noise().get(static_cast<double>(origin.x) / 2.0,
                                   static_cast<double>(origin.z) / 2.0));
        const auto expected = java_int_cast(std::ceil((noise + 0.25) * 7));
        expect(out.size() == static_cast<std::size_t>(expected));
    };

    "noise based count defaults the offset to zero"_test = [] {
        block_state_registry registry;
        auto with_default =
            decode_placement(parse(R"({"type":"minecraft:noise_based_count",
                      "noise_to_count_ratio":120,
                      "noise_factor":80.0})"),
                             registry);
        auto with_zero =
            decode_placement(parse(R"({"type":"minecraft:noise_based_count",
                      "noise_to_count_ratio":120,
                      "noise_factor":80.0,
                      "noise_offset":0.0})"),
                             registry);
        expect(with_default.has_value() && with_zero.has_value());
        if (!with_default || !with_zero) {
            return;
        }
        const placement_context context{};
        auto first_random = make_random(3);
        auto second_random = make_random(3);
        std::vector<block_pos> first;
        std::vector<block_pos> second;
        const block_pos origin{.x = 123, .y = 0, .z = -456};
        (*with_default)->modify(context, first_random, origin, first);
        (*with_zero)->modify(context, second_random, origin, second);
        expect(first.size() == second.size());
    };

    "a zero ratio emits no positions"_test = [] {
        block_state_registry registry;
        auto modifier =
            decode_placement(parse(R"({"type":"minecraft:noise_based_count",
                      "noise_to_count_ratio":0,
                      "noise_factor":80.0})"),
                             registry);
        expect(modifier.has_value());
        if (!modifier) {
            return;
        }
        const placement_context context{};
        auto random = make_random(1);
        std::vector<block_pos> out;
        (*modifier)->modify(context, random, block_pos{.x = 0, .y = 0, .z = 0},
                            out);
        expect(out.empty());
    };

    "noise based count rejects missing fields"_test = [] {
        block_state_registry registry;
        expect(!decode_placement(parse(R"({"type":"minecraft:noise_based_count",
                                          "noise_factor":80.0})"),
                                 registry)
                    .has_value());
        expect(!decode_placement(parse(R"({"type":"minecraft:noise_based_count",
                                          "noise_to_count_ratio":120})"),
                                 registry)
                    .has_value());
        expect(!decode_placement(parse(R"({"type":"minecraft:noise_based_count",
                                          "noise_to_count_ratio":120,
                                          "noise_factor":80.0,
                                          "noise_offset":"zero"})"),
                                 registry)
                    .has_value());
    };
};

suite<"worldgen/placement-surface"> placement_surface_tests = [] {
    "the surface relative filter compares the origin against the surface"_test =
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
            (void)chunk.set_state(1, 5, 1, *stone);
            chunk.set_status(chunk_status::terrain);
            chunk_region region{chunk, 1};
            const placement_context context{
                .region = &region,
                .chunk = &chunk,
                .states = &registry,
                .height =
                    [&region](heightmap_kind kind, jint x, jint z) {
                        return region.height_at(kind, x, z);
                    },
            };
            auto modifier = decode_placement(
                parse(R"({"type":"minecraft:surface_relative_threshold_filter",
                          "heightmap":"WORLD_SURFACE",
                          "min_inclusive":-1,
                          "max_inclusive":0})"),
                registry);
            expect(modifier.has_value()) << modifier.error().message;
            if (!modifier) {
                return;
            }
            auto random = make_random(1);
            std::vector<block_pos> out;
            (*modifier)->modify(context, random,
                                block_pos{.x = 1, .y = 5, .z = 1}, out);
            expect(out.size() == 1U);
            out.clear();
            (*modifier)->modify(context, random,
                                block_pos{.x = 1, .y = 7, .z = 1}, out);
            expect(out.empty());
        };

    "the surface relative filter leaves the bounds open by default"_test = [] {
        block_state_registry registry;
        proto_chunk chunk{registry, 0, 0, 0, 32};
        chunk_region region{chunk, 1};
        const placement_context context{
            .region = &region,
            .chunk = &chunk,
            .states = &registry,
            .height =
                [&region](heightmap_kind kind, jint x, jint z) {
                    return region.height_at(kind, x, z);
                },
        };
        auto modifier = decode_placement(
            parse(R"({"type":"minecraft:surface_relative_threshold_filter",
                      "heightmap":"WORLD_SURFACE"})"),
            registry);
        expect(modifier.has_value());
        if (!modifier) {
            return;
        }
        auto random = make_random(1);
        std::vector<block_pos> out;
        (*modifier)->modify(context, random,
                            block_pos{.x = 1, .y = 400, .z = 1}, out);
        expect(out.size() == 1U);
    };

    "the surface relative filter rejects an invalid heightmap"_test = [] {
        block_state_registry registry;
        expect(
            !decode_placement(
                 parse(
                     R"({"type":"minecraft:surface_relative_threshold_filter"})"),
                 registry)
                 .has_value());
        expect(
            !decode_placement(
                 parse(R"({"type":"minecraft:surface_relative_threshold_filter",
                                          "heightmap":9})"),
                 registry)
                 .has_value());
        expect(
            !decode_placement(
                 parse(R"({"type":"minecraft:surface_relative_threshold_filter",
                                          "heightmap":"NO_SUCH_HEIGHTMAP"})"),
                 registry)
                 .has_value());
        expect(
            !decode_placement(
                 parse(R"({"type":"minecraft:surface_relative_threshold_filter",
                                          "heightmap":"WORLD_SURFACE",
                                          "min_inclusive":"low"})"),
                 registry)
                 .has_value());
    };
};

suite<"worldgen/placement-biome"> placement_biome_tests = [] {
    "the biome filter keeps a listed feature in its biome"_test = [] {
        block_state_registry registry;
        auto modifier =
            decode_placement(parse(R"({"type":"minecraft:biome"})"), registry);
        expect(modifier.has_value()) << modifier.error().message;
        if (!modifier) {
            return;
        }
        placement_context context{};
        const std::string feature{"oracle:included"};
        context.top_feature = feature;
        context.biome = [](const block_pos &) -> std::string_view {
            return "oracle:biome";
        };
        context.biome_has_feature = [](std::string_view biome,
                                       std::string_view placed) {
            return biome == "oracle:biome" && placed == "oracle:included";
        };
        auto random = make_random(1);
        std::vector<block_pos> out;
        (*modifier)->modify(context, random, block_pos{.x = 1, .y = 2, .z = 3},
                            out);
        expect(out.size() == 1U);
    };

    "the biome filter drops a feature that the biome does not list"_test = [] {
        block_state_registry registry;
        auto modifier =
            decode_placement(parse(R"({"type":"minecraft:biome"})"), registry);
        expect(modifier.has_value());
        if (!modifier) {
            return;
        }
        placement_context context{};
        const std::string feature{"oracle:outside"};
        context.top_feature = feature;
        context.biome = [](const block_pos &) -> std::string_view {
            return "oracle:biome";
        };
        context.biome_has_feature = [](std::string_view, std::string_view) {
            return false;
        };
        auto random = make_random(1);
        std::vector<block_pos> out;
        (*modifier)->modify(context, random, block_pos{.x = 1, .y = 2, .z = 3},
                            out);
        expect(out.empty());
    };

    "the biome filter is inert without a top feature or biome source"_test =
        [] {
            block_state_registry registry;
            auto modifier = decode_placement(
                parse(R"({"type":"minecraft:biome"})"), registry);
            expect(modifier.has_value());
            if (!modifier) {
                return;
            }
            auto random = make_random(1);
            std::vector<block_pos> out;
            const placement_context empty{};
            (*modifier)->modify(empty, random,
                                block_pos{.x = 1, .y = 2, .z = 3}, out);
            expect(out.empty());
            placement_context context{};
            context.top_feature = "oracle:included";
            (*modifier)->modify(context, random,
                                block_pos{.x = 1, .y = 2, .z = 3}, out);
            expect(out.empty());
        };

    "decoration applies the biome filter after the earlier modifiers"_test = [] {
        block_state_registry registry;
        auto stone = registry.intern("minecraft:stone", {.air = false});
        auto gold = registry.intern("minecraft:gold_block", {.air = false});
        expect(stone.has_value() && gold.has_value());
        if (!stone || !gold) {
            return;
        }
        feature_data_set data;
        data.features.emplace(
            "oracle:inside",
            configured_feature_document{
                .id = "oracle:inside",
                .document = parse(
                    R"({"type":"minecraft:simple_block","to_place":{"id":"minecraft:stone"}})")});
        data.features.emplace(
            "oracle:outside",
            configured_feature_document{
                .id = "oracle:outside",
                .document = parse(
                    R"({"type":"minecraft:simple_block","to_place":{"id":"minecraft:gold_block"}})")});
        const auto placed = [](std::string id, std::string feature) {
            return placed_feature_document{
                .id = id,
                .feature = std::move(feature),
                .placement = parse(
                    R"([{"type":"minecraft:fixed_placement","positions":[[8,5,8]]},
                            {"type":"minecraft:biome"}])")};
        };
        data.placed.emplace("oracle:inside_placed",
                            placed("oracle:inside_placed", "oracle:inside"));
        data.placed.emplace("oracle:outside_placed",
                            placed("oracle:outside_placed", "oracle:outside"));
        data.biomes.push_back(biome_feature_list{
            .biome = "oracle:a", .steps = {{"oracle:inside_placed"}}});
        data.biomes.push_back(biome_feature_list{
            .biome = "oracle:b", .steps = {{"oracle:outside_placed"}}});

        feature_registry features{registry};
        const auto loaded = features.load(data);
        expect(loaded.has_value()) << loaded.error().message;
        if (!loaded) {
            return;
        }
        const auto plan =
            feature_sorter::build({"oracle:a", "oracle:b"}, data.biomes);
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
                                }};
        context.placement.biome = [](const block_pos &) -> std::string_view {
            return "oracle:a";
        };
        decorate(context, *plan, features.placed(), {"oracle:a", "oracle:b"},
                 data.biomes, 12345);
        expect(chunk.state_at(8, 5, 8) == *stone);
        expect(chunk.state_at(8, 5, 8) != *gold);
    };
};

suite<"worldgen/block-predicate-volume"> new_block_predicate_tests = [] {
    "inside world bounds checks the level height with an optional offset"_test =
        [] {
            block_state_registry registry;
            proto_chunk chunk{registry, 0, 0, 0, 32};
            chunk_region region{chunk, 1};
            auto predicate = decode_block_predicate(
                parse(R"({"type":"minecraft:inside_world_bounds"})"), registry);
            expect(predicate.has_value()) << predicate.error().message;
            if (!predicate) {
                return;
            }
            expect((*predicate)
                       ->test(region, registry,
                              block_pos{.x = 0, .y = 0, .z = 0}));
            expect((*predicate)
                       ->test(region, registry,
                              block_pos{.x = 0, .y = 31, .z = 0}));
            expect(!(*predicate)
                        ->test(region, registry,
                               block_pos{.x = 0, .y = -1, .z = 0}));
            expect(!(*predicate)
                        ->test(region, registry,
                               block_pos{.x = 0, .y = 32, .z = 0}));
            auto offset = decode_block_predicate(
                parse(R"({"type":"minecraft:inside_world_bounds",
                          "offset":[0,16,0]})"),
                registry);
            expect(offset.has_value());
            if (offset) {
                expect((*offset)->test(region, registry,
                                       block_pos{.x = 0, .y = 0, .z = 0}));
                expect(!(*offset)->test(region, registry,
                                        block_pos{.x = 0, .y = 16, .z = 0}));
            }
        };

    "inside world bounds rejects malformed offsets"_test = [] {
        block_state_registry registry;
        for (
            const auto document : std::vector<std::string_view>{
                R"({"type":"minecraft:inside_world_bounds","offset":[0,0]})",
                R"({"type":"minecraft:inside_world_bounds","offset":[0,"x",0]})",
                R"({"type":"minecraft:inside_world_bounds","offset":[0,17,0]})",
            }) {
            expect(
                !decode_block_predicate(parse(document), registry).has_value())
                << document;
        }
    };

    "volume match tests every block in the inclusive box"_test = [] {
        block_state_registry registry;
        auto stone = registry.intern("minecraft:stone", {.air = false});
        expect(stone.has_value());
        if (!stone) {
            return;
        }
        proto_chunk chunk{registry, 0, 0, 0, 32};
        (void)chunk.set_state(2, 3, 4, *stone);
        chunk_region region{chunk, 1};
        auto predicate =
            decode_block_predicate(parse(R"({"type":"minecraft:volume_match",
                      "min":[0,0,0],
                      "max":[0,0,0],
                      "match":{"type":"minecraft:matching_blocks",
                               "blocks":["minecraft:stone"]}})"),
                                   registry);
        expect(predicate.has_value()) << predicate.error().message;
        if (!predicate) {
            return;
        }
        expect((*predicate)
                   ->test(region, registry, block_pos{.x = 2, .y = 3, .z = 4}));
        expect(
            !(*predicate)
                 ->test(region, registry, block_pos{.x = 1, .y = 3, .z = 4}));
    };

    "volume match rejects malformed bounds and predicates"_test = [] {
        block_state_registry registry;
        for (
            const auto document : std::vector<std::string_view>{
                R"({"type":"minecraft:volume_match","min":[0,0,0],"max":[0,0,0]})",
                R"({"type":"minecraft:volume_match","min":[0,0,0],"match":{"type":"minecraft:true"}})",
                R"({"type":"minecraft:volume_match","min":[1,0,0],"max":[0,0,0],"match":{"type":"minecraft:true"}})",
                R"({"type":"minecraft:volume_match","min":[0,0],"max":[0,0,0],"match":{"type":"minecraft:true"}})",
                R"({"type":"minecraft:volume_match","min":[0,17,0],"max":[0,17,0],"match":{"type":"minecraft:true"}})",
                R"({"type":"minecraft:volume_match","min":[0,0,0],"max":[0,0,0],"match":{"type":"minecraft:nope"}})",
            }) {
            expect(
                !decode_block_predicate(parse(document), registry).has_value())
                << document;
        }
    };

    "replaceable follows the block replaceable tag and its offset"_test = [] {
        block_state_registry registry;
        auto grass = registry.intern("minecraft:short_grass", {.air = false});
        auto stone = registry.intern("minecraft:stone", {.air = false});
        expect(grass.has_value() && stone.has_value());
        if (!grass || !stone) {
            return;
        }
        const std::array replaceable{registry.air(), *grass};
        registry.add_tag("minecraft:replaceable", replaceable);
        proto_chunk chunk{registry, 0, 0, 0, 32};
        (void)chunk.set_state(1, 2, 3, *grass);
        (void)chunk.set_state(1, 2, 4, *stone);
        chunk_region region{chunk, 1};
        auto predicate = decode_block_predicate(
            parse(R"({"type":"minecraft:replaceable"})"), registry);
        expect(predicate.has_value());
        if (!predicate) {
            return;
        }
        expect((*predicate)
                   ->test(region, registry, block_pos{.x = 1, .y = 2, .z = 3}));
        expect(
            !(*predicate)
                 ->test(region, registry, block_pos{.x = 1, .y = 2, .z = 4}));
        auto offset = decode_block_predicate(
            parse(R"({"type":"minecraft:replaceable","offset":[0,0,1]})"),
            registry);
        expect(offset.has_value());
        if (offset) {
            expect((*offset)->test(region, registry,
                                   block_pos{.x = 1, .y = 2, .z = 2}));
            expect(!(*offset)->test(region, registry,
                                    block_pos{.x = 1, .y = 2, .z = 3}));
        }
        expect(!decode_block_predicate(parse(R"({"type":"minecraft:replaceable",
                                  "offset":[0,0]})"),
                                       registry)
                    .has_value());
    };

    "would survive follows the cactus and sugar cane support rules"_test = [] {
        block_state_registry registry;
        auto sand = registry.intern("minecraft:sand", {.air = false});
        auto water = registry.intern(
            "minecraft:water", {.air = false, .fluid = true, .water = true});
        auto stone = registry.intern("minecraft:stone",
                                     {.air = false,
                                      .blocks_motion = true,
                                      .blocks_motion_no_leaves = true});
        expect(sand.has_value() && water.has_value() && stone.has_value());
        if (!sand || !water || !stone) {
            return;
        }
        const std::array cactus_support{*sand};
        registry.add_tag("minecraft:supports_cactus", cactus_support);
        registry.add_tag("minecraft:supports_sugar_cane", cactus_support);
        proto_chunk chunk{registry, 0, 0, 0, 32};
        (void)chunk.set_state(0, 0, 0, *sand);
        (void)chunk.set_state(1, 0, 0, *water);
        chunk_region region{chunk, 1};
        auto cactus =
            decode_block_predicate(parse(R"({"type":"minecraft:would_survive",
                          "state":"minecraft:cactus"})"),
                                   registry);
        expect(cactus.has_value());
        if (cactus) {
            expect((*cactus)->test(region, registry,
                                   block_pos{.x = 0, .y = 1, .z = 0}));
            (void)chunk.set_state(1, 1, 0, *stone);
            expect(!(*cactus)->test(region, registry,
                                    block_pos{.x = 0, .y = 1, .z = 0}));
            (void)chunk.set_state(1, 1, 0, registry.air());
        }
        auto cane =
            decode_block_predicate(parse(R"({"type":"minecraft:would_survive",
                          "state":"minecraft:sugar_cane"})"),
                                   registry);
        expect(cane.has_value());
        if (cane) {
            expect((*cane)->test(region, registry,
                                 block_pos{.x = 0, .y = 1, .z = 0}));
            expect(!(*cane)->test(region, registry,
                                  block_pos{.x = 0, .y = 1, .z = 1}));
        }
        expect(
            !decode_block_predicate(parse(R"({"type":"minecraft:would_survive",
                                  "state":"minecraft:oak_log"})"),
                                    registry)
                 .has_value());
    };
};

suite<"worldgen/placement-random"> placement_random_tests = [] {
    "randomly selected runs exactly one nested placement"_test = [] {
        block_state_registry registry;
        auto modifier = decode_placement(
            parse(R"({"type":"minecraft:randomly_selected","placements":[
                      {"type":"minecraft:count","count":1},
                      {"type":"minecraft:count","count":3}]})"),
            registry);
        expect(modifier.has_value()) << modifier.error().message;
        if (!modifier) {
            return;
        }
        auto selection = make_random(99);
        const auto expected =
            selection.next_int(2) == 0 ? std::size_t{1} : std::size_t{3};
        auto random = make_random(99);
        const placement_context context{};
        std::vector<block_pos> out;
        (*modifier)->modify(context, random, block_pos{.x = 0, .y = 0, .z = 0},
                            out);
        expect(out.size() == expected);
    };

    "randomly selected rejects empty and malformed lists"_test = [] {
        block_state_registry registry;
        expect(
            !decode_placement(
                 parse(R"({"type":"minecraft:randomly_selected"})"), registry)
                 .has_value());
        expect(!decode_placement(parse(R"({"type":"minecraft:randomly_selected",
                                          "placements":[]})"),
                                 registry)
                    .has_value());
        expect(!decode_placement(parse(R"({"type":"minecraft:randomly_selected",
                                          "placements":[{"type":"minecraft:nope"}]})"),
                                 registry)
                    .has_value());
    };

    "cuboid emits every cell of the sampled box in xyz order"_test = [] {
        block_state_registry registry;
        auto modifier = decode_placement(parse(R"({"type":"minecraft:cuboid",
                      "xz_size":2,
                      "y_size":2})"),
                                         registry);
        expect(modifier.has_value()) << modifier.error().message;
        if (!modifier) {
            return;
        }
        auto random = make_random(7);
        const placement_context context{};
        std::vector<block_pos> out;
        (*modifier)->modify(context, random,
                            block_pos{.x = 10, .y = 4, .z = 20}, out);
        expect(out.size() == 27U);
        if (out.size() == 27U) {
            expect(out.front().x == 10 && out.front().y == 4 &&
                   out.front().z == 20);
            expect(out[1].z == 21);
            expect(out[3].y == 5);
            expect(out[9].x == 11);
            expect(out.back().x == 12 && out.back().y == 6 &&
                   out.back().z == 22);
        }
    };

    "cuboid can select edges, interior or both"_test = [] {
        block_state_registry registry;
        const auto run = [&](std::string_view document) {
            auto modifier = decode_placement(parse(document), registry);
            expect(modifier.has_value()) << modifier.error().message;
            std::vector<block_pos> out;
            if (modifier) {
                auto random = make_random(3);
                const placement_context context{};
                (*modifier)->modify(context, random,
                                    block_pos{.x = 0, .y = 0, .z = 0}, out);
            }
            return out;
        };
        const auto faces = run(
            R"({"type":"minecraft:cuboid","xz_size":2,"y_size":2,
                "include_interior":false})");
        expect(faces.size() == 26U);
        expect(std::none_of(faces.begin(), faces.end(), [](const block_pos &p) {
            return p.x == 1 && p.y == 1 && p.z == 1;
        }));
        const auto no_edges = run(
            R"({"type":"minecraft:cuboid","xz_size":2,"y_size":2,
                "include_edges":false})");
        expect(no_edges.size() == 7U);
        expect(std::none_of(no_edges.begin(), no_edges.end(),
                            [](const block_pos &p) {
                                const int boundary = (p.x == 0 || p.x == 2) +
                                                     (p.y == 0 || p.y == 2) +
                                                     (p.z == 0 || p.z == 2);
                                return boundary >= 2;
                            }));
        expect(
            !decode_placement(
                 parse(R"({"type":"minecraft:cuboid","xz_size":1})"), registry)
                 .has_value());
    };
};

namespace {

using structure::structure_template_manager;
using structure::template_rotation;

auto ints(std::initializer_list<jint> values) -> data::nbt::value {
    auto list = data::nbt::make_list(data::nbt::tag_type::int_);
    for (const auto value : values) {
        (void)list.push(data::nbt::value{value});
    }
    return list;
}

auto palette_entry(std::string name) -> data::nbt::compound {
    data::nbt::compound entry;
    entry["Name"] = data::nbt::value{std::move(name)};
    return entry;
}

auto block_entry(std::initializer_list<jint> pos, jint state)
    -> data::nbt::compound {
    data::nbt::compound entry;
    entry["pos"] = ints(pos);
    entry["state"] = data::nbt::value{state};
    return entry;
}

auto write_template(const std::filesystem::path &root) -> status {
    std::filesystem::create_directories(root / "oracle" / "structure" / "well");
    data::nbt::compound document;
    document["size"] = ints({1, 1, 1});
    auto palette = data::nbt::make_list(data::nbt::tag_type::compound);
    (void)palette.push(data::nbt::value{palette_entry("minecraft:stone")});
    document["palette"] = std::move(palette);
    auto blocks = data::nbt::make_list(data::nbt::tag_type::compound);
    (void)blocks.push(data::nbt::value{block_entry({0, 0, 0}, 0)});
    document["blocks"] = std::move(blocks);
    return data::nbt::write_file(
        (root / "oracle" / "structure" / "well" / "stone.nbt").string(),
        data::nbt::value{std::move(document)}, "",
        data::nbt::compression::gzip);
}

auto write_wide_template(const std::filesystem::path &root) -> status {
    data::nbt::compound document;
    document["size"] = ints({2, 1, 4});
    auto palette = data::nbt::make_list(data::nbt::tag_type::compound);
    (void)palette.push(data::nbt::value{palette_entry("minecraft:stone")});
    document["palette"] = std::move(palette);
    auto blocks = data::nbt::make_list(data::nbt::tag_type::compound);
    (void)blocks.push(data::nbt::value{block_entry({0, 0, 0}, 0)});
    document["blocks"] = std::move(blocks);
    return data::nbt::write_file(
        (root / "oracle" / "structure" / "well" / "wide.nbt").string(),
        data::nbt::value{std::move(document)}, "",
        data::nbt::compression::gzip);
}

auto make_template_root() -> std::filesystem::path {
    const auto root = std::filesystem::path{core::io::temp_directory()} /
                      "refrain-feature-templates";
    std::filesystem::remove_all(root);
    expect(write_template(root).has_value());
    return root;
}

} // namespace

suite<"worldgen/feature-template"> feature_template_tests = [] {
    "a rotated template is centred on the placement origin"_test = [] {
        block_state_registry registry;
        auto stone = registry.intern("minecraft:stone",
                                     {.air = false,
                                      .blocks_motion = true,
                                      .blocks_motion_no_leaves = true});
        expect(stone.has_value());
        if (!stone) {
            return;
        }
        const auto root = make_template_root();
        expect(write_wide_template(root).has_value());
        structure_template_manager manager{root.string(), registry};
        feature_data_set data;
        data.features.emplace(
            "oracle:template",
            configured_feature_document{.id = "oracle:template",
                                        .document = parse(
                                            R"({"type":"minecraft:template",
                        "templates":[{"data":{"id":"oracle:well/wide",
                                              "rotations":["clockwise_90"]},
                                      "weight":1}]})")});
        data.placed.emplace(
            "oracle:template_placed",
            placed_feature_document{
                .id = "oracle:template_placed",
                .feature = "oracle:template",
                .placement = parse(
                    R"([{"type":"minecraft:fixed_placement","positions":[[8,5,8]]}])")});
        data.biomes.push_back(biome_feature_list{
            .biome = "oracle:test", .steps = {{"oracle:template_placed"}}});
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
        feature_context context{.placement =
                                    placement_context{
                                        .region = &region,
                                        .chunk = &chunk,
                                        .states = &registry,
                                    },
                                .templates = &manager,
                                .registry = &registry};
        decorate(context, *plan, features.placed(), {"oracle:test"},
                 data.biomes, 12345);
        // The template is 2 wide and 4 deep; a clockwise quarter turn adds
        // half of the depth in x and subtracts half of the width in z.
        expect(chunk.state_at(10, 5, 7) == *stone);
    };

    "a template feature places a weighted rotated template"_test = [] {
        block_state_registry registry;
        auto stone = registry.intern("minecraft:stone",
                                     {.air = false,
                                      .blocks_motion = true,
                                      .blocks_motion_no_leaves = true});
        expect(stone.has_value());
        if (!stone) {
            return;
        }
        const auto root = make_template_root();
        structure_template_manager manager{root.string(), registry};
        feature_data_set data;
        data.features.emplace(
            "oracle:template",
            configured_feature_document{.id = "oracle:template",
                                        .document = parse(
                                            R"({"type":"minecraft:template",
                        "templates":[{"data":{"id":"oracle:well/stone",
                                              "rotations":["none"]},
                                      "weight":1}]})")});
        data.placed.emplace(
            "oracle:template_placed",
            placed_feature_document{
                .id = "oracle:template_placed",
                .feature = "oracle:template",
                .placement = parse(
                    R"([{"type":"minecraft:fixed_placement","positions":[[8,5,8]]}])")});
        data.biomes.push_back(biome_feature_list{
            .biome = "oracle:test", .steps = {{"oracle:template_placed"}}});
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
        feature_context context{.placement =
                                    placement_context{
                                        .region = &region,
                                        .chunk = &chunk,
                                        .states = &registry,
                                    },
                                .templates = &manager,
                                .registry = &registry};
        decorate(context, *plan, features.placed(), {"oracle:test"},
                 data.biomes, 12345);
        expect(chunk.state_at(8, 5, 8) == *stone);
    };

    "an overlay can embed an inline template feature"_test = [] {
        block_state_registry registry;
        auto stone = registry.intern("minecraft:stone",
                                     {.air = false,
                                      .blocks_motion = true,
                                      .blocks_motion_no_leaves = true});
        expect(stone.has_value());
        if (!stone) {
            return;
        }
        const auto root = make_template_root();
        structure_template_manager manager{root.string(), registry};
        feature_data_set data;
        data.features.emplace(
            "oracle:overlay",
            configured_feature_document{
                .id = "oracle:overlay",
                .document = parse(
                    R"({"type":"minecraft:overlay","features":[
                        {"feature":{"type":"minecraft:template",
                                    "templates":[{"data":{"id":"oracle:well/stone",
                                                          "rotations":["none"]},
                                                  "weight":1}]},
                         "placement":[]}]})")});
        data.placed.emplace(
            "oracle:overlay_placed",
            placed_feature_document{
                .id = "oracle:overlay_placed",
                .feature = "oracle:overlay",
                .placement = parse(
                    R"([{"type":"minecraft:fixed_placement","positions":[[8,5,8]]}])")});
        data.biomes.push_back(biome_feature_list{
            .biome = "oracle:test", .steps = {{"oracle:overlay_placed"}}});
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
        feature_context context{.placement =
                                    placement_context{
                                        .region = &region,
                                        .chunk = &chunk,
                                        .states = &registry,
                                    },
                                .templates = &manager,
                                .registry = &registry};
        decorate(context, *plan, features.placed(), {"oracle:test"},
                 data.biomes, 12345);
        expect(chunk.state_at(8, 5, 8) == *stone);
    };

    "a template feature without a template manager places nothing"_test = [] {
        block_state_registry registry;
        auto stone = registry.intern("minecraft:stone", {.air = false});
        expect(stone.has_value());
        feature_data_set data;
        data.features.emplace(
            "oracle:template",
            configured_feature_document{.id = "oracle:template",
                                        .document = parse(
                                            R"({"type":"minecraft:template",
                        "templates":[{"data":{"id":"oracle:well/stone"},
                                      "weight":1}]})")});
        data.placed.emplace(
            "oracle:template_placed",
            placed_feature_document{
                .id = "oracle:template_placed",
                .feature = "oracle:template",
                .placement = parse(
                    R"([{"type":"minecraft:fixed_placement","positions":[[8,5,8]]}])")});
        data.biomes.push_back(biome_feature_list{
            .biome = "oracle:test", .steps = {{"oracle:template_placed"}}});
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
        feature_context context{.placement =
                                    placement_context{
                                        .region = &region,
                                        .chunk = &chunk,
                                        .states = &registry,
                                    },
                                .registry = &registry};
        decorate(context, *plan, features.placed(), {"oracle:test"},
                 data.biomes, 12345);
        expect(chunk.state_at(8, 5, 8) == registry.air());
    };

    "template feature decoding rejects invalid documents"_test = [] {
        const auto cases = std::vector<std::string_view>{
            R"({"type":"minecraft:template"})",
            R"({"type":"minecraft:template","templates":[]})",
            R"({"type":"minecraft:template","templates":[{"data":{"id":"oracle:well/stone"}}]})",
            R"({"type":"minecraft:template","templates":[{"data":{"id":"oracle:well/stone"},"weight":0}]})",
            R"({"type":"minecraft:template","templates":[{"data":{"id":"oracle:well/stone"},"weight":-1}]})",
            R"({"type":"minecraft:template","templates":[{"data":{"id":"oracle:well/stone","rotations":[]},"weight":1}]})",
            R"({"type":"minecraft:template","templates":[{"data":{"id":"oracle:well/stone","rotations":["sideways"]},"weight":1}]})",
            R"({"type":"minecraft:template","templates":[{"data":{"id":9},"weight":1}]})",
            R"({"type":"minecraft:template","templates":[{"data":{"id":"oracle:well/stone"},"weight":1}],"processors":9})",
        };
        for (const auto document : cases) {
            feature_data_set data;
            data.features.emplace(
                "oracle:template",
                configured_feature_document{.id = "oracle:template",
                                            .document = parse(document)});
            block_state_registry registry;
            feature_registry features{registry};
            expect(!features.load(data).has_value()) << document;
        }
    };
};
