// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Network defaults can come from Java `server.properties`: a key absent from
// the network namespace uses the fallback supplied by the host.

#include "test_protocol.hpp"

#include <refrain/core/config.hpp>
#include <refrain/net/bot.hpp>
#include <refrain/net/server.hpp>
#include <refrain/protocol/current.hpp>

#include "support/scratch_directory.hpp"

#include <boost/ut.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace net = refrain::net;
namespace play = refrain::protocol::current::play::clientbound;

suite config_fallback_tests = [] {
    "net/server.properties fills keys missing from network.toml"_test = [] {
        const refrain::test_support::scratch_directory scratch;
        const auto dir = scratch.path();
        {
            std::ofstream stream{dir / "network.toml",
                                 std::ios::binary | std::ios::trunc};
            stream << "bind_host = \"127.0.0.1\"\nport = 0\n"
                      "online_mode = false\n";
        }
        auto store{refrain::core::config::store::empty()};
        expect(static_cast<bool>(store.mount(
            {.name = "network", .path = (dir / "network.toml").string()})));
        expect(static_cast<bool>(store.load_all()));
        auto view{store.module("network")};
        expect(static_cast<bool>(view));
        if (!view) {
            return;
        }
        net::server_inputs inputs{};
        inputs.protocol = refrain::test::make_test_protocol();
        inputs.fallback_simulation_distance = 3;
        inputs.fallback_secure_chat = false;
        auto running{net::server::start(std::move(inputs), *view)};
        expect(running.has_value());
        if (!running) {
            return;
        }
        auto report{net::run_bot(net::bot_options{
            .target = {.host = "127.0.0.1", .port = (*running)->local_port()},
            .username = "Fallback",
            .timeout = std::chrono::milliseconds{5000},
            .capture = true,
        })};
        expect(report.has_value());
        if (report) {
            const auto packet{std::ranges::find_if(
                report->capture, [](const net::captured_packet &entry) {
                    return entry.state == 2 && entry.id == 50;
                })};
            expect(packet != report->capture.end());
            if (packet != report->capture.end()) {
                net::reader reader{packet->body};
                auto id{reader.read_varint()};
                expect(id.has_value() && *id == 50);
                play::login login{};
                auto decoded{play::decode_login(reader, login)};
                expect(static_cast<bool>(decoded));
                expect(login.simulation_distance == 3);
                expect(!login.enforces_secure_chat);
            }
        }
        (*running)->stop();
    };
};

} // namespace
