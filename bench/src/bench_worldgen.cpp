// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/chunk/chunk_region.hpp>
#include <refrain/worldgen/chunk/proto_chunk.hpp>
#include <refrain/worldgen/density/density_router.hpp>
#include <refrain/worldgen/feature/feature.hpp>
#include <refrain/worldgen/feature/feature_data.hpp>
#include <refrain/worldgen/feature/feature_plan.hpp>
#include <refrain/worldgen/feature/feature_registry.hpp>
#include <refrain/worldgen/feature/feature_stage.hpp>
#include <refrain/worldgen/feature/placement.hpp>
#include <refrain/worldgen/structure/generator.hpp>
#include <refrain/worldgen/structure/mansion_layout.hpp>
#include <refrain/worldgen/structure/placement.hpp>
#include <refrain/worldgen/structure/placement_stage.hpp>
#include <refrain/worldgen/structure/stage.hpp>
#include <refrain/worldgen/structure/start.hpp>
#include <refrain/worldgen/structure/structure_data.hpp>
#include <refrain/worldgen/structure/template.hpp>
#include <refrain/worldgen/terrain/aquifer.hpp>
#include <refrain/worldgen/terrain/terrain.hpp>

#include <benchmark/benchmark.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <thread>
#include <vector>

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

void chunk_fill(benchmark::State &state) {
    const char *data_root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
    if (data_root == nullptr) {
        state.SkipWithError("REFRAIN_VANILLA_WORLDGEN is not set");
        return;
    }
    auto settings = density::load_noise_settings(
        data_root, "minecraft/overworld", algorithm::xoroshiro, 12345);
    if (!settings) {
        state.SkipWithError("cannot load the noise settings");
        return;
    }
    constexpr jint min_y = -64;
    constexpr jint height = 384;
    jint position = 0;
    for (auto _ : state) {
        block_state_registry states;
        (void)states.intern("minecraft:air", {.air = true});
        (void)states.intern("minecraft:stone",
                            {.air = false,
                             .blocks_motion = true,
                             .blocks_motion_no_leaves = true});
        (void)states.intern("minecraft:water",
                            {.air = false, .fluid = true, .water = true});
        (void)states.intern("minecraft:lava", {.air = false, .fluid = true});
        auto random = new_positional_factory(algorithm::xoroshiro, 12345);
        auto factory =
            random->from_hash_of("minecraft:aquifer")->fork_positional();
        proto_chunk chunk{states, position, 0, min_y, height};
        chunk.set_status(chunk_status::biomes);
        const density::sampler_context sampler_context;
        const volume volume{16, height, 16, position * 16, min_y, 0};
        terrain::noise_aquifer aquifer{
            settings->aquifer,
            *factory,
            volume,
            terrain::fluid_picker{.sea_level = settings->sea_level,
                                  .default_fluid = 2,
                                  .lava = 3,
                                  .air = 0},
            terrain::block_ids{
                .air = 0, .default_block = 1, .default_fluid = 2, .lava = 3},
            sampler_context};
        terrain::fill_proto_chunk(
            chunk,
            terrain::proto_fill_options{
                .air = 0, .default_block = 1, .default_fluid = 2, .lava = 3},
            *settings->router.final_density, sampler_context, aquifer);
        benchmark::DoNotOptimize(chunk.state_at(8, 0, 8));
        ++position;
    }
    state.SetItemsProcessed(state.iterations());
}

void chunk_fill_parallel(benchmark::State &state) {
    const char *data_root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
    if (data_root == nullptr) {
        state.SkipWithError("REFRAIN_VANILLA_WORLDGEN is not set");
        return;
    }
    auto settings = density::load_noise_settings(
        data_root, "minecraft/overworld", algorithm::xoroshiro, 12345);
    if (!settings) {
        state.SkipWithError("cannot load the noise settings");
        return;
    }
    const auto workers = static_cast<std::size_t>(state.range(0));
    constexpr jint min_y = -64;
    constexpr jint height = 384;
    for (auto _ : state) {
        std::vector<std::jthread> threads;
        threads.reserve(workers);
        for (std::size_t worker = 0; worker < workers; ++worker) {
            threads.emplace_back([&settings, worker] {
                for (jint batch = 0; batch < 4; ++batch) {
                    block_state_registry states;
                    (void)states.intern("minecraft:air", {.air = true});
                    (void)states.intern("minecraft:stone",
                                        {.air = false,
                                         .blocks_motion = true,
                                         .blocks_motion_no_leaves = true});
                    (void)states.intern(
                        "minecraft:water",
                        {.air = false, .fluid = true, .water = true});
                    (void)states.intern("minecraft:lava",
                                        {.air = false, .fluid = true});
                    auto random =
                        new_positional_factory(algorithm::xoroshiro, 12345);
                    auto factory = random->from_hash_of("minecraft:aquifer")
                                       ->fork_positional();
                    const jint chunk_x = static_cast<jint>(worker) + batch * 8;
                    proto_chunk chunk{states, chunk_x, 0, min_y, height};
                    chunk.set_status(chunk_status::biomes);
                    const density::sampler_context sampler_context;
                    const volume volume{16, height, 16, chunk_x * 16, min_y, 0};
                    terrain::noise_aquifer aquifer{
                        settings->aquifer,
                        *factory,
                        volume,
                        terrain::fluid_picker{.sea_level = settings->sea_level,
                                              .default_fluid = 2,
                                              .lava = 3,
                                              .air = 0},
                        terrain::block_ids{.air = 0,
                                           .default_block = 1,
                                           .default_fluid = 2,
                                           .lava = 3},
                        sampler_context};
                    terrain::fill_proto_chunk(
                        chunk,
                        terrain::proto_fill_options{.air = 0,
                                                    .default_block = 1,
                                                    .default_fluid = 2,
                                                    .lava = 3},
                        *settings->router.final_density, sampler_context,
                        aquifer);
                    benchmark::DoNotOptimize(chunk.state_at(8, 0, 8));
                }
            });
        }
        for (auto &thread : threads) {
            thread.join();
        }
    }
    state.counters["chunks_per_iteration"] = static_cast<double>(workers) * 4.0;
}

void structure_corridor(benchmark::State &state) {
    block_state_registry registry;
    (void)registry.intern("minecraft:air", {.air = true});
    (void)registry.intern("minecraft:cave_air", {.air = true});
    const auto stone = registry.intern(
        "minecraft:stone",
        {.air = false, .blocks_motion = true, .blocks_motion_no_leaves = true});
    const auto planks = registry.intern(
        "minecraft:oak_planks",
        {.air = false, .blocks_motion = true, .blocks_motion_no_leaves = true});
    if (!stone || !planks) {
        return;
    }
    proto_chunk chunk{registry, 0, 0, 0, 32};
    for (jint y = 0; y < 32; ++y) {
        for (jint z = 0; z < 16; ++z) {
            for (jint x = 0; x < 16; ++x) {
                chunk.set_state(x, y, z, *stone);
            }
        }
    }
    chunk.set_status(chunk_status::terrain);
    chunk_region region{chunk, 0};
    const auto resolve = [&](std::string_view key) -> block_state_id {
        if (const auto found = registry.find(key); found) {
            return *found;
        }
        return registry
            .intern(key, {.air = false,
                          .blocks_motion = false,
                          .blocks_motion_no_leaves = false})
            .value_or(no_block_state);
    };
    const structure::structure_place_context context{
        .region = &region,
        .chunk = &chunk,
        .states = &registry,
        .state = resolve,
        .registry = &registry,
        .seed = 1,
    };
    const ::refrain::worldgen::structure_piece_record corridor{
        .type = "minecraft:mscorridor",
        .min_x = 0,
        .min_y = 10,
        .min_z = 0,
        .max_x = 2,
        .max_y = 12,
        .max_z = 19,
        .orientation = 0,
        .sections = 4,
        .rails = true,
    };
    for (auto _ : state) {
        worldgen_random random{std::make_unique<xoroshiro_random_source>(1)};
        structure::place_structure_piece(context, corridor, random);
        benchmark::DoNotOptimize(chunk.state_at(1, 10, 2));
    }
}

void feature_decorate(benchmark::State &state) {
    block_state_registry registry;
    (void)registry.intern("minecraft:air", {.air = true});
    const auto stone = registry.intern(
        "minecraft:stone",
        {.air = false, .blocks_motion = true, .blocks_motion_no_leaves = true});
    if (!stone) {
        return;
    }
    proto_chunk chunk{registry, 0, 0, 0, 64};
    for (jint y = 0; y < 4; ++y) {
        for (jint z = 0; z < 16; ++z) {
            for (jint x = 0; x < 16; ++x) {
                chunk.set_state(x, y, z, *stone);
            }
        }
    }
    chunk.set_status(chunk_status::terrain);
    chunk_region region{chunk, 1};
    const std::vector<feature::biome_feature_list> lists{{
        .biome = "bench:test",
        .steps = {{}, {}, {}, {}, {}, {}, {}, {}, {}, {"bench:stone"}},
    }};
    const auto plan = feature::feature_sorter::build({"bench:test"}, lists);
    if (!plan) {
        return;
    }
    feature::feature_registry feature_registry{registry};
    feature::placed_feature_runtime placed;
    placed.id = "bench:stone";
    const feature::simple_block_feature feature{*stone, false};
    placed.configured = &feature;
    const auto placement_document = data::json::value::parse(
        R"([{"type":"minecraft:count","count":4},
            {"type":"minecraft:in_square"},
            {"type":"minecraft:heightmap","heightmap":"WORLD_SURFACE_WG"}])");
    if (!placement_document || !placement_document->is_array()) {
        return;
    }
    for (const auto &entry : *placement_document->as_array()) {
        auto modifier = feature::decode_placement(entry, registry);
        if (!modifier) {
            return;
        }
        placed.placement.push_back(std::move(*modifier));
    }
    std::unordered_map<std::string, feature::placed_feature_runtime> runtime;
    runtime.emplace("bench:stone", std::move(placed));
    feature::placement_context placement{
        .region = &region,
        .chunk = &chunk,
        .states = &registry,
        .generation = provider::generation_context{.min_y = 0,
                                                   .height = 64,
                                                   .sea_level = 16},
        .height =
            [&chunk](worldgen::heightmap_kind kind, jint x, jint z) {
                return chunk.first_available(kind, x & 15, z & 15);
            },
    };
    feature::feature_context context{.placement = placement};
    for (auto _ : state) {
        feature::decorate(context, *plan, runtime, {"bench:test"}, lists,
                          12345);
        benchmark::DoNotOptimize(chunk.state_at(1, 5, 1));
    }
}

void structure_generator(benchmark::State &state) {
    const char *data_root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
    const char *templates_root = std::getenv("REFRAIN_VANILLA_STRUCTURES");
    if (data_root == nullptr || templates_root == nullptr) {
        state.SkipWithError("worldgen paths are not set");
        return;
    }
    auto generator = structure::structure_generator::create(
        structure::structure_generator_config{
            .data_root = data_root,
            .templates_root = templates_root,
            .biome = "minecraft:plains",
            .seed = 1,
        });
    if (!generator) {
        state.SkipWithError("cannot load the structure generator");
        return;
    }
    for (auto _ : state) {
        const auto chunk = (*generator)->generate(chunk_pos{.x = 0, .z = 0});
        if (!chunk) {
            state.SkipWithError("generation failed");
            return;
        }
        benchmark::DoNotOptimize((*chunk)->state_at(8, 0, 8));
    }
}

void mansion_layout(benchmark::State &state) {
    for (auto _ : state) {
        worldgen_random random{std::make_unique<legacy_random_source>(1)};
        structure::mansion_grid grid{random};
        const auto pieces = structure::generate_mansion(
            random, block_pos{.x = 0, .y = 60, .z = 0},
            structure::template_rotation::none, grid);
        benchmark::DoNotOptimize(pieces.size());
    }
}

BENCHMARK(structure_area);
BENCHMARK(structure_corridor);
BENCHMARK(feature_decorate);
BENCHMARK(structure_generator)->Unit(benchmark::kMillisecond);
BENCHMARK(mansion_layout);
BENCHMARK(template_place);
BENCHMARK(feature_freeze);
BENCHMARK(chunk_fill);
BENCHMARK(chunk_fill_parallel)->Arg(1)->Arg(2)->Arg(4)->Arg(8);
