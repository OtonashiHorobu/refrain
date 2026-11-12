// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Periodic maintenance above the tick thread: autosave snapshots, content GC,
// aux-store compaction and region compaction. The service owns the scheduler
// tasks and the two background workers they borrow, so the tick loop only
// calls collect_completed(), pre_tick() and post_tick().

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/core/schedule.hpp>
#include <refrain/server/server_runtime.hpp>
#include <refrain/sim/world.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace refrain::server {

/// Host maintenance tasks the runtime registers on the simulation thread.
///
/// One instance owns the background snapshot writer and, for an applying
/// content-GC policy, the delete worker. `start()` resolves the task set from
/// the effective options once; later ticks only run the scheduler grid.
class maintenance_service {
  public:
    maintenance_service();
    ~maintenance_service();

    maintenance_service(const maintenance_service &) = delete;
    auto operator=(const maintenance_service &)
        -> maintenance_service & = delete;

    /// Start the workers and register the enabled tasks. On failure the
    /// service still owns started workers and stops them at teardown.
    [[nodiscard]] auto start(refrain::sim::world &world,
                             const server_run_options &options) -> status;

    /// Hand the autosave batches the writer finished since the previous call
    /// back to the world.
    void collect_completed();

    void pre_tick(std::int64_t tick);
    void post_tick(std::int64_t tick);

    /// Wait until every submitted snapshot has been written. The console calls
    /// this before a synchronous flush so a stale snapshot cannot resurrect an
    /// older chunk revision.
    void drain();

    /// Stop the writer and the GC worker. Idempotent; call after the listener
    /// stopped and before the world is flushed.
    void stop() noexcept;

    [[nodiscard]] auto tasks() const -> std::vector<core::schedule::task_info>;

  private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace refrain::server
