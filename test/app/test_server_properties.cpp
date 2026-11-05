// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/app/server_properties.hpp>

#include <boost/ut.hpp>

#include <string_view>

namespace {
using namespace boost::ut;

[[maybe_unused]] const suite tests = [] {
    "server-properties/defaults for empty input"_test = [] {
        const auto properties{refrain::app::parse_server_properties("")};
        expect(properties.has_value());
        if (properties) {
            expect(properties->level_name == "world");
            expect(properties->max_players == 20);
            expect(properties->view_distance == 10);
            expect(properties->online_mode);
        }
    };

    "server-properties/accepts =, :, comments and CRLF"_test = [] {
        constexpr std::string_view text{"# comment\r\n"
                                        "! another comment\r\n"
                                        "level-name: my-world\r\n"
                                        "max-players=42\r\n"
                                        "view-distance = 8\r\n"
                                        "simulation-distance:6\r\n"
                                        "online-mode=false\r\n"
                                        "allow-nether=false\r\n"
                                        "motd=Hello\\ World\\nSecond line\r\n"
                                        "unknown-key=ignored\r\n"};
        const auto properties{refrain::app::parse_server_properties(text)};
        expect(properties.has_value()) << "parse failed";
        if (properties) {
            expect(properties->level_name == "my-world");
            expect(properties->max_players == 42);
            expect(properties->view_distance == 8);
            expect(properties->simulation_distance == 6);
            expect(!properties->online_mode);
            expect(!properties->allow_nether);
            expect(properties->motd == "Hello World\nSecond line");
        }
    };

    "server-properties/rejects malformed assignments"_test = [] {
        expect(!refrain::app::parse_server_properties("just-a-key"));
        expect(!refrain::app::parse_server_properties("max-players=many"));
        expect(!refrain::app::parse_server_properties("online-mode=maybe"));
        expect(!refrain::app::parse_server_properties("=value"));
    };
};

} // namespace
