// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// World clock and weather: advancement on the tick grid, the reference's
// weather state machine, persistence into the world store and the restored
// values after a reopen.

#include <refrain/sim/world.hpp>
#include <refrain/sim/world_clock.hpp>
#include <refrain/world/create.hpp>
#include <refrain/world/store.hpp>

#include <boost/ut.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace sim = refrain::sim;
namespace world = refrain::world;

[[nodiscard]] auto scratch_directory() -> std::filesystem::path {
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    const auto path{std::filesystem::temp_directory_path() /
                    ("refrain_clock_" + std::to_string(stamp))};
    std::error_code error{};
    std::filesystem::create_directories(path, error);
    return path;
}

struct world_fixture {
    std::filesystem::path directory{};
    world_fixture() : directory{scratch_directory()} {}
    world_fixture(const world_fixture &) = delete;
    auto operator=(const world_fixture &) -> world_fixture & = delete;
    world_fixture(world_fixture &&) = delete;
    auto operator=(world_fixture &&) -> world_fixture & = delete;
    ~world_fixture() {
        std::error_code error{};
        std::filesystem::remove_all(directory, error);
    }
};

} // namespace

suite<"sim/world-clock"> world_clock_tests = [] {
    "the game time and the daylight clock advance one tick per server tick"_test =
        [] {
            sim::world_clock clock{};
            expect(clock.game_time() == 0);
            expect(clock.total_ticks() == 0);
            for (auto index{0}; index < 100; ++index) {
                clock.tick();
            }
            expect(clock.game_time() == 100);
            expect(clock.total_ticks() == 100);
            const auto snapshot{clock.snapshot()};
            expect(snapshot.game_time == 100);
            expect(snapshot.total_ticks == 100);
            expect(snapshot.partial_tick == 0.0F);
            expect(!snapshot.paused);
        };

    "a paused clock keeps its ticks while the game time still advances"_test =
        [] {
            sim::world_clock clock{};
            clock.tick();
            clock.set_paused(true);
            clock.tick();
            clock.tick();
            expect(clock.game_time() == 3);
            expect(clock.total_ticks() == 1);
            clock.set_paused(false);
            clock.tick();
            expect(clock.total_ticks() == 2);
        };

    "a clock rate scales the daylight clock and carries its fraction"_test =
        [] {
            sim::world_clock clock{};
            clock.set_rate(2.0F);
            clock.tick();
            expect(clock.total_ticks() == 2);
            clock.set_rate(0.25F);
            clock.tick();
            expect(clock.total_ticks() == 2);
            expect(clock.snapshot().partial_tick == 0.25F);
            for (auto index{0}; index < 3; ++index) {
                clock.tick();
            }
            expect(clock.total_ticks() == 3);
            expect(clock.snapshot().partial_tick == 0.0F);
        };

    "setting the daylight clock resets its fractional carry"_test = [] {
        sim::world_clock clock{};
        clock.set_rate(0.5F);
        clock.tick();
        expect(clock.total_ticks() == 0);
        clock.set_total_ticks(6000);
        expect(clock.total_ticks() == 6000);
        expect(clock.snapshot().partial_tick == 0.0F);
        clock.add_total_ticks(-7000);
        expect(clock.total_ticks() == 0);
    };

    "rain ramps its level towards one by one hundredth per tick"_test = [] {
        sim::world_clock clock{77};
        expect(!clock.is_raining());
        expect(clock.weather().rain_level == 0.0F);
        clock.set_weather(true, false, 0, 200, 0);
        for (auto index{0}; index < 100; ++index) {
            clock.tick();
        }
        expect(clock.is_raining());
        expect(clock.weather().rain_level > 0.99F);
        expect(clock.weather().rain_time == 100);
        for (auto index{0}; index < 100; ++index) {
            clock.tick();
        }
        expect(!clock.is_raining());
        expect(clock.weather().rain_time == 0);
        // The next tick resamples a clear-weather delay within the reference
        // range and counts it down.
        clock.tick();
        const auto rain_time{clock.weather().rain_time};
        expect(rain_time >= sim::rain_delay_min - 1);
        expect(rain_time <= sim::rain_delay_max);
    };

    "the rain timer flips the weather when it expires"_test = [] {
        sim::world_clock clock{5};
        clock.set_weather(true, false, 0, 3, 0);
        clock.tick();
        expect(clock.is_raining());
        clock.tick();
        expect(clock.is_raining());
        clock.tick();
        // The final countdown tick flips the flag.
        clock.tick();
        expect(!clock.is_raining());
        expect(clock.weather().rain_time > 0);
    };

    "clear weather cancels both phases after the requested ticks"_test = [] {
        sim::world_clock clock{3};
        clock.set_weather(true, true, 0, 1000, 1000);
        for (auto index{0}; index < 10; ++index) {
            clock.tick();
        }
        expect(clock.is_raining());
        expect(clock.is_thundering());
        clock.set_weather(false, false, 5, 0, 0);
        for (auto index{0}; index < 5; ++index) {
            clock.tick();
        }
        expect(!clock.is_raining());
        expect(!clock.is_thundering());
        expect(clock.weather().clear_weather_time == 0);
    };

    "the weather stream is deterministic for the same seed"_test = [] {
        sim::world_clock first{12345};
        sim::world_clock second{12345};
        for (auto index{0}; index < 400; ++index) {
            first.tick();
            second.tick();
            if (index % 50 == 0) {
                first.set_weather(index % 100 == 0, false, 0, 20, 0);
                second.set_weather(index % 100 == 0, false, 0, 20, 0);
            }
        }
        const auto left{first.weather()};
        const auto right{second.weather()};
        expect(left.raining == right.raining);
        expect(left.thundering == right.thundering);
        expect(left.rain_time == right.rain_time);
        expect(left.thunder_time == right.thunder_time);
        expect(left.rain_level == right.rain_level);
    };
    "the weather sequence is keyed by the seed"_test = [] {
        sim::world_clock first{12345};
        sim::world_clock second{54321};
        sim::world_clock repeat{12345};
        bool differs{false};
        for (int tick{0}; tick < 200; ++tick) {
            first.tick();
            second.tick();
            repeat.tick();
            expect(repeat.weather().rain_time == first.weather().rain_time)
                << tick;
            expect(repeat.weather().thunder_time ==
                   first.weather().thunder_time)
                << tick;
            if (first.weather().rain_time != second.weather().rain_time ||
                first.weather().thunder_time != second.weather().thunder_time) {
                differs = true;
            }
        }
        expect(differs) << "two seeds must not share one weather sequence";
    };
};

suite<"sim/world-clock/world"> world_clock_world_tests = [] {
    "worlds created with different seeds do not share a weather sequence"_test =
        [] {
            world_fixture first_fixture{};
            world_fixture second_fixture{};
            const auto first_path{(first_fixture.directory / "world").string()};
            const auto second_path{
                (second_fixture.directory / "world").string()};
            expect(
                world::create_world(first_path, 111, "weather-a").has_value());
            expect(
                world::create_world(second_path, 222, "weather-b").has_value());
            auto first{sim::world::open(first_path)};
            auto second{sim::world::open(second_path)};
            expect(first.has_value());
            expect(second.has_value());
            if (!first || !second) {
                return;
            }
            // The first tick samples the pending rain and thunder delays.
            first->tick();
            second->tick();
            const auto left{first->clock().weather()};
            const auto right{second->clock().weather()};
            expect(left.rain_time != right.rain_time ||
                   left.thunder_time != right.thunder_time);
        };

    "a reopened world continues the weather sequence it was saved at"_test =
        [] {
            world_fixture fixture{};
            const auto path{(fixture.directory / "world").string()};
            expect(world::create_world(path, 424242, "weather").has_value());
            std::vector<sim::weather_snapshot> expected{};
            sim::weather_snapshot saved{};
            {
                auto opened{sim::world::open(path)};
                expect(opened.has_value());
                if (!opened) {
                    return;
                }
                // A short rain timer makes the continuation cross a duration
                // resample, so restoring the weather derivation is observable
                // in the timer values and not only in the countdown.
                opened->clock().set_weather(true, false, 0, 4, 0);
                for (int tick{0}; tick < 2; ++tick) {
                    opened->tick();
                }
                auto flushed{opened->flush()};
                expect(flushed.has_value())
                    << (flushed ? "" : flushed.error().message);
                if (!flushed) {
                    return;
                }
                saved = opened->clock().weather();
                for (int tick{0}; tick < 120; ++tick) {
                    opened->tick();
                    expected.push_back(opened->clock().weather());
                }
            }
            auto reopened{sim::world::open(path)};
            expect(reopened.has_value())
                << (reopened ? "" : reopened.error().message);
            if (!reopened) {
                return;
            }
            const auto restored{reopened->clock().weather()};
            expect(restored.raining == saved.raining);
            expect(restored.thundering == saved.thundering);
            expect(restored.rain_time == saved.rain_time);
            expect(restored.thunder_time == saved.thunder_time);
            expect(restored.rain_level == saved.rain_level);
            expect(restored.thunder_level == saved.thunder_level);
            for (std::size_t index{0}; index < expected.size(); ++index) {
                reopened->tick();
                const auto actual{reopened->clock().weather()};
                expect(actual.raining == expected[index].raining) << index;
                expect(actual.thundering == expected[index].thundering)
                    << index;
                expect(actual.rain_time == expected[index].rain_time) << index;
                expect(actual.thunder_time == expected[index].thunder_time)
                    << index;
                expect(actual.rain_level == expected[index].rain_level)
                    << index;
                expect(actual.thunder_level == expected[index].thunder_level)
                    << index;
            }
        };

    "a world ticks its clock and restores it from stored data"_test = [] {
        world_fixture fixture{};
        const auto path{(fixture.directory / "world").string()};
        {
            auto created{world::create_world(path, 1234, "world")};
            expect(created.has_value());
            if (!created) {
                return;
            }
        }
        {
            auto opened{sim::world::open(path)};
            expect(opened.has_value());
            if (!opened) {
                return;
            }
            auto &runtime{*opened};
            expect(runtime.clock().game_time() == 0);
            for (auto index{0}; index < 30; ++index) {
                runtime.tick();
            }
            expect(runtime.clock().game_time() == 30);
            expect(runtime.clock().total_ticks() == 30);
            runtime.clock().set_total_ticks(6000);
            runtime.clock().set_weather(true, false, 0, 500, 0);
            auto flushed{runtime.flush()};
            expect(flushed.has_value())
                << (flushed ? "" : flushed.error().message);
        }
        auto reopened{sim::world::open(path)};
        expect(reopened.has_value());
        if (!reopened) {
            return;
        }
        auto &runtime{*reopened};
        expect(runtime.clock().game_time() == 30);
        expect(runtime.clock().total_ticks() == 6000);
        expect(runtime.clock().is_raining());
        expect(runtime.clock().weather().rain_time == 500);
        // The next tick consumes one tick of the restored rain timer.
        runtime.tick();
        expect(runtime.clock().weather().rain_time == 499);
    };

    "the persisted level age is readable through the level projection"_test =
        [] {
            world_fixture fixture{};
            const auto path{(fixture.directory / "world").string()};
            {
                auto created{world::create_world(path, 99, "world")};
                expect(created.has_value());
                if (!created) {
                    return;
                }
            }
            {
                auto opened{sim::world::open(path)};
                expect(opened.has_value());
                if (!opened) {
                    return;
                }
                for (auto index{0}; index < 7; ++index) {
                    opened->tick();
                }
                expect(opened->flush().has_value());
            }
            auto store{world::world_store::open(path)};
            expect(store.has_value());
            if (!store) {
                return;
            }
            auto projection{store->level()};
            expect(projection.has_value());
            if (projection) {
                expect(projection->time == 7);
            }
        };
};
