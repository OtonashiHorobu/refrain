// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <utility>

namespace refrain::core {

/// Minimal asynchronous runtime built on standalone Asio.
///
/// The runtime owns an Asio `io_context`, an executor work guard and the
/// threads that drive it. It is the base for timers, block-wise streaming
/// reads and (later) the network server; the synchronous core APIs are
/// unaffected.
///
/// PIMPL: Asio is an implementation detail of `src/core/async.cpp`. This
/// header deliberately stays Asio-free, so `<asio/...>` never leaks into the
/// public API and callers compile without the Asio include path.
///
/// Thread-safety: `post()` and `run()` may be called concurrently from any
/// thread; `stop()` is idempotent and may race with them. Two rules:
/// (1) `stop()` must never be called from a task that the runtime itself is
/// executing (it has to join the worker threads), and a task must not destroy
/// the runtime it runs on; (2) a caller-side thread blocked in `run()` must be
/// joined before the runtime is destroyed - `stop()` unblocks it, but the
/// destructor only joins the runtime-owned worker threads. The destructor
/// itself must not race with another member call.
class async_runtime {
  public:
    /// Create a runtime driven by `worker_threads` background threads.
    ///
    /// `worker_threads == 0` starts no thread: the caller drives the loop with
    /// `run()` on a thread of its own (single-threaded/test mode). Any other
    /// value starts that many threads immediately, so `post()`ed tasks run
    /// without an explicit `run()`.
    ///
    /// Thread-creation/allocation failures are reported as values (this API is
    /// exception-free); only a hard OOM inside the error path itself may
    /// propagate `std::bad_alloc`.
    [[nodiscard]] static auto open(std::size_t worker_threads = 1)
        -> result<async_runtime>;

    async_runtime(async_runtime &&) noexcept;
    auto operator=(async_runtime &&) noexcept -> async_runtime &;
    ~async_runtime();

    async_runtime(const async_runtime &) = delete;
    auto operator=(const async_runtime &) -> async_runtime & = delete;

    /// False for a moved-from runtime and for one that was stopped.
    [[nodiscard]] auto valid() const noexcept -> bool;

    /// Queue `task` for execution on the io_context. Never blocks the caller.
    /// A no-op for an invalid/stopped runtime. Asio may throw `std::bad_alloc`
    /// while growing its handler queue; that is the accepted hard-OOM
    /// boundary (the function itself has no error channel).
    void post(std::function<void()> task);

    /// Run the io_context on the calling thread, blocking until `stop()`.
    /// Callable on several threads at once and equivalent to joining the
    /// runtime's worker set (this is how `open(0)` gets driven). Returns
    /// immediately for an invalid/stopped runtime.
    auto run() -> void;

    /// Stop the io_context and join every runtime-owned thread. Pending tasks
    /// are dropped. Idempotent; safe to call from any thread except a runtime
    /// task (see the class comment).
    void stop() noexcept;

  private:
    struct impl;
    explicit async_runtime(std::unique_ptr<impl> state) noexcept;

    std::unique_ptr<impl> impl_;
};

} // namespace refrain::core
