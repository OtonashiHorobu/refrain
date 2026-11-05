// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/async.hpp>

#include <boost/ut.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include <utility>

namespace {

using refrain::core::async_runtime;

/// Upper bound for every "the runtime should have executed this by now" wait:
/// a broken runtime fails the test instead of hanging CI forever.
constexpr auto kWaitTimeout = std::chrono::seconds{5};

} // namespace

boost::ut::suite async_tests = [] {
    using namespace boost::ut;
    using namespace std::chrono_literals;

    "async/open starts workers and runs posted tasks"_test = [] {
        auto opened = async_runtime::open(2);
        expect(opened.has_value());
        if (!opened) {
            return;
        }
        auto runtime = std::move(*opened);
        expect(runtime.valid());

        constexpr std::size_t total{64};
        std::atomic<std::size_t> completed{0};
        std::mutex mutex;
        std::condition_variable cv;

        for (std::size_t index{0}; index < total; ++index) {
            runtime.post([&] {
                completed.fetch_add(1, std::memory_order_acq_rel);
                // Holding the mutex across notify_one() closes the window in
                // which the waiter could check the predicate and then miss the
                // wakeup before entering the wait.
                std::lock_guard lock{mutex};
                cv.notify_one();
            });
        }

        {
            std::unique_lock lock{mutex};
            expect(cv.wait_for(lock, kWaitTimeout, [&] {
                return completed.load(std::memory_order_acquire) == total;
            }));
        }
        runtime.stop();
        expect(completed.load(std::memory_order_acquire) == total);
        expect(!runtime.valid());
    };

    "async/run drives a zero-worker runtime until stop"_test = [] {
        auto opened = async_runtime::open(0);
        expect(opened.has_value());
        if (!opened) {
            return;
        }
        auto runtime = std::move(*opened);
        expect(runtime.valid());

        std::atomic<bool> task_ran{false};
        std::atomic<bool> run_returned{false};
        std::mutex mutex;
        std::condition_variable cv;

        std::thread driver{[&runtime, &run_returned] {
            runtime.run();
            run_returned.store(true, std::memory_order_release);
        }};

        runtime.post([&] {
            task_ran.store(true, std::memory_order_release);
            std::lock_guard lock{mutex};
            cv.notify_one();
        });

        {
            std::unique_lock lock{mutex};
            expect(cv.wait_for(lock, kWaitTimeout, [&] {
                return task_ran.load(std::memory_order_acquire);
            }));
        }

        runtime.stop();
        driver.join();
        expect(run_returned.load(std::memory_order_acquire));
        expect(!runtime.valid());
    };

    "async/stop is idempotent and drops later posts"_test = [] {
        auto opened = async_runtime::open(1);
        expect(opened.has_value());
        if (!opened) {
            return;
        }
        auto runtime = std::move(*opened);

        runtime.stop();
        runtime.stop();
        expect(!runtime.valid());

        // Posting to a stopped runtime must be a no-op, not a crash or a task
        // that piles up on a context that will never run again.
        runtime.post([] {});
        runtime.stop();
    };

    "async/move transfers ownership"_test = [] {
        auto opened = async_runtime::open(1);
        expect(opened.has_value());
        if (!opened) {
            return;
        }
        auto runtime = std::move(*opened);
        expect(runtime.valid());

        async_runtime moved{std::move(runtime)};
        expect(!runtime.valid());
        expect(moved.valid());

        std::atomic<bool> completed{false};
        std::mutex mutex;
        std::condition_variable cv;
        moved.post([&] {
            completed.store(true, std::memory_order_release);
            std::lock_guard lock{mutex};
            cv.notify_one();
        });
        {
            std::unique_lock lock{mutex};
            expect(cv.wait_for(lock, kWaitTimeout, [&] {
                return completed.load(std::memory_order_acquire);
            }));
        }

        auto other = async_runtime::open(1);
        expect(other.has_value());
        if (other) {
            *other = std::move(moved);
            expect(!moved.valid());
            expect(other->valid());
            other->stop();
        }
        moved.stop();
    };
};
