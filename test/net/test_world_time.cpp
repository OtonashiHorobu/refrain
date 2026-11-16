// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The `set_time` encoding the login burst and the periodic sync share: game
// time plus the overworld clock state the client interpolates the day-night
// cycle from.

#include <refrain/net/server.hpp>
#include <refrain/net/wire.hpp>
#include <refrain/protocol/current.hpp>

#include <boost/ut.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace net = refrain::net;
namespace play_cb = refrain::protocol::current::play::clientbound;

} // namespace

suite<"net/world-time"> world_time_tests = [] {
    "the world time packet carries the game time and the clock state"_test =
        [] {
            net::world_state_snapshot state{};
            state.game_time = 12'345;
            state.clock_total_ticks = 6'000;
            state.clock_partial_tick = 0.25F;
            state.clock_rate = 1.0F;
            auto body{net::encode_world_time(state, 7)};
            expect(body.has_value());
            if (!body) {
                return;
            }
            net::reader in{*body};
            auto packet_id{in.read_varint()};
            expect(packet_id.has_value());
            if (!packet_id) {
                return;
            }
            expect(*packet_id == play_cb::set_time::packet_id);
            play_cb::set_time decoded{};
            auto ok{play_cb::decode_set_time(in, decoded)};
            expect(ok.has_value());
            if (!ok) {
                return;
            }
            expect(decoded.game_time == 12'345);
            expect(decoded.clock_updates.size() == 1U);
            if (decoded.clock_updates.size() != 1U) {
                return;
            }
            const auto &clock{decoded.clock_updates.front()};
            expect(clock.clock == 7U);
            expect(clock.total_ticks == 6'000);
            expect(clock.partial_tick == 0.25F);
            expect(clock.rate == 1.0F);
            expect(in.empty());
        };

    "a paused clock is encoded with a zero rate"_test = [] {
        net::world_state_snapshot state{};
        state.game_time = 42;
        state.clock_total_ticks = 18'000;
        state.clock_rate = 0.0F;
        auto body{net::encode_world_time(state, 0)};
        expect(body.has_value());
        if (!body) {
            return;
        }
        net::reader in{*body};
        static_cast<void>(in.read_varint());
        play_cb::set_time decoded{};
        expect(play_cb::decode_set_time(in, decoded).has_value());
        expect(decoded.clock_updates.size() == 1U);
        if (decoded.clock_updates.size() == 1U) {
            expect(decoded.clock_updates.front().rate == 0.0F);
        }
    };
};
