// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/async.hpp>

#include <refrain/core/log.hpp>

#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include <atomic>
#include <cstddef>
#include <exception>
#include <expected>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <utility>
#include <vector>

namespace refrain::core {
namespace {

/// Drive the context until `stop()`.
///
/// Asio reports its own failures through `error_code`, but a *posted task* may
/// still throw: `io_context::run()` propagates handler exceptions, so they are
/// contained here at the thread boundary (the jobs pool does the same) instead
/// of terminating the process or killing the event loop.
auto drain(asio::io_context &context) noexcept -> void {
    while (!context.stopped()) {
        try {
            (void)context.run();
        } catch (...) {
            // The exception belongs to one task; keep the loop alive.
        }
    }
}

} // namespace

struct async_runtime::impl {
    using work_guard_type =
        asio::executor_work_guard<asio::io_context::executor_type>;

    impl() : work{asio::make_work_guard(context)} {}

    /// Start `count` threads that drive the context. On failure the threads
    /// started so far are left joinable; the caller calls stop() to roll back.
    auto start(std::size_t count) -> bool {
        threads.reserve(count);
        for (std::size_t index{0}; index < count; ++index) {
            try {
                threads.emplace_back([this, index] {
                    char name[16]{};
                    const auto written{fmt::format_to_n(name, sizeof(name) - 1,
                                                        "io-{}", index)};
                    log::set_thread_name(std::string_view{
                        name, static_cast<std::size_t>(written.out - name)});
                    drain(context);
                });
            } catch (...) {
                return false;
            }
        }
        return true;
    }

    /// Join the threads owned by this runtime. Requires `stop_mutex`.
    void join_threads_locked() noexcept {
        const auto self{std::this_thread::get_id()};
        for (auto &thread : threads) {
            if (!thread.joinable()) {
                continue;
            }
            if (thread.get_id() == self) {
                // Misuse (stop() called from a runtime task): joining self
                // would throw. Detach instead so the caller can still unwind;
                // the runtime must outlive the task.
                thread.detach();
                continue;
            }
            try {
                thread.join();
            } catch (...) {
                // join() only fails for a self/deadlocked thread; detach so
                // the noexcept stop() cannot terminate the process.
                thread.detach();
            }
        }
        threads.clear();
    }

    /// Not `noexcept`: it locks `stop_mutex` and joins threads. The public
    /// `async_runtime::stop()` (declared `noexcept`) is the barrier.
    void stop() {
        // Serialise concurrent stop() calls: only the first one stops the
        // context, but every caller must not race on `threads`.
        std::lock_guard lock{stop_mutex};
        if (!stopped.exchange(true, std::memory_order_acq_rel)) {
            // context.stop() makes run()/drain() return promptly and drops
            // queued handlers; resetting the guard releases the work
            // reference that keeps the context alive when idle.
            context.stop();
            work.reset();
        }
        join_threads_locked();
    }

    [[nodiscard]] auto valid() const noexcept -> bool {
        return !stopped.load(std::memory_order_acquire);
    }

    asio::io_context context{};
    work_guard_type work;
    std::vector<std::thread> threads{};
    std::atomic<bool> stopped{false};
    std::mutex stop_mutex{};
};

async_runtime::async_runtime(std::unique_ptr<impl> state) noexcept
    : impl_{std::move(state)} {}

async_runtime::async_runtime(async_runtime &&other) noexcept
    : impl_{std::move(other.impl_)} {}

auto async_runtime::operator=(async_runtime &&other) noexcept
    -> async_runtime & {
    if (this != &other) {
        stop();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

async_runtime::~async_runtime() { stop(); }

auto async_runtime::open(std::size_t worker_threads) -> result<async_runtime> {
    std::unique_ptr<impl> state;
    try {
        state = std::make_unique<impl>();
        if (!state->start(worker_threads)) {
            state->stop();
            return std::unexpected{
                make_error(error_code::unknown,
                           "could not start an async runtime worker thread")};
        }
    } catch (const std::bad_alloc &) {
        if (state) {
            state->stop();
        }
        return std::unexpected{make_error(error_code::out_of_memory,
                                          "async runtime allocation failed")};
    } catch (const std::exception &failure) {
        if (state) {
            state->stop();
        }
        return std::unexpected{make_error(error_code::unknown, failure.what())};
    } catch (...) {
        // Nothing in the runtime is supposed to throw a non-std exception;
        // keep the public API value-based even if a platform/allocator hook
        // does something unusual.
        if (state) {
            state->stop();
        }
        return std::unexpected{make_error(
            error_code::unknown, "async runtime initialisation failed")};
    }
    return async_runtime{std::move(state)};
}

auto async_runtime::valid() const noexcept -> bool {
    return impl_ != nullptr && impl_->valid();
}

void async_runtime::post(std::function<void()> task) {
    if (impl_ == nullptr || !impl_->valid() || !task) {
        return;
    }
    // Asio moves the handler into the io_context queue. The valid() gate
    // keeps tasks from piling up on a stopped context that will never run
    // them again.
    asio::post(impl_->context, std::move(task));
}

auto async_runtime::run() -> void {
    if (impl_ == nullptr) {
        return;
    }
    drain(impl_->context);
}

void async_runtime::stop() noexcept {
    // Exception barrier: impl::stop() locks and joins; this entry point is
    // `noexcept` and is also called from the destructor, so it must not let a
    // `std::system_error` escape.
    if (impl_ != nullptr) {
        try {
            impl_->stop();
        } catch (...) {
        }
    }
}

} // namespace refrain::core
