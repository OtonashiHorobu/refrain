// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/entity_broadcaster.hpp>

#include <refrain/protocol/current.hpp>
#include <refrain/server/entity_broadcast.hpp>
#include <refrain/server/player_visibility.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace refrain::server {

namespace {

constexpr std::uint8_t player_info_add{1U};
constexpr std::uint8_t player_info_game_mode{4U};
constexpr std::uint8_t player_info_listed{8U};
constexpr std::uint8_t player_info_latency{16U};
// TODO: the protocol entity type id is hard-coded for the player; non-player
// entities need a table built from the entity_type registry.
constexpr std::uint32_t player_entity_type{159U};

template <typename Packet, typename Encoder>
[[nodiscard]] auto encode_play_body(std::uint16_t packet_id,
                                    const Packet &packet, Encoder encode)
    -> std::vector<std::byte> {
    std::vector<std::byte> body;
    refrain::net::writer writer{body};
    writer.write_varint(packet_id);
    if (auto encoded{encode(writer, packet)}; !encoded) {
        body.clear();
    }
    return body;
}

[[nodiscard]] auto session_entry(const refrain::session::player &value)
    -> refrain::net::player_info_entry {
    return refrain::net::player_info_entry{
        .id = value.id,
        .name = value.name,
        .game_mode = value.game_mode,
        .listed = true,
        .latency = 0U,
    };
}

void send_player_info(play_gateway &gateway, std::uint64_t connection,
                      std::uint8_t actions,
                      const std::vector<refrain::session::player> &players) {
    refrain::net::player_info_update payload{};
    payload.actions = actions;
    payload.entries.reserve(players.size());
    for (const auto &player : players) {
        payload.entries.push_back(session_entry(player));
    }
    refrain::protocol::current::play::clientbound::player_info_update packet{};
    packet.payload = std::move(payload);
    auto body{encode_play_body(refrain::protocol::current::play::clientbound::
                                   player_info_update::packet_id,
                               packet,
                               refrain::protocol::current::play::clientbound::
                                   encode_player_info_update)};
    if (!body.empty()) {
        gateway.send_raw(connection, body);
    }
}

[[nodiscard]] auto encode_add_entity_body(const refrain::session::player &value)
    -> std::vector<std::byte> {
    refrain::protocol::current::play::clientbound::add_entity packet{
        .id = static_cast<std::int32_t>(value.entity_id),
        .uuid = value.id,
        .type_ = player_entity_type,
        .x = value.x,
        .y = value.y,
        .z = value.z,
        .movement = {},
        .x_rot = 0,
        .y_rot = 0,
        .y_head_rot = 0,
        .data = 0,
    };
    return encode_play_body(
        refrain::protocol::current::play::clientbound::add_entity::packet_id,
        packet,
        refrain::protocol::current::play::clientbound::encode_add_entity);
}

[[nodiscard]] auto encode_remove_entity_body(std::uint64_t entity_id)
    -> std::vector<std::byte> {
    refrain::protocol::current::play::clientbound::remove_entities packet{};
    packet.entity_ids.push_back(static_cast<std::uint32_t>(entity_id));
    return encode_play_body(
        refrain::protocol::current::play::clientbound::remove_entities::
            packet_id,
        packet,
        refrain::protocol::current::play::clientbound::encode_remove_entities);
}

void send_remove_entity(play_gateway &gateway, std::uint64_t connection,
                        std::uint64_t entity_id) {
    auto body{encode_remove_entity_body(entity_id)};
    if (!body.empty()) {
        gateway.send_raw(connection, body);
    }
}

void announce_session_join(play_gateway &gateway,
                           const refrain::session::play_session &players,
                           std::uint64_t connection) {
    // Pointers into the session table: the simulation thread is the only
    // writer, and copying every player record per tick is pure overhead.
    const auto snapshot{players.all()};
    send_player_info(gateway, connection, player_info_add, snapshot);
    send_player_info(gateway, connection,
                     static_cast<std::uint8_t>(player_info_game_mode |
                                               player_info_listed |
                                               player_info_latency),
                     snapshot);
    for (const auto &other : snapshot) {
        if (other.connection == connection) {
            continue;
        }
        std::vector<refrain::session::player> joining{};
        for (const auto &candidate : snapshot) {
            if (candidate.connection == connection) {
                joining.push_back(candidate);
                break;
            }
        }
        send_player_info(
            gateway, other.connection,
            static_cast<std::uint8_t>(player_info_add | player_info_game_mode |
                                      player_info_listed | player_info_latency),
            joining);
    }
}

void announce_session_leave(play_gateway &gateway,
                            const refrain::session::player &left,
                            const refrain::session::play_session &players) {
    std::vector<std::uint64_t> targets;
    for (const auto &player : players.all()) {
        targets.push_back(player.connection);
    }
    refrain::protocol::current::play::clientbound::player_info_remove remove{};
    remove.profile_ids.push_back(left.id);
    auto info_body{
        encode_play_body(refrain::protocol::current::play::clientbound::
                             player_info_remove::packet_id,
                         remove,
                         refrain::protocol::current::play::clientbound::
                             encode_player_info_remove)};
    for (const auto target : targets) {
        if (!info_body.empty()) {
            gateway.send_raw(target, info_body);
        }
    }
}

void send_session_health(play_gateway &gateway, std::uint64_t connection,
                         float health) {
    refrain::protocol::current::play::clientbound::set_health packet{
        .health = health, .food = 20, .saturation = 5.0F};
    auto body{encode_play_body(
        refrain::protocol::current::play::clientbound::set_health::packet_id,
        packet,
        refrain::protocol::current::play::clientbound::encode_set_health)};
    if (!body.empty()) {
        gateway.send_raw(connection, body);
    }
}

void send_session_experience(play_gateway &gateway, std::uint64_t connection) {
    refrain::protocol::current::play::clientbound::set_experience packet{
        .experience_progress = 0.0F,
        .experience_level = 0,
        .total_experience = 0,
    };
    auto body{encode_play_body(
        refrain::protocol::current::play::clientbound::set_experience::
            packet_id,
        packet,
        refrain::protocol::current::play::clientbound::encode_set_experience)};
    if (!body.empty()) {
        gateway.send_raw(connection, body);
    }
}

void send_session_animation(play_gateway &gateway,
                            const refrain::session::play_session &players,
                            std::uint64_t source, std::uint64_t target,
                            std::int8_t action) {
    const auto mover{players.player_of(source)};
    if (mover == nullptr) {
        return;
    }
    refrain::protocol::current::play::clientbound::animate packet{
        .id = static_cast<std::int32_t>(mover->entity_id),
        .action = action,
    };
    auto body{encode_play_body(
        refrain::protocol::current::play::clientbound::animate::packet_id,
        packet, refrain::protocol::current::play::clientbound::encode_animate)};
    if (!body.empty()) {
        gateway.send_raw(target, body);
    }
}

void send_session_hurt(play_gateway &gateway,
                       const refrain::session::player &target) {
    refrain::protocol::current::play::clientbound::hurt_animation packet{
        .id = static_cast<std::int32_t>(target.entity_id),
        .yaw = 0.0F,
    };
    auto body{encode_play_body(
        refrain::protocol::current::play::clientbound::hurt_animation::
            packet_id,
        packet,
        refrain::protocol::current::play::clientbound::encode_hurt_animation)};
    if (!body.empty()) {
        gateway.send_raw(target.connection, body);
    }
}

void handle_session_attack(play_gateway &gateway,
                           refrain::session::play_session &players,
                           const refrain::net::attack_event &event) {
    const auto target{players.player_by_entity(
        static_cast<std::uint32_t>(event.target_entity))};
    if (target == nullptr || target->connection == event.connection) {
        return;
    }
    const auto player_id{target->connection};
    const auto health{std::max(0.0F, target->health - 1.0F)};
    static_cast<void>(players.set_health(player_id, health));
    send_session_health(gateway, player_id, health);
    send_session_hurt(gateway, *target);
    for (const auto &other : players.all()) {
        if (other.connection != event.connection) {
            send_session_animation(gateway, players, event.connection,
                                   other.connection, 0);
        }
    }
}

void broadcast_session_animation(play_gateway &gateway,
                                 const refrain::session::play_session &players,
                                 std::uint64_t source, std::int8_t action) {
    for (const auto &other : players.all()) {
        if (other.connection != source) {
            send_session_animation(gateway, players, source, other.connection,
                                   action);
        }
    }
}

} // namespace

entity_broadcaster::entity_broadcaster(play_gateway &gateway,
                                       std::int32_t tracking_chunks)
    : gateway_(gateway), tracking_chunks_(tracking_chunks) {}

void entity_broadcaster::tick(const refrain::session::play_session &players) {
    const auto angle = [](float degrees) {
        return static_cast<std::int8_t>(
            std::lround(static_cast<double>(degrees) * (256.0 / 360.0)));
    };
    // Pointers into the session table: the simulation thread is the only
    // writer, and copying every player record per tick is pure overhead.
    std::vector<const refrain::session::player *> snapshot{};
    snapshot.reserve(players.size());
    players.for_each([&snapshot](const refrain::session::player &value) {
        snapshot.push_back(&value);
    });
    // One buffer per target per tick: the transport then copies and posts once
    // per target instead of once per entity update.
    std::unordered_map<std::uint64_t, std::vector<std::byte>> outbox;
    const auto append = [&outbox](std::uint64_t target,
                                  const std::vector<std::byte> &body) {
        auto &buffer{outbox[target]};
        buffer.insert(buffer.end(), body.begin(), body.end());
    };

    // Visibility transitions first: a viewer that has just come into range
    // needs the spawn packet, one that left needs the removal.
    for (const auto *viewer : snapshot) {
        auto &seen{visible_[viewer->connection]};
        for (const auto *other : snapshot) {
            if (other->connection == viewer->connection) {
                continue;
            }
            const refrain::server::visibility_query query{
                .world = viewer->world,
                .x = viewer->x,
                .z = viewer->z,
                .view_distance = viewer->view_distance,
            };
            const bool now{refrain::server::player_visible(
                query, other->world, other->x, other->z, tracking_chunks_)};
            const bool was{seen.contains(other->connection)};
            if (now && !was) {
                if (auto body{encode_add_entity_body(*other)}; !body.empty()) {
                    append(viewer->connection, body);
                }
                seen.insert(other->connection);
            } else if (!now && was) {
                if (auto body{encode_remove_entity_body(other->entity_id)};
                    !body.empty()) {
                    append(viewer->connection, body);
                }
                seen.erase(other->connection);
            }
        }
    }

    // Movement reaches only viewers that already track the mover.
    for (const auto *mover : snapshot) {
        const refrain::net::movement_event current{
            .connection = mover->connection,
            .x = mover->x,
            .y = mover->y,
            .z = mover->z,
            .yaw = mover->yaw,
            .pitch = mover->pitch,
            .on_ground = mover->on_ground,
        };
        const auto previous{last_moves_.find(mover->connection)};
        if (previous != last_moves_.end() && previous->second.x == current.x &&
            previous->second.y == current.y &&
            previous->second.z == current.z &&
            previous->second.yaw == current.yaw &&
            previous->second.pitch == current.pitch) {
            continue;
        }
        std::vector<std::byte> body;
        if (previous == last_moves_.end()) {
            refrain::protocol::current::play::clientbound::teleport_entity
                packet{};
            packet.id = static_cast<std::int32_t>(mover->entity_id);
            packet.change.position = {
                .x = mover->x, .y = mover->y, .z = mover->z};
            packet.change.delta_movement = {};
            packet.change.y_rot = mover->yaw;
            packet.change.x_rot = mover->pitch;
            packet.relatives = 0;
            packet.on_ground = mover->on_ground;
            body = encode_play_body(refrain::protocol::current::play::
                                        clientbound::teleport_entity::packet_id,
                                    packet,
                                    refrain::protocol::current::play::
                                        clientbound::encode_teleport_entity);
        } else {
            const auto move{
                refrain::server::choose_entity_move(previous->second, current)};
            switch (move.kind) {
            case refrain::server::entity_move_kind::teleport: {
                refrain::protocol::current::play::clientbound::teleport_entity
                    packet{};
                packet.id = static_cast<std::int32_t>(mover->entity_id);
                packet.change.position = {
                    .x = mover->x, .y = mover->y, .z = mover->z};
                packet.change.delta_movement = {};
                packet.change.y_rot = mover->yaw;
                packet.change.x_rot = mover->pitch;
                packet.relatives = 0;
                packet.on_ground = mover->on_ground;
                body = encode_play_body(
                    refrain::protocol::current::play::clientbound::
                        teleport_entity::packet_id,
                    packet,
                    refrain::protocol::current::play::clientbound::
                        encode_teleport_entity);
                break;
            }
            case refrain::server::entity_move_kind::position: {
                refrain::protocol::current::play::clientbound::move_entity_pos
                    packet{};
                packet.entity_id = static_cast<std::uint32_t>(mover->entity_id);
                packet.delta = {.on_ground = current.on_ground,
                                .x = move.dx,
                                .y = move.dy,
                                .z = move.dz,
                                .steps = {}};
                body = encode_play_body(
                    refrain::protocol::current::play::clientbound::
                        move_entity_pos::packet_id,
                    packet,
                    refrain::protocol::current::play::clientbound::
                        encode_move_entity_pos);
                break;
            }
            case refrain::server::entity_move_kind::position_rotation: {
                refrain::protocol::current::play::clientbound::
                    move_entity_pos_rot packet{};
                packet.entity_id = static_cast<std::uint32_t>(mover->entity_id);
                packet.delta = {.on_ground = current.on_ground,
                                .x = move.dx,
                                .y = move.dy,
                                .z = move.dz,
                                .steps = {}};
                packet.y_rot = angle(current.yaw);
                packet.x_rot = angle(current.pitch);
                body = encode_play_body(
                    refrain::protocol::current::play::clientbound::
                        move_entity_pos_rot::packet_id,
                    packet,
                    refrain::protocol::current::play::clientbound::
                        encode_move_entity_pos_rot);
                break;
            }
            case refrain::server::entity_move_kind::rotation: {
                refrain::protocol::current::play::clientbound::move_entity_rot
                    packet{};
                packet.entity_id = static_cast<std::uint32_t>(mover->entity_id);
                packet.on_ground = current.on_ground;
                packet.y_rot = angle(current.yaw);
                packet.x_rot = angle(current.pitch);
                body = encode_play_body(
                    refrain::protocol::current::play::clientbound::
                        move_entity_rot::packet_id,
                    packet,
                    refrain::protocol::current::play::clientbound::
                        encode_move_entity_rot);
                break;
            }
            case refrain::server::entity_move_kind::none:
                break;
            }
        }
        last_moves_[mover->connection] = current;
        if (body.empty()) {
            continue;
        }
        for (const auto *target : snapshot) {
            if (target->connection == mover->connection) {
                continue;
            }
            const auto found{visible_.find(target->connection)};
            if (found == visible_.end() ||
                !found->second.contains(mover->connection)) {
                continue;
            }
            append(target->connection, body);
        }
    }

    for (const auto &[target, buffer] : outbox) {
        if (!buffer.empty()) {
            gateway_.send_raw(target, buffer);
        }
    }
}

void entity_broadcaster::joined(const refrain::session::play_session &players,
                                std::uint64_t connection) {
    announce_session_join(gateway_, players, connection);
}

void entity_broadcaster::left(const refrain::session::play_session &players,
                              const refrain::session::player &value) {
    last_moves_.erase(value.connection);
    visible_.erase(value.connection);
    for (auto &[viewer, seen] : visible_) {
        if (seen.erase(value.connection) > 0) {
            send_remove_entity(gateway_, viewer, value.entity_id);
        }
    }
    announce_session_leave(gateway_, value, players);
}

void entity_broadcaster::attack(refrain::session::play_session &players,
                                const refrain::net::attack_event &event) {
    handle_session_attack(gateway_, players, event);
}

void entity_broadcaster::animation(
    const refrain::session::play_session &players, std::uint64_t source,
    std::int8_t action) {
    broadcast_session_animation(gateway_, players, source, action);
}

void entity_broadcaster::play_ready(
    const refrain::session::play_session &players, std::uint64_t connection) {
    const auto value{players.player_of(connection)};
    send_session_health(gateway_, connection,
                        value == nullptr ? 20.0F : value->health);
    send_session_experience(gateway_, connection);
}

void entity_broadcaster::respawn(refrain::session::play_session &players,
                                 std::uint64_t connection) {
    static_cast<void>(players.set_health(connection, 20.0F));
    send_session_health(gateway_, connection, 20.0F);
}

} // namespace refrain::server
