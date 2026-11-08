// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/generator.hpp>

#include <boost/ut.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace {

using refrain::error_code;
using refrain::make_error;
using refrain::status;
using refrain::worldgen::block_state_registry;
using refrain::worldgen::chunk_pos;
using refrain::worldgen::chunk_status;
using refrain::worldgen::generator;
using refrain::worldgen::generator_stages;
using refrain::worldgen::heightmap_kind;
using refrain::worldgen::proto_chunk;

} // namespace

using namespace boost::ut;
using namespace refrain::worldgen;

suite<"worldgen/generator"> generator_tests = [] {
    "terrain runs fill then surface and primes the final heightmaps"_test = [] {
        block_state_registry registry;
        auto stone = registry.intern("minecraft:stone",
                                     {.air = false,
                                      .blocks_motion = true,
                                      .blocks_motion_no_leaves = true});
        auto dirt = registry.intern("minecraft:dirt",
                                    {.air = false,
                                     .blocks_motion = true,
                                     .blocks_motion_no_leaves = true});
        expect(stone.has_value());
        expect(dirt.has_value());
        if (!stone || !dirt) {
            return;
        }

        std::vector<std::string> order;
        generator_stages stages;
        stages.biomes = [&order](proto_chunk &chunk) {
            order.emplace_back("biomes");
            chunk.set_biome(0, 1, 0, 3);
            return refrain::status{};
        };
        stages.fill = [&order, stone = *stone](proto_chunk &chunk) {
            order.emplace_back("fill");
            chunk.set_state(0, 1, 0, stone);
        };
        stages.surface = [&order, dirt = *dirt](proto_chunk &chunk) {
            order.emplace_back("surface");
            chunk.set_state(0, 2, 0, dirt);
            return refrain::status{};
        };

        generator gen{registry, 0, 32, std::move(stages)};
        const auto generated = gen.generate_terrain(chunk_pos{.x = 1, .z = 2});
        expect(generated.has_value());
        if (!generated) {
            return;
        }
        expect((*generated)->chunk_x() == 1);
        expect((*generated)->chunk_z() == 2);
        expect((*generated)->status() == chunk_status::terrain);
        expect(order.size() == 3U);
        expect(order[0] == "biomes");
        expect(order[1] == "fill");
        expect(order[2] == "surface");
        expect((*generated)->biome_at(0, 1, 0) == 3);
        expect((*generated)
                   ->first_available(heightmap_kind::world_surface_wg, 0, 0) ==
               3);
        expect((*generated)
                   ->first_available(heightmap_kind::world_surface, 0, 0) == 3);
    };

    "generation reports missing and failing stages"_test = [] {
        block_state_registry registry;
        generator no_fill{registry, 0, 32, generator_stages{}};
        const auto missing =
            no_fill.generate_terrain(chunk_pos{.x = 0, .z = 0});
        expect(!missing.has_value());
        if (!missing) {
            expect(missing.error().code == error_code::invalid_argument);
        }

        generator_stages failing;
        failing.fill = [](proto_chunk &) {};
        failing.surface = [](proto_chunk &) -> status {
            return std::unexpected{refrain::make_error(
                refrain::error_code::unsupported, "surface failed")};
        };
        generator gen{registry, 0, 32, std::move(failing)};
        const auto failed = gen.generate_terrain(chunk_pos{.x = 0, .z = 0});
        expect(!failed.has_value());

        generator_stages failing_biomes;
        failing_biomes.biomes = [](proto_chunk &) -> status {
            return std::unexpected{refrain::make_error(
                refrain::error_code::unsupported, "biomes failed")};
        };
        failing_biomes.fill = [](proto_chunk &) {};
        generator biome_gen{registry, 0, 32, std::move(failing_biomes)};
        const auto biome_failed =
            biome_gen.generate_terrain(chunk_pos{.x = 0, .z = 0});
        expect(!biome_failed.has_value());

        generator_stages ok;
        ok.fill = [](proto_chunk &) {};
        generator status_gen{registry, 0, 32, std::move(ok)};
        const auto unsupported = status_gen.generate(chunk_pos{.x = 0, .z = 0},
                                                     chunk_status::features);
        expect(!unsupported.has_value());
        if (!unsupported) {
            expect(unsupported.error().code == error_code::unsupported);
        }
    };

    "features and full targets run the later stages in order"_test = [] {
        block_state_registry registry;
        (void)registry.intern("minecraft:stone",
                              {.air = false,
                               .blocks_motion = true,
                               .blocks_motion_no_leaves = true});
        std::vector<std::string> order;
        generator_stages stages;
        stages.fill = [](proto_chunk &) {};
        stages.structures = [&order](proto_chunk &) {
            order.emplace_back("structures");
            return refrain::status{};
        };
        stages.features = [&order](proto_chunk &) {
            order.emplace_back("features");
            return refrain::status{};
        };
        generator gen{registry, 0, 32, std::move(stages)};
        const auto features =
            gen.generate(chunk_pos{.x = 0, .z = 0}, chunk_status::features);
        expect(features.has_value());
        if (features) {
            expect((*features)->status() == chunk_status::features);
        }
        expect(order.size() == 2U);
        expect(order[0] == "structures");
        expect(order[1] == "features");
        order.clear();
        const auto full =
            gen.generate(chunk_pos{.x = 1, .z = 1}, chunk_status::full);
        expect(full.has_value());
        if (full) {
            expect((*full)->status() == chunk_status::full);
        }
        expect(order.size() == 2U);
        expect(order[0] == "structures");
        expect(order[1] == "features");
    };
};
