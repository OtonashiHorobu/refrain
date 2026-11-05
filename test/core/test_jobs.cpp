// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/jobs.hpp>

#include <boost/ut.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

namespace {

using refrain::core::jobs::executor;
using refrain::core::jobs::job_id;
using refrain::core::jobs::priority;

/// Single-worker gate used by the ordering tests: while `open` is false the
/// only worker is parked inside the gate task, so everything submitted
/// afterwards is guaranteed to sit in the priority queue when the gate is
/// released.
struct worker_gate {
    std::mutex mutex{};
    std::condition_variable cv{};
    bool open{false};

    void hold(executor &pool) {
        (void)pool.submit(priority::interactive, [this] {
            std::unique_lock lock{mutex};
            cv.wait(lock, [this] { return open; });
        });
    }

    void release() {
        {
            std::lock_guard lock{mutex};
            open = true;
        }
        cv.notify_one();
    }
};

boost::ut::suite jobs_tests = [] {
    using namespace boost::ut;
    using namespace std::chrono_literals;

    "jobs/submit runs every task and updates stats"_test = [] {
        executor pool{4};
        std::atomic<std::size_t> runs{0};
        constexpr std::size_t total{64};

        for (std::size_t index{0}; index < total; ++index) {
            (void)pool.submit(
                [&runs] { runs.fetch_add(1, std::memory_order_relaxed); });
        }
        pool.wait_idle();

        expect(runs.load(std::memory_order_relaxed) == total);
        const auto snapshot{pool.stats_snapshot()};
        expect(snapshot.submitted == total);
        expect(snapshot.completed == total);
        expect(snapshot.queued == 0U);
        expect(snapshot.active == 0U);
        expect(snapshot.workers == pool.worker_count());
    };

    "jobs/interactive tasks start before background ones"_test = [] {
        executor pool{1};
        worker_gate gate{};
        std::mutex order_mutex{};
        std::vector<int> order{};

        gate.hold(pool);
        (void)pool.submit(priority::background, [&] {
            std::lock_guard lock{order_mutex};
            order.push_back(3);
        });
        (void)pool.submit(priority::interactive, [&] {
            std::lock_guard lock{order_mutex};
            order.push_back(1);
        });
        (void)pool.submit(priority::normal, [&] {
            std::lock_guard lock{order_mutex};
            order.push_back(2);
        });
        gate.release();
        pool.wait_idle();

        expect(order.size() == 3U);
        if (order.size() == 3U) {
            expect(order[0] == 1); // interactive first
            expect(order[1] == 2); // then normal
            expect(order[2] == 3); // background last
        }
    };

    "jobs/same priority is FIFO"_test = [] {
        executor pool{1};
        worker_gate gate{};
        std::mutex order_mutex{};
        std::vector<int> order{};

        gate.hold(pool);
        for (int index{1}; index <= 3; ++index) {
            (void)pool.submit(priority::normal, [&order, &order_mutex, index] {
                std::lock_guard lock{order_mutex};
                order.push_back(index);
            });
        }
        gate.release();
        pool.wait_idle();

        expect(order.size() == 3U);
        if (order.size() == 3U) {
            expect(order[0] == 1);
            expect(order[1] == 2);
            expect(order[2] == 3);
        }
    };

    "jobs/parallel_for sums the range"_test = [] {
        executor pool{4};
        std::atomic<std::uint64_t> sum{0};
        constexpr std::size_t count{1000};

        pool.parallel_for(count, 7, [&sum](std::size_t index) {
            sum.fetch_add(static_cast<std::uint64_t>(index),
                          std::memory_order_relaxed);
        });

        expect(sum.load(std::memory_order_relaxed) == 499'500U);
        // parallel_for returns once every chunk body ran; let the pool finish
        // its bookkeeping before reading `completed` (ceil(1000 / 7) == 143).
        pool.wait_idle();
        expect(pool.stats_snapshot().completed >= 143U);
    };

    "jobs/parallel_for handles zero count and zero grain"_test = [] {
        executor pool{2};
        std::atomic<std::size_t> calls{0};

        pool.parallel_for(0, 1, [&calls](std::size_t) {
            calls.fetch_add(1, std::memory_order_relaxed);
        });
        expect(calls.load(std::memory_order_relaxed) == 0U);

        pool.parallel_for(10, 0, [&calls](std::size_t) {
            calls.fetch_add(1, std::memory_order_relaxed);
        });
        expect(calls.load(std::memory_order_relaxed) == 10U);
    };

    "jobs/parallel_for blocks until every chunk ran"_test = [] {
        executor pool{4};
        std::atomic<bool> completed{false};

        pool.parallel_for(4, 1, [&completed](std::size_t index) {
            if (index == 3) {
                completed.store(true, std::memory_order_relaxed);
            }
        });
        expect(completed.load(std::memory_order_relaxed));
    };

    "jobs/wait_idle waits for in-flight work"_test = [] {
        executor pool{2};
        std::atomic<bool> finished{false};

        (void)pool.submit([&finished] {
            std::this_thread::sleep_for(20ms);
            finished.store(true, std::memory_order_relaxed);
        });
        pool.wait_idle();

        expect(finished.load(std::memory_order_relaxed));
        expect(pool.stats_snapshot().active == 0U);
    };

    "jobs/1000 submitted tasks run in parallel"_test = [] {
        executor pool{4};
        std::atomic<std::uint64_t> sum{0};
        constexpr std::size_t total{1000};

        for (std::size_t index{0}; index < total; ++index) {
            (void)pool.submit([&sum, index] {
                sum.fetch_add(static_cast<std::uint64_t>(index),
                              std::memory_order_relaxed);
            });
        }
        pool.wait_idle();

        expect(sum.load(std::memory_order_relaxed) == 499'500U);
        expect(pool.stats_snapshot().completed == total);
    };

    "jobs/worker_count is at least one"_test = [] {
        executor automatic{};
        expect(automatic.worker_count() >= 1U);
        expect(executor::default_worker_count() >= 1U);

        executor single{1};
        expect(single.worker_count() == 1U);
    };

    "jobs/stats completed grows with finished work"_test = [] {
        executor pool{2};
        pool.wait_idle();
        const auto before{pool.stats_snapshot().completed};

        constexpr std::size_t total{8};
        for (std::size_t index{0}; index < total; ++index) {
            (void)pool.submit([] {});
        }
        pool.wait_idle();

        const auto after{pool.stats_snapshot()};
        expect(after.completed >= before + total);
        expect(after.queued == 0U);
        expect(after.active == 0U);
    };

    "jobs/submit after shutdown is dropped without blocking"_test = [] {
        executor pool{1};
        pool.shutdown();

        std::atomic<bool> ran{false};
        const auto id{pool.submit(priority::normal, [&ran] {
            ran.store(true, std::memory_order_relaxed);
        })};

        expect(id == job_id{0});
        pool.wait_idle(); // must not block once the pool is stopping
        expect(!ran.load(std::memory_order_relaxed));

        const auto snapshot{pool.stats_snapshot()};
        expect(snapshot.queued == 0U);
        expect(snapshot.active == 0U);
        expect(snapshot.submitted == 1U);
    };

    // A throwing task must not escape worker_loop() and call std::terminate
    // (exit 134), leaving active/completed corrupted and making wait_idle()
    // hang forever. The worker contains the exception, keeps the counters
    // consistent and remains available for further work.
    "jobs/throwing task is contained and workers stay alive"_test = [] {
        executor pool{2};
        std::atomic<std::size_t> ran{0};
        constexpr std::size_t throwing_total{8};

        for (std::size_t index{0}; index < throwing_total; ++index) {
            (void)pool.submit([index] {
                if (index % 2 == 0) {
                    throw std::runtime_error{"task failure"};
                }
                throw 42; // non-std exception type
            });
        }
        (void)pool.submit(
            [&ran] { ran.fetch_add(1, std::memory_order_relaxed); });

        pool.wait_idle(); // must return: no leaked `active` count

        expect(ran.load(std::memory_order_relaxed) == 1U);
        const auto snapshot{pool.stats_snapshot()};
        expect(snapshot.failed == throwing_total);
        expect(snapshot.completed == throwing_total + 1);
        expect(snapshot.queued == 0U);
        expect(snapshot.active == 0U);

        // The pool is still usable after the failures.
        std::atomic<bool> after{false};
        (void)pool.submit(
            [&after] { after.store(true, std::memory_order_relaxed); });
        pool.wait_idle();
        expect(after.load(std::memory_order_relaxed));
        expect(pool.stats_snapshot().failed == throwing_total);
    };

    // parallel_for from a worker must not deadlock by parking every worker on
    // chunks it can never run; it runs inline.
    "jobs/parallel_for from a worker runs inline without deadlock"_test = [] {
        executor pool{2};
        std::atomic<std::uint64_t> outer_sum{0};
        std::atomic<std::uint64_t> inner_sum{0};
        std::atomic<std::size_t> inner_calls{0};

        (void)pool.submit([&] {
            pool.parallel_for(100, 7, [&](std::size_t index) {
                outer_sum.fetch_add(static_cast<std::uint64_t>(index),
                                    std::memory_order_relaxed);
            });
        });
        (void)pool.submit([&] {
            // Nested re-entrancy inside the inline path must also be safe.
            pool.parallel_for(10, 3, [&](std::size_t index) {
                inner_calls.fetch_add(1, std::memory_order_relaxed);
                inner_sum.fetch_add(static_cast<std::uint64_t>(index),
                                    std::memory_order_relaxed);
            });
        });
        pool.wait_idle();

        expect(outer_sum.load(std::memory_order_relaxed) == 4'950U);
        expect(inner_calls.load(std::memory_order_relaxed) == 10U);
        expect(inner_sum.load(std::memory_order_relaxed) == 45U);
        expect(pool.stats_snapshot().failed == 0U);
    };

    // shutdown() drops queued chunks, so their completion lambdas never run and
    // `remaining` never reaches zero. The wait must end once shutdown() has
    // joined the workers (the dropped chunks are not counted as completed: the
    // work is lost, which is shutdown()'s documented behaviour).
    "jobs/parallel_for unblocks when shutdown drops queued chunks"_test = [] {
        executor pool{1};
        std::mutex gate_mutex{};
        std::condition_variable gate_cv{};
        bool gate_open{false};
        std::atomic<bool> gate_task_done{false};
        // Park the only worker in a gate task; it records its own completion so
        // the test can prove parallel_for() only returns once the pool has
        // really joined its workers.
        (void)pool.submit(priority::interactive, [&] {
            std::unique_lock lock{gate_mutex};
            gate_cv.wait(lock, [&] { return gate_open; });
            gate_task_done.store(true, std::memory_order_release);
        });

        std::promise<bool> returned{};
        auto finished{returned.get_future()};
        std::thread caller{[&pool, &returned, &gate_task_done] {
            pool.parallel_for(8, 1, [](std::size_t) {});
            returned.set_value(gate_task_done.load(std::memory_order_acquire));
        }};

        // Wait until every chunk is queued: the worker is in the gate and the
        // chunks are normal priority, so none of them can start.
        const auto queue_deadline{std::chrono::steady_clock::now() + 2s};
        while (pool.stats_snapshot().queued < 8U &&
               std::chrono::steady_clock::now() < queue_deadline) {
            std::this_thread::sleep_for(1ms);
        }
        expect(pool.stats_snapshot().queued == 8U);

        // shutdown() joins the worker parked in the gate task, so the gate has
        // to be released from another thread - but only after shutdown() has
        // flipped the pool to stopping (submit() then returns 0). Releasing it
        // earlier would let the worker drain the chunks and hide the missing
        // unblock.
        std::thread releaser{[&pool, &gate_mutex, &gate_cv, &gate_open] {
            const auto stop_deadline{std::chrono::steady_clock::now() + 2s};
            while (pool.submit(priority::normal, [] {}) != 0 &&
                   std::chrono::steady_clock::now() < stop_deadline) {
                std::this_thread::sleep_for(1ms);
            }
            {
                std::lock_guard lock{gate_mutex};
                gate_open = true;
            }
            gate_cv.notify_one();
        }};

        pool.shutdown(); // drops the eight queued chunks
        releaser.join();

        const auto status{finished.wait_for(1s)};
        expect(status == std::future_status::ready)
            << "parallel_for did not return within 1s of shutdown";
        if (status == std::future_status::ready) {
            // The gate task had not finished if parallel_for returned on
            // "shutdown started" instead of "shutdown joined the workers".
            expect(finished.get())
                << "parallel_for returned before the worker task finished";
            caller.join();
        } else {
            // A regression must fail the assertion instead of hanging the test
            // binary (std::async would join in its future destructor): the
            // caller thread is detached and only waits on its own completion
            // state from here on.
            caller.detach();
        }
    };

    // Partial thread creation must roll back the workers already started and
    // propagate the failure instead of terminating in ~executor.
    "jobs/constructor rolls back when thread creation fails"_test = [] {
        int calls{0};
        auto factory = [&calls](std::function<void()> body) {
            if (++calls == 2) {
                throw std::system_error{std::make_error_code(
                    std::errc::resource_unavailable_try_again)};
            }
            return std::thread{std::move(body)};
        };
        bool threw{false};
        try {
            executor pool{2, factory};
        } catch (const std::system_error &) {
            threw = true;
        }
        expect(threw);
        expect(calls == 2);
    };

    "jobs/worker scheduling options are applied without affecting work"_test =
        [] {
            executor pool{
                2,
                {},
                {.affinity = {0U},
                 .priority = refrain::platform::thread_priority::background}};
            std::atomic<int> sum{0};
            pool.parallel_for(64, 8, [&sum](std::size_t index) {
                sum.fetch_add(static_cast<int>(index),
                              std::memory_order_relaxed);
            });
            expect(sum.load() == 2016);
        };
};

} // namespace
