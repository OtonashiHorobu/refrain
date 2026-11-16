// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// World clock and weather of the overworld level.
//
// The clock has two counters. `game_time` is the level's age in server ticks
// and advances every tick. `total_ticks` is the daylight clock: it advances by
// `rate` while the clock is not paused, wraps every 24000 ticks for rendering
// and is what `/time` moves and what the client uses for the day-night cycle.
//
// Weather follows the reference server's state machine: a rain timer and a
// thunder timer count down, flip their flag at zero and are then resampled from
// the reference's uniform durations; rain and thunder levels ease towards their
// flag by 0.01 per tick so a rain front ramps over a hundred ticks instead of
// appearing instantly.
//
// The distribution draws use a dedicated random stream, so weather does not
// perturb the simulation's per-tick draw (the state hash covers the tick draw).

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/sim/rng.hpp>

#include <cstdint>

namespace refrain::world {
class object_store;
} // namespace refrain::world

namespace refrain::sim {

/// Number of ticks in one day-night cycle (the overworld clock period).
inline constexpr std::int64_t day_length_ticks = 24000;

/// Uniform weather timing ranges of the reference server, in ticks.
inline constexpr std::int32_t rain_delay_min = 12000;
inline constexpr std::int32_t rain_delay_max = 180000;
inline constexpr std::int32_t rain_duration_min = 12000;
inline constexpr std::int32_t rain_duration_max = 24000;
inline constexpr std::int32_t thunder_delay_min = 12000;
inline constexpr std::int32_t thunder_delay_max = 180000;
inline constexpr std::int32_t thunder_duration_min = 3600;
inline constexpr std::int32_t thunder_duration_max = 15600;

/// Clock values the client receives in `set_time`.
struct clock_snapshot {
    std::int64_t game_time{0};
    std::int64_t total_ticks{0};
    float partial_tick{0.0F};
    float rate{1.0F};
    bool paused{false};
};

/// Weather values the level exposes to the tick loop and the commands.
struct weather_snapshot {
    bool raining{false};
    bool thundering{false};
    std::int32_t rain_time{0};
    std::int32_t thunder_time{0};
    std::int32_t clear_weather_time{0};
    float rain_level{0.0F};
    float thunder_level{0.0F};
};

/// One level's clock and weather state.
class world_clock {
  public:
    world_clock();
    explicit world_clock(std::uint64_t seed) noexcept;

    /// Reseed the weather distribution stream. The clock counters keep their
    /// values.
    void seed(std::uint64_t seed) noexcept;

    /// Advance one server tick: age, daylight clock and weather.
    void tick();

    [[nodiscard]] auto snapshot() const noexcept -> clock_snapshot;
    [[nodiscard]] auto weather() const noexcept -> weather_snapshot;

    [[nodiscard]] auto game_time() const noexcept -> std::int64_t {
        return game_time_;
    }
    [[nodiscard]] auto total_ticks() const noexcept -> std::int64_t {
        return total_ticks_;
    }
    [[nodiscard]] auto paused() const noexcept -> bool { return paused_; }
    [[nodiscard]] auto rate() const noexcept -> float { return rate_; }
    [[nodiscard]] auto is_raining() const noexcept -> bool { return raining_; }
    [[nodiscard]] auto is_thundering() const noexcept -> bool {
        return thundering_;
    }

    /// Move the daylight clock, keeping the fractional part.
    void set_total_ticks(std::int64_t ticks) noexcept;
    void add_total_ticks(std::int64_t ticks) noexcept;
    /// Restore the level age, for example from stored world data.
    void set_game_time(std::int64_t ticks) noexcept;
    void set_paused(bool paused) noexcept;
    void set_rate(float rate) noexcept;

    /// Set the weather flags and clear or set the transition timers, matching
    /// the reference's `/weather` command semantics. `-1` for a duration keeps
    /// the timer at zero, which lets the state machine resample it.
    void set_weather(bool raining, bool thundering, std::int32_t clear_time = 0,
                     std::int32_t rain_duration = 0,
                     std::int32_t thunder_duration = 0) noexcept;

    /// Restore a persisted weather state without resampling a timer.
    void restore_weather(bool raining, bool thundering, std::int32_t rain_time,
                         std::int32_t thunder_time,
                         std::int32_t clear_weather_time, float rain_level,
                         float thunder_level) noexcept;

  private:
    void advance_weather();

    std::int64_t game_time_{0};
    std::int64_t total_ticks_{0};
    float partial_tick_{0.0F};
    float rate_{1.0F};
    bool paused_{false};

    bool raining_{false};
    bool thundering_{false};
    std::int32_t rain_time_{0};
    std::int32_t thunder_time_{0};
    std::int32_t clear_weather_time_{0};
    float rain_level_{0.0F};
    float thunder_level_{0.0F};

    rng weather_random_{};
};

/// Read `global/data/minecraft/world_clocks.dat` and
/// `global/data/minecraft/weather.dat` into `clock`. Missing objects leave the
/// matching fields untouched; a malformed object fails with `parse_error`.
[[nodiscard]] auto read_world_clock(refrain::world::object_store &objects,
                                    world_clock &clock) -> status;

/// Write the two saved-data objects. Only the fields the file carries are
/// written; the level age lives in `level.dat` and is written by the world
/// store, not here.
[[nodiscard]] auto write_world_clock(refrain::world::object_store &objects,
                                     const world_clock &clock) -> status;

} // namespace refrain::sim
