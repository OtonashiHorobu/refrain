// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/entity_broadcaster.hpp>

#include <refrain/protocol/current.hpp>
#include <refrain/server/entity_broadcast.hpp>
#include <refrain/server/player_visibility.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace refrain::server {

namespace {

constexpr std::uint8_t player_info_add{1U};
constexpr std::uint8_t player_info_game_mode{4U};
constexpr std::uint8_t player_info_listed{8U};
constexpr std::uint8_t player_info_latency{16U};
/// The action set that leaves an entry in the same state whether the client
/// learns it from the initial list or from a later announcement.
constexpr std::uint8_t player_info_full{
    player_info_add | player_info_game_mode | player_info_listed |
    player_info_latency};
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

[[nodiscard]] auto session_entry(const refrain::session::listing_entry &value)
    -> refrain::net::player_info_entry {
    return refrain::net::player_info_entry{
        .id = value.id,
        .name = std::string{value.name},
        .game_mode = value.game_mode,
        .listed = true,
        .latency = 0U,
    };
}

/// Encode one player-info update. Every recipient of a broadcast shares the
/// body, so the encoder runs once per cohort instead of once per connection.
[[nodiscard]] auto encode_player_info(refrain::net::player_info_update payload)
    -> std::vector<std::byte> {
    refrain::protocol::current::play::clientbound::player_info_update packet{};
    packet.payload = std::move(payload);
    return encode_play_body(refrain::protocol::current::play::clientbound::
                                player_info_update::packet_id,
                            packet,
                            refrain::protocol::current::play::clientbound::
                                encode_player_info_update);
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

[[nodiscard]] auto
encode_remove_entities_body(std::span<const std::uint32_t> entity_ids)
    -> std::vector<std::byte> {
    refrain::protocol::current::play::clientbound::remove_entities packet{};
    packet.entity_ids.assign(entity_ids.begin(), entity_ids.end());
    return encode_play_body(
        refrain::protocol::current::play::clientbound::remove_entities::
            packet_id,
        packet,
        refrain::protocol::current::play::clientbound::encode_remove_entities);
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

[[nodiscard]] auto chunk_coordinate(double value) -> std::int64_t {
    return static_cast<std::int64_t>(std::floor(value / 16.0));
}

[[nodiscard]] auto angle_byte(float degrees) -> std::int8_t {
    return static_cast<std::int8_t>(
        std::lround(static_cast<double>(degrees) * (256.0 / 360.0)));
}

} // namespace

entity_broadcaster::entity_broadcaster(play_gateway &gateway,
                                       std::int32_t tracking_chunks)
    : gateway_(gateway), tracking_chunks_(tracking_chunks),
      index_(tracking_chunks) {}

void entity_broadcaster::tick(const refrain::session::play_session &players) {
    // The lifecycle updates go out before the visibility pass: a client that
    // just entered play learns the player list before it sees any entity.
    flush_lifecycle(players);
    // Pointers into the session table: the simulation thread is the only
    // writer, and copying every player record per tick is pure overhead.
    snapshot_.clear();
    snapshot_.reserve(players.size());
    players.for_each([this](const refrain::session::player &value) {
        snapshot_.push_back(&value);
    });
    // The index and every emitted stream follow ascending entity id; the
    // session table itself iterates in hash order.
    std::sort(snapshot_.begin(), snapshot_.end(),
              [](const refrain::session::player *left,
                 const refrain::session::player *right) {
                  return left->entity_id < right->entity_id;
              });
    snapshot_by_connection_.clear();
    snapshot_by_connection_.reserve(snapshot_.size());
    for (std::size_t position{0}; position < snapshot_.size(); ++position) {
        snapshot_by_connection_.emplace(snapshot_[position]->connection,
                                        position);
    }
    entities_.clear();
    entities_.reserve(snapshot_.size());
    for (const auto *value : snapshot_) {
        entities_.push_back(visibility_entity{
            .entity_id = value->entity_id,
            .world = value->world,
            .x = value->x,
            .z = value->z,
            .view_distance = value->view_distance,
        });
    }
    index_.rebuild(entities_);

    const auto viewer_count{snapshot_.size()};
    outbox_.resize(viewer_count);
    removals_.resize(viewer_count);
    spawns_.resize(viewer_count);
    for (std::size_t position{0}; position < viewer_count; ++position) {
        outbox_[position].clear();
        removals_[position].clear();
        spawns_[position].clear();
    }

    // The incremental pass reacts to what changed since the previous tick; the
    // rotating reconcile below is the safety net for a pair whose distance
    // test changed while both players stayed inside their chunks.
    collect_visibility_changes();
    pair_work_ = 0;
    spawn_work_ = 0;

    std::size_t viewer_index{0};
    while (viewer_index < dirty_viewer_queue_.size() &&
           !visibility_budget_spent()) {
        const auto found{
            snapshot_by_connection_.find(dirty_viewer_queue_[viewer_index])};
        ++viewer_index;
        if (found == snapshot_by_connection_.end()) {
            continue;
        }
        refresh_viewer(found->second);
    }
    // A budget cut defers the rest of the cohort to the next tick; the player
    // keeps its current tracking set until then.
    for (; viewer_index < dirty_viewer_queue_.size(); ++viewer_index) {
        dirty_viewers_.insert(dirty_viewer_queue_[viewer_index]);
    }

    std::size_t entity_index{0};
    while (entity_index < dirty_entity_queue_.size() &&
           !visibility_budget_spent()) {
        const auto connection{dirty_entity_queue_[entity_index]};
        ++entity_index;
        refresh_entity(connection);
    }
    for (; entity_index < dirty_entity_queue_.size(); ++entity_index) {
        dirty_entities_.insert(dirty_entity_queue_[entity_index]);
    }

    ++tick_index_;
    if (viewer_count > 0 &&
        tick_index_ % full_reconcile_interval == std::int64_t{0}) {
        reconciling_ = true;
        viewer_cursor_ = 0;
        reconcile_remaining_ = viewer_count;
    }
    if (reconciling_) {
        sweep_viewers();
    }

    flush_visibility();
    flush_movement();
}

void entity_broadcaster::collect_visibility_changes() {
    dirty_viewer_queue_.clear();
    dirty_entity_queue_.clear();
    for (const auto *value : snapshot_) {
        const auto chunk_x{chunk_coordinate(value->x)};
        const auto chunk_z{chunk_coordinate(value->z)};
        const auto found{anchors_.find(value->connection)};
        if (found == anchors_.end()) {
            const entity_anchor anchor{
                .world = std::string{value->world},
                .chunk_x = chunk_x,
                .chunk_z = chunk_z,
                .view_distance = value->view_distance,
            };
            anchors_.emplace(value->connection, anchor);
            dirty_viewers_.insert(value->connection);
            dirty_entities_.insert(value->connection);
        } else if (found->second.world != value->world ||
                   found->second.chunk_x != chunk_x ||
                   found->second.chunk_z != chunk_z ||
                   found->second.view_distance != value->view_distance) {
            // Only a change touches the stored anchor: the steady state is one
            // comparison against the record, not a copy of it.
            found->second.world.assign(value->world);
            found->second.chunk_x = chunk_x;
            found->second.chunk_z = chunk_z;
            found->second.view_distance = value->view_distance;
            dirty_viewers_.insert(value->connection);
            dirty_entities_.insert(value->connection);
        }
        // Walking the snapshot in entity order produces queues that already
        // follow the deterministic order the emitted streams use.
        if (dirty_viewers_.contains(value->connection)) {
            dirty_viewer_queue_.push_back(value->connection);
        }
        if (dirty_entities_.contains(value->connection)) {
            dirty_entity_queue_.push_back(value->connection);
        }
    }
    dirty_viewers_.clear();
    dirty_entities_.clear();
}

void entity_broadcaster::refresh_viewer(std::size_t viewer_position) {
    const auto *viewer{snapshot_[viewer_position]};
    const refrain::server::visibility_query query{
        .world = viewer->world,
        .x = viewer->x,
        .z = viewer->z,
        .view_distance = viewer->view_distance,
    };
    auto &seen{visible_[viewer->connection]};
    const auto tracked_pairs{seen.size()};
    // Tracked entities are checked directly: one that left the viewport is no
    // longer returned by the index, so it cannot be found there.
    for (auto tracked{seen.begin()}; tracked != seen.end();) {
        const auto found{snapshot_by_connection_.find(*tracked)};
        const auto *other{found == snapshot_by_connection_.end()
                              ? nullptr
                              : snapshot_[found->second]};
        if (other != nullptr &&
            refrain::server::player_visible(query, other->world, other->x,
                                            other->z, tracking_chunks_)) {
            ++tracked;
            continue;
        }
        if (other != nullptr) {
            removals_[viewer_position].push_back(other->entity_id);
            const auto reverse{tracked_by_.find(other->connection)};
            if (reverse != tracked_by_.end()) {
                reverse->second.erase(viewer->connection);
                if (reverse->second.empty()) {
                    tracked_by_.erase(reverse);
                }
            }
        }
        tracked = seen.erase(tracked);
    }
    // The index narrows the spawn candidates to the viewer's chunk viewport;
    // player_visible still decides every pair.
    index_.candidates(query, candidates_);
    for (const auto other_position : candidates_) {
        const auto *other{snapshot_[other_position]};
        if (other == viewer || seen.contains(other->connection)) {
            continue;
        }
        if (!refrain::server::player_visible(query, other->world, other->x,
                                             other->z, tracking_chunks_)) {
            continue;
        }
        seen.insert(other->connection);
        tracked_by_[other->connection].insert(
            {viewer->connection, viewer_position});
        spawns_[viewer_position].push_back(other_position);
        ++spawn_work_;
    }
    pair_work_ += tracked_pairs + candidates_.size();
}

void entity_broadcaster::refresh_entity(std::uint64_t connection) {
    const auto found{snapshot_by_connection_.find(connection)};
    if (found == snapshot_by_connection_.end()) {
        return;
    }
    const auto entity_position{found->second};
    const auto *entity{snapshot_[entity_position]};
    // A viewer that still tracks the entity but can no longer see it has to
    // drop it. The tracking set is the authority here: the pair may have been
    // created while the entity stood in another chunk. A view distance is per
    // viewer, so the viewer's own refresh of the same pair does not cover this
    // direction when the two distances differ.
    for (std::size_t viewer_position{0}; viewer_position < snapshot_.size();
         ++viewer_position) {
        if (viewer_position == entity_position) {
            continue;
        }
        const auto *viewer{snapshot_[viewer_position]};
        const auto tracked{visible_.find(viewer->connection)};
        if (tracked == visible_.end() ||
            !tracked->second.contains(connection)) {
            continue;
        }
        const refrain::server::visibility_query query{
            .world = viewer->world,
            .x = viewer->x,
            .z = viewer->z,
            .view_distance = viewer->view_distance,
        };
        if (refrain::server::player_visible(query, entity->world, entity->x,
                                            entity->z, tracking_chunks_)) {
            continue;
        }
        tracked->second.erase(connection);
        const auto reverse{tracked_by_.find(connection)};
        if (reverse != tracked_by_.end()) {
            reverse->second.erase(viewer->connection);
            if (reverse->second.empty()) {
                tracked_by_.erase(reverse);
            }
        }
        removals_[viewer_position].push_back(entity->entity_id);
    }
    pair_work_ += snapshot_.size();
    // Every viewer that may see the entity now sits inside the chunk window
    // of its current chunk.
    index_.viewers_at(entity->world, chunk_coordinate(entity->x),
                      chunk_coordinate(entity->z), candidates_);
    pair_work_ += candidates_.size();
    for (const auto viewer_position : candidates_) {
        if (viewer_position == entity_position) {
            continue;
        }
        const auto *viewer{snapshot_[viewer_position]};
        const auto tracked{visible_.find(viewer->connection)};
        if (tracked != visible_.end() && tracked->second.contains(connection)) {
            continue;
        }
        const refrain::server::visibility_query query{
            .world = viewer->world,
            .x = viewer->x,
            .z = viewer->z,
            .view_distance = viewer->view_distance,
        };
        if (!refrain::server::player_visible(query, entity->world, entity->x,
                                             entity->z, tracking_chunks_)) {
            continue;
        }
        visible_[viewer->connection].insert(connection);
        tracked_by_[connection].insert({viewer->connection, viewer_position});
        spawns_[viewer_position].push_back(entity_position);
        ++spawn_work_;
    }
}

void entity_broadcaster::sweep_viewers() {
    const auto viewer_count{snapshot_.size()};
    while (reconcile_remaining_ > 0 && viewer_count > 0 &&
           !visibility_budget_spent()) {
        refresh_viewer(viewer_cursor_);
        viewer_cursor_ = (viewer_cursor_ + 1) % viewer_count;
        --reconcile_remaining_;
    }
    if (reconcile_remaining_ == 0) {
        reconciling_ = false;
    }
}

void entity_broadcaster::flush_visibility() {
    for (std::size_t position{0}; position < snapshot_.size(); ++position) {
        auto &removed{removals_[position]};
        auto &spawned{spawns_[position]};
        if (removed.empty() && spawned.empty()) {
            continue;
        }
        auto &body{outbox_[position]};
        if (!removed.empty()) {
            std::sort(removed.begin(), removed.end());
            if (auto encoded{encode_remove_entities_body(removed)};
                !encoded.empty()) {
                body.insert(body.end(), encoded.begin(), encoded.end());
            }
        }
        std::sort(spawned.begin(), spawned.end());
        for (const auto spawn_position : spawned) {
            if (auto encoded{
                    encode_add_entity_body(*snapshot_[spawn_position])};
                !encoded.empty()) {
                body.insert(body.end(), encoded.begin(), encoded.end());
            }
        }
        if (!body.empty()) {
            gateway_.send_raw(snapshot_[position]->connection, body);
            body.clear();
        }
    }
}

void entity_broadcaster::flush_movement() {
    for (std::size_t mover_position{0}; mover_position < snapshot_.size();
         ++mover_position) {
        const auto *mover{snapshot_[mover_position]};
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
                packet.y_rot = angle_byte(current.yaw);
                packet.x_rot = angle_byte(current.pitch);
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
                packet.y_rot = angle_byte(current.yaw);
                packet.x_rot = angle_byte(current.pitch);
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
        // The reverse index holds exactly the viewers that track the mover, in
        // the order the pairs were created; the body order stays the mover
        // order because movers are walked in ascending entity id.
        const auto tracked{tracked_by_.find(mover->connection)};
        if (tracked == tracked_by_.end()) {
            continue;
        }
        for (auto &[viewer_connection, hint] : tracked->second) {
            auto viewer_position{hint};
            if (viewer_position >= snapshot_.size() ||
                snapshot_[viewer_position]->connection != viewer_connection) {
                // The population changed since the pair was created: fall
                // back to the connection index and refresh the hint.
                const auto viewer{
                    snapshot_by_connection_.find(viewer_connection)};
                if (viewer == snapshot_by_connection_.end()) {
                    continue;
                }
                viewer_position = viewer->second;
                hint = viewer_position;
            }
            auto &buffer{outbox_[viewer_position]};
            buffer.insert(buffer.end(), body.begin(), body.end());
        }
    }
    for (std::size_t position{0}; position < snapshot_.size(); ++position) {
        auto &body{outbox_[position]};
        if (body.empty()) {
            continue;
        }
        gateway_.send_raw(snapshot_[position]->connection, body);
        body.clear();
    }
}

void entity_broadcaster::joined(std::uint64_t connection) {
    pending_joins_.push_back(connection);
}

void entity_broadcaster::left(const refrain::session::player &value) {
    // Drop the tracking state now so the visibility pass of this tick does not
    // consider the connection; the packets go out with the rest of the tick's
    // leaves.
    last_moves_.erase(value.connection);
    tracked_by_.erase(value.connection);
    const auto leaving{visible_.find(value.connection)};
    if (leaving != visible_.end()) {
        for (const auto tracked : leaving->second) {
            const auto reverse{tracked_by_.find(tracked)};
            if (reverse == tracked_by_.end()) {
                continue;
            }
            reverse->second.erase(value.connection);
            if (reverse->second.empty()) {
                tracked_by_.erase(reverse);
            }
        }
        visible_.erase(leaving);
    }
    anchors_.erase(value.connection);
    dirty_viewers_.erase(value.connection);
    dirty_entities_.erase(value.connection);
    for (auto &[viewer, seen] : visible_) {
        if (seen.erase(value.connection) > 0) {
            pending_removals_[viewer].push_back(
                static_cast<std::uint32_t>(value.entity_id));
        }
    }
    pending_leaves_.push_back(
        pending_leave{.connection = value.connection, .id = value.id});
}

void entity_broadcaster::queue_info_send(
    std::vector<std::byte> body, std::vector<std::uint64_t> recipients) {
    if (body.empty() || recipients.empty()) {
        return;
    }
    pending_info_bytes_ += body.size() * recipients.size();
    pending_info_sends_.push_back(pending_info{
        .body = std::move(body),
        .recipients = std::move(recipients),
        .next = 0,
    });
}

void entity_broadcaster::drain_info_sends() {
    // One budget for the whole tick. Compression dominates the cost of a
    // player-list send and scales with the body, so bytes handed to the
    // transport are the work unit.
    std::size_t budget{max_info_bytes_per_tick};
    if (pending_info_bytes_ > max_queued_info_bytes) {
        budget = std::numeric_limits<std::size_t>::max();
    }
    while (!pending_info_sends_.empty() && budget > 0) {
        auto &job{pending_info_sends_.front()};
        const auto begin{job.next};
        std::size_t bytes{0};
        while (job.next < job.recipients.size() &&
               (bytes == 0 || bytes + job.body.size() <= budget)) {
            bytes += job.body.size();
            ++job.next;
        }
        gateway_.send_raw_many(
            std::span<const std::uint64_t>{job.recipients}.subspan(
                begin, job.next - begin),
            job.body);
        budget -= std::min(budget, bytes);
        pending_info_bytes_ -= std::min(pending_info_bytes_, bytes);
        if (job.next >= job.recipients.size()) {
            pending_info_sends_.pop_front();
        }
    }
}

void entity_broadcaster::flush_lifecycle(
    const refrain::session::play_session &players) {
    // A connection that joins and leaves between two ticks is not announced:
    // the leave carries no entry the client has seen and the join resolution
    // below skips it, so the cohort excludes these connections from the
    // broadcasts of this tick.
    const std::unordered_set<std::uint64_t> joining{pending_joins_.begin(),
                                                    pending_joins_.end()};

    for (auto &[viewer, removed] : pending_removals_) {
        std::sort(removed.begin(), removed.end());
        if (auto body{encode_remove_entities_body(removed)}; !body.empty()) {
            gateway_.send_raw(viewer, body);
        }
    }
    pending_removals_.clear();

    if (!pending_leaves_.empty()) {
        refrain::protocol::current::play::clientbound::player_info_remove
            remove{};
        for (const auto &leave : pending_leaves_) {
            // A connection that left before its join was announced has no
            // entry in any player list yet.
            if (!joining.contains(leave.connection)) {
                remove.profile_ids.push_back(leave.id);
            }
        }
        pending_leaves_.clear();
        if (!remove.profile_ids.empty()) {
            auto body{
                encode_play_body(refrain::protocol::current::play::clientbound::
                                     player_info_remove::packet_id,
                                 remove,
                                 refrain::protocol::current::play::clientbound::
                                     encode_player_info_remove)};
            std::vector<std::uint64_t> recipients{};
            recipients.reserve(players.size());
            players.for_each_listing(
                [&joining,
                 &recipients](const refrain::session::listing_entry &value) {
                    if (!joining.contains(value.connection)) {
                        recipients.push_back(value.connection);
                    }
                });
            queue_info_send(std::move(body), std::move(recipients));
        }
    }

    if (!pending_joins_.empty()) {
        // Resolve the cohort against the table as it stands at flush time. A
        // connection that left since it queued has no record and is dropped.
        std::vector<std::uint64_t> cohort_connections{};
        std::vector<refrain::net::player_info_entry> cohort_entries{};
        cohort_connections.reserve(pending_joins_.size());
        cohort_entries.reserve(pending_joins_.size());
        for (const auto connection : pending_joins_) {
            const auto *value{players.player_of(connection)};
            if (value == nullptr) {
                continue;
            }
            cohort_connections.push_back(connection);
            cohort_entries.push_back(session_entry(*value));
        }
        pending_joins_.clear();
        if (!cohort_connections.empty()) {
            // Every joiner of this tick sees the same player list, so the list
            // is encoded once and one body serves the whole cohort. The other
            // players get one update holding the cohort, also encoded once.
            refrain::net::player_info_update listed{};
            listed.actions = player_info_full;
            listed.entries.reserve(players.size());
            players.for_each_listing(
                [&listed](const refrain::session::listing_entry &value) {
                    listed.entries.push_back(session_entry(value));
                });
            auto listed_body{encode_player_info(std::move(listed))};

            refrain::net::player_info_update announced{};
            announced.actions = player_info_full;
            announced.entries = std::move(cohort_entries);
            auto announced_body{encode_player_info(std::move(announced))};

            const std::unordered_set<std::uint64_t> added{
                cohort_connections.begin(), cohort_connections.end()};
            std::vector<std::uint64_t> recipients{};
            recipients.reserve(players.size());
            players.for_each_listing(
                [&added,
                 &recipients](const refrain::session::listing_entry &value) {
                    if (!added.contains(value.connection)) {
                        recipients.push_back(value.connection);
                    }
                });
            queue_info_send(std::move(listed_body),
                            std::move(cohort_connections));
            queue_info_send(std::move(announced_body), std::move(recipients));
        }
    }
    drain_info_sends();
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
