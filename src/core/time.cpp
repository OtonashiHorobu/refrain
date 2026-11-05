// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/time.hpp>

#include <algorithm>
#include <chrono>
#include <thread>

namespace refrain::core {

auto now_ns() noexcept -> std::int64_t {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               clock::now().time_since_epoch())
        .count();
}

tick_clock::tick_clock(std::int64_t tps) noexcept {
    if (tps <= 0) {
        tps = ticks_per_second;
    }
    // Sub-nanosecond periods are impossible: clamp to 1 ns so `interval_` is
    // never zero (a zero interval made wait_next() divide by zero -> SIGFPE for
    // tps > 1e9). tps() reports the effective (clamped) rate.
    const auto nanoseconds_per_tick{
        std::max<std::int64_t>(1, 1'000'000'000 / tps)};
    interval_ = std::chrono::nanoseconds{nanoseconds_per_tick};
}

void tick_clock::start() noexcept {
    if (running_) {
        return;
    }
    start_ = clock::now();
    last_ = start_;
    current_ = -1;
    missed_ = 0;
    running_ = true;
}

void tick_clock::stop() noexcept { running_ = false; }

auto tick_clock::running() const noexcept -> bool { return running_; }

auto tick_clock::current_tick() const noexcept -> tick { return current_; }

auto tick_clock::tps() const noexcept -> std::int64_t {
    const auto count = interval_.count();
    return count > 0 ? 1'000'000'000 / count : 0;
}

auto tick_clock::wait_next() -> result<tick_info> {
    if (!running_) {
        return std::unexpected{
            make_error(error_code::not_initialized,
                       "tick_clock::wait_next() called before start()")};
    }

    tick_info info{};
    ++current_;
    const auto target = start_ + interval_ * current_;

    const auto before_sleep = clock::now();
    if (before_sleep < target) {
        std::this_thread::sleep_until(target);
        info.sleep = std::chrono::duration_cast<std::chrono::nanoseconds>(
            clock::now() - before_sleep);
    }

    const auto now = clock::now();
    if (now > target) {
        // Running behind: skip the ticks that are already in the past instead
        // of busy-catching up one by one. `interval_` is guaranteed non-zero by
        // the constructor, so this division is safe.
        const auto behind =
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - target);
        const auto skipped =
            static_cast<std::int64_t>(behind.count() / interval_.count());
        if (skipped > 0) {
            current_ += skipped;
            missed_ += skipped;
            info.missed = skipped;
        }
    }

    info.index = current_;
    info.actual =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - last_);
    last_ = now;
    return info;
}

} // namespace refrain::core
