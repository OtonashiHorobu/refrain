// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>

#include <chrono>
#include <cstdint>

namespace refrain::core {

/// Simulation runs at 20 ticks per second (Minecraft-compatible).
inline constexpr std::int64_t ticks_per_second = 20;
inline constexpr std::int64_t tick_nanoseconds =
    1'000'000'000 / ticks_per_second;
inline constexpr std::int64_t tick_milliseconds = tick_nanoseconds / 1'000'000;

using tick = std::int64_t;
using clock = std::chrono::steady_clock;

[[nodiscard]] auto now_ns() noexcept -> std::int64_t;

/// Result of one tick boundary.
struct tick_info {
    /// Monotonic tick counter (first tick after start() is 0).
    tick index{0};
    /// Wall time since the previous tick boundary
    /// (= expected interval when the server keeps up).
    std::chrono::nanoseconds actual{0};
    /// Time spent sleeping before the tick started (0 when running behind).
    std::chrono::nanoseconds sleep{0};
    /// Ticks that were due but skipped because the server ran behind.
    std::int64_t missed{0};
};

/// Fixed-timestep scheduler: sleeps until the next 20 Hz boundary and reports
/// drift/missed ticks. Single consumer (the sim thread). Tests may use a high
/// tps value to keep runtimes short; periods below 1 ns are clamped to 1 ns
/// (so the effective `tps()` never exceeds 1e9, and wait_next() never divides
/// by zero).
class tick_clock {
  public:
    explicit tick_clock(std::int64_t tps = ticks_per_second) noexcept;

    /// Anchor the schedule to the current steady_clock time. Idempotent.
    void start() noexcept;

    void stop() noexcept;

    [[nodiscard]] auto running() const noexcept -> bool;

    [[nodiscard]] auto current_tick() const noexcept -> tick;

    /// Effective ticks per second after clamping (`1'000'000'000 / interval`).
    [[nodiscard]] auto tps() const noexcept -> std::int64_t;

    /// Block until the next tick boundary, catch up if the caller is late, then
    /// advance the logical tick counter. Returns `not_initialized` when the
    /// clock was not started (no fabricated tick 0).
    [[nodiscard]] auto wait_next() -> result<tick_info>;

  private:
    std::chrono::nanoseconds interval_{};
    clock::time_point start_{};
    clock::time_point last_{};
    tick current_{-1};
    tick missed_{0};
    bool running_{false};
};

} // namespace refrain::core
