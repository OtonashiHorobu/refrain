// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Host-owned periodic task scheduler.
//
// Owner-thread only: no locks, no timers, no threads of its own. Ordering is
// exactly the registration order, so determinism is a function of `add()`
// order plus the tick value.
//
// Time model: a fixed grid anchored at tick 0. A task with interval N is due
// at N, 2N, 3N, ... . When the tick jumps past several slots the task is
// invoked once and the skipped slots are only counted in `coalesced`; missed
// work is never replayed in a burst.

#include <refrain/core/schedule.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::core::schedule {

namespace {

/// `task_stats::last_error` keeps diagnostics readable: only the first 256
/// bytes of the provider message are retained.
constexpr auto max_error_bytes{std::size_t{256}};

void assign_truncated(std::string &target, std::string_view message) {
    target.assign(message.substr(0, max_error_bytes));
}

} // namespace

auto phase_name(phase value) noexcept -> std::string_view {
    switch (value) {
    case phase::pre_tick:
        return "pre_tick";
    case phase::post_tick:
        return "post_tick";
    }
    return "unknown";
}

auto scheduler::add(std::span<const task_descriptor> tasks, void *self,
                    const void *vtable) -> status {
    // `self` is the provider/context every invoker receives; a null context
    // would only be discovered by the first invocation, so reject it here.
    // `vtable` is allowed to be null for host-owned maintenance tasks.
    if (self == nullptr) {
        return std::unexpected{
            make_error(error_code::invalid_argument, "schedule: null self")};
    }
    if (tasks.empty()) {
        return status_ok();
    }

    // Validate the whole batch before touching entries_: a rejected add must
    // leave the scheduler exactly as it was. Duplicates are checked against
    // both the registered tasks and the earlier descriptors of this batch.
    for (std::size_t index{}; index < tasks.size(); ++index) {
        const auto &task{tasks[index]};
        if (task.service.empty() || task.method.empty()) {
            return std::unexpected{make_error(
                error_code::invalid_argument,
                "schedule: service and method names must not be empty")};
        }
        if (task.invoke == nullptr) {
            return std::unexpected{make_error(
                error_code::invalid_argument,
                fmt::format("schedule: task {}.{} has a null invoker",
                            task.service, task.method))};
        }
        if (task.interval_ticks < min_interval_ticks ||
            task.interval_ticks > max_interval_ticks) {
            return std::unexpected{make_error(
                error_code::invalid_argument,
                fmt::format(
                    "schedule: task {}.{} interval {} is outside [{}, {}]",
                    task.service, task.method, task.interval_ticks,
                    min_interval_ticks, max_interval_ticks))};
        }
        for (const auto &existing : entries_) {
            if (std::string_view{existing.service} == task.service &&
                std::string_view{existing.method} == task.method) {
                return std::unexpected{
                    make_error(error_code::invalid_argument,
                               fmt::format("schedule: duplicate task {}.{}",
                                           task.service, task.method))};
            }
        }
        for (std::size_t prior{}; prior < index; ++prior) {
            if (tasks[prior].service == task.service &&
                tasks[prior].method == task.method) {
                return std::unexpected{
                    make_error(error_code::invalid_argument,
                               fmt::format("schedule: duplicate task {}.{}",
                                           task.service, task.method))};
            }
        }
    }

    // Stage the copies first, then reserve the live vector once. Moving the
    // staged entries into `entries_` afterwards is noexcept (capacity is
    // reserved and entry's move is a noexcept string move), so a partially
    // copied batch can never become visible.
    std::vector<entry> staged{};
    staged.reserve(tasks.size());
    for (const auto &task : tasks) {
        staged.push_back(entry{
            .service = std::string{task.service},
            .method = std::string{task.method},
            .interval_ticks = task.interval_ticks,
            .phase_of = task.phase_of,
            .invoke = task.invoke,
            .self = self,
            .vtable = vtable,
            .next_due = task.interval_ticks,
            .enabled = true,
            .stats = task_stats{},
        });
    }
    entries_.reserve(entries_.size() + staged.size());
    for (auto &staged_entry : staged) {
        entries_.push_back(std::move(staged_entry));
    }
    return status_ok();
}

auto scheduler::remove_service(std::string_view service) -> std::size_t {
    // Own the name for the duration: callers may pass a view borrowed from
    // tasks() (i.e. into an entry that this call is about to erase).
    const std::string name{service};
    return std::erase_if(entries_, [&name](const entry &current) {
        return current.service == name;
    });
}

auto scheduler::set_enabled(std::string_view service, std::string_view method,
                            bool enabled) -> bool {
    auto *target{find(service, method)};
    if (target == nullptr) {
        return false;
    }
    target->enabled = enabled;
    return true;
}

auto scheduler::run(phase phase_of, std::int64_t tick) -> std::size_t {
    // Negative ticks are not a valid boundary; ignore them instead of
    // rewinding the grid or the last-tick marker.
    if (tick < 0) {
        return 0;
    }
    // `last_tick()` is the last tick handed to run(), even when no task is
    // due in this phase (the tick loop still advanced).
    last_tick_ = tick;

    auto invoked{std::size_t{}};
    for (auto &current : entries_) {
        if (!current.enabled || current.phase_of != phase_of ||
            tick < current.next_due) {
            continue;
        }
        ++invoked;
        const auto outcome{current.invoke(current.self, current.vtable, tick)};
        // Catch-up policy is coalescing: advance over every grid slot the tick
        // passed and run once, never once per missed slot.
        const auto missed{(tick - current.next_due) / current.interval_ticks +
                          1};
        current.next_due += missed * current.interval_ticks;
        ++current.stats.runs;
        current.stats.coalesced += missed - 1;
        current.stats.last_tick = tick;
        if (!outcome.has_value()) {
            ++current.stats.failures;
            assign_truncated(current.stats.last_error, outcome.error().message);
        }
    }
    return invoked;
}

auto scheduler::due(phase target, std::int64_t tick) const noexcept -> bool {
    for (const auto &current : entries_) {
        if (current.enabled && current.phase_of == target &&
            tick >= current.next_due) {
            return true;
        }
    }
    return false;
}

auto scheduler::empty() const noexcept -> bool { return entries_.empty(); }

auto scheduler::size() const noexcept -> std::size_t { return entries_.size(); }

auto scheduler::tasks() const -> std::vector<task_info> {
    auto snapshot{std::vector<task_info>{}};
    snapshot.reserve(entries_.size());
    for (const auto &current : entries_) {
        snapshot.push_back(task_info{
            .service = current.service,
            .method = current.method,
            .interval_ticks = current.interval_ticks,
            .phase_of = current.phase_of,
            .enabled = current.enabled,
            .stats = current.stats,
        });
    }
    return snapshot;
}

auto scheduler::last_tick() const noexcept -> std::int64_t {
    return last_tick_;
}

void scheduler::clear() noexcept {
    entries_.clear();
    last_tick_ = -1;
}

auto scheduler::find(std::string_view service, std::string_view method) noexcept
    -> entry * {
    for (auto &current : entries_) {
        if (std::string_view{current.service} == service &&
            std::string_view{current.method} == method) {
            return &current;
        }
    }
    return nullptr;
}

} // namespace refrain::core::schedule
