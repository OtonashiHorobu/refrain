// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Simulation tick cost at scale (Google Benchmark 1.10.0).
//
// One `world::tick()` walks every player record and every runtime entity in
// ascending logical id order, so the cost of a storage change is the cost of
// one tick over a known population. These benchmarks report the median and the
// 99th percentile of a tick at 1k and 10k entities and at 1k players: a mean
// hides a scheduling spike, and the tick budget is judged by its p99.
//
// Run with:
//   ./build/Release/bin/refrain-bench --benchmark_filter=world_tick
//
// A number from a local iteration build (LTO off, baseline architecture) is
// indicative only; benchmark-role numbers come from the benchmark host.

#include <refrain/sim/player.hpp>
#include <refrain/sim/world.hpp>
#include <refrain/world/create.hpp>
#include <refrain/world/store.hpp>

#include <benchmark/benchmark.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace sim = refrain::sim;
namespace world = refrain::world;

[[nodiscard]] auto scratch_directory() -> std::filesystem::path {
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    auto path{std::filesystem::temp_directory_path() /
              ("refrain-bench-tick-" + std::to_string(stamp))};
    std::error_code error;
    std::filesystem::create_directories(path, error);
    return path;
}

/// One opened synthetic world plus the temporary directory that backs it. The
/// fixture owns both; the timing loop only ticks.
struct tick_world {
    std::filesystem::path directory{scratch_directory()};
    std::optional<sim::world> opened{};

    tick_world(std::size_t entities, std::size_t players) {
        static_cast<void>(world::create_world(directory.string(), 42, "world"));
        auto created{sim::world::open(directory.string())};
        if (!created) {
            return;
        }
        opened.emplace(std::move(*created));
        for (std::size_t index{0}; index < players; ++index) {
            static_cast<void>(
                opened->add_player(sim::player_table::registration{
                    .connection = index + 1,
                    .name = "bench-" + std::to_string(index),
                    .world = "overworld",
                    .x = static_cast<double>(index),
                    .y = 64.0,
                    .z = static_cast<double>(index) * 2.0,
                    .game_mode = 0,
                    .inventory_slots = 36,
                }));
        }
        for (std::size_t index{0}; index < entities; ++index) {
            static_cast<void>(opened->spawn_entity(
                "minecraft:pig", static_cast<double>(index) * 0.25, 64.0,
                static_cast<double>(index) * 0.5));
        }
    }

    ~tick_world() {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }

    tick_world(const tick_world &) = delete;
    auto operator=(const tick_world &) -> tick_world & = delete;
    tick_world(tick_world &&) = delete;
    auto operator=(tick_world &&) -> tick_world & = delete;
};

/// Nearest-rank percentile of an already sorted sample set.
[[nodiscard]] auto percentile(const std::vector<double> &sorted, double part)
    -> double {
    if (sorted.empty()) {
        return 0.0;
    }
    const auto ratio{std::clamp(part, 0.0, 1.0)};
    const auto index{static_cast<std::size_t>(
        ratio * static_cast<double>(sorted.size() - 1))};
    return sorted[index];
}

/// Tick `bench.opened` once per iteration and report the tick distribution.
void run_tick_benchmark(benchmark::State &state, tick_world &bench) {
    if (!bench.opened.has_value()) {
        state.SkipWithError("the benchmark world did not open");
        return;
    }
    using clock = std::chrono::steady_clock;
    std::vector<double> samples{};
    const auto reserve{std::min<std::int64_t>(state.max_iterations, 1 << 20)};
    samples.reserve(static_cast<std::size_t>(reserve));
    for (auto _ : state) {
        const auto start{clock::now()};
        bench.opened->tick();
        const auto stop{clock::now()};
        samples.push_back(
            std::chrono::duration<double, std::micro>(stop - start).count());
    }
    std::sort(samples.begin(), samples.end());
    const auto p50{percentile(samples, 0.50)};
    const auto p99{percentile(samples, 0.99)};
    const auto entities{
        static_cast<double>(state.range(0) < 0 ? 0 : state.range(0))};
    state.counters["p50_us"] = p50;
    state.counters["p99_us"] = p99;
    state.counters["samples"] = static_cast<double>(samples.size());
    if (entities > 0.0) {
        state.counters["p50_ns_entity"] = p50 * 1000.0 / entities;
        state.counters["p99_ns_entity"] = p99 * 1000.0 / entities;
    }
    state.SetItemsProcessed(state.iterations() *
                            static_cast<std::int64_t>(entities));
}

/// `Arg(0)` is the runtime entity count.
void bench_world_tick_entities(benchmark::State &state) {
    const auto entities{
        static_cast<std::size_t>(state.range(0) < 0 ? 0 : state.range(0))};
    tick_world bench{entities, 0};
    state.SetLabel(std::to_string(entities) + " entities");
    run_tick_benchmark(state, bench);
}
BENCHMARK(bench_world_tick_entities)
    ->Arg(1000)
    ->Arg(10000)
    ->MinTime(3.0)
    ->Unit(benchmark::kMicrosecond);

/// `Arg(0)` is the player record count. A player tick runs the movement and
/// survival systems, so it is measured separately from the point-entity tick.
void bench_world_tick_players(benchmark::State &state) {
    const auto players{
        static_cast<std::size_t>(state.range(0) < 0 ? 0 : state.range(0))};
    tick_world bench{0, players};
    state.SetLabel(std::to_string(players) + " players");
    run_tick_benchmark(state, bench);
}
BENCHMARK(bench_world_tick_players)
    ->Arg(1000)
    ->MinTime(3.0)
    ->Unit(benchmark::kMicrosecond);

} // namespace
