// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Determinism check: the fill stage must produce the same blocks whether
// chunks are generated on one worker or several. REFRAIN_VANILLA_WORLDGEN
// overrides the datapack `data` root; the shipped snapshot is the default.

#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/chunk/proto_chunk.hpp>
#include <refrain/worldgen/density/density_router.hpp>
#include <refrain/worldgen/terrain/aquifer.hpp>
#include <refrain/worldgen/terrain/terrain.hpp>

#include "support/vanilla_data.hpp"

#include <boost/ut.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace boost::ut;
using namespace refrain;
using namespace refrain::worldgen;

constexpr jint min_y = -64;
constexpr jint height = 384;
constexpr jlong seed = 12345;

auto make_registry() -> block_state_registry {
    block_state_registry states;
    (void)states.intern("minecraft:air", {.air = true});
    (void)states.intern(
        "minecraft:stone",
        {.air = false, .blocks_motion = true, .blocks_motion_no_leaves = true});
    (void)states.intern("minecraft:water",
                        {.air = false, .fluid = true, .water = true});
    (void)states.intern("minecraft:lava", {.air = false, .fluid = true});
    return states;
}

} // namespace

suite<"worldgen/parallel"> parallel_generation_tests = [] {
    "the fill stage is deterministic across worker counts"_test = [] {
        const char *override_root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
        std::string data_root{};
        if (override_root != nullptr) {
            data_root = override_root;
        } else {
            const auto snapshot = refrain::test_support::vanilla_data();
            if (!snapshot.available) {
                return;
            }
            data_root = snapshot.path;
        }
        auto settings = density::load_noise_settings(
            data_root, "minecraft/overworld", algorithm::xoroshiro, seed);
        expect(settings.has_value());
        if (!settings) {
            return;
        }
        constexpr std::array<std::array<jint, 2>, 16> chunks{{
            {0, 0},
            {1, 0},
            {2, 0},
            {3, 0},
            {0, 1},
            {1, 1},
            {2, 1},
            {3, 1},
            {0, 2},
            {1, 2},
            {2, 2},
            {3, 2},
            {0, 3},
            {1, 3},
            {2, 3},
            {3, 3},
        }};
        std::vector<std::vector<std::uint16_t>> blocks(chunks.size());
        const auto generate = [&](std::size_t index) {
            const auto &position = chunks[index];
            auto random = new_positional_factory(algorithm::xoroshiro, seed);
            auto factory =
                random->from_hash_of("minecraft:aquifer")->fork_positional();
            auto states = make_registry();
            proto_chunk chunk{states, position[0], position[1], min_y, height};
            chunk.set_status(chunk_status::biomes);
            const density::sampler_context sampler_context;
            const volume volume{16,    height,          16, position[0] * 16,
                                min_y, position[1] * 16};
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
                *settings->router.final_density, sampler_context, aquifer);
            auto &out = blocks[index];
            out.resize(static_cast<std::size_t>(height) * 256U);
            for (jint y = min_y; y < min_y + height; ++y) {
                for (jint z = 0; z < 16; ++z) {
                    for (jint x = 0; x < 16; ++x) {
                        out[static_cast<std::size_t>(y - min_y) * 256U +
                            static_cast<std::size_t>(z) * 16U +
                            static_cast<std::size_t>(x)] =
                            chunk.state_at(x, y, z);
                    }
                }
            }
        };
        for (std::size_t index = 0; index < chunks.size(); ++index) {
            generate(index);
        }
        const auto sequential = blocks;
        blocks.assign(chunks.size(), {});
        std::array<std::jthread, 4> workers{};
        for (std::size_t worker = 0; worker < workers.size(); ++worker) {
            workers[worker] = std::jthread([&, worker] {
                for (std::size_t index = worker; index < chunks.size();
                     index += workers.size()) {
                    generate(index);
                }
            });
        }
        for (auto &worker : workers) {
            worker.join();
        }
        expect(blocks == sequential);
    };
};
