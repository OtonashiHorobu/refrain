// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The per-chunk status scheduler: every demanded chunk queues its three status
// jobs on one worker in pipeline order, the worker assignment comes from the
// demand order and never from which worker finished first, and a failed step
// stops the chunk before any later step runs. The datapack is local and not
// committed, so the content cases skip when the checkout has no copy.

#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/chunk_generator.hpp>

#include "support/scratch_directory.hpp"
#include "support/vanilla_data.hpp"

#include <boost/ut.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace worldgen = refrain::worldgen;
namespace test_support = refrain::test_support;

constexpr worldgen::jlong test_seed = 12345;
constexpr std::string_view no_structure_set =
    "oracle:scheduler_no_structure_set";

auto create_scheduler(std::size_t workers, worldgen::jint cache_capacity = 128,
                      worldgen::jint tile_size = 8)
    -> refrain::result<std::unique_ptr<worldgen::chunk_scheduler>> {
    return worldgen::chunk_scheduler::create(worldgen::chunk_scheduler::config{
        .generator =
            worldgen::chunk_generator_config{
                .data_root = test_support::vanilla_data_root(),
                .templates_root = "",
                .seed = test_seed,
                .kind = worldgen::dimension_kind::overworld,
                .fixed_biome = "minecraft:plains",
                .target = worldgen::generation_target::full,
                .structure_sets = {std::string{no_structure_set}},
                .tile_size = tile_size,
                .proto_cache_capacity = cache_capacity,
            },
        .workers = workers,
    });
}

auto mix(std::uint64_t hash, std::uint64_t value) -> std::uint64_t {
    constexpr std::uint64_t multiplier = 1099511628211ULL;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        hash ^= (value >> shift) & 0xFFU;
        hash *= multiplier;
    }
    return hash;
}

auto mix_bytes(std::uint64_t hash, std::string_view text) -> std::uint64_t {
    constexpr std::uint64_t multiplier = 1099511628211ULL;
    for (const char character : text) {
        hash ^= static_cast<unsigned char>(character);
        hash *= multiplier;
    }
    return hash;
}

auto chunk_digest(const worldgen::proto_chunk &chunk) -> std::uint64_t {
    auto hash = std::uint64_t{0xcbf29ce484222325ULL};
    hash = mix(hash, static_cast<std::uint64_t>(chunk.chunk_x()));
    hash = mix(hash, static_cast<std::uint64_t>(chunk.chunk_z()));
    for (worldgen::jint y = chunk.min_y(); y < chunk.min_y() + chunk.height();
         ++y) {
        for (worldgen::jint z = 0; z < 16; ++z) {
            for (worldgen::jint x = 0; x < 16; ++x) {
                hash = mix_bytes(hash,
                                 chunk.table().name(chunk.state_at(x, y, z)));
            }
        }
    }
    return hash;
}

/// Drives a scheduler to completion with `workers` threads and returns the
/// digest of every served chunk, keyed by position.
auto run_scheduler(worldgen::chunk_scheduler &scheduler, std::size_t workers,
                   const std::vector<worldgen::chunk_pos> &demands)
    -> std::map<std::pair<worldgen::jint, worldgen::jint>, std::uint64_t> {
    std::mutex result_mutex;
    std::map<std::pair<worldgen::jint, worldgen::jint>, std::uint64_t> results;
    for (const auto &pos : demands) {
        expect(scheduler.demand(pos));
    }
    std::atomic<bool> stop{false};
    std::vector<std::jthread> threads;
    threads.reserve(workers);
    for (std::size_t worker = 0; worker < workers; ++worker) {
        threads.emplace_back(
            [&scheduler, &stop, &result_mutex, &results, worker] {
                for (;;) {
                    if (stop.load(std::memory_order_acquire)) {
                        return;
                    }
                    auto job{scheduler.next_job(worker)};
                    if (!job.has_value()) {
                        std::this_thread::yield();
                        continue;
                    }
                    auto generated{scheduler.run(*job)};
                    if (!generated) {
                        const auto message{generated.error().message};
                        scheduler.complete(*job, &message);
                        continue;
                    }
                    scheduler.complete(*job, nullptr);
                    if (job->step != worldgen::pipeline_step::served) {
                        continue;
                    }
                    const auto digest{chunk_digest(**generated)};
                    const std::lock_guard lock{result_mutex};
                    results[{job->position.x, job->position.z}] = digest;
                }
            });
    }
    while (scheduler.pending() != 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    stop.store(true, std::memory_order_release);
    threads.clear();
    return results;
}

} // namespace

suite<"server/chunk-scheduler"> chunk_scheduler_tests = [] {
    "server/chunk scheduler runs one chunk's steps in pipeline order"_test =
        [] {
            auto scheduler{create_scheduler(1)};
            expect(scheduler.has_value())
                << (scheduler ? "" : scheduler.error().message);
            if (!scheduler) {
                return;
            }
            const worldgen::chunk_pos probe{.x = 0, .z = 0};
            expect((*scheduler)->demand(probe));
            expect(!(*scheduler)->demand(probe))
                << "the same chunk was demanded twice";
            auto job{(*scheduler)->next_job(0)};
            expect(job.has_value());
            if (!job) {
                return;
            }
            expect(job->position == probe);
            expect(job->worker == 0U);
            expect(job->step == worldgen::pipeline_step::terrain);
            // No generator runs here; only the queue order is under test.
            const std::string message{"queue order probe"};
            (*scheduler)->complete(*job, &message);
        };

    "server/chunk scheduler assigns a chunk to one worker in demand order"_test =
        [] {
            auto scheduler{create_scheduler(2)};
            expect(scheduler.has_value());
            if (!scheduler) {
                return;
            }
            const std::vector<worldgen::chunk_pos> demands{
                {.x = 0, .z = 0},
                {.x = 1, .z = 0},
                {.x = 2, .z = 0},
                {.x = 3, .z = 0},
            };
            for (const auto &pos : demands) {
                expect((*scheduler)->demand(pos));
            }
            // Demand order round-robin: the first chunk goes to worker 0 and
            // the second to worker 1, whichever thread asks first.
            const auto first_worker_a{(*scheduler)->next_job(0)};
            const auto first_worker_b{(*scheduler)->next_job(1)};
            expect(first_worker_a.has_value());
            expect(first_worker_b.has_value());
            if (!first_worker_a || !first_worker_b) {
                return;
            }
            expect(first_worker_a->position == demands[0]);
            expect(first_worker_a->step == worldgen::pipeline_step::terrain);
            expect(first_worker_a->worker == 0U);
            expect(first_worker_b->position == demands[1]);
            expect(first_worker_b->step == worldgen::pipeline_step::terrain);
            expect(first_worker_b->worker == 1U);
        };

    "server/chunk scheduler reports a failed chunk once and skips the rest"_test =
        [] {
            test_support::scratch_directory scratch;
            auto scheduler{worldgen::chunk_scheduler::create(
                worldgen::chunk_scheduler::config{
                    .generator =
                        worldgen::chunk_generator_config{
                            .data_root = scratch.path().string(),
                            .templates_root = "",
                            .seed = test_seed,
                            .kind = worldgen::dimension_kind::overworld,
                            .target = worldgen::generation_target::full,
                            .structure_sets = {std::string{no_structure_set}},
                        },
                    .workers = 1,
                })};
            expect(scheduler.has_value());
            if (!scheduler) {
                return;
            }
            const worldgen::chunk_pos probe{.x = 0, .z = 0};
            expect((*scheduler)->demand(probe));
            auto terrain{(*scheduler)->next_job(0)};
            expect(terrain.has_value());
            if (!terrain) {
                return;
            }
            auto generated{(*scheduler)->run(*terrain)};
            expect(!generated.has_value())
                << "an empty data root produced a generator";
            if (generated) {
                return;
            }
            (*scheduler)->complete(*terrain, &generated.error().message);
            // The decorated and served jobs of the chunk are skipped.
            expect(!(*scheduler)->next_job(0).has_value());
            const auto failures{(*scheduler)->take_failures()};
            expect(failures.size() == 1U);
            expect(failures.front().position == probe);
            const auto stats{(*scheduler)->stats()};
            expect(stats.failed == 1U);
            expect(stats.served == 0U);
            expect(stats.pending == 0U);
            // A retried demand starts a fresh chain, not the skipped statuses
            // of the failed one.
            expect((*scheduler)->demand(probe));
            auto retry{(*scheduler)->next_job(0)};
            expect(retry.has_value());
            if (retry.has_value()) {
                expect(retry->step == worldgen::pipeline_step::terrain);
                expect(retry->position == probe);
            }
        };

    "server/chunk scheduler serves the pipeline chunk in status steps"_test =
        [] {
            if (!test_support::vanilla_data().available) {
                return;
            }
            auto scheduler{create_scheduler(1)};
            auto generator{worldgen::chunk_generator::create(
                worldgen::chunk_generator_config{
                    .data_root = test_support::vanilla_data_root(),
                    .templates_root = "",
                    .seed = test_seed,
                    .kind = worldgen::dimension_kind::overworld,
                    .fixed_biome = "minecraft:plains",
                    .target = worldgen::generation_target::full,
                    .structure_sets = {std::string{no_structure_set}},
                    .proto_cache_capacity = 128,
                })};
            expect(scheduler.has_value());
            expect(generator.has_value());
            if (!scheduler || !generator) {
                return;
            }
            const worldgen::chunk_pos probe{.x = 0, .z = 0};
            const auto results{run_scheduler(**scheduler, 1, {probe})};
            auto expected{(*generator)->generate_chunk(probe)};
            expect(expected.has_value());
            if (!expected || results.size() != 1U) {
                return;
            }
            const auto found{results.find({probe.x, probe.z})};
            expect(found != results.end());
            if (found == results.end()) {
                return;
            }
            expect(found->second == chunk_digest(**expected));
        };

    "server/chunk scheduler digest does not depend on worker finish order"_test =
        [] {
            if (!test_support::vanilla_data().available) {
                return;
            }
            // Eight worker threads run the same demand sequence twice. The
            // per-worker queues and the worker assignment come from the demand
            // order only, so the served digests must not move with the thread
            // schedule.
            std::vector<worldgen::chunk_pos> demands;
            for (worldgen::jint x = 0; x < 3; ++x) {
                for (worldgen::jint z = 0; z < 3; ++z) {
                    demands.push_back({.x = x, .z = z});
                }
            }
            auto first{create_scheduler(8)};
            auto second{create_scheduler(8)};
            expect(first.has_value());
            expect(second.has_value());
            if (!first || !second) {
                return;
            }
            const auto first_results{run_scheduler(**first, 8, demands)};
            const auto second_results{run_scheduler(**second, 8, demands)};
            expect(first_results.size() == demands.size());
            expect(second_results.size() == demands.size());
            if (first_results.size() != demands.size() ||
                second_results.size() != demands.size()) {
                return;
            }
            for (const auto &[position, digest] : first_results) {
                const auto found{second_results.find(position)};
                expect(found != second_results.end());
                if (found == second_results.end()) {
                    continue;
                }
                expect(digest == found->second)
                    << "chunk (" << position.first << ", " << position.second
                    << ") moved with the worker finish order";
            }
        };

    "server/chunk scheduler content ignores the configured tile edge"_test =
        [] {
            if (!test_support::vanilla_data().available) {
                return;
            }
            // Production no longer generates tiles: the scheduler's jobs are
            // per chunk. The configured edge of the generator is a reference
            // path setting and must not reach the served chunks, with the
            // dimension's own biomes included.
            std::vector<worldgen::chunk_pos> demands;
            for (worldgen::jint x = 0; x < 3; ++x) {
                for (worldgen::jint z = 0; z < 3; ++z) {
                    demands.push_back({.x = x, .z = z});
                }
            }
            std::map<std::pair<worldgen::jint, worldgen::jint>, std::uint64_t>
                reference{};
            for (const worldgen::jint tile_size : {2, 4, 8}) {
                auto scheduler{create_scheduler(1, 128, tile_size)};
                expect(scheduler.has_value())
                    << (scheduler ? "" : scheduler.error().message);
                if (!scheduler) {
                    return;
                }
                const auto results{run_scheduler(**scheduler, 1, demands)};
                expect(results.size() == demands.size());
                if (results.size() != demands.size()) {
                    return;
                }
                if (tile_size == 2) {
                    reference = results;
                    continue;
                }
                for (const auto &[position, digest] : results) {
                    const auto expected{reference.find(position)};
                    expect(expected != reference.end());
                    if (expected == reference.end()) {
                        continue;
                    }
                    expect(digest == expected->second)
                        << "tile edge " << tile_size << " changed chunk ("
                        << position.first << ", " << position.second << ")";
                }
            }
        };

    "server/chunk scheduler keeps its cache under the configured bound"_test =
        [] {
            if (!test_support::vanilla_data().available) {
                return;
            }
            auto scheduler{create_scheduler(1, 25)};
            expect(scheduler.has_value());
            if (!scheduler) {
                return;
            }
            std::vector<worldgen::chunk_pos> demands;
            for (worldgen::jint x = 0; x < 8; ++x) {
                demands.push_back({.x = x, .z = 0});
            }
            const auto results{run_scheduler(**scheduler, 1, demands)};
            expect(results.size() == demands.size());
            const auto stats{(*scheduler)->stats()};
            expect(stats.cache_entries <= 25U)
                << "the proto cache grew to " << stats.cache_entries
                << " entries over a bound of 25";
            expect(stats.cache_evictions > 0U)
                << "the small cache did not evict an entry";
            // Unloading drops every cached chunk.
            (*scheduler)->clear();
            expect((*scheduler)->stats().cache_entries == 0U);
        };
};
