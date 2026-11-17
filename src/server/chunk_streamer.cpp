// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/chunk_streamer.hpp>

#include <refrain/core/log.hpp>
#include <refrain/core/time.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/nbt.hpp>
#include <refrain/data/vanilla.hpp>
#include <refrain/protocol/current.hpp>
#include <refrain/server/chunk_view.hpp>
#include <refrain/sim/world.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr refrain::log::logger log_net{"net"};

/// Chunk packets one wire batch may carry. This bounds one batch, not the
/// tick's work: the drain keeps batching until the time budget expires.
constexpr std::size_t max_batch_packets{8};

namespace play_cb = refrain::protocol::current::play::clientbound;

/// True when `(x, z)` is inside the square of `radius` around a centre.
[[nodiscard]] auto inside_view(std::int32_t x, std::int32_t z,
                               std::int32_t center_x, std::int32_t center_z,
                               std::int32_t radius) -> bool {
    return std::abs(x - center_x) <= radius && std::abs(z - center_z) <= radius;
}

/// A column position as the streamer's sets key it.
using column_key = std::pair<std::int32_t, std::int32_t>;

/// The static entity-type registry of the protocol. Unlike the registries the
/// server sends during configuration, entity type ids are built into the
/// client, so they come from the tracked vanilla tables.
[[nodiscard]] auto entity_type_id(const refrain::data::vanilla_tables *tables,
                                  std::string_view key)
    -> std::optional<std::uint32_t> {
    if (tables == nullptr) {
        return std::nullopt;
    }
    const auto registry{tables->find_registry("minecraft:entity_type")};
    if (!registry.has_value()) {
        return std::nullopt;
    }
    const auto count{tables->registry_entry_count(*registry)};
    for (std::uint32_t index{0}; index < count; ++index) {
        const auto entry{tables->registry_entry(*registry, index)};
        if (entry.name == key) {
            return static_cast<std::uint32_t>(entry.protocol_id);
        }
    }
    return std::nullopt;
}

/// Packet body with the leading packet-id varint.
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

/// The reference entity rotation encoding: degrees scaled into a signed byte.
[[nodiscard]] auto pack_degrees(float degrees) -> std::int8_t {
    const auto scaled{static_cast<std::int32_t>(degrees * 256.0F / 360.0F)};
    return static_cast<std::int8_t>(static_cast<std::uint8_t>(scaled & 0xFF));
}

/// The `data` field of the spawn packet. The reference derives it from the
/// entity type: an item frame carries the direction it hangs from, which its
/// template NBT keeps as `Facing`. Other types carried by world generation
/// leave it at zero.
[[nodiscard]] auto
entity_spawn_data(const refrain::world::entity_record &record) -> std::int32_t {
    if (record.id != "minecraft:item_frame" &&
        record.id != "minecraft:glow_item_frame") {
        return 0;
    }
    const auto document{refrain::data::nbt::read(record.data)};
    if (!document) {
        return 0;
    }
    const auto *facing{document->get("Facing")};
    if (facing == nullptr) {
        return 0;
    }
    const auto value{facing->as_byte()};
    if (!value.has_value() || *value < 0 || *value > 5) {
        return 0;
    }
    return static_cast<std::int32_t>(*value);
}

/// A stable, data-derived uuid for a generated entity: the same record in the
/// same column always announces the same uuid, so a client that re-receives a
/// column after a server restart sees the same entity identity.
[[nodiscard]] auto entity_uuid(const refrain::sim::chunk_entity &entity,
                               std::int32_t chunk_x, std::int32_t chunk_z,
                               std::size_t index) -> refrain::net::uuid {
    std::string key;
    key.reserve(80);
    key.append(entity.record.id);
    key.push_back('\0');
    key.append(std::to_string(chunk_x));
    key.push_back(',');
    key.append(std::to_string(chunk_z));
    key.push_back(',');
    key.append(std::to_string(index));
    const auto digest{refrain::data::blake3(std::as_bytes(std::span{key}))};
    refrain::net::uuid out{};
    for (std::size_t byte{0}; byte < out.bytes.size(); ++byte) {
        out.bytes[byte] = static_cast<std::uint8_t>(
            std::to_integer<std::uint8_t>(digest[byte]));
    }
    // RFC 4122 version 4 shape; the value stays deterministic for the record.
    out.bytes[6] = static_cast<std::uint8_t>((out.bytes[6] & 0x0FU) | 0x40U);
    out.bytes[8] = static_cast<std::uint8_t>((out.bytes[8] & 0x3FU) | 0x80U);
    return out;
}

} // namespace

namespace refrain::server {

chunk_streamer::chunk_streamer(play_gateway &gateway,
                               chunk_packet_source source,
                               backlog_limits limits)
    : gateway_(gateway), source_(source), limits_(limits.clamped()) {}

chunk_streamer::~chunk_streamer() {
    if (cache_hits_ == 0 && cache_misses_ == 0) {
        return;
    }
    log_net.info("chunk cache served {} hits and {} misses", cache_hits_,
                 cache_misses_);
}

void chunk_streamer::set_chunk_requester(chunk_requester requester) {
    requester_ = std::move(requester);
}

void chunk_streamer::set_encoder(chunk_encoder encoder) {
    encoder_ = std::move(encoder);
}

void chunk_streamer::set_entity_source(chunk_entity_source source) {
    entity_source_ = std::move(source);
}

void chunk_streamer::encode_column_entities(std::uint64_t connection,
                                            stream_state &stream,
                                            std::int32_t x, std::int32_t z) {
    if (!entity_source_) {
        return;
    }
    const auto entities{entity_source_(refrain::sim::chunk_pos{x, z})};
    if (entities.empty()) {
        return;
    }
    std::vector<std::uint32_t> announced;
    announced.reserve(entities.size());
    for (std::size_t index{0}; index < entities.size(); ++index) {
        const auto &entity{entities[index]};
        const auto type{entity_type_id(source_.tables, entity.record.id)};
        if (!type.has_value()) {
            log_net.debug("skipping entity '{}' in chunk ({}, {}): the entity "
                          "type registry has no id for it",
                          entity.record.id, x, z);
            continue;
        }
        play_cb::add_entity packet{
            .id = static_cast<std::int32_t>(entity.id),
            .uuid = entity_uuid(entity, x, z, index),
            .type_ = *type,
            .x = entity.record.x,
            .y = entity.record.y,
            .z = entity.record.z,
            .movement = {},
            .x_rot = pack_degrees(entity.record.pitch),
            .y_rot = pack_degrees(entity.record.yaw),
            .y_head_rot = pack_degrees(entity.record.yaw),
            .data = entity_spawn_data(entity.record),
        };
        // TODO: the spawn packet carries the type, position, rotation and the
        // reference object `data`; per-type synchronized data components (an
        // item frame's item, a shulker's colour, a sheep's colour) need an
        // entity metadata model this build does not have yet. The column
        // record keeps the components, so adding that model later does not
        // change the format.
        auto body{encode_play_body(play_cb::add_entity::packet_id, packet,
                                   play_cb::encode_add_entity)};
        if (body.empty()) {
            continue;
        }
        gateway_.send_raw(connection, body);
        announced.push_back(static_cast<std::uint32_t>(entity.id));
    }
    if (!announced.empty()) {
        stream.entity_ids[{x, z}] = std::move(announced);
    }
}

void chunk_streamer::remove_column_entities(std::uint64_t connection,
                                            stream_state &stream,
                                            std::int32_t x, std::int32_t z) {
    const auto found{stream.entity_ids.find({x, z})};
    if (found == stream.entity_ids.end()) {
        return;
    }
    auto ids{std::move(found->second)};
    stream.entity_ids.erase(found);
    std::sort(ids.begin(), ids.end());
    play_cb::remove_entities packet{};
    packet.entity_ids = std::move(ids);
    auto body{encode_play_body(play_cb::remove_entities::packet_id, packet,
                               play_cb::encode_remove_entities)};
    if (!body.empty()) {
        gateway_.send_raw(connection, body);
    }
}

auto chunk_streamer::encode(refrain::sim::chunk_pos pos)
    -> result<std::vector<std::byte>> {
    if (encoder_) {
        return encoder_(source_, pos);
    }
    return refrain::server::encode_full_chunk(source_, pos);
}

auto chunk_streamer::cache_capacity(std::int32_t radius) -> std::size_t {
    const auto clamped{std::clamp(radius, std::int32_t{0}, max_view_radius)};
    const auto side{static_cast<std::size_t>(2 * clamped + 1)};
    return side * side;
}

void chunk_streamer::evict_least_recently_used() {
    auto victim{cache_.begin()};
    for (auto candidate{cache_.begin()}; candidate != cache_.end();
         ++candidate) {
        if (candidate->second.last_use < victim->second.last_use) {
            victim = candidate;
        }
    }
    cache_.erase(victim);
}

auto chunk_streamer::cache_entry_stale(const cache_entry &entry, std::int32_t x,
                                       std::int32_t z) const -> bool {
    if (!entry.world_backed) {
        // The encoder that produced this body does not read the world, so
        // there is no column state to compare against.
        return false;
    }
    if (source_.world == nullptr) {
        // The body was world-backed when it was encoded, but the stream has no
        // world to attest it now; re-encoding is the only safe answer.
        return true;
    }
    const refrain::sim::chunk_pos pos{.x = x, .z = z};
    if (!source_.world->column_loaded(pos)) {
        // The column left memory after the body was encoded. A reload restarts
        // the runtime revision, so the revision comparison below would accept
        // the old body even when the store now carries a newer edit.
        return true;
    }
    const auto loaded{source_.world->chunk_load(pos)};
    if (!loaded) {
        return true;
    }
    return (*loaded)->revision != entry.revision;
}

auto chunk_streamer::make_cache_entry(std::vector<std::byte> body,
                                      std::int32_t x, std::int32_t z)
    -> cache_entry {
    cache_entry entry{.body = std::move(body), .last_use = ++clock_};
    if (source_.world == nullptr) {
        return entry;
    }
    const auto loaded{
        source_.world->chunk_load(refrain::sim::chunk_pos{.x = x, .z = z})};
    if (!loaded) {
        return entry;
    }
    entry.world_backed = true;
    entry.revision = (*loaded)->revision;
    return entry;
}

void chunk_streamer::chunk_ready(std::int32_t x, std::int32_t z) {
    for (auto &[id, stream] : streams_) {
        static_cast<void>(id);
        // The generator has answered every connection that asked for this
        // column, including the ones whose viewport no longer contains it.
        // Clearing the marker here is what lets a player who walked away and
        // came back stream the column from the world instead of waiting for
        // an answer that already arrived. A look-ahead column keeps its
        // prefetch entry, because the entry is the residency claim while the
        // column waits outside the viewport, but it is no longer requested.
        stream.requested.erase({x, z});
        stream.attempts.erase({x, z});
        stream.abandoned.erase({x, z});
        stream.prefetch_asked.erase({x, z});
        const auto dx{x - stream.center_x};
        const auto dz{z - stream.center_z};
        if (std::max(std::abs(dx), std::abs(dz)) > stream.radius) {
            continue;
        }
        stream.known.erase({x, z});
        if (stream.queued.insert({x, z}).second) {
            stream.pending.emplace_back(x, z);
        }
    }
}

void chunk_streamer::chunk_failed(std::int32_t x, std::int32_t z) {
    for (auto &[id, stream] : streams_) {
        static_cast<void>(id);
        const auto key{std::pair{x, z}};
        if (stream.requested.erase(key) == 0) {
            // The connection never asked for this column, or another path
            // already answered it; nothing is waiting on the answer.
            continue;
        }
        const auto &attempts{++stream.attempts[key]};
        const auto dx{x - stream.center_x};
        const auto dz{z - stream.center_z};
        if (std::max(std::abs(dx), std::abs(dz)) > stream.radius) {
            // The viewport left the column while the task ran. Keep the
            // attempt count for a possible return, but do not queue off-screen
            // work.
            continue;
        }
        if (attempts >= max_chunk_attempts) {
            // Bound the retry: a tile that keeps failing would otherwise be
            // requested once per tick for as long as the player stands there.
            // A later viewport change clears the history, so walking away and
            // back does give the column another chance.
            stream.abandoned.insert(key);
            stream.queued.erase(key);
            log_net.debug(
                "abandoned chunk ({}, {}) after {} generation failures", x, z,
                attempts);
            continue;
        }
        if (stream.known.contains(key) || !stream.queued.insert(key).second) {
            continue;
        }
        // Put the column back where the drain will find it; the request is
        // repeated on the next pass rather than on every tick from here.
        stream.pending.emplace_back(x, z);
    }
}

auto chunk_streamer::viewports() const -> std::vector<viewport> {
    std::vector<viewport> views{};
    views.reserve(streams_.size());
    for (const auto &[connection, stream] : streams_) {
        views.push_back(viewport{.connection = connection,
                                 .chunk_x = stream.center_x,
                                 .chunk_z = stream.center_z,
                                 .radius = stream.radius});
    }
    // The caller derives a pinned set from this; hash-map order is not an
    // order, so sort by the connection id that produced each viewport.
    std::sort(views.begin(), views.end(),
              [](const viewport &left, const viewport &right) {
                  return left.connection < right.connection;
              });
    return views;
}

void chunk_streamer::update_prefetch(
    std::uint64_t connection, stream_state &stream,
    const refrain::session::chunk_tracker &views) {
    const auto estimate{views.lookahead(connection)};
    // The band is only a claim on generation and residency: the packet queue
    // is rebuilt from the accepted viewport alone, so an entry here can never
    // reach the client.
    if (!estimate.active) {
        stream.prefetch.clear();
        stream.prefetch_asked.clear();
        stream.prefetch_radius = -1;
        return;
    }
    const auto same_band{stream.prefetch_center_x == estimate.chunk_x &&
                         stream.prefetch_center_z == estimate.chunk_z &&
                         stream.prefetch_radius == stream.radius};
    if (!same_band) {
        stream.prefetch.clear();
        const auto radius{stream.radius};
        const auto add = [&stream, radius](std::int32_t x, std::int32_t z) {
            if (inside_view(x, z, stream.center_x, stream.center_z, radius)) {
                return;
            }
            stream.prefetch.insert({x, z});
        };
        // The predicted square minus the accepted square, walked as two
        // side bands. Its size is at most twice the predicted edge, so a
        // look-ahead cannot turn into a second viewport.
        for (std::int32_t x{estimate.chunk_x - radius};
             x <= estimate.chunk_x + radius; ++x) {
            if (x >= stream.center_x - radius &&
                x <= stream.center_x + radius) {
                continue;
            }
            for (std::int32_t z{estimate.chunk_z - radius};
                 z <= estimate.chunk_z + radius; ++z) {
                add(x, z);
            }
        }
        for (std::int32_t z{estimate.chunk_z - radius};
             z <= estimate.chunk_z + radius; ++z) {
            if (z >= stream.center_z - radius &&
                z <= stream.center_z + radius) {
                continue;
            }
            for (std::int32_t x{stream.center_x - radius};
                 x <= stream.center_x + radius; ++x) {
                add(x, z);
            }
        }
        // Bound what one connection may claim, nearest to the predicted
        // centre first, then by position so the set is deterministic.
        if (stream.prefetch.size() > max_prefetch_columns) {
            std::vector<column_key> ordered{stream.prefetch.begin(),
                                            stream.prefetch.end()};
            const auto distance = [&estimate](const column_key &column) {
                return std::max(std::abs(column.first - estimate.chunk_x),
                                std::abs(column.second - estimate.chunk_z));
            };
            std::sort(
                ordered.begin(), ordered.end(),
                [&distance](const column_key &left, const column_key &right) {
                    if (distance(left) != distance(right)) {
                        return distance(left) < distance(right);
                    }
                    return std::tie(left.first, left.second) <
                           std::tie(right.first, right.second);
                });
            ordered.resize(max_prefetch_columns);
            stream.prefetch =
                std::set<column_key>{ordered.begin(), ordered.end()};
        }
        stream.prefetch_center_x = estimate.chunk_x;
        stream.prefetch_center_z = estimate.chunk_z;
        stream.prefetch_radius = stream.radius;
        // Asked entries outside the new band are dropped: the generator was
        // asked once and the answer stays in the world, but a column the
        // estimate no longer wants must not keep a residency claim.
        for (auto entry = stream.prefetch_asked.begin();
             entry != stream.prefetch_asked.end();) {
            if (stream.prefetch.contains(*entry)) {
                ++entry;
            } else {
                entry = stream.prefetch_asked.erase(entry);
            }
        }
    }
    if (!requester_) {
        return;
    }
    if (stream.prefetch_asked.size() >= stream.prefetch.size()) {
        return;
    }
    std::size_t requested{0};
    for (const auto &column : stream.prefetch) {
        if (requested >= max_prefetch_requests_per_tick) {
            break;
        }
        if (stream.prefetch_asked.contains(column)) {
            continue;
        }
        ++requested;
        const auto position{
            refrain::sim::chunk_pos{.x = column.first, .z = column.second}};
        if (source_.world != nullptr &&
            source_.world->column_loaded(position)) {
            // The world already holds the column; it only needs the residency
            // claim, and asking the generator again would regenerate it.
            stream.prefetch_asked.insert(column);
            continue;
        }
        // A refused request means the tile is already wanted or in flight, or
        // the generation queue is full. Either way the column is marked asked:
        // the answer arrives on its own when the tile was in flight, and a
        // queue-full refusal is retried when the band is rebuilt, not once per
        // tick for as long as the estimate lasts.
        static_cast<void>(requester_(column.first, column.second));
        stream.prefetch_asked.insert(column);
    }
}

auto chunk_streamer::prefetch_columns() const
    -> std::vector<refrain::sim::chunk_pos> {
    std::vector<std::uint64_t> connections{};
    connections.reserve(streams_.size());
    for (const auto &[connection, stream] : streams_) {
        static_cast<void>(stream);
        connections.push_back(connection);
    }
    // Hash-map order is not an order: a capped set has to be chosen from a
    // state-derived sequence, and the connection id is the only stable key.
    std::sort(connections.begin(), connections.end());
    std::vector<refrain::sim::chunk_pos> columns{};
    for (const auto connection : connections) {
        const auto found{streams_.find(connection)};
        if (found == streams_.end()) {
            continue;
        }
        for (const auto &column : found->second.prefetch) {
            if (columns.size() >= max_total_prefetch_columns) {
                break;
            }
            columns.push_back(
                refrain::sim::chunk_pos{.x = column.first, .z = column.second});
        }
        if (columns.size() >= max_total_prefetch_columns) {
            break;
        }
    }
    return columns;
}

auto chunk_streamer::drain(std::uint64_t connection, stream_state &stream,
                           std::int64_t deadline_ns) -> bool {
    bool expired{false};
    std::vector<std::vector<std::byte>> packets;
    packets.reserve(max_batch_packets);
    /// Columns whose chunk packet this batch carries. Their entities are
    /// announced after the batch, so the client holds the column before it
    /// receives the entity.
    std::vector<std::pair<std::int32_t, std::int32_t>> sent_columns;
    sent_columns.reserve(max_batch_packets);
    while (packets.size() < max_batch_packets) {
        // Entries the world delivered on its own cost nothing to skip.
        while (!stream.pending.empty() &&
               stream.known.contains(stream.pending.front())) {
            stream.queued.erase(stream.pending.front());
            stream.pending.pop_front();
        }
        if (stream.pending.empty()) {
            break;
        }
        if (refrain::core::now_ns() >= deadline_ns) {
            // Leave the coordinate pending: the next tick picks it up again
            // instead of dropping it from the viewport.
            expired = true;
            break;
        }
        const auto [x, z]{stream.pending.front()};
        const auto cache_key{std::pair{x, z}};
        auto cached{cache_.find(cache_key)};
        if (cached != cache_.end() && cache_entry_stale(cached->second, x, z)) {
            // The body was encoded before an edit or from a column that has
            // since left memory. Drop it and encode the current column; the
            // stale body must never reach the client.
            cache_.erase(cached);
            cached = cache_.end();
        }
        if (cached == cache_.end()) {
            auto encoded{encode(refrain::sim::chunk_pos{x, z})};
            if (!encoded) {
                // The world does not hold this column yet. Ask the generator
                // once; `chunk_ready` re-queues it when the column arrives.
                if (encoded.error().code == error_code::not_found &&
                    requester_ && stream.requested.insert({x, z}).second) {
                    if (!requester_(x, z)) {
                        stream.requested.erase({x, z});
                    }
                }
                log_net.debug("failed to encode chunk ({}, {}): {}", x, z,
                              encoded.error().message);
                stream.queued.erase({x, z});
                stream.pending.pop_front();
                continue;
            }
            // A lookup that failed because the column is still generating is
            // not a cache miss: the cache was never asked for a body the world
            // could produce. Only a successful encode counts, so the hit rate
            // describes the cache and not the generation latency.
            ++cache_misses_;
            // Evict before inserting so the new entry is never its own
            // victim, and so `cached` stays valid while the batch is built.
            while (cache_.size() >= cache_capacity_) {
                evict_least_recently_used();
            }
            cached = cache_
                         .emplace(cache_key,
                                  make_cache_entry(std::move(*encoded), x, z))
                         .first;
        } else {
            ++cache_hits_;
            cached->second.last_use = ++clock_;
        }
        stream.pending.pop_front();
        stream.queued.erase({x, z});
        packets.push_back(cached->second.body);
        stream.known.emplace(x, z);
        sent_columns.emplace_back(x, z);
        // The body came from the world, so no generator answer is outstanding
        // for this connection even if a request is still in flight elsewhere.
        stream.requested.erase({x, z});
        ++stream.sent;
    }
    if (stream.center_dirty || !packets.empty()) {
        gateway_.send_chunk_batch(connection, stream.center_x, stream.center_z,
                                  packets, stream.center_dirty);
        stream.center_dirty = false;
    }
    // The entity packets follow the batch: the client applies the column first
    // and then spawns what it carries.
    for (const auto &[x, z] : sent_columns) {
        encode_column_entities(connection, stream, x, z);
    }
    return expired;
}

void chunk_streamer::tick(refrain::session::chunk_tracker &views) {

    const auto active_ids{gateway_.active_connections()};
    const std::set<std::uint64_t> active{active_ids.begin(), active_ids.end()};
    for (auto iterator = streams_.begin(); iterator != streams_.end();) {
        if (!active.contains(iterator->first)) {
            iterator = streams_.erase(iterator);
        } else {
            ++iterator;
        }
    }
    for (const auto &request : views.take_requests()) {
        auto &stream{streams_[request.connection]};
        const auto center_changed{stream.radius != request.radius ||
                                  stream.center_x != request.chunk_x ||
                                  stream.center_z != request.chunk_z};
        stream.center_x = request.chunk_x;
        stream.center_z = request.chunk_z;
        stream.radius = request.radius;
        if (center_changed) {
            // Chunks that leave the viewport are dropped from the client
            // cache, so a walking player keeps a bounded chunk set.
            std::vector<refrain::server::chunk_coordinate> tracked{};
            tracked.reserve(stream.known.size());
            for (const auto &[x, z] : stream.known) {
                tracked.push_back({x, z});
            }
            for (const auto &position : refrain::server::chunks_outside_view(
                     tracked, {stream.center_x, stream.center_z},
                     stream.radius)) {
                // Entities leave before the client forgets their column, so no
                // id outlives the chunk it was announced for.
                remove_column_entities(request.connection, stream, position.x,
                                       position.z);
                gateway_.send_forget_chunk(request.connection, position.x,
                                           position.z);
                stream.known.erase({position.x, position.z});
            }
            // Failure history belongs to one viewport visit. A column that
            // leaves the view gets a fresh attempt allowance on the next visit
            // instead of staying abandoned forever.
            for (auto entry = stream.attempts.begin();
                 entry != stream.attempts.end();) {
                const auto dx{entry->first.first - stream.center_x};
                const auto dz{entry->first.second - stream.center_z};
                if (std::max(std::abs(dx), std::abs(dz)) <= stream.radius) {
                    ++entry;
                    continue;
                }
                stream.abandoned.erase(entry->first);
                entry = stream.attempts.erase(entry);
            }
        }
        if (center_changed || stream.pending.empty()) {
            // A walking player changes the centre often. Rebuilding the queue
            // from scratch discards the work already queued and starves the
            // connection, so keep the entries that are still inside the
            // viewport and append the ones the new viewport added. `queued`
            // mirrors `pending` so the scan below stays linear.
            std::deque<std::pair<std::int32_t, std::int32_t>> kept{};
            for (const auto &entry : stream.pending) {
                const auto dx{entry.first - stream.center_x};
                const auto dz{entry.second - stream.center_z};
                if (std::max(std::abs(dx), std::abs(dz)) <= stream.radius &&
                    !stream.known.contains(entry)) {
                    kept.push_back(entry);
                } else {
                    stream.queued.erase(entry);
                }
            }
            stream.pending = std::move(kept);
            for (std::int32_t distance{0}; distance <= stream.radius;
                 ++distance) {
                for (std::int32_t dz{-distance}; dz <= distance; ++dz) {
                    for (std::int32_t dx{-distance}; dx <= distance; ++dx) {
                        if (std::max(std::abs(dx), std::abs(dz)) != distance) {
                            continue;
                        }
                        const auto key{std::pair{stream.center_x + dx,
                                                 stream.center_z + dz}};
                        if (stream.known.contains(key) ||
                            stream.queued.contains(key) ||
                            stream.abandoned.contains(key) ||
                            stream.requested.contains(key)) {
                            // A column already handed to the generator stays
                            // out of the queue until it answers; otherwise a
                            // slow worker would make the streamer encode the
                            // missing column once per tick. A column whose
                            // attempts are exhausted stays out until it
                            // arrives through another path.
                            continue;
                        }
                        stream.pending.push_back(key);
                        stream.queued.insert(key);
                    }
                }
            }
        }
        stream.center_dirty = stream.center_dirty || center_changed;
        stream.reported = false;
    }
    // Recompute every moving connection's look-ahead band. This runs on the
    // accepted centres the tracker recorded from validated movement, never on
    // a client-reported velocity, and it only asks for generation: the drain
    // below still sends exactly the accepted viewports.
    for (auto &[connection, stream] : streams_) {
        update_prefetch(connection, stream, views);
    }
    if (!streams_.empty()) {
        // The reusable bodies are the ones the largest viewport holds, so the
        // cache follows the view distance rather than a fixed entry count.
        std::int32_t largest_radius{0};
        for (const auto &[id, stream] : streams_) {
            static_cast<void>(id);
            largest_radius = std::max(largest_radius, stream.radius);
        }
        cache_capacity_ = cache_capacity(largest_radius);
    }
    // Bound the bytes queued for clients that cannot drain them. A slow
    // client keeps its pending list and resumes once the socket catches
    // up; the whole stream pauses when the aggregate backlog is large.
    std::size_t total_backlog{0};
    for (const auto id : active_ids) {
        total_backlog += gateway_.backlog(id);
    }
    if (total_backlog > limits_.max_total_bytes) {
        return;
    }
    std::vector<std::uint64_t> drainable{};
    drainable.reserve(active_ids.size());
    for (const auto id : active_ids) {
        if (!streams_.contains(id)) {
            continue;
        }
        if (gateway_.backlog(id) > limits_.max_connection_bytes) {
            continue;
        }
        drainable.push_back(id);
    }
    const auto deadline_ns{refrain::core::now_ns() +
                           static_cast<std::int64_t>(tick_budget_ns)};
    // Round-robin: one batch per connection per pass, so a connection with a
    // long queue cannot spend the whole budget before the others run.
    bool expired{false};
    bool progressed{true};
    while (progressed && !expired) {
        progressed = false;
        for (const auto id : drainable) {
            auto match{streams_.find(id)};
            if (match == streams_.end()) {
                continue;
            }
            auto &stream{match->second};
            while (!stream.pending.empty() &&
                   stream.known.contains(stream.pending.front())) {
                stream.queued.erase(stream.pending.front());
                stream.pending.pop_front();
            }
            if (stream.pending.empty()) {
                continue;
            }
            progressed = true;
            expired = drain(id, stream, deadline_ns);
            if (expired) {
                break;
            }
        }
    }
    for (const auto id : drainable) {
        auto &stream{streams_[id]};
        if (stream.center_dirty) {
            // A connection that never got a turn this tick still learns that
            // its cache centre moved.
            gateway_.send_chunk_batch(id, stream.center_x, stream.center_z,
                                      std::span<const std::vector<std::byte>>{},
                                      true);
            stream.center_dirty = false;
        }
        if (stream.pending.empty() && !stream.reported) {
            log_net.info("prepared chunk view for connection {} with {} chunks "
                         "streamed",
                         id, stream.sent);
            stream.reported = true;
        }
    }
}

auto chunk_streamer::connections_with_chunk(std::int32_t x,
                                            std::int32_t z) const
    -> std::vector<std::uint64_t> {
    std::vector<std::uint64_t> connections{};
    const auto key{std::pair<std::int32_t, std::int32_t>{x, z}};
    for (const auto &[connection, state] : streams_) {
        if (state.known.contains(key)) {
            connections.push_back(connection);
        }
    }
    std::sort(connections.begin(), connections.end());
    return connections;
}

} // namespace refrain::server
