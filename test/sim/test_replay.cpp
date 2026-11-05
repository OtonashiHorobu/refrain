// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/sim/replay.hpp>

#include <boost/ut.hpp>

#include <string>

namespace {
using namespace boost::ut;

[[maybe_unused]] const suite tests = [] {
    "sim/replay parses tick-indexed commands"_test = [] {
        constexpr std::string_view text{"# comment\r\n"
                                        "\r\n"
                                        "0 move 1 0 0\r\n"
                                        "5 dig 64 73 -48\r\n"
                                        "10 place 63 73 -48 minecraft:dirt\r\n"
                                        "20 hash\r\n"};
        const auto steps{refrain::sim::parse_replay_script(text)};
        expect(steps.has_value()) << "parse failed";
        if (steps) {
            expect(steps->size() == 4U);
            expect((*steps)[0].tick == 0);
            expect((*steps)[0].command == "move 1 0 0");
            expect((*steps)[2].tick == 10);
            expect((*steps)[3].command == "hash");
        }
    };

    "sim/replay rejects malformed or descending ticks"_test = [] {
        expect(!refrain::sim::parse_replay_script("no-tick-command"));
        expect(!refrain::sim::parse_replay_script("x move"));
        expect(!refrain::sim::parse_replay_script("-1 move"));
        expect(
            !refrain::sim::parse_replay_script("5 move 1 0 0\n4 move 0 0 0\n"));
        expect(!refrain::sim::parse_replay_script("0\n"));
    };
};

} // namespace
