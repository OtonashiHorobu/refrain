// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Host-owned periodic task scheduler. RIDL `#[schedule]` generates
// the descriptor table and per-method trampolines; the host composes them with
// a provider instance here. The scheduler is deliberately small and
// single-threaded: the tick thread owns it, ordering is registration order, and
// there are no timers/threads of its own.
//
// Time model: a fixed grid anchored at tick 0. A task with interval N is due at
// ticks N, 2N, 3N, ... (the same semantics as `ticks_run % N == 0`). When the
// tick jumps past several grid slots the task runs at most once and reports the
// coalesced slots; missed slots are never replayed in a burst.

#pragma once

#include <refrain/core/error.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::core::schedule {

/// Tick boundary a task runs on. `pre_tick` runs before the world tick,
/// `post_tick` after it (autosave/GC style work belongs to `post_tick`).
enum class phase : std::uint8_t {
    pre_tick = 0,
    post_tick = 1,
};

[[nodiscard]] auto phase_name(phase value) noexcept -> std::string_view;

/// Interval bounds shared with the schema validator (`#[schedule(interval=N)]`)
/// so a schema-legal value is never rejected at registration time.
inline constexpr std::int64_t min_interval_ticks = 1;
inline constexpr std::int64_t max_interval_ticks = 1'000'000;

/// Type-erased invoker generated per scheduled service method.
///
/// `self`/`vtable` are the provider instance the host registered; `tick` is the
/// tick the task is running on. `noexcept`: a provider failure is reported by
/// the returned status, never by an exception.
using invoker = status (*)(void *self, const void *vtable,
                           std::int64_t tick) noexcept;

/// One scheduled method. The generator emits
/// `std::array<task_descriptor, N> schedule_tasks` per service module.
struct task_descriptor {
    std::string_view service{};
    std::string_view method{};
    std::int64_t interval_ticks{min_interval_ticks};
    phase phase_of{phase::post_tick};
    invoker invoke{nullptr};
};

/// Per-task counters. `last_error` keeps the last failure message (bounded to
/// 256 bytes) for console/diagnostics; it is never part of world state.
struct task_stats {
    std::int64_t runs{0};
    std::int64_t failures{0};
    /// Grid slots skipped because the tick jumped past them (catch-up policy is
    /// coalescing, not replaying).
    std::int64_t coalesced{0};
    std::int64_t last_tick{-1};
    std::string last_error;
};

/// Read-only view of one registered task, as returned by `scheduler::tasks()`.
struct task_info {
    std::string_view service{};
    std::string_view method{};
    std::int64_t interval_ticks{min_interval_ticks};
    phase phase_of{phase::post_tick};
    bool enabled{true};
    task_stats stats{};
};

/// Periodic task scheduler for one owner thread (the tick thread).
///
/// Not thread-safe by design (same contract as `core::events::bus`): register
/// and run from the tick thread, or quiesce before handing it to another one.
/// Task order is registration order; the host must register services in a fixed
/// order to keep runs deterministic.
class scheduler {
  public:
    scheduler() = default;
    scheduler(const scheduler &) = delete;
    auto operator=(const scheduler &) -> scheduler & = delete;
    scheduler(scheduler &&) = delete;
    auto operator=(scheduler &&) -> scheduler & = delete;
    ~scheduler() = default;

    /// Register every task of one owner instance: the provider `self` and the
    /// provider `vtable` for generated service tasks, or a host-owned context
    /// with a null `vtable` for host maintenance tasks. Rejects a null
    /// `invoke`, an empty `self`, out-of-range intervals and duplicate
    /// `(service, method)` pairs.
    ///
    /// Failure leaves the scheduler unchanged (all-or-nothing).
    [[nodiscard]] auto add(std::span<const task_descriptor> tasks, void *self,
                           const void *vtable) -> status;

    /// Drop every task of a service. Returns the number removed.
    auto remove_service(std::string_view service) -> std::size_t;

    /// Enable/disable one task without unregistering it. Disabled tasks keep
    /// their grid position: re-enabling does not replay missed slots.
    /// Returns false when the task is unknown.
    auto set_enabled(std::string_view service, std::string_view method,
                     bool enabled) -> bool;

    /// Run every enabled task of `phase` that is due at `tick`, in registration
    /// order. A failing task is counted and skipped; the others still run.
    /// Returns the number of tasks invoked (failures included).
    auto run(phase phase_of, std::int64_t tick) -> std::size_t;

    /// True when at least one enabled task of `target` is due at `tick`.
    [[nodiscard]] auto due(phase target, std::int64_t tick) const noexcept
        -> bool;

    [[nodiscard]] auto empty() const noexcept -> bool;
    [[nodiscard]] auto size() const noexcept -> std::size_t;

    /// Stats snapshot in registration order (copies; for console/tests).
    [[nodiscard]] auto tasks() const -> std::vector<task_info>;

    /// Last tick passed to `run`; -1 before the first call.
    [[nodiscard]] auto last_tick() const noexcept -> std::int64_t;

    void clear() noexcept;

  private:
    struct entry {
        std::string service{};
        std::string method{};
        std::int64_t interval_ticks{min_interval_ticks};
        phase phase_of{phase::post_tick};
        invoker invoke{nullptr};
        void *self{nullptr};
        const void *vtable{nullptr};
        std::int64_t next_due{min_interval_ticks};
        bool enabled{true};
        task_stats stats{};
    };

    [[nodiscard]] auto find(std::string_view service,
                            std::string_view method) noexcept -> entry *;

    std::vector<entry> entries_{};
    std::int64_t last_tick_{-1};
};

} // namespace refrain::core::schedule
