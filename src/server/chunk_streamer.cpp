// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/chunk_streamer.hpp>

#include <refrain/core/log.hpp>
#include <refrain/core/time.hpp>
#include <refrain/server/chunk_view.hpp>
#include <refrain/sim/world.hpp>

#include <algorithm>
#include <cstdlib>
#include <span>

namespace {

constexpr refrain::log::logger log_net{"net"};

/// Chunk packets one wire batch may carry. This bounds one batch, not the
/// tick's work: the drain keeps batching until the time budget expires.
constexpr std::size_t max_batch_packets{8};

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
    log_net.info("chunk cache hits={} misses={}", cache_hits_, cache_misses_);
}

void chunk_streamer::set_chunk_requester(chunk_requester requester) {
    requester_ = std::move(requester);
}

void chunk_streamer::set_encoder(chunk_encoder encoder) {
    encoder_ = std::move(encoder);
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

void chunk_streamer::chunk_ready(std::int32_t x, std::int32_t z) {
    for (auto &[id, stream] : streams_) {
        static_cast<void>(id);
        // The generator has answered every connection that asked for this
        // column, including the ones whose viewport no longer contains it.
        // Clearing the marker here is what lets a player who walked away and
        // came back stream the column from the world instead of waiting for
        // an answer that already arrived.
        stream.requested.erase({x, z});
        stream.attempts.erase({x, z});
        stream.abandoned.erase({x, z});
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
                "chunk ({}, {}) abandoned after {} generation failure(s)", x, z,
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

auto chunk_streamer::drain(std::uint64_t connection, stream_state &stream,
                           std::int64_t deadline_ns) -> bool {
    bool expired{false};
    std::vector<std::vector<std::byte>> packets;
    packets.reserve(max_batch_packets);
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
                log_net.debug("chunk ({}, {}) encode failed: {}", x, z,
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
                                  cache_entry{
                                      .body = std::move(*encoded),
                                      .last_use = ++clock_,
                                  })
                         .first;
        } else {
            ++cache_hits_;
            cached->second.last_use = ++clock_;
        }
        stream.pending.pop_front();
        stream.queued.erase({x, z});
        packets.push_back(cached->second.body);
        stream.known.emplace(x, z);
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
            log_net.info("chunk view ready for connection {} ({} chunk(s) "
                         "streamed)",
                         id, stream.sent);
            stream.reported = true;
        }
    }
}

} // namespace refrain::server
