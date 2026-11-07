// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/chunk/proto_chunk.hpp>
#include <refrain/worldgen/terrain/surface.hpp>

#include <boost/ut.hpp>

#include <cstdint>
#include <optional>

namespace {

using refrain::worldgen::biome_climates;
using refrain::worldgen::biome_temperatures;
using refrain::worldgen::block_state_registry;
using refrain::worldgen::chunk_status;
using refrain::worldgen::proto_chunk;
using refrain::worldgen::surface::block_states;
using refrain::worldgen::surface::generation_bounds;
using refrain::worldgen::surface::material_context;
using refrain::worldgen::surface::material_providers;
using refrain::worldgen::surface::noise_registry;
using refrain::worldgen::surface::rule_evaluator;
using refrain::worldgen::surface::state_id;
using refrain::worldgen::surface::surface_system;

} // namespace

using namespace boost::ut;
using namespace refrain::worldgen;

suite<"worldgen/surface-proto"> surface_proto_tests = [] {
    "an empty rule leaves the chunk intact"_test = [] {
        block_state_registry registry;
        auto stone = registry.intern("minecraft:stone",
                                     {.air = false,
                                      .blocks_motion = true,
                                      .blocks_motion_no_leaves = true});
        auto air = registry.find("minecraft:air");
        expect(stone.has_value());
        expect(air.has_value());
        if (!stone || !air) {
            return;
        }

        block_states states{registry};
        const generation_bounds bounds{
            .min_gen_y = 0,
            .gen_depth = 32,
            .sea_level = 8,
            .min_y = 0,
            .max_block_y = 31,
            .chunk_min_x = 0,
            .chunk_min_z = 0,
        };
        auto root_factory = new_positional_factory(algorithm::legacy, 0);
        noise_registry noises{algorithm::legacy, 0};
        surface_system system{states,
                              bounds,
                              noises,
                              *root_factory,
                              static_cast<state_id>(*stone),
                              static_cast<state_id>(*air)};

        material_providers providers;
        providers.biome = [](jint, jint, jint) {
            return std::string_view{"minecraft:plains"};
        };
        providers.preliminary_surface = [](jint, jint) { return 0.0F; };
        providers.surface_depth = [](jint, jint) { return 3; };
        providers.surface_secondary = [](jint, jint) { return 0.0; };
        providers.band = [](jint, jint, jint) -> state_id { return 0; };
        providers.noise = [](std::string_view) -> const noise_stack * {
            return nullptr;
        };
        providers.cold_enough_to_snow = [](std::string_view, jint, jint, jint) {
            return false;
        };
        providers.random_factory =
            [&root_factory](std::string_view) -> positional_random_factory & {
            return *root_factory;
        };
        const density::sampler_context density_context;
        material_context context{states, bounds, providers, density_context};

        proto_chunk chunk{registry, 0, 0, 0, 32};
        chunk.set_status(chunk_status::biomes);
        expect(chunk.set_state(0, 4, 0, *stone));
        expect(chunk.set_state(15, 4, 15, *stone));

        const rule_evaluator empty_rule = [](jint, jint,
                                             jint) -> std::optional<state_id> {
            return std::nullopt;
        };
        const biome_climates climates;
        const biome_temperatures temperatures;
        const auto built =
            system.build_surface(chunk, context, empty_rule, climates,
                                 temperatures, providers.biome);
        expect(built.has_value());
        expect(chunk.state_at(0, 4, 0) == *stone);
        expect(chunk.state_at(15, 4, 15) == *stone);
        expect(chunk.state_at(0, 5, 0) == *air);
    };
};
