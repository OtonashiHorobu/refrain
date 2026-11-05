// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/platform/thread.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace refrain::core::jobs {

/// Scheduling priority: lower value wins.
enum class priority : std::uint8_t {
    interactive = 0,    ///< player interaction, teleports
    player_visible = 1, ///< chunk load/generate inside view distance
    normal = 2,         ///< autosave, background light
    background = 3,     ///< compaction, export, cache warming
};

using job_id = std::uint64_t;

using task = std::function<void()>;

/// Per-worker scheduling hints applied once at thread startup.
struct worker_options {
    /// Logical CPUs for the worker pool (empty = OS default).
    std::vector<std::uint32_t> affinity{};
    /// Relative scheduling class (normal = leave the OS default).
    platform::thread_priority priority{platform::thread_priority::normal};
};

/// Test/embedding seam for the constructor's thread-creation failure path.
/// Production callers leave it empty (the default `std::thread` is used); a
/// test factory can throw after N creations to exercise rollback.
using thread_factory = std::function<std::thread(std::function<void()>)>;

struct stats {
    std::uint64_t submitted{0};
    /// Tasks that finished, whether successfully or by throwing (a thrown task
    /// is counted in both `completed` and `failed`).
    std::uint64_t completed{0};
    /// Tasks whose callable threw; the worker caught the exception and stayed
    /// alive (a throwing task never terminates the process).
    std::uint64_t failed{0};
    std::uint64_t queued{0};
    std::uint64_t active{0};
    std::size_t workers{0};
};

/// Fixed-size worker pool with a priority queue.
///
/// Single sim thread plus workers: workers never write sim state directly;
/// results are posted back to stage queues by the task itself. Tasks may
/// throw: the exception is caught at the worker boundary (counted in
/// `stats::failed`) and never terminates the process.
/// `parallel_for` called from inside a worker degrades to inline serial
/// execution instead of deadlocking.
class executor {
  public:
    /// `worker_count = 0` means auto: max(1, hardware_concurrency - 2).
    ///
    /// Thread creation failures (`std::system_error`) are propagated to the
    /// caller after rolling back every thread already started: the executor
    /// object is never left with joinable threads.
    explicit executor(std::size_t worker_count = 0, thread_factory factory = {},
                      worker_options options = {});

    ~executor();

    executor(const executor &) = delete;
    executor &operator=(const executor &) = delete;

    [[nodiscard]] auto submit(priority prio, task fn) -> job_id;

    /// Fire-and-forget helper for the common "no result, normal priority" case.
    void submit(task fn) { (void)submit(priority::normal, std::move(fn)); }

    /// Split [0, count) into tasks of at least `grain` items and run them on
    /// the pool. Blocks until all tasks complete. When invoked from a worker
    /// task it runs inline (serially) instead of deadlocking; callers that need
    /// real parallelism must call it from the sim/owner thread.
    void parallel_for(std::size_t count, std::size_t grain,
                      const std::function<void(std::size_t)> &fn);

    /// Block until the queue is empty and all tasks finished. Bootstrap /
    /// shutdown / tests only; never called from the tick loop.
    void wait_idle();

    /// Drain the queue (queued tasks are dropped) and join workers.
    void shutdown();

    [[nodiscard]] auto worker_count() const noexcept -> std::size_t;

    /// Snapshot of the pool counters. Not `noexcept`: it takes the pool mutex,
    /// whose lock may throw `std::system_error`.
    [[nodiscard]] auto stats_snapshot() const -> stats;

    [[nodiscard]] static auto default_worker_count() noexcept -> std::size_t;

  private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace refrain::core::jobs
