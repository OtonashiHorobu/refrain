// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Determinism check: the fill stage must produce the same blocks whether
// chunks are generated on one worker or several. REFRAIN_VANILLA_WORLDGEN
// overrides the datapack `data` root; the shipped snapshot is the default.

#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/chunk/proto_chunk.hpp>
#include <refrain/worldgen/chunk_generator.hpp>
#include <refrain/worldgen/density/density_router.hpp>
#include <refrain/worldgen/terrain/aquifer.hpp>
#include <refrain/worldgen/terrain/terrain.hpp>

#include "support/vanilla_data.hpp"

#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
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

constexpr std::string_view no_structure_set =
    "oracle:scheduler_no_structure_set";

auto scheduler_config(const std::string &data_root, jint capacity)
    -> chunk_generator_config {
    return chunk_generator_config{
        .data_root = data_root,
        .templates_root = "",
        .seed = seed,
        .kind = dimension_kind::overworld,
        .fixed_biome = "minecraft:plains",
        .target = generation_target::full,
        .structure_sets = {std::string{no_structure_set}},
        .tile_size = 8,
        .proto_cache_capacity = capacity,
    };
}

auto mix_bytes(std::uint64_t hash, std::string_view text) -> std::uint64_t {
    constexpr std::uint64_t prime = 1099511628211ULL;
    for (const char character : text) {
        hash ^= static_cast<unsigned char>(character);
        hash *= prime;
    }
    return hash;
}

auto mix_integer(std::uint64_t hash, std::uint64_t value) -> std::uint64_t {
    constexpr std::uint64_t prime = 1099511628211ULL;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        hash ^= (value >> shift) & 0xFFU;
        hash *= prime;
    }
    return hash;
}

auto chunk_digest(const proto_chunk &chunk) -> std::uint64_t {
    const auto &table = chunk.table();
    auto hash = std::uint64_t{0xcbf29ce484222325ULL};
    hash = mix_integer(hash, static_cast<std::uint64_t>(chunk.chunk_x()));
    hash = mix_integer(hash, static_cast<std::uint64_t>(chunk.chunk_z()));
    hash = mix_integer(hash, static_cast<std::uint64_t>(chunk.status()));
    for (jint y = chunk.min_y(); y < chunk.min_y() + chunk.height(); ++y) {
        for (jint z = 0; z < 16; ++z) {
            for (jint x = 0; x < 16; ++x) {
                hash = mix_bytes(hash, table.name(chunk.state_at(x, y, z)));
            }
        }
    }
    for (jint y = chunk.min_y(); y < chunk.min_y() + chunk.height(); y += 4) {
        for (jint z = 0; z < 16; z += 4) {
            for (jint x = 0; x < 16; x += 4) {
                hash = mix_integer(hash, chunk.biome_at(x, y, z));
            }
        }
    }
    for (const auto &start : chunk.structure_starts()) {
        hash = mix_bytes(hash, start.structure);
        hash = mix_integer(hash, static_cast<std::uint64_t>(start.chunk_x));
        hash = mix_integer(hash, static_cast<std::uint64_t>(start.chunk_z));
    }
    hash = mix_integer(
        hash, static_cast<std::uint64_t>(chunk.block_entities().size()));
    for (const auto &[pos, entity] : chunk.block_entities()) {
        (void)entity;
        hash = mix_integer(hash, static_cast<std::uint64_t>(pos.x));
        hash = mix_integer(hash, static_cast<std::uint64_t>(pos.y));
        hash = mix_integer(hash, static_cast<std::uint64_t>(pos.z));
    }
    for (const auto &section : chunk.post_processing()) {
        for (const auto packed : section) {
            hash = mix_integer(hash, packed);
        }
    }
    return hash;
}

using digest_map = std::map<std::pair<jint, jint>, std::uint64_t>;

/// Drives a scheduler with real workers over one demand order and returns the
/// digest of every served chunk.
auto run_demands(const std::vector<chunk_pos> &demands, std::size_t workers,
                 bool structures = false) -> digest_map {
    const auto snapshot = test_support::vanilla_data();
    if (!snapshot.available) {
        return {};
    }
    auto config = scheduler_config(snapshot.path, 128);
    if (structures) {
        config.structure_sets = {};
    }
    auto scheduler{chunk_scheduler::create(chunk_scheduler::config{
        .generator = config,
        .workers = workers,
    })};
    expect(scheduler.has_value());
    if (!scheduler) {
        return {};
    }
    for (const auto &pos : demands) {
        expect((*scheduler)->demand(pos));
    }
    std::mutex result_mutex;
    digest_map results;
    std::vector<std::jthread> threads;
    for (std::size_t worker = 0; worker < workers; ++worker) {
        threads.emplace_back([&, worker] {
            for (;;) {
                auto job{(*scheduler)->next_job(worker)};
                if (!job.has_value()) {
                    return;
                }
                auto generated{(*scheduler)->run(*job)};
                if (!generated) {
                    const auto message{generated.error().message};
                    (*scheduler)->complete(*job, &message);
                    return;
                }
                (*scheduler)->complete(*job, nullptr);
                if (job->step != pipeline_step::served) {
                    continue;
                }
                const auto digest{chunk_digest(**generated)};
                const std::lock_guard lock{result_mutex};
                results[{job->position.x, job->position.z}] = digest;
            }
        });
    }
    threads.clear();
    expect((*scheduler)->pending() == 0U);
    return results;
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

    "the scheduler ignores demand order when decorating"_test = [] {
        const auto snapshot = test_support::vanilla_data();
        if (!snapshot.available) {
            return;
        }
        std::vector<chunk_pos> ascending;
        for (jint x = 0; x < 4; ++x) {
            for (jint z = 0; z < 4; ++z) {
                ascending.push_back(chunk_pos{.x = x, .z = z});
            }
        }
        auto reversed = ascending;
        std::ranges::reverse(reversed);
        auto by_z = ascending;
        std::ranges::sort(
            by_z, [](const chunk_pos &left, const chunk_pos &right) {
                return std::pair{left.z, left.x} < std::pair{right.z, right.x};
            });
        digest_map reference;
        bool first = true;
        for (const auto &demands : {ascending, reversed, by_z}) {
            for (const std::size_t workers : {1U, 4U, 8U}) {
                const auto results{run_demands(demands, workers)};
                expect(results.size() == ascending.size());
                if (results.size() != ascending.size()) {
                    return;
                }
                if (first) {
                    reference = results;
                    first = false;
                    continue;
                }
                for (const auto &[position, digest] : results) {
                    const auto expected{reference.find(position)};
                    expect(expected != reference.end());
                    if (expected != reference.end()) {
                        expect(digest == expected->second)
                            << "chunk (" << position.first << ", "
                            << position.second
                            << ") changed with the demand order";
                    }
                }
            }
        }
        // The position order is the serial pipeline's order, so the shared
        // state must still serve that pipeline's chunks.
        auto generator{
            chunk_generator::create(scheduler_config(snapshot.path, 128))};
        expect(generator.has_value());
        if (!generator) {
            return;
        }
        for (const auto &position :
             {chunk_pos{.x = 0, .z = 0}, chunk_pos{.x = 3, .z = 3}}) {
            auto expected{(*generator)->generate_chunk(position)};
            expect(expected.has_value());
            if (!expected) {
                return;
            }
            const auto found = reference.find({position.x, position.z});
            expect(found != reference.end());
            if (found != reference.end()) {
                expect(found->second == chunk_digest(**expected))
                    << "the position order is not the serial decoration order";
            }
        }
    };

    "the scheduler ignores demand order with structures enabled"_test = [] {
        if (!test_support::vanilla_data().available) {
            return;
        }
        std::vector<chunk_pos> ascending;
        for (jint x = 0; x < 3; ++x) {
            for (jint z = 0; z < 3; ++z) {
                ascending.push_back(chunk_pos{.x = x, .z = z});
            }
        }
        auto reversed = ascending;
        std::ranges::reverse(reversed);
        digest_map reference;
        bool first = true;
        for (const auto &demands : {ascending, reversed}) {
            for (const std::size_t workers : {1U, 4U, 8U}) {
                const auto results{run_demands(demands, workers, true)};
                expect(results.size() == ascending.size());
                if (results.size() != ascending.size()) {
                    return;
                }
                if (first) {
                    reference = results;
                    first = false;
                    continue;
                }
                for (const auto &[position, digest] : results) {
                    const auto expected{reference.find(position)};
                    expect(expected != reference.end());
                    if (expected != reference.end()) {
                        expect(digest == expected->second)
                            << "structure content changed with the demand "
                               "order";
                    }
                }
            }
        }
    };

    "non-interacting decoration passes run concurrently"_test = [] {
        if (!test_support::vanilla_data().available) {
            return;
        }
        auto scheduler{chunk_scheduler::create(chunk_scheduler::config{
            .generator =
                scheduler_config(test_support::vanilla_data_root(), 128),
            .workers = 2,
        })};
        expect(scheduler.has_value());
        if (!scheduler) {
            return;
        }
        const chunk_pos first{.x = 0, .z = 0};
        const chunk_pos far{.x = 20, .z = 20};
        const chunk_pos near{.x = 1, .z = 0};
        expect((*scheduler)->demand(first));
        expect((*scheduler)->demand(far));
        for (std::size_t index = 0; index < 2; ++index) {
            auto job{(*scheduler)->next_job(0)};
            expect(job.has_value());
            if (!job || job->step != pipeline_step::terrain) {
                return;
            }
            auto generated{(*scheduler)->run(*job)};
            expect(generated.has_value());
            if (!generated) {
                return;
            }
            (*scheduler)->complete(*job, nullptr);
        }
        auto left{(*scheduler)->next_job(0)};
        auto right{(*scheduler)->next_job(1)};
        expect(left.has_value());
        expect(right.has_value());
        if (!left || !right) {
            return;
        }
        expect(left->step == pipeline_step::decorated);
        expect(right->step == pipeline_step::decorated);
        expect(left->position != right->position);
        // A pass whose write radius overlaps the one in flight must wait even
        // though the worker is free and its terrain is ready.
        expect((*scheduler)->demand(near));
        auto terrain{(*scheduler)->next_job(0)};
        expect(terrain.has_value());
        if (!terrain) {
            return;
        }
        auto filled{(*scheduler)->run(*terrain)};
        expect(filled.has_value());
        if (!filled) {
            return;
        }
        (*scheduler)->complete(*terrain, nullptr);
        expect(!(*scheduler)->next_job(0).has_value());
        (*scheduler)->complete(*left, nullptr);
        (*scheduler)->complete(*right, nullptr);
        auto waiting{(*scheduler)->next_job(0)};
        expect(waiting.has_value());
        if (waiting) {
            expect(waiting->step == pipeline_step::decorated);
            expect(waiting->position == near);
        }
    };
};
