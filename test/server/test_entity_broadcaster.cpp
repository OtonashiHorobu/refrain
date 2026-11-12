// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/entity_broadcaster.hpp>

#include <refrain/protocol/current.hpp>

#include <boost/ut.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "support/fake_play_gateway.hpp"

using namespace boost::ut;
using namespace refrain::server;

namespace {

namespace net = refrain::net;
namespace session = refrain::session;
namespace sim = refrain::sim;
namespace support = refrain::test_support;

/// Reads the leading packet-id varint of an encoded body.
[[nodiscard]] auto packet_id(std::span<const std::byte> body) -> std::uint32_t {
    std::uint32_t value{0};
    std::uint32_t shift{0};
    for (const auto byte : body) {
        const auto current{std::to_integer<std::uint8_t>(byte)};
        value |= static_cast<std::uint32_t>(current & 0x7FU) << shift;
        if ((current & 0x80U) == 0U) {
            break;
        }
        shift += 7;
    }
    return value;
}

void join(session::play_session &players, std::uint64_t connection, double x,
          double z, std::string_view world = "world") {
    static_cast<void>(players.add_player(connection, net::uuid{}, "player",
                                         std::string{world}, x, 64.0, z, 1));
}

} // namespace

suite<"server/entity-broadcaster"> entity_broadcaster_tests = [] {
    "players in range spawn each other and send one initial position"_test =
        [] {
            sim::player_table table{};
            session::play_session players{table};
            join(players, 1, 0.0, 0.0);
            join(players, 2, 32.0, 0.0);
            support::fake_play_gateway gateway{};
            entity_broadcaster broadcaster{gateway, 16};

            broadcaster.tick(players);

            expect(gateway.sent.size() == 2U);
            for (const auto &sent : gateway.sent) {
                expect(packet_id(sent.body) ==
                       refrain::protocol::current::play::clientbound::
                           add_entity::packet_id);
                expect(sent.connection == 1U || sent.connection == 2U);
            }
        };

    "a second tick with no movement sends nothing"_test = [] {
        sim::player_table table{};
        session::play_session players{table};
        join(players, 1, 0.0, 0.0);
        join(players, 2, 32.0, 0.0);
        support::fake_play_gateway gateway{};
        entity_broadcaster broadcaster{gateway, 16};

        broadcaster.tick(players);
        gateway.sent.clear();
        broadcaster.tick(players);

        expect(gateway.sent.empty());
    };

    "players outside the tracking range never see each other"_test = [] {
        sim::player_table table{};
        session::play_session players{table};
        join(players, 1, 0.0, 0.0);
        join(players, 2, 1000.0, 0.0);
        support::fake_play_gateway gateway{};
        entity_broadcaster broadcaster{gateway, 2};

        broadcaster.tick(players);

        expect(gateway.sent.empty());
    };

    "a movement step reaches only the viewers tracking the mover"_test = [] {
        sim::player_table table{};
        session::play_session players{table};
        join(players, 1, 0.0, 0.0);
        join(players, 2, 32.0, 0.0);
        support::fake_play_gateway gateway{};
        entity_broadcaster broadcaster{gateway, 16};

        broadcaster.tick(players);
        gateway.sent.clear();
        static_cast<void>(
            players.update_position(1, 0.25, 64.0, 0.0, 0.0F, 0.0F));
        broadcaster.tick(players);

        expect(gateway.sent.size() == 1U);
        expect(gateway.sent[0].connection == 2U)
            << "the mover does not receive its own position";
        expect(packet_id(gateway.sent[0].body) ==
               refrain::protocol::current::play::clientbound::move_entity_pos::
                   packet_id);
    };

    "leaving removes the entity before the tab entry"_test = [] {
        sim::player_table table{};
        session::play_session players{table};
        join(players, 1, 0.0, 0.0);
        join(players, 2, 32.0, 0.0);
        support::fake_play_gateway gateway{};
        entity_broadcaster broadcaster{gateway, 16};

        broadcaster.tick(players);
        gateway.sent.clear();
        const auto leaving{*players.player_of(2)};
        static_cast<void>(players.remove_player(2));
        broadcaster.left(players, leaving);

        expect(gateway.sent.size() == 2U);
        expect(gateway.sent[0].connection == 1U);
        expect(packet_id(gateway.sent[0].body) ==
               refrain::protocol::current::play::clientbound::remove_entities::
                   packet_id);
        expect(gateway.sent[1].connection == 1U);
        expect(packet_id(gateway.sent[1].body) ==
               refrain::protocol::current::play::clientbound::
                   player_info_remove::packet_id);
    };

    "an attack sends health, hurt and one animation to the target"_test = [] {
        sim::player_table table{};
        session::play_session players{table};
        join(players, 1, 0.0, 0.0);
        join(players, 2, 32.0, 0.0);
        support::fake_play_gateway gateway{};
        entity_broadcaster broadcaster{gateway, 16};

        broadcaster.attack(
            players, net::attack_event{.connection = 1, .target_entity = 2});

        expect(gateway.sent.size() == 3U);
        expect(gateway.sent[0].connection == 2U);
        expect(packet_id(gateway.sent[0].body) ==
               refrain::protocol::current::play::clientbound::set_health::
                   packet_id);
        expect(packet_id(gateway.sent[1].body) ==
               refrain::protocol::current::play::clientbound::hurt_animation::
                   packet_id);
        expect(gateway.sent[2].connection == 2U);
        expect(
            packet_id(gateway.sent[2].body) ==
            refrain::protocol::current::play::clientbound::animate::packet_id);
        expect(players.player_of(2)->health == 19.0F);
    };

    "play ready sends health then experience"_test = [] {
        sim::player_table table{};
        session::play_session players{table};
        join(players, 1, 0.0, 0.0);
        support::fake_play_gateway gateway{};
        entity_broadcaster broadcaster{gateway, 16};

        broadcaster.play_ready(players, 1);

        expect(gateway.sent.size() == 2U);
        expect(packet_id(gateway.sent[0].body) ==
               refrain::protocol::current::play::clientbound::set_health::
                   packet_id);
        expect(packet_id(gateway.sent[1].body) ==
               refrain::protocol::current::play::clientbound::set_experience::
                   packet_id);
    };
};
