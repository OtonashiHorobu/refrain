// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/chunk/chunk_region.hpp>
#include <refrain/worldgen/feature/feature.hpp>
#include <refrain/worldgen/feature/placement.hpp>
#include <refrain/worldgen/structure/placement.hpp>
#include <refrain/worldgen/structure/stage.hpp>
#include <refrain/worldgen/structure/start.hpp>
#include <refrain/worldgen/structure/structure_data.hpp>
#include <refrain/worldgen/structure/template.hpp>

#include <benchmark/benchmark.h>

#include <cstdint>
#include <string_view>

namespace {

using namespace refrain;
using namespace refrain::worldgen;

void structure_area(benchmark::State &state) {
    structure::structure_data_set data;
    data.structures.emplace(
        "minecraft:buried_treasure",
        structure::structure_document{.id = "minecraft:buried_treasure",
                                      .type = "minecraft:buried_treasure",
                                      .step = "underground_structures",
                                      .biome_ids = {"minecraft:beach"}});
    data.sets.emplace(
        "minecraft:buried_treasures",
        structure::structure_set{
            .id = "minecraft:buried_treasures",
            .structures = {{.structure = "minecraft:buried_treasure",
                            .weight = 1}},
            .random_spread =
                structure::random_spread_placement{
                    .spacing = 1,
                    .separation = 0,
                    .salt = 0,
                    .spread = structure::spread_type::linear,
                    .frequency = 1.0F},
        });
    const structure::structure_start_context context{
        .seed = 12345,
        .data = &data,
        .biome_at = [](jint, jint, jint) { return std::uint16_t{0}; },
        .biome_name = [](std::uint16_t) { return "minecraft:beach"; },
        .height_at = [](jint, jint, heightmap_kind) { return jint{70}; },
    };
    for (auto _ : state) {
        const auto area = structure::generate_structure_area(
            chunk_pos{.x = 0, .z = 0}, context, 8);
        auto starts = area.starts.size();
        auto references = area.references.size();
        benchmark::DoNotOptimize(starts);
        benchmark::DoNotOptimize(references);
    }
}

void template_place(benchmark::State &state) {
    block_state_registry registry;
    (void)registry.intern("minecraft:air", {.air = true});
    const auto stone = registry.intern(
        "minecraft:stone",
        {.air = false, .blocks_motion = true, .blocks_motion_no_leaves = true});
    if (!stone) {
        return;
    }
    registry.add_tag("minecraft:features_cannot_replace", {});
    structure::structure_template templ{};
    templ.size_x = 8;
    templ.size_y = 8;
    templ.size_z = 8;
    std::vector<structure::template_block_info> blocks;
    blocks.reserve(512);
    for (jint y = 0; y < 8; ++y) {
        for (jint x = 0; x < 8; ++x) {
            for (jint z = 0; z < 8; ++z) {
                blocks.push_back(structure::template_block_info{
                    .x = x, .y = y, .z = z, .state = *stone});
            }
        }
    }
    templ.palettes.push_back(std::move(blocks));
    proto_chunk chunk{registry, 0, 0, 0, 64};
    chunk_region region{chunk, 0};
    for (auto _ : state) {
        const auto placed = structure::place_template(
            region, registry, templ, block_pos{.x = 0, .y = 0, .z = 0},
            structure::template_place_settings{});
        auto count = placed;
        benchmark::DoNotOptimize(count);
    }
}

void feature_freeze(benchmark::State &state) {
    block_state_registry registry;
    (void)registry.intern("minecraft:air", {.air = true});
    const auto ice =
        registry.intern("minecraft:ice", {.air = false, .blocks_motion = true});
    const auto snow =
        registry.intern("minecraft:snow[layers=1]", {.air = false});
    const auto stone = registry.intern(
        "minecraft:stone",
        {.air = false, .blocks_motion = true, .blocks_motion_no_leaves = true});
    if (!ice || !snow || !stone) {
        return;
    }
    proto_chunk chunk{registry, 0, 0, 0, 32};
    for (jint z = 0; z < 16; ++z) {
        for (jint x = 0; x < 16; ++x) {
            for (jint y = 0; y < 5; ++y) {
                chunk.set_state(x, y, z, *stone);
            }
        }
    }
    chunk_region region{chunk, 0};
    feature::placement_context placement{
        .region = &region,
        .chunk = &chunk,
        .states = &registry,
        .generation = provider::generation_context{.min_y = 0,
                                                   .height = 32,
                                                   .sea_level = 8},
        .height = [](heightmap_kind, jint, jint) { return jint{5}; },
        .resolve_state =
            [&registry](std::string_view key) {
                return registry.find(key).value_or(no_block_state);
            },
        .should_freeze = [](const block_pos &) { return false; },
        .should_snow = [](const block_pos &) { return true; },
    };
    feature::feature_context context{.placement = placement};
    const feature::freeze_top_layer_feature freeze{*ice, *snow};
    auto random = worldgen_random{std::make_unique<legacy_random_source>(1)};
    for (auto _ : state) {
        const auto placed =
            freeze.place(context, random, block_pos{.x = 0, .y = 0, .z = 0});
        auto result = placed;
        benchmark::DoNotOptimize(result);
    }
}

} // namespace

BENCHMARK(structure_area);
BENCHMARK(template_place);
BENCHMARK(feature_freeze);
