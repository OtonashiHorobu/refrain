// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The shared deterministic per-chunk scheduler: one frozen block-state table,
// one proto chunk cache and one ordered decoration lane serve the status jobs
// of every demanded column. The tests pin the job contract, the failure and
// retry path, and the content gate: the same demand sequence must serve the
// same chunks at one, four and eight workers, and the served chunks must be
// the serial per-chunk pipeline's chunks. The datapack is local and not
// committed, so the content cases skip when the checkout has no copy.

#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/chunk_generator.hpp>

#include "support/scratch_directory.hpp"
#include "support/vanilla_data.hpp"

#include <boost/ut.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
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

auto generator_config(std::string data_root, worldgen::jint cache_capacity,
                      worldgen::jint tile_size = 8)
    -> worldgen::chunk_generator_config {
    return worldgen::chunk_generator_config{
        .data_root = std::move(data_root),
        .templates_root = "",
        .seed = test_seed,
        .kind = worldgen::dimension_kind::overworld,
        .fixed_biome = "minecraft:plains",
        .target = worldgen::generation_target::full,
        .structure_sets = {std::string{no_structure_set}},
        .tile_size = tile_size,
        .proto_cache_capacity = cache_capacity,
    };
}

auto all_structures_config(std::string data_root)
    -> worldgen::chunk_generator_config {
    auto config = generator_config(std::move(data_root), 128);
    config.structure_sets = {};
    return config;
}

auto create_scheduler(std::size_t workers, worldgen::jint cache_capacity = 128,
                      worldgen::jint tile_size = 8)
    -> refrain::result<std::unique_ptr<worldgen::chunk_scheduler>> {
    return worldgen::chunk_scheduler::create(worldgen::chunk_scheduler::config{
        .generator = generator_config(test_support::vanilla_data_root(),
                                      cache_capacity, tile_size),
        .workers = workers,
    });
}

auto hash_bytes(std::uint64_t hash, std::string_view text) -> std::uint64_t {
    constexpr std::uint64_t prime = 1099511628211ULL;
    for (const char character : text) {
        hash ^= static_cast<unsigned char>(character);
        hash *= prime;
    }
    return hash;
}

auto hash_integer(std::uint64_t hash, std::uint64_t value) -> std::uint64_t {
    constexpr std::uint64_t prime = 1099511628211ULL;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        hash ^= (value >> shift) & 0xFFU;
        hash *= prime;
    }
    return hash;
}

/// Everything a served chunk carries that a reader could observe.
auto chunk_digest(const worldgen::proto_chunk &chunk) -> std::uint64_t {
    const auto &table = chunk.table();
    auto hash = std::uint64_t{0xcbf29ce484222325ULL};
    hash = hash_integer(hash, static_cast<std::uint64_t>(chunk.chunk_x()));
    hash = hash_integer(hash, static_cast<std::uint64_t>(chunk.chunk_z()));
    hash = hash_integer(hash, static_cast<std::uint64_t>(chunk.status()));
    for (worldgen::jint y = chunk.min_y(); y < chunk.min_y() + chunk.height();
         ++y) {
        for (worldgen::jint z = 0; z < 16; ++z) {
            for (worldgen::jint x = 0; x < 16; ++x) {
                hash = hash_bytes(hash, table.name(chunk.state_at(x, y, z)));
            }
        }
    }
    for (worldgen::jint y = chunk.min_y(); y < chunk.min_y() + chunk.height();
         y += 4) {
        for (worldgen::jint z = 0; z < 16; z += 4) {
            for (worldgen::jint x = 0; x < 16; x += 4) {
                hash = hash_integer(hash, chunk.biome_at(x, y, z));
            }
        }
    }
    for (const auto &start : chunk.structure_starts()) {
        hash = hash_bytes(hash, start.structure);
        hash = hash_bytes(hash, start.step);
        hash = hash_integer(hash, static_cast<std::uint64_t>(start.chunk_x));
        hash = hash_integer(hash, static_cast<std::uint64_t>(start.chunk_z));
        hash = hash_integer(hash, static_cast<std::uint64_t>(start.min_x));
        hash = hash_integer(hash, static_cast<std::uint64_t>(start.min_y));
        hash = hash_integer(hash, static_cast<std::uint64_t>(start.min_z));
        hash = hash_integer(hash, static_cast<std::uint64_t>(start.max_x));
        hash = hash_integer(hash, static_cast<std::uint64_t>(start.max_y));
        hash = hash_integer(hash, static_cast<std::uint64_t>(start.max_z));
        for (const auto &piece : start.pieces) {
            hash = hash_bytes(hash, piece.type);
        }
    }
    hash = hash_integer(
        hash, static_cast<std::uint64_t>(chunk.block_entities().size()));
    for (const auto &[pos, entity] : chunk.block_entities()) {
        (void)entity;
        hash = hash_integer(hash, static_cast<std::uint64_t>(pos.x));
        hash = hash_integer(hash, static_cast<std::uint64_t>(pos.y));
        hash = hash_integer(hash, static_cast<std::uint64_t>(pos.z));
    }
    for (const auto &section : chunk.post_processing()) {
        for (const auto packed : section) {
            hash = hash_integer(hash, packed);
        }
    }
    return hash;
}

using digest_map =
    std::map<std::pair<worldgen::jint, worldgen::jint>, std::uint64_t>;

/// Drives a scheduler to completion with `workers` threads and returns the
/// digest of every served chunk, keyed by position. The workers run the same
/// loop the runtime uses, so the test covers the scheduler's wake-up contract
/// rather than a single-threaded replay.
auto run_scheduler(worldgen::chunk_scheduler &scheduler, std::size_t workers,
                   const std::vector<worldgen::chunk_pos> &demands)
    -> digest_map {
    std::mutex result_mutex;
    digest_map results;
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

auto make_positions(worldgen::jint width, worldgen::jint depth)
    -> std::vector<worldgen::chunk_pos> {
    std::vector<worldgen::chunk_pos> positions;
    positions.reserve(static_cast<std::size_t>(width) *
                      static_cast<std::size_t>(depth));
    for (worldgen::jint x = 0; x < width; ++x) {
        for (worldgen::jint z = 0; z < depth; ++z) {
            positions.push_back({.x = x, .z = z});
        }
    }
    return positions;
}

} // namespace

suite<"server/chunk-scheduler"> chunk_scheduler_tests = [] {
    "server/chunk scheduler queues a demanded column once"_test = [] {
        if (!test_support::vanilla_data().available) {
            return;
        }
        auto scheduler{create_scheduler(2)};
        expect(scheduler.has_value())
            << (scheduler ? "" : scheduler.error().message);
        if (!scheduler) {
            return;
        }
        const worldgen::chunk_pos probe{.x = 0, .z = 0};
        expect((*scheduler)->demand(probe));
        expect(!(*scheduler)->demand(probe))
            << "the same column was demanded twice";
        expect((*scheduler)->pending() == 1U);
    };

    "server/chunk scheduler runs terrain before the ordered decoration and serve steps"_test =
        [] {
            if (!test_support::vanilla_data().available) {
                return;
            }
            auto scheduler{create_scheduler(1)};
            expect(scheduler.has_value());
            if (!scheduler) {
                return;
            }
            const worldgen::chunk_pos probe{.x = 0, .z = 0};
            expect((*scheduler)->demand(probe));
            std::vector<worldgen::pipeline_step> steps;
            for (;;) {
                auto job{(*scheduler)->next_job(0)};
                if (!job.has_value()) {
                    break;
                }
                auto generated{(*scheduler)->run(*job)};
                expect(generated.has_value());
                if (!generated) {
                    return;
                }
                (*scheduler)->complete(*job, nullptr);
                steps.push_back(job->step);
                if (job->step == worldgen::pipeline_step::served) {
                    break;
                }
            }
            expect(steps.size() == 3U);
            if (steps.size() != 3U) {
                return;
            }
            expect(steps[0] == worldgen::pipeline_step::terrain);
            expect(steps[1] == worldgen::pipeline_step::decorated);
            expect(steps[2] == worldgen::pipeline_step::served);
        };

    "server/chunk scheduler refuses content without a block-state table"_test =
        [] {
            if (!test_support::vanilla_data().available) {
                return;
            }
            // The datapack is readable through a symlink, but no
            // `tables.rvtb` sits beside it, so there is no shared state table.
            test_support::scratch_directory scratch;
            const auto content = scratch.path() / "content";
            std::filesystem::create_directories(content);
            std::filesystem::create_directory_symlink(
                test_support::vanilla_data_root(), content / "data");
            auto scheduler{worldgen::chunk_scheduler::create(
                worldgen::chunk_scheduler::config{
                    .generator =
                        generator_config((content / "data").string(), 128),
                    .workers = 1,
                })};
            // The shared pipeline state needs the content's block-state table;
            // starting without it fails before any column is demanded instead
            // of serving content whose state ids could not be shared.
            expect(!scheduler.has_value());
            if (!scheduler) {
                expect(scheduler.error().message.find("tables.rvtb") !=
                       std::string::npos)
                    << scheduler.error().message;
            }
        };

    "server/chunk scheduler reports a failed job once and retries the column"_test =
        [] {
            if (!test_support::vanilla_data().available) {
                return;
            }
            auto scheduler{create_scheduler(1)};
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
            // Drive the job to a worker that does not exist: the scheduler
            // reports the column once and drops its remaining statuses.
            auto faulty{*terrain};
            faulty.worker = 64U;
            auto generated{(*scheduler)->run(faulty)};
            expect(!generated.has_value());
            if (generated) {
                return;
            }
            const auto message{generated.error().message};
            (*scheduler)->complete(faulty, &message);
            const auto failures{(*scheduler)->take_failures()};
            expect(failures.size() == 1U);
            expect(failures.front().position == probe);
            const auto stats{(*scheduler)->stats()};
            expect(stats.failed == 1U);
            expect(stats.served == 0U);
            expect(stats.pending == 0U);
            // The retried demand is served again from the seed; the failed
            // chain left no partially generated column behind.
            expect((*scheduler)->demand(probe));
            std::unique_ptr<worldgen::proto_chunk> served;
            for (;;) {
                auto job{(*scheduler)->next_job(0)};
                if (!job.has_value()) {
                    return;
                }
                auto retried{(*scheduler)->run(*job)};
                expect(retried.has_value())
                    << (retried ? "" : retried.error().message);
                if (!retried) {
                    return;
                }
                (*scheduler)->complete(*job, nullptr);
                if (job->step == worldgen::pipeline_step::served) {
                    served = std::move(*retried);
                    break;
                }
            }
            expect(served != nullptr);
            if (served == nullptr) {
                return;
            }
            auto generator{worldgen::chunk_generator::create(
                generator_config(test_support::vanilla_data_root(), 128))};
            expect(generator.has_value());
            if (!generator) {
                return;
            }
            auto expected{(*generator)->generate_chunk(probe)};
            expect(expected.has_value());
            if (expected) {
                expect(chunk_digest(*served) == chunk_digest(**expected));
            }
        };

    "server/chunk scheduler serves the serial per-chunk pipeline content"_test =
        [] {
            if (!test_support::vanilla_data().available) {
                return;
            }
            auto scheduler{create_scheduler(1)};
            auto generator{worldgen::chunk_generator::create(
                generator_config(test_support::vanilla_data_root(), 128))};
            expect(scheduler.has_value());
            expect(generator.has_value());
            if (!scheduler || !generator) {
                return;
            }
            const std::vector<worldgen::chunk_pos> demands{
                {.x = 0, .z = 0},
                {.x = 1, .z = 0},
                {.x = 0, .z = 1},
                {.x = 2, .z = 2},
            };
            const auto results{run_scheduler(**scheduler, 1, demands)};
            expect(results.size() == demands.size());
            if (results.size() != demands.size()) {
                return;
            }
            for (const auto &position : demands) {
                auto expected{(*generator)->generate_chunk(position)};
                expect(expected.has_value());
                if (!expected) {
                    return;
                }
                const auto found{results.find({position.x, position.z})};
                expect(found != results.end());
                if (found == results.end()) {
                    return;
                }
                expect(found->second == chunk_digest(**expected))
                    << "chunk (" << position.x << ", " << position.z
                    << ") is not the serial pipeline chunk";
            }
        };

    "server/chunk scheduler serves the serial content with structures enabled"_test =
        [] {
            if (!test_support::vanilla_data().available) {
                return;
            }
            // The default structure sets exercise structure piece and
            // template state resolution, where a content state key that the
            // shared table does not hold would show up as a broken block.
            auto scheduler{worldgen::chunk_scheduler::create(
                worldgen::chunk_scheduler::config{
                    .generator = all_structures_config(
                        test_support::vanilla_data_root()),
                    .workers = 1,
                })};
            auto generator{worldgen::chunk_generator::create(
                all_structures_config(test_support::vanilla_data_root()))};
            expect(scheduler.has_value());
            expect(generator.has_value());
            if (!scheduler || !generator) {
                return;
            }
            const auto demands{make_positions(3, 3)};
            const auto results{run_scheduler(**scheduler, 1, demands)};
            expect(results.size() == demands.size());
            if (results.size() != demands.size()) {
                return;
            }
            for (const auto &position : demands) {
                auto expected{(*generator)->generate_chunk(position)};
                expect(expected.has_value());
                if (!expected) {
                    return;
                }
                const auto found{results.find({position.x, position.z})};
                expect(found != results.end());
                if (found == results.end()) {
                    continue;
                }
                expect(found->second == chunk_digest(**expected))
                    << "chunk (" << position.x << ", " << position.z
                    << ") is not the serial pipeline chunk";
            }
        };

    "server/chunk scheduler digest is worker-count independent"_test = [] {
        if (!test_support::vanilla_data().available) {
            return;
        }
        // One frozen block-state table, one shared cache and one ordered
        // decoration lane: the demand sequence fixes the served content, so
        // the worker count only changes how the terrain fills and the ordered
        // lane are spread over threads.
        const auto demands{make_positions(4, 4)};
        digest_map reference{};
        for (const std::size_t workers : {1U, 4U, 8U}) {
            auto scheduler{create_scheduler(workers)};
            expect(scheduler.has_value())
                << (scheduler ? "" : scheduler.error().message);
            if (!scheduler) {
                return;
            }
            const auto results{run_scheduler(**scheduler, workers, demands)};
            expect(results.size() == demands.size());
            if (results.size() != demands.size()) {
                return;
            }
            if (workers == 1U) {
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
                    << "chunk (" << position.first << ", " << position.second
                    << ") moved from one to " << workers << " workers";
            }
        }
    };

    "server/chunk scheduler digest repeats run to run"_test = [] {
        if (!test_support::vanilla_data().available) {
            return;
        }
        const auto demands{make_positions(4, 4)};
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
            if (found != second_results.end()) {
                expect(digest == found->second);
            }
        }
    };

    "server/chunk scheduler digest is worker-count independent for 64 chunks"_test =
        [] {
            if (!test_support::vanilla_data().available) {
                return;
            }
            const auto demands{make_positions(8, 8)};
            digest_map reference{};
            for (const std::size_t workers : {1U, 4U, 8U}) {
                auto scheduler{create_scheduler(workers)};
                expect(scheduler.has_value())
                    << (scheduler ? "" : scheduler.error().message);
                if (!scheduler) {
                    return;
                }
                const auto results{
                    run_scheduler(**scheduler, workers, demands)};
                expect(results.size() == demands.size());
                if (results.size() != demands.size()) {
                    return;
                }
                if (workers == 1U) {
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
                        << "chunk (" << position.first << ", "
                        << position.second << ") moved from one to " << workers
                        << " workers";
                }
            }
        };

    "server/chunk scheduler content ignores the configured tile edge"_test =
        [] {
            if (!test_support::vanilla_data().available) {
                return;
            }
            const auto demands{make_positions(3, 3)};
            digest_map reference{};
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

    "server/chunk scheduler bounds its terrain working set deterministically"_test =
        [] {
            if (!test_support::vanilla_data().available) {
                return;
            }
            // A capacity of 25 keeps one column's terrain window, so the
            // retention window drops terrain chunks as the ordered lane
            // advances. The retention rule is a function of the demand
            // sequence, so the served content stays identical across worker
            // counts at that small bound even though the terrain entries are
            // re-filled along the way.
            const auto demands{make_positions(4, 4)};
            digest_map reference{};
            for (const std::size_t workers : {1U, 4U, 8U}) {
                auto scheduler{create_scheduler(workers, 25)};
                expect(scheduler.has_value())
                    << (scheduler ? "" : scheduler.error().message);
                if (!scheduler) {
                    return;
                }
                const auto results{
                    run_scheduler(**scheduler, workers, demands)};
                expect(results.size() == demands.size());
                if (results.size() != demands.size()) {
                    return;
                }
                if (workers == 1U) {
                    reference = results;
                } else {
                    for (const auto &[position, digest] : results) {
                        const auto expected{reference.find(position)};
                        expect(expected != reference.end());
                        if (expected == reference.end()) {
                            continue;
                        }
                        expect(digest == expected->second)
                            << "a small terrain working set changed chunk ("
                            << position.first << ", " << position.second
                            << ") between one and " << workers << " workers";
                    }
                }
                const auto stats{(*scheduler)->stats()};
                expect(stats.cache_terrain_entries <= 25U)
                    << "the terrain working set grew to "
                    << stats.cache_terrain_entries
                    << " entries over a bound of 25";
                expect(stats.cache_evictions > 0U)
                    << "the small cache did not evict a terrain entry";
                // Unloading drops every cached chunk.
                (*scheduler)->clear();
                expect((*scheduler)->stats().cache_entries == 0U);
            }
        };
};
