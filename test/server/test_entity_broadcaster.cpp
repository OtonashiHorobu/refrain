// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/entity_broadcaster.hpp>

#include <refrain/protocol/current.hpp>
#include <refrain/server/entity_broadcast.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "support/counting_play_gateway.hpp"
#include "support/fake_play_gateway.hpp"

using namespace boost::ut;
using namespace refrain::server;

namespace {

namespace net = refrain::net;
namespace session = refrain::session;
namespace sim = refrain::sim;
namespace support = refrain::test_support;
using support::counting_play_gateway;
namespace play_clientbound = refrain::protocol::current::play::clientbound;

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

/// Packet classes the broadcaster emits from `tick`.
enum class tick_packet {
    spawn,
    remove,
    teleport,
    position,
    position_rotation,
    rotation,
};

struct emitted_packet {
    tick_packet kind{tick_packet::spawn};
    std::uint32_t entity_id{0};
    friend auto operator==(const emitted_packet &, const emitted_packet &)
        -> bool = default;
};

[[nodiscard]] auto movement_of(const session::player &value)
    -> net::movement_event {
    return net::movement_event{
        .connection = value.connection,
        .x = value.x,
        .y = value.y,
        .z = value.z,
        .yaw = value.yaw,
        .pitch = value.pitch,
        .on_ground = value.on_ground,
    };
}

[[nodiscard]] auto tick_kind(entity_move_kind kind) -> tick_packet {
    switch (kind) {
    case entity_move_kind::teleport:
        return tick_packet::teleport;
    case entity_move_kind::position:
        return tick_packet::position;
    case entity_move_kind::position_rotation:
        return tick_packet::position_rotation;
    case entity_move_kind::rotation:
        return tick_packet::rotation;
    case entity_move_kind::none:
        break;
    }
    return tick_packet::teleport;
}

/// Decode every packet a tick appended to one connection's buffer.
[[nodiscard]] auto decode_tick_body(std::span<const std::byte> body,
                                    std::vector<emitted_packet> &out) -> bool {
    net::reader in{body};
    while (!in.empty()) {
        const auto id{in.read_varint()};
        if (!id) {
            return false;
        }
        switch (*id) {
        case play_clientbound::add_entity::packet_id: {
            play_clientbound::add_entity value{};
            if (!play_clientbound::decode_add_entity(in, value)) {
                return false;
            }
            out.push_back(
                {tick_packet::spawn, static_cast<std::uint32_t>(value.id)});
            break;
        }
        case play_clientbound::remove_entities::packet_id: {
            play_clientbound::remove_entities value{};
            if (!play_clientbound::decode_remove_entities(in, value)) {
                return false;
            }
            for (const auto entity : value.entity_ids) {
                out.push_back({tick_packet::remove, entity});
            }
            break;
        }
        case play_clientbound::teleport_entity::packet_id: {
            play_clientbound::teleport_entity value{};
            if (!play_clientbound::decode_teleport_entity(in, value)) {
                return false;
            }
            out.push_back(
                {tick_packet::teleport, static_cast<std::uint32_t>(value.id)});
            break;
        }
        case play_clientbound::move_entity_pos::packet_id: {
            play_clientbound::move_entity_pos value{};
            if (!play_clientbound::decode_move_entity_pos(in, value)) {
                return false;
            }
            out.push_back({tick_packet::position, value.entity_id});
            break;
        }
        case play_clientbound::move_entity_pos_rot::packet_id: {
            play_clientbound::move_entity_pos_rot value{};
            if (!play_clientbound::decode_move_entity_pos_rot(in, value)) {
                return false;
            }
            out.push_back({tick_packet::position_rotation, value.entity_id});
            break;
        }
        case play_clientbound::move_entity_rot::packet_id: {
            play_clientbound::move_entity_rot value{};
            if (!play_clientbound::decode_move_entity_rot(in, value)) {
                return false;
            }
            out.push_back({tick_packet::rotation, value.entity_id});
            break;
        }
        default:
            return false;
        }
    }
    return true;
}

[[nodiscard]] auto decode_gateway(const support::fake_play_gateway &gateway)
    -> std::unordered_map<std::uint64_t, std::vector<emitted_packet>> {
    std::unordered_map<std::uint64_t, std::vector<emitted_packet>> streams;
    for (const auto &sent : gateway.sent) {
        const bool decoded{
            decode_tick_body(sent.body, streams[sent.connection])};
        expect(decoded);
    }
    return streams;
}

using visible_relation = std::vector<std::unordered_set<std::uint32_t>>;

/// The visibility relation of one tick, evaluated pair by pair with the rule
/// itself and no index.
[[nodiscard]] auto
brute_force_relation(const std::vector<const session::player *> &ordered,
                     std::int32_t tracking_chunks) -> visible_relation {
    visible_relation relation(ordered.size());
    for (std::size_t viewer{0}; viewer < ordered.size(); ++viewer) {
        const auto *source{ordered[viewer]};
        const visibility_query query{
            .world = source->world,
            .x = source->x,
            .z = source->z,
            .view_distance = source->view_distance,
        };
        for (const auto *other : ordered) {
            if (other->connection == source->connection) {
                continue;
            }
            if (player_visible(query, other->world, other->x, other->z,
                               tracking_chunks)) {
                relation[viewer].insert(other->entity_id);
            }
        }
    }
    return relation;
}

struct oracle_result {
    std::vector<std::vector<emitted_packet>> streams{};
    visible_relation relation{};
};

/// The packets one tick must emit, derived from the brute-force relation.
[[nodiscard]] auto brute_force_tick(
    const std::vector<const session::player *> &ordered,
    std::int32_t tracking_chunks, const visible_relation &was,
    std::unordered_map<std::uint64_t, net::movement_event> &last_moves)
    -> oracle_result {
    oracle_result result{};
    result.streams.resize(ordered.size());
    result.relation = brute_force_relation(ordered, tracking_chunks);

    for (std::size_t viewer{0}; viewer < ordered.size(); ++viewer) {
        std::vector<std::uint32_t> removals;
        std::vector<std::uint32_t> additions;
        for (const auto *other : ordered) {
            if (other == ordered[viewer]) {
                continue;
            }
            const bool was_visible{was[viewer].contains(other->entity_id)};
            const bool is_visible{
                result.relation[viewer].contains(other->entity_id)};
            if (was_visible && !is_visible) {
                removals.push_back(other->entity_id);
            } else if (!was_visible && is_visible) {
                additions.push_back(other->entity_id);
            }
        }
        std::sort(removals.begin(), removals.end());
        for (const auto entity : removals) {
            result.streams[viewer].push_back({tick_packet::remove, entity});
        }
        for (const auto entity : additions) {
            result.streams[viewer].push_back({tick_packet::spawn, entity});
        }
    }

    for (std::size_t mover{0}; mover < ordered.size(); ++mover) {
        const auto *source{ordered[mover]};
        const auto current{movement_of(*source)};
        const auto previous{last_moves.find(source->connection)};
        if (previous != last_moves.end() && previous->second.x == current.x &&
            previous->second.y == current.y &&
            previous->second.z == current.z &&
            previous->second.yaw == current.yaw &&
            previous->second.pitch == current.pitch) {
            continue;
        }
        entity_move move{};
        if (previous == last_moves.end()) {
            move.kind = entity_move_kind::teleport;
        } else {
            move = choose_entity_move(previous->second, current);
        }
        last_moves[source->connection] = current;
        if (move.kind == entity_move_kind::none) {
            continue;
        }
        for (std::size_t viewer{0}; viewer < ordered.size(); ++viewer) {
            if (viewer == mover ||
                !result.relation[viewer].contains(source->entity_id)) {
                continue;
            }
            result.streams[viewer].push_back(
                {tick_kind(move.kind), source->entity_id});
        }
    }
    return result;
}

void expect_streams(
    const std::unordered_map<std::uint64_t, std::vector<emitted_packet>>
        &decoded,
    const std::vector<std::vector<emitted_packet>> &expected,
    const std::vector<const session::player *> &ordered) {
    const std::vector<emitted_packet> empty{};
    for (std::size_t position{0}; position < ordered.size(); ++position) {
        const auto found{decoded.find(ordered[position]->connection)};
        const auto &actual{found == decoded.end() ? empty : found->second};
        expect(actual == expected[position])
            << "connection " << ordered[position]->connection;
    }
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

            // Visibility and movement are separate sends; the contract is the
            // packet stream of each connection, not the frame boundaries.
            const auto streams{decode_gateway(gateway)};
            expect(streams.size() == 2U);
            const auto *first{players.player_of(1)};
            const auto *second{players.player_of(2)};
            expect(first != nullptr);
            expect(second != nullptr);
            if (first == nullptr || second == nullptr) {
                return;
            }
            const auto expected_for = [](std::uint32_t other) {
                return std::vector<emitted_packet>{
                    {tick_packet::spawn, other},
                    {tick_packet::teleport, other},
                };
            };
            expect(streams.at(1) == expected_for(second->entity_id));
            expect(streams.at(2) == expected_for(first->entity_id));
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
        broadcaster.left(leaving);
        broadcaster.flush_lifecycle(players);

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

    "the indexed tick emits the same packets as a brute-force scan"_test = [] {
        constexpr std::uint64_t player_count{60};
        constexpr std::int32_t tracking_chunks{6};
        std::mt19937 engine{0xC0FFEEU};
        std::uniform_real_distribution<double> position{-500.0, 500.0};
        std::uniform_int_distribution<std::int32_t> view_distance{2, 12};
        std::uniform_int_distribution<std::size_t> world_index{0, 2};
        const std::array<std::string, 3> worlds{"overworld", "nether", "end"};

        session::play_session players{};
        for (std::uint64_t connection{1}; connection <= player_count;
             ++connection) {
            const auto index{static_cast<std::size_t>(connection - 1)};
            auto x{position(engine)};
            auto z{position(engine)};
            std::string world{worlds[world_index(engine)]};
            // Put every third player next to an earlier one so the windows
            // hold more than a single entity.
            if (index >= 3 && index % 3 == 0) {
                std::uniform_int_distribution<std::uint64_t> anchor_index{
                    1, connection - 1};
                const auto *anchor{players.player_of(anchor_index(engine))};
                x = anchor->x + position(engine) / 50.0;
                z = anchor->z + position(engine) / 50.0;
                world = anchor->world;
            }
            join(players, connection, x, z, world);
            static_cast<void>(
                players.set_view_distance(connection, view_distance(engine)));
        }

        std::vector<const session::player *> ordered;
        players.for_each([&ordered](const session::player &value) {
            ordered.push_back(&value);
        });
        std::sort(
            ordered.begin(), ordered.end(),
            [](const session::player *left, const session::player *right) {
                return left->entity_id < right->entity_id;
            });
        expect(ordered.size() == player_count);

        support::fake_play_gateway gateway{};
        entity_broadcaster broadcaster{gateway, tracking_chunks};
        broadcaster.tick(players);

        visible_relation first_relation(ordered.size());
        std::unordered_map<std::uint64_t, net::movement_event> last_moves{};
        const auto first{brute_force_tick(ordered, tracking_chunks,
                                          first_relation, last_moves)};
        const auto decoded_first{decode_gateway(gateway)};
        expect_streams(decoded_first, first.streams, ordered);

        gateway.sent.clear();
        std::uniform_int_distribution<int> motion{0, 3};
        for (const auto *value : ordered) {
            const auto roll{motion(engine)};
            if (roll == 0) {
                continue;
            }
            double x{value->x};
            double z{value->z};
            float yaw{value->yaw};
            const float pitch{value->pitch};
            if (roll == 1) {
                x += 17.0;
                z -= 17.0;
                yaw += 0.25F;
            } else if (roll == 2) {
                x -= 17.0;
                yaw += 0.5F;
            } else {
                x += position(engine) * 2.0;
                z += position(engine) * 2.0;
            }
            static_cast<void>(players.update_position(value->connection, x,
                                                      64.0, z, yaw, pitch));
        }

        const auto second{brute_force_tick(ordered, tracking_chunks,
                                           first.relation, last_moves)};
        broadcaster.tick(players);
        const auto decoded_second{decode_gateway(gateway)};
        expect_streams(decoded_second, second.streams, ordered);

        std::size_t transitions{0};
        std::size_t movements{0};
        for (const auto *streams : {&decoded_first, &decoded_second}) {
            for (const auto &[connection, stream] : *streams) {
                static_cast<void>(connection);
                for (const auto &packet : stream) {
                    if (packet.kind == tick_packet::spawn ||
                        packet.kind == tick_packet::remove) {
                        ++transitions;
                    } else {
                        ++movements;
                    }
                }
            }
        }
        expect(transitions > 0U) << "the run must exercise spawns or removals";
        expect(movements > 0U) << "the run must exercise movement packets";
    };
    "a dense visibility sweep is bounded per tick and still completes"_test =
        [] {
            // Every player stands on the same spot, so the relation is
            // O(players^2): the sweep must spread it over ticks instead of
            // walking it in one.
            constexpr std::uint64_t player_count{500};
            constexpr std::int32_t tracking_chunks{32};
            session::play_session players{};
            for (std::uint64_t connection{1}; connection <= player_count;
                 ++connection) {
                join(players, connection, 0.0, 0.0);
            }
            counting_play_gateway gateway{};
            entity_broadcaster broadcaster{gateway, tracking_chunks};

            constexpr std::size_t pairs{player_count * (player_count - 1)};
            constexpr std::size_t budget{
                entity_broadcaster::max_visibility_pairs_per_tick};
            constexpr std::size_t spawn_budget{
                entity_broadcaster::max_spawns_per_tick};
            std::size_t spawns{0};
            std::size_t ticks{0};
            while (ticks == 0 || gateway.spawn_packets > 0) {
                gateway.reset();
                broadcaster.tick(players);
                spawns += gateway.spawn_packets;
                // One tick may overrun a budget by the work of the viewer it
                // was processing when the budget ran out.
                expect(gateway.spawn_packets <= spawn_budget + player_count);
                expect(gateway.spawn_packets <= budget + player_count);
                expect(gateway.malformed == 0U);
                ++ticks;
                expect(ticks < 32U) << "the sweep must make progress";
            }
            expect(spawns == pairs);
            // The whole relation cannot fit in one tick's pair budget.
            expect(ticks >= (pairs + budget - 1) / budget);
        };

    "a dense cohort stays inside the per-tick bound and idles in between"_test =
        [] {
            // Every player stands on the same spot, so the relation is
            // O(players^2). The initial fill must respect the per-tick spawn
            // bound, a tick with no change must do no visibility work, and a
            // step inside the chunk must reach every viewer.
            constexpr std::uint64_t player_count{200};
            constexpr std::int32_t tracking_chunks{32};
            session::play_session players{};
            for (std::uint64_t connection{1}; connection <= player_count;
                 ++connection) {
                join(players, connection, 0.0, 0.0);
            }
            counting_play_gateway gateway{};
            entity_broadcaster broadcaster{gateway, tracking_chunks};

            std::size_t ticks{0};
            do {
                gateway.reset();
                broadcaster.tick(players);
                expect(gateway.spawn_packets <=
                       entity_broadcaster::max_spawns_per_tick + player_count);
                expect(gateway.malformed == 0U);
                expect(gateway.other_packets == 0U);
                ++ticks;
                expect(ticks < 64U) << "the dense fill must complete";
            } while (gateway.spawn_packets > 0);
            expect(ticks > 1U) << "the dense fill must be spread over ticks";

            // No player changed chunk or view distance, so the incremental
            // pass has nothing to do; only a periodic reconcile would run, and
            // it is not due.
            gateway.reset();
            broadcaster.tick(players);
            expect(gateway.spawn_packets == 0U);
            expect(gateway.remove_packets == 0U);
            expect(gateway.move_packets == 0U);
            expect(gateway.bytes == 0U);

            // A step that stays inside the chunk changes no visibility pair;
            // the movement fan-out must still reach every other player.
            for (std::uint64_t connection{1}; connection <= player_count;
                 ++connection) {
                const auto *value{players.player_of(connection)};
                static_cast<void>(players.update_position(
                    connection, value->x + 1.0, value->y, value->z, value->yaw,
                    value->pitch));
            }
            gateway.reset();
            broadcaster.tick(players);
            expect(gateway.spawn_packets == 0U);
            expect(gateway.remove_packets == 0U);
            expect(gateway.move_packets == player_count * (player_count - 1));
            expect(gateway.malformed == 0U);
        };
};
