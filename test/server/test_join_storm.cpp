// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// A join storm must cost one player-list update per recipient per cohort, not
// one per join and recipient, and must hand the transport no more than the
// per-tick fan-out budget. The tests drive the broadcaster's lifecycle flush
// with synthetic joins over a table that grows to a thousand players and pin
// the per-tick fan-out; the soak run measures the wall clock on the benchmark
// host.

#include <refrain/net/base.hpp>
#include <refrain/server/entity_broadcaster.hpp>
#include <refrain/session/play_session.hpp>

#include <boost/ut.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "support/counting_play_gateway.hpp"

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace net = refrain::net;
namespace session = refrain::session;
namespace support = refrain::test_support;

constexpr std::size_t player_count{1000};

[[nodiscard]] auto profile_id(std::uint64_t connection) -> net::uuid {
    net::uuid value{};
    value.bytes[0] = static_cast<std::uint8_t>(connection & 0xFFU);
    value.bytes[1] = static_cast<std::uint8_t>((connection >> 8U) & 0xFFU);
    return value;
}

void add(session::play_session &players, std::uint64_t connection) {
    const auto added{players.add_player(connection, profile_id(connection),
                                        "bot" + std::to_string(connection),
                                        "world", 0.0, 64.0, 0.0, 1)};
    expect(static_cast<bool>(added));
}

suite<"server/join_storm"> join_storm_tests = [] {
    "a join storm fans out one cohort update per tick"_test = [] {
        // Seven hundred players are already in play when the storm starts; the
        // next three hundred arrive over five ticks.
        constexpr std::size_t established{700};
        constexpr std::size_t cohort{60};
        constexpr std::size_t storm_ticks{5};
        constexpr auto wall_bound{std::chrono::milliseconds{200}};

        session::play_session players{};
        for (std::uint64_t connection{1}; connection <= established;
             ++connection) {
            add(players, connection);
        }
        support::counting_play_gateway gateway{};
        refrain::server::entity_broadcaster broadcaster{gateway, 16};

        std::uint64_t next_connection{established + 1};
        std::size_t expected_packets{0};
        std::size_t expected_entries{0};
        std::size_t total_packets{0};
        std::size_t total_entries{0};
        for (std::size_t tick{0}; tick < storm_ticks; ++tick) {
            for (std::size_t index{0}; index < cohort; ++index) {
                add(players, next_connection);
                broadcaster.joined(next_connection);
                ++next_connection;
            }
            const auto connected{established + cohort * (tick + 1)};
            expected_packets += connected;
            expected_entries +=
                cohort * connected + (connected - cohort) * cohort;

            gateway.reset();
            const auto started{std::chrono::steady_clock::now()};
            broadcaster.flush_lifecycle(players);
            const auto elapsed{std::chrono::steady_clock::now() - started};

            // One update per joiner with the whole list and one update per
            // established player with the cohort; the body is encoded once
            // and the per-tick byte budget bounds what reaches the transport.
            expect(gateway.info_packets <= 2 * connected);
            expect(gateway.info_entries <=
                   cohort * connected + (connected - cohort) * cohort);
            expect(gateway.other_packets == 0U);
            expect(gateway.malformed == 0U);
            expect(
                gateway.bytes <=
                refrain::server::entity_broadcaster::max_info_bytes_per_tick +
                    connected * 64U);
            // A tripwire for an order-of-magnitude regression; the
            // deterministic bound above is what the change is measured by.
            expect(elapsed < wall_bound);
            total_packets += gateway.info_packets;
            total_entries += gateway.info_entries;
        }
        // The queued fan-out drains over the following ticks. Every player
        // receives exactly one update per cohort tick, so the totals match the
        // fan-out of the storm with nothing dropped or announced twice.
        for (std::size_t tick{0}; tick < 128; ++tick) {
            gateway.reset();
            broadcaster.flush_lifecycle(players);
            if (gateway.info_packets == 0U) {
                break;
            }
            total_packets += gateway.info_packets;
            total_entries += gateway.info_entries;
        }
        expect(gateway.info_packets == 0U);
        expect(total_packets == expected_packets);
        expect(total_entries == expected_entries);
    };

    "a connection that leaves before its flush is not announced"_test = [] {
        session::play_session players{};
        add(players, 1);
        add(players, 2);
        support::counting_play_gateway gateway{};
        refrain::server::entity_broadcaster broadcaster{gateway, 16};

        broadcaster.joined(2);
        const auto leaving{*players.player_of(2)};
        static_cast<void>(players.remove_player(2));
        broadcaster.left(leaving);
        broadcaster.flush_lifecycle(players);

        // The join was never announced, so the departure is silent: neither
        // the cohort nor the removal reaches the player that stayed.
        expect(gateway.info_packets == 0U);
        expect(gateway.info_remove_packets == 0U);
        expect(gateway.malformed == 0U);
    };
};

} // namespace
