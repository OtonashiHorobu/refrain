// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/console_service.hpp>

#include <boost/ut.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace boost::ut;
using namespace refrain::server;

namespace {

[[nodiscard]] auto tick_info(std::int64_t index = 0)
    -> refrain::core::tick_info {
    return refrain::core::tick_info{
        .index = index,
        .actual = std::chrono::milliseconds{50},
        .sleep = std::chrono::milliseconds{0},
        .missed = 0,
    };
}

struct recorder {
    std::vector<std::string> lines{};
    console_service *service{nullptr};
    std::atomic<bool> *stop{nullptr};

    [[nodiscard]] auto handler() {
        return [this](std::string_view line, const refrain::core::tick_info &) {
            lines.emplace_back(line);
            if (line == "stop" && stop != nullptr) {
                stop->store(true, std::memory_order_release);
            }
            if (line == "again" && service != nullptr) {
                // A command that queues more work must wait for the next tick.
                service->push_command("later");
            }
        };
    }
};

} // namespace

suite<"server/console-service"> console_service_tests = [] {
    "queued lines are drained in order at the next tick"_test = [] {
        recorder record{};
        console_service console{record.handler()};
        std::atomic<bool> stop{false};

        console.push_command("first");
        console.push_text("second\nthird\r\nfourth");
        console.push_command("");
        console.tick(tick_info(), 0, stop);

        expect(record.lines.size() == 4U);
        expect(record.lines[0] == "first");
        expect(record.lines[1] == "second");
        expect(record.lines[2] == "third");
        expect(record.lines[3] == "fourth");
        console.tick(tick_info(1), 1, stop);
        expect(record.lines.size() == 4U) << "an empty tick drains nothing";
    };

    "a line queued while ticking runs on the following tick"_test = [] {
        recorder record{};
        console_service console{record.handler()};
        record.service = &console;
        std::atomic<bool> stop{false};

        console.push_command("again");
        console.tick(tick_info(), 0, stop);
        expect(record.lines.size() == 1U);
        expect(record.lines[0] == "again");

        console.tick(tick_info(1), 1, stop);
        expect(record.lines.size() == 2U);
        expect(record.lines[1] == "later");
    };

    "replay steps run on their tick index only"_test = [] {
        recorder record{};
        console_service console{record.handler()};
        std::atomic<bool> stop{false};
        console.load_replay({
            {.tick = 0, .command = "zero"},
            {.tick = 2, .command = "two"},
            {.tick = 2, .command = "two-b"},
            {.tick = 5, .command = "five"},
        });

        console.tick(tick_info(), 0, stop);
        console.tick(tick_info(1), 1, stop);
        console.tick(tick_info(2), 2, stop);
        console.tick(tick_info(3), 3, stop);

        expect(record.lines.size() == 3U);
        expect(record.lines[0] == "zero");
        expect(record.lines[1] == "two");
        expect(record.lines[2] == "two-b");
    };

    "a stop command ends the replay at that step"_test = [] {
        recorder record{};
        console_service console{record.handler()};
        std::atomic<bool> stop{false};
        record.stop = &stop;
        console.load_replay({
            {.tick = 0, .command = "stop"},
            {.tick = 0, .command = "after"},
        });

        console.tick(tick_info(), 0, stop);
        console.tick(tick_info(1), 1, stop);

        expect(record.lines.size() == 1U);
        expect(record.lines[0] == "stop");
        expect(stop.load(std::memory_order_acquire));
    };
};
