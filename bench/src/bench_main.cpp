// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Kernel primitive benchmarks (Google Benchmark 1.9.5).
//
// Covers the job system and the arena allocator. Run with:
//   ./build/Release/bin/refrain-bench --benchmark_min_time=0.2s

#include <refrain/core/jobs.hpp>
#include <refrain/core/memory.hpp>

#include <benchmark/benchmark.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>

namespace {

/// Lazily constructed: a namespace-scope executor starts its worker threads
/// during static initialization, before the log registry's globals are
/// guaranteed constructed. A function-local static starts the pool on first
/// benchmark use (after main), avoiding static init order UB.
[[nodiscard]] auto parallel_pool() -> refrain::core::jobs::executor & {
    static refrain::core::jobs::executor pool{4};
    return pool;
}

void bench_allocator_default_resource(benchmark::State &state) {
    auto *resource = refrain::core::memory::default_resource();
    const auto size = static_cast<std::size_t>(state.range(0));
    state.SetLabel(std::string{"backend:"} +
                   std::string{refrain::core::memory::to_string(
                       refrain::core::memory::compiled_backend())});
    for (auto _ : state) {
        for (int i = 0; i < 64; ++i) {
            void *pointer = resource->allocate(size, 8);
            benchmark::DoNotOptimize(pointer);
            resource->deallocate(pointer, size, 8);
        }
    }
    state.SetItemsProcessed(state.iterations() * 64);
}
BENCHMARK(bench_allocator_default_resource)->Arg(64)->Arg(512)->Arg(4096);

void bench_arena_alloc_reset(benchmark::State &state) {
    const auto allocations = static_cast<std::size_t>(state.range(0));
    for (auto _ : state) {
        refrain::core::arena arena{};
        for (std::size_t i = 0; i < allocations; ++i) {
            auto block = arena.allocate(64);
            if (!block) {
                // Google Benchmark requires returning immediately after
                // SkipWithError(); a `break` here would keep iterating the
                // outer `for (auto _ : state)` loop and reuse the state.
                state.SkipWithError("arena allocation failed");
                return;
            }
            benchmark::DoNotOptimize(block->data());
            if (i % 1024 == 1023) {
                arena.reset();
            }
        }
        benchmark::DoNotOptimize(arena.used_bytes());
    }
    state.SetItemsProcessed(state.iterations() *
                            static_cast<std::int64_t>(allocations));
}
BENCHMARK(bench_arena_alloc_reset)->Arg(4096);

// What this measures: each `parallel_for` call submits `count / grain` chunks
// and then *blocks* on the pool until all of them ran, so the reported time is
// end-to-end throughput (submit + schedule + execute + join), not pure
// scheduler dispatch latency. It answers "how expensive is one parallel_for
// round trip", which is what the kernel hot paths call. A
// dispatch-only figure would need a separate submit/wait-split benchmark once
// jobs exposes non-blocking submission.
void bench_jobs_parallel_for(benchmark::State &state) {
    const auto count = static_cast<std::size_t>(state.range(0));
    state.SetLabel("submit+execute+join (blocking round trip)");
    for (auto _ : state) {
        std::atomic<std::uint64_t> sum{0};
        parallel_pool().parallel_for(count, 64, [&sum](std::size_t index) {
            sum.fetch_add(index + 1, std::memory_order_relaxed);
        });
        benchmark::DoNotOptimize(sum.load(std::memory_order_relaxed));
    }
    state.SetItemsProcessed(state.iterations() *
                            static_cast<std::int64_t>(count));
}
BENCHMARK(bench_jobs_parallel_for)->Arg(2048);

} // namespace

// This file provides only benchmark definitions; the `main()` comes from
// benchmark::benchmark_main (linked by CMake). Defining BENCHMARK_MAIN() here
// too would risk a duplicate main under LTO or link-order changes.
