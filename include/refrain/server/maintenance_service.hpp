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
#include <refrain/server/world_port.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::server {

/// Host maintenance tasks the runtime registers on the simulation thread.
///
/// One instance owns the background snapshot writer and, for an applying
/// content-GC policy, the delete worker. `start()` resolves the task set from
/// the effective options once; later ticks only run the scheduler grid.
/// What the background writer tells an operator about autosave health.
struct writer_status {
    /// Snapshots the writer has failed to persist in a row. Zero while the
    /// world is being saved.
    std::uint64_t consecutive_failures{0};
    /// Message of the most recent failure; empty after a success.
    std::string last_error{};
};

/// Decide when a repeated write failure deserves another log line.
///
/// A world that cannot be written fails on every autosave, and one line per
/// autosave buries the first one. The first failure is reported, then only a
/// changed message or every `report_every` consecutive failure, so a persistent
/// failure keeps surfacing without flooding the log.
class write_failure_tracker {
  public:
    explicit write_failure_tracker(std::uint64_t report_every = 12)
        : report_every_{report_every == 0 ? 1 : report_every} {}

    /// Records a failure and returns whether the operator should hear about it.
    auto record(std::string_view message) -> bool {
        ++consecutive_;
        const bool changed{message != last_error_};
        last_error_.assign(message);
        if (consecutive_ == 1) {
            return true;
        }
        return changed || consecutive_ % report_every_ == 0;
    }

    void record_success() noexcept {
        consecutive_ = 0;
        last_error_.clear();
    }

    [[nodiscard]] auto consecutive() const noexcept -> std::uint64_t {
        return consecutive_;
    }
    [[nodiscard]] auto last_error() const noexcept -> const std::string & {
        return last_error_;
    }

  private:
    std::uint64_t report_every_{12};
    std::uint64_t consecutive_{0};
    std::string last_error_{};
};

class maintenance_service {
  public:
    maintenance_service();
    ~maintenance_service();

    maintenance_service(const maintenance_service &) = delete;
    auto operator=(const maintenance_service &)
        -> maintenance_service & = delete;

    /// Start the workers and register the enabled tasks. On failure the
    /// service still owns started workers and stops them at teardown.
    [[nodiscard]] auto start(world_port &world,
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

    /// Autosave health: how many snapshots the writer has failed in a row and
    /// why. The console reports it so a world that stopped saving is visible
    /// without reading the log.
    [[nodiscard]] auto writer_status() const -> writer_status;

  private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace refrain::server
