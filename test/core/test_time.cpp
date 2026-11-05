// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/time.hpp>

#include <boost/ut.hpp>

#include <chrono>
#include <cstdint>
#include <thread>

namespace {
using namespace boost::ut;
using namespace std::chrono_literals;

[[maybe_unused]] const suite tests = [] {
    "now_ns is monotonic"_test = [] {
        const auto first = refrain::core::now_ns();
        const auto second = refrain::core::now_ns();
        expect(second >= first);
    };

    "tick_clock advances at the requested rate"_test = [] {
        refrain::core::tick_clock ticker{
            200}; // 5 ms per tick keeps the test fast
        expect(!ticker.running());

        ticker.start();
        expect(ticker.running());
        expect(ticker.current_tick() == -1);

        const auto begin = refrain::core::now_ns();
        const auto first = ticker.wait_next();
        const auto second = ticker.wait_next();
        const auto third = ticker.wait_next();
        const auto elapsed = refrain::core::now_ns() - begin;

        expect(static_cast<bool>(first));
        expect(static_cast<bool>(second));
        expect(static_cast<bool>(third));
        if (first && second && third) {
            expect(first->index == 0);
            expect(second->index == 1);
            expect(third->index == 2);
            expect(third->missed == 0);
        }
        expect(ticker.current_tick() == 2);
        expect(elapsed >= 10'000'000); // at least two full 5 ms intervals

        ticker.stop();
        expect(!ticker.running());
        expect(ticker.tps() == 200);
    };

    "tick_clock with 20 tps reports the default rate"_test = [] {
        const refrain::core::tick_clock ticker{};
        expect(ticker.tps() == 20);
        expect(refrain::core::ticks_per_second == 20);
        expect(refrain::core::tick_milliseconds == 50);
    };

    // A tps value above 1e9 must clamp the interval to 1 ns instead of
    // yielding a zero interval and dividing by zero in wait_next() (SIGFPE).
    "tick_clock clamps sub-nanosecond periods instead of dividing by zero"_test =
        [] {
            refrain::core::tick_clock ticker{2'000'000'000};
            expect(ticker.tps() == 1'000'000'000); // effective, clamped rate
            ticker.start();
            const auto info = ticker.wait_next(); // must not raise SIGFPE
            expect(static_cast<bool>(info));
            if (info) {
                // A 1 ns period is effectively "always behind"; the counter may
                // skip ticks, but it must never divide by zero.
                expect(info->index >= 0);
                expect(info->missed >= 0);
            }
            ticker.stop();

            // Boundary: 1'000'000'001 tps also rounds down to a zero interval.
            refrain::core::tick_clock boundary{1'000'000'001};
            expect(boundary.tps() == 1'000'000'000);
            boundary.start();
            expect(static_cast<bool>(boundary.wait_next()));
            boundary.stop();
        };

    // wait_next() before start() must report not_initialized instead of
    // fabricating tick index 0.
    "tick_clock wait_next before start reports not_initialized"_test = [] {
        refrain::core::tick_clock ticker{1000};
        const auto info = ticker.wait_next();
        expect(!info);
        if (!info) {
            expect(info.error().code == refrain::error_code::not_initialized);
        }
        expect(ticker.current_tick() == -1);
    };

    // When the caller is late the clock reports the skipped ticks and the
    // logical counter jumps accordingly.
    "tick_clock reports missed ticks when the caller runs behind"_test = [] {
        refrain::core::tick_clock ticker{1'000'000}; // 1 us period
        ticker.start();
        std::this_thread::sleep_for(5ms); // ~5000 periods late

        const auto info = ticker.wait_next();
        expect(static_cast<bool>(info));
        if (info) {
            expect(info->missed >= 1);
            expect(info->index == info->missed);
        }
        expect(ticker.current_tick() >= 1);
        ticker.stop();
    };
};

} // namespace
