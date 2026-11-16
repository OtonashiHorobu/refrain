// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/sim/world_clock.hpp>

#include <refrain/data/nbt.hpp>
#include <refrain/world/object_store.hpp>
#include <refrain/world/saved_data.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

namespace refrain::sim {
namespace {

/// Seed of the weather stream. The value is fixed so a world replays the same
/// weather sequence; it is mixed with the world seed by `seed()`.
constexpr std::uint64_t weather_stream_salt{0x9E3779B97F4A7C15ULL};

/// Uniform integer in `[minimum, maximum]` from the world's deterministic
/// stream. The reference samples an inclusive integer range.
[[nodiscard]] auto sample(rng &random, std::int32_t minimum,
                          std::int32_t maximum) noexcept -> std::int32_t {
    if (maximum <= minimum) {
        return minimum;
    }
    const auto span{static_cast<std::uint32_t>(maximum - minimum) + 1U};
    return minimum + static_cast<std::int32_t>(random.next_bounded(span));
}

[[nodiscard]] auto clamp_level(float value) noexcept -> float {
    return std::clamp(value, 0.0F, 1.0F);
}

[[nodiscard]] auto read_int_like(const data::nbt::value *value,
                                 std::int32_t fallback) -> std::int32_t {
    if (value == nullptr) {
        return fallback;
    }
    if (const auto number{value->as_byte()}; number) {
        return *number;
    }
    if (const auto number{value->as_short()}; number) {
        return *number;
    }
    if (const auto number{value->as_int()}; number) {
        return *number;
    }
    if (const auto number{value->as_long()}; number) {
        return static_cast<std::int32_t>(*number);
    }
    return fallback;
}

/// Duration of the pending rain or thunder phase. A running phase is resampled
/// from its duration range; a stopped one from its delay range.
[[nodiscard]] auto sample_phase(rng &random, bool active,
                                std::int32_t active_min,
                                std::int32_t active_max, std::int32_t delay_min,
                                std::int32_t delay_max) noexcept
    -> std::int32_t {
    return active ? sample(random, active_min, active_max)
                  : sample(random, delay_min, delay_max);
}

} // namespace

world_clock::world_clock() : weather_random_{0} { seed(weather_stream_salt); }

world_clock::world_clock(std::uint64_t seed_value) noexcept
    : weather_random_{seed_value ^ weather_stream_salt} {}

void world_clock::seed(std::uint64_t value) noexcept {
    weather_random_.seed(value ^ weather_stream_salt);
}

void world_clock::tick() {
    ++game_time_;
    if (!paused_) {
        partial_tick_ += rate_;
        const auto whole{std::floor(partial_tick_)};
        partial_tick_ -= whole;
        total_ticks_ += static_cast<std::int64_t>(whole);
    }
    advance_weather();
}

void world_clock::advance_weather() {
    if (clear_weather_time_ > 0) {
        --clear_weather_time_;
        thunder_time_ = thundering_ ? 0 : 1;
        rain_time_ = raining_ ? 0 : 1;
        thundering_ = false;
        raining_ = false;
    } else {
        if (thunder_time_ > 0) {
            if (--thunder_time_ == 0) {
                thundering_ = !thundering_;
            }
        } else {
            thunder_time_ = sample_phase(
                weather_random_, thundering_, thunder_duration_min,
                thunder_duration_max, thunder_delay_min, thunder_delay_max);
        }
        if (rain_time_ > 0) {
            if (--rain_time_ == 0) {
                raining_ = !raining_;
            }
        } else {
            rain_time_ =
                sample_phase(weather_random_, raining_, rain_duration_min,
                             rain_duration_max, rain_delay_min, rain_delay_max);
        }
    }
    thunder_level_ =
        clamp_level(thunder_level_ + (thundering_ ? 0.01F : -0.01F));
    rain_level_ = clamp_level(rain_level_ + (raining_ ? 0.01F : -0.01F));
}

auto world_clock::snapshot() const noexcept -> clock_snapshot {
    return clock_snapshot{.game_time = game_time_,
                          .total_ticks = total_ticks_,
                          .partial_tick = partial_tick_,
                          .rate = rate_,
                          .paused = paused_};
}

auto world_clock::weather() const noexcept -> weather_snapshot {
    return weather_snapshot{.raining = raining_,
                            .thundering = thundering_,
                            .rain_time = rain_time_,
                            .thunder_time = thunder_time_,
                            .clear_weather_time = clear_weather_time_,
                            .rain_level = rain_level_,
                            .thunder_level = thunder_level_};
}

void world_clock::set_total_ticks(std::int64_t ticks) noexcept {
    total_ticks_ = ticks;
    partial_tick_ = 0.0F;
}

void world_clock::add_total_ticks(std::int64_t ticks) noexcept {
    total_ticks_ = std::max<std::int64_t>(total_ticks_ + ticks, 0);
    partial_tick_ = 0.0F;
}

void world_clock::set_game_time(std::int64_t ticks) noexcept {
    game_time_ = std::max<std::int64_t>(ticks, 0);
}

void world_clock::set_paused(bool paused) noexcept { paused_ = paused; }

void world_clock::set_rate(float rate) noexcept { rate_ = rate; }

void world_clock::set_weather(bool raining, bool thundering,
                              std::int32_t clear_time,
                              std::int32_t rain_duration,
                              std::int32_t thunder_duration) noexcept {
    clear_weather_time_ = clear_time;
    rain_time_ = rain_duration;
    thunder_time_ = thunder_duration;
    raining_ = raining;
    thundering_ = thundering;
}

void world_clock::restore_weather(bool raining, bool thundering,
                                  std::int32_t rain_time,
                                  std::int32_t thunder_time,
                                  std::int32_t clear_weather_time,
                                  float rain_level,
                                  float thunder_level) noexcept {
    raining_ = raining;
    thundering_ = thundering;
    rain_time_ = rain_time;
    thunder_time_ = thunder_time;
    clear_weather_time_ = clear_weather_time;
    rain_level_ = clamp_level(rain_level);
    thunder_level_ = clamp_level(thunder_level);
}

auto read_world_clock(refrain::world::object_store &objects, world_clock &clock)
    -> status {
    using namespace refrain::world;
    using namespace refrain::data::nbt;
    const auto clocks_path{saved_data_path("minecraft", "world_clocks.dat")};
    if (!clocks_path) {
        return std::unexpected{clocks_path.error()};
    }
    if (objects.exists(*clocks_path)) {
        auto document{read_nbt_object(objects, *clocks_path)};
        if (!document) {
            return std::unexpected{std::move(document.error())};
        }
        const auto *data{document->get("data")};
        if (data == nullptr || data->as_compound() == nullptr) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "world_clocks.dat has no data compound")};
        }
        const auto *overworld{data->get("minecraft:overworld")};
        if (overworld != nullptr && overworld->as_compound() != nullptr) {
            const auto *ticks{overworld->get("total_ticks")};
            if (ticks != nullptr) {
                const auto value{ticks->as_long()};
                if (!value) {
                    return std::unexpected{make_error(
                        error_code::parse_error, "world_clocks.dat overworld "
                                                 "total_ticks is not a long")};
                }
                clock.set_total_ticks(*value);
            }
        }
    }
    const auto weather_path{saved_data_path("minecraft", "weather.dat")};
    if (!weather_path) {
        return std::unexpected{weather_path.error()};
    }
    if (objects.exists(*weather_path)) {
        auto document{read_nbt_object(objects, *weather_path)};
        if (!document) {
            return std::unexpected{std::move(document.error())};
        }
        const auto *data{document->get("data")};
        if (data == nullptr || data->as_compound() == nullptr) {
            return std::unexpected{make_error(
                error_code::parse_error, "weather.dat has no data compound")};
        }
        const auto raining{data->get("raining")};
        const auto thundering{data->get("thundering")};
        const auto rain_level{data->get("rain_level")};
        const auto thunder_level{data->get("thunder_level")};
        clock.restore_weather(
            raining != nullptr && read_int_like(raining, 0) != 0,
            thundering != nullptr && read_int_like(thundering, 0) != 0,
            read_int_like(data->get("rain_time"), 0),
            read_int_like(data->get("thunder_time"), 0),
            read_int_like(data->get("clear_weather_time"), 0),
            rain_level == nullptr
                ? ((raining != nullptr && read_int_like(raining, 0) != 0)
                       ? 1.0F
                       : 0.0F)
                : rain_level->as_float().value_or(0.0F),
            thunder_level == nullptr
                ? ((thundering != nullptr && read_int_like(thundering, 0) != 0)
                       ? 1.0F
                       : 0.0F)
                : thunder_level->as_float().value_or(0.0F));
    }
    return status_ok();
}

auto write_world_clock(refrain::world::object_store &objects,
                       const world_clock &clock) -> status {
    using namespace refrain::world;
    using namespace refrain::data::nbt;
    const auto clocks_path{saved_data_path("minecraft", "world_clocks.dat")};
    if (!clocks_path) {
        return std::unexpected{clocks_path.error()};
    }
    // Only the modelled overworld clock is written; a clock this server does
    // not simulate keeps whatever data the world already carried.
    const auto clocks{make_compound(compound{
        {"minecraft:overworld",
         make_compound(
             compound{{"total_ticks", make_long(clock.total_ticks())}})},
    })};
    if (auto written{write_nbt_object(
            objects, *clocks_path,
            make_compound(
                compound{{"data", clocks}, {"DataVersion", make_int(5023)}}),
            false)};
        !written) {
        return written;
    }
    const auto weather_path{saved_data_path("minecraft", "weather.dat")};
    if (!weather_path) {
        return std::unexpected{weather_path.error()};
    }
    // The reference persists the flags and timers; the levels are derived from
    // the flags on load (a raining world starts at level one).
    const auto weather{clock.weather()};
    return write_nbt_object(
        objects, *weather_path,
        make_compound(compound{
            {"data",
             make_compound(compound{
                 {"clear_weather_time", make_int(weather.clear_weather_time)},
                 {"rain_time", make_int(weather.rain_time)},
                 {"raining", make_byte(weather.raining ? 1 : 0)},
                 {"thunder_time", make_int(weather.thunder_time)},
                 {"thundering", make_byte(weather.thundering ? 1 : 0)},
             })},
            {"DataVersion", make_int(5023)}}),
        false);
}

} // namespace refrain::sim
