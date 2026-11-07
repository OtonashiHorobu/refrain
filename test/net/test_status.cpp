// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/net/ping.hpp>
#include <refrain/net/server.hpp>
#include <refrain/net/status.hpp>

#include <boost/ut.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using namespace boost::ut;

namespace net = refrain::net;

const suite tests = [] {
    "net/status JSON round trips its fields"_test = [] {
        net::status_info info{};
        info.description = "A Refrain Server";
        info.max_players = 20;
        info.online_players = 3;
        info.sample.push_back(net::status_sample{
            .name = "Dev",
            .id = *net::uuid::parse("12345678-90ab-cdef-8123-456789abcdef")});
        info.enforces_secure_chat = true;

        const auto json{net::build_status_json(info)};
        auto parsed{net::parse_status_json(json)};
        expect(parsed.has_value());
        if (!parsed) {
            return;
        }
        expect(parsed->description == info.description);
        expect(parsed->max_players == info.max_players);
        expect(parsed->online_players == info.online_players);
        expect(parsed->sample.size() == 1U);
        expect(parsed->sample.front().name == "Dev");
        expect(parsed->enforces_secure_chat);
        expect(parsed->protocol == 777);
        expect(parsed->version_name == "26.3");
    };

    "net/handshake intention round trips"_test = [] {
        const net::handshake_intention original{
            .protocol = 777, .host = "localhost", .port = 25565, .next = 1};
        const auto encoded{net::encode_handshake(original)};
        expect(encoded.has_value());
        if (!encoded) {
            return;
        }
        const auto decoded{net::decode_handshake(*encoded)};
        expect(decoded.has_value());
        if (!decoded) {
            return;
        }
        expect(decoded->protocol == original.protocol);
        expect(decoded->host == original.host);
        expect(decoded->port == original.port);
        expect(decoded->next == original.next);
    };

    "net/status server answers a ping"_test = [] {
        net::server_config config{};
        config.bind = net::endpoint{.host = "127.0.0.1", .port = 0};
        config.status.description = "Refrain test";
        config.status.max_players = 7;
        config.status.online_players = 1;

        auto running{net::server::start(config)};
        expect(running.has_value());
        if (!running) {
            return;
        }
        const auto port{(*running)->local_port()};
        expect(port != 0);

        auto result{net::ping(net::endpoint{.host = "127.0.0.1", .port = port},
                              std::chrono::milliseconds{5000})};
        expect(result.has_value());
        if (result) {
            expect(result->status.description == "Refrain test");
            expect(result->status.max_players == 7);
            expect(result->status.online_players == 1);
        }
        (*running)->stop();
    };
};

} // namespace
