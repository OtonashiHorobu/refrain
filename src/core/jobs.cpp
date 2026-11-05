// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/jobs.hpp>

#include <refrain/core/log.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>
#include <vector>

namespace refrain::core::jobs {
namespace {

/// Set on a thread while it is executing a pool worker loop. Used to detect
/// `parallel_for` re-entrancy from a worker (submitting chunks and then waiting
/// for them would deadlock when every worker is blocked in the same wait).
thread_local const void *g_worker_pool{nullptr};

/// Number of chunks for `count` items with the given (non-zero) grain without
/// `count + grain - 1` size_t wraparound.
[[nodiscard]] constexpr auto chunk_count(std::size_t count,
                                         std::size_t grain) noexcept
    -> std::size_t {
    return count / grain + (count % grain != 0 ? 1 : 0);
}

} // namespace

struct executor::impl {
    explicit impl(std::size_t count, thread_factory factory,
                  worker_options worker_hints)
        : worker_count{count}, make_thread{std::move(factory)},
          options{std::move(worker_hints)} {}

    struct entry {
        priority prio{priority::normal};
        job_id id{0};
        task fn{};
    };

    /// std::priority_queue is a max-heap: invert the ordering so that lower
    /// priority values surface first, and equal priorities stay FIFO by id.
    struct entry_order {
        auto operator()(const entry &lhs, const entry &rhs) const noexcept
            -> bool {
            const auto lhs_prio{static_cast<std::uint8_t>(lhs.prio)};
            const auto rhs_prio{static_cast<std::uint8_t>(rhs.prio)};
            if (lhs_prio != rhs_prio) {
                return lhs_prio > rhs_prio;
            }
            return lhs.id > rhs.id;
        }
    };

    /// Completion bookkeeping for one in-flight `parallel_for` call. Shared
    /// with the chunk lambdas, and registered in `parallel_waiters` so
    /// `shutdown()` can wake the caller when queued chunks are dropped.
    struct completion_state {
        std::mutex mutex{};
        std::condition_variable cv{};
        std::size_t remaining{0};
    };

    /// Worker main loop: block on the queue while idle, run the best-priority
    /// task, then update counters. Returns only after shutdown(). Task
    /// exceptions are caught here so a buggy/misbehaving task can never call
    /// std::terminate or corrupt the queue/idle bookkeeping.
    void worker_loop() {
        g_worker_pool = this;
        for (;;) {
            entry job{};
            {
                std::unique_lock lock{mutex};
                work_cv.wait(lock,
                             [this] { return stopping || !queue.empty(); });
                if (queue.empty()) {
                    g_worker_pool = nullptr;
                    return;
                }
                // priority_queue::top() returns a const reference; the element
                // is not const, so stripping the const to move the task out is
                // safe.
                job = std::move(const_cast<entry &>(queue.top()));
                queue.pop();
                queued.fetch_sub(1, std::memory_order_relaxed);
                active.fetch_add(1, std::memory_order_relaxed);
            }

            if (job.fn) {
                try {
                    job.fn();
                } catch (...) {
                    // A task exception is contained at the worker boundary:
                    // record it and keep the worker (and counters) healthy.
                    failed.fetch_add(1, std::memory_order_relaxed);
                }
            }

            {
                std::lock_guard lock{mutex};
                active.fetch_sub(1, std::memory_order_relaxed);
                completed.fetch_add(1, std::memory_order_relaxed);
                if (!stopping && queue.empty() &&
                    active.load(std::memory_order_relaxed) == 0) {
                    idle_cv.notify_all();
                }
            }
        }
    }

    /// Drop every queued (not yet started) task. Requires `mutex` to be held.
    ///
    /// The dropped tasks' completion lambdas never run, so a `parallel_for`
    /// waiting on them would block forever; see `shutdown()`.
    void drop_queued_locked() {
        while (!queue.empty()) {
            queue.pop();
        }
        queued.store(0, std::memory_order_relaxed);
    }

    std::vector<std::thread> workers{};
    std::size_t worker_count{0};
    thread_factory make_thread{};
    worker_options options{};

    mutable std::mutex mutex{};
    std::condition_variable work_cv{};
    std::condition_variable idle_cv{};
    std::priority_queue<entry, std::vector<entry>, entry_order> queue{};

    std::atomic<std::uint64_t> submitted{0};
    std::atomic<std::uint64_t> completed{0};
    std::atomic<std::uint64_t> failed{0};
    std::atomic<std::uint64_t> queued{0};
    std::atomic<std::uint64_t> active{0};
    std::atomic<job_id> next_id{1};

    /// Waiters currently blocked inside `parallel_for`, guarded by `mutex`.
    /// `shutdown()` wakes every live entry because the completion lambdas of
    /// dropped chunks are gone (weak_ptr: a caller that already returned leaves
    /// no state behind).
    std::vector<std::weak_ptr<completion_state>> parallel_waiters{};

    /// Set by `shutdown()` *after* every worker thread has been joined.
    /// `parallel_for` uses it as the second half of its wait predicate: once
    /// this is true no worker can still be executing a chunk, so returning is
    /// safe even though dropped chunks never complete (see `parallel_for`).
    std::atomic<bool> shutdown_complete{false};

    bool stopping{false};
};

executor::executor(std::size_t worker_count, thread_factory factory,
                   worker_options options)
    : impl_{std::make_unique<impl>(worker_count == 0 ? default_worker_count()
                                                     : worker_count,
                                   std::move(factory), std::move(options))} {
    impl *state{impl_.get()};
    try {
        state->workers.reserve(state->worker_count);
        for (std::size_t index{0}; index < state->worker_count; ++index) {
            std::function<void()> body{[state, index] {
                // Cold path: keep the name in a stack buffer so thread startup
                // cannot fail with an allocation before the worker loop runs.
                char name[16]{};
                const auto written{fmt::format_to_n(name, sizeof(name) - 1,
                                                    "worker-{}", index)};
                log::set_thread_name(std::string_view{
                    name, static_cast<std::size_t>(written.out - name)});
                if (!state->options.affinity.empty()) {
                    static_cast<void>(platform::set_current_thread_affinity(
                        state->options.affinity));
                }
                if (state->options.priority !=
                    platform::thread_priority::normal) {
                    static_cast<void>(platform::set_current_thread_priority(
                        state->options.priority));
                }
                state->worker_loop();
            }};
            if (state->make_thread) {
                state->workers.push_back(state->make_thread(std::move(body)));
            } else {
                state->workers.emplace_back(std::move(body));
            }
        }
    } catch (...) {
        // Partial startup: tell the already-running workers to exit and join
        // them before propagating. Without this rollback the joinable threads
        // would be destroyed by ~impl and std::terminate would be called
        // instead of reporting the allocation/resource failure to the caller.
        {
            std::lock_guard lock{state->mutex};
            state->stopping = true;
        }
        state->work_cv.notify_all();
        for (auto &worker : state->workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        state->workers.clear();
        throw;
    }
}

executor::~executor() { shutdown(); }

auto executor::submit(priority prio, task fn) -> job_id {
    auto &state{*impl_};
    job_id id{0};
    {
        std::lock_guard lock{state.mutex};
        if (state.stopping) {
            // Pool is shutting down: the task is dropped instead of executed.
            // Count the attempt so submitted stays monotonic, and return 0 (no
            // job id was assigned) so callers can detect the drop.
            state.submitted.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        id = state.next_id.fetch_add(1, std::memory_order_relaxed);
        state.queue.push(impl::entry{prio, id, std::move(fn)});
        state.queued.fetch_add(1, std::memory_order_relaxed);
        state.submitted.fetch_add(1, std::memory_order_relaxed);
    }
    state.work_cv.notify_one();
    return id;
}

void executor::parallel_for(std::size_t count, std::size_t grain,
                            const std::function<void(std::size_t)> &fn) {
    if (count == 0) {
        return;
    }
    if (grain == 0) {
        grain = 1;
    }

    // Re-entrant call from a worker: never submit-and-block (that deadlocks
    // once every worker is parked in the same wait). Run the range inline.
    if (g_worker_pool == impl_.get()) {
        for (std::size_t index{0}; index < count; ++index) {
            fn(index);
        }
        return;
    }

    const auto chunks{chunk_count(count, grain)};
    auto state{std::make_shared<impl::completion_state>()};
    state->remaining = chunks;

    // Register before the first chunk is queued so shutdown() can find this
    // waiter even when it races with the submissions below.
    {
        std::lock_guard lock{impl_->mutex};
        impl_->parallel_waiters.push_back(state);
    }

    // Remove this call's entry from the waiter list; used on every exit path so
    // the list cannot accumulate expired weak_ptrs.
    const auto unregister_waiter{[this, &state] {
        std::lock_guard lock{impl_->mutex};
        auto &waiters{impl_->parallel_waiters};
        waiters.erase(
            std::remove_if(
                waiters.begin(), waiters.end(),
                [&state](const std::weak_ptr<impl::completion_state> &waiter) {
                    const auto current{waiter.lock()};
                    return current == nullptr || current == state;
                }),
            waiters.end());
    }};

    // Wait until every chunk that may still run has counted itself done.
    //
    // shutdown() drops queued chunks and their completion lambdas never run, so
    // `remaining` can stay above zero forever. The predicate therefore also
    // accepts `shutdown_complete`, which shutdown() publishes only after it has
    // joined every worker: at that point no chunk can still be inside `fn`, so
    // the caller may safely destroy its callable. Waiting for "shutdown
    // started" instead would be wrong - a chunk that was already running would
    // still hold `&fn` when this returns. Dropped chunks are *not* counted as
    // completed: the work is lost, which is shutdown()'s documented behaviour.
    const auto await_chunks{[this, &state] {
        std::unique_lock lock{state->mutex};
        state->cv.wait(lock, [this, &state] {
            return state->remaining == 0 ||
                   impl_->shutdown_complete.load(std::memory_order_acquire);
        });
    }};

    std::size_t chunk{0};
    try {
        for (; chunk < chunks; ++chunk) {
            const auto begin{chunk * grain};
            const auto end{std::min(begin + grain, count)};
            const auto id{submit(priority::normal, [state, &fn, begin, end] {
                try {
                    for (auto index{begin}; index < end; ++index) {
                        fn(index);
                    }
                } catch (...) {
                    // Count the chunk as complete before rethrowing so the
                    // parallel_for caller is never left waiting on a chunk
                    // that threw; the worker boundary records the failure.
                    std::lock_guard lock{state->mutex};
                    if (state->remaining > 0) {
                        --state->remaining;
                        if (state->remaining == 0) {
                            state->cv.notify_all();
                        }
                    }
                    throw;
                }
                std::lock_guard lock{state->mutex};
                if (state->remaining > 0) {
                    --state->remaining;
                    if (state->remaining == 0) {
                        state->cv.notify_all();
                    }
                }
            })};
            if (id == 0) {
                // Pool already shut down: the chunk was dropped, count it as
                // done so this call never blocks forever (the work itself is
                // lost, matching submit()'s documented drop behaviour).
                std::lock_guard lock{state->mutex};
                if (state->remaining > 0) {
                    --state->remaining;
                }
            }
        }
    } catch (...) {
        // submit() threw (e.g. allocation failure) before chunk `chunk` was
        // queued: chunks [chunk, chunks) will never run, so take them out of
        // the outstanding count. The chunks already queued captured `&fn` by
        // reference, so wait for them before unwinding; unwinding first would
        // leave the pool queue holding a dangling reference to the caller's
        // callable.
        {
            std::lock_guard lock{state->mutex};
            const auto unsent{chunks - chunk};
            state->remaining =
                state->remaining > unsent ? state->remaining - unsent : 0;
        }
        await_chunks();
        unregister_waiter();
        throw;
    }

    await_chunks();
    unregister_waiter();
}

void executor::wait_idle() {
    auto &state{*impl_};
    std::unique_lock lock{state.mutex};
    state.idle_cv.wait(lock, [&state] {
        return state.stopping ||
               (state.queue.empty() &&
                state.active.load(std::memory_order_relaxed) == 0);
    });
}

void executor::shutdown() {
    auto &state{*impl_};
    std::vector<std::shared_ptr<impl::completion_state>> waiters{};
    {
        std::lock_guard lock{state.mutex};
        if (state.stopping) {
            return;
        }
        // Queued tasks are dropped (documented behaviour): the pool never
        // finishes work that was not already running when shutdown started.
        state.stopping = true;
        state.drop_queued_locked();
        // A parallel_for() blocked on dropped chunks would never be woken by a
        // completion (its lambdas are gone). Take a strong reference to every
        // live waiter now and notify them after the workers are joined (see
        // below); a waiter registered after this point cannot have queued
        // anything (submit() returns 0 once `stopping` is set), so it sees
        // `remaining == 0` or `shutdown_complete` without needing a wakeup.
        waiters.reserve(state.parallel_waiters.size());
        for (auto &waiter : state.parallel_waiters) {
            if (auto live{waiter.lock()}) {
                waiters.push_back(std::move(live));
            }
        }
        state.parallel_waiters.clear();
    }
    state.work_cv.notify_all();
    state.idle_cv.notify_all();
    for (auto &worker : state.workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    state.workers.clear();
    // Every worker is gone: no chunk can still be running, so a parallel_for
    // waiting on work that shutdown() dropped may now return without leaving a
    // dangling `&fn` behind.
    state.shutdown_complete.store(true, std::memory_order_release);
    for (auto &waiter : waiters) {
        waiter->cv.notify_all();
    }
}

auto executor::worker_count() const noexcept -> std::size_t {
    return impl_->worker_count;
}

auto executor::stats_snapshot() const -> stats {
    // Snapshot under the pool lock so queue/active counters cannot move between
    // the individual reads; workers update them while holding this lock too.
    // Not `noexcept`: locking may throw `std::system_error`.
    std::lock_guard lock{impl_->mutex};
    return stats{
        .submitted = impl_->submitted.load(std::memory_order_relaxed),
        .completed = impl_->completed.load(std::memory_order_relaxed),
        .failed = impl_->failed.load(std::memory_order_relaxed),
        .queued = impl_->queued.load(std::memory_order_relaxed),
        .active = impl_->active.load(std::memory_order_relaxed),
        .workers = impl_->worker_count,
    };
}

auto executor::default_worker_count() noexcept -> std::size_t {
    const auto hardware{std::thread::hardware_concurrency()};
    if (hardware <= 2) {
        return 1;
    }
    return static_cast<std::size_t>(hardware - 2);
}

} // namespace refrain::core::jobs
