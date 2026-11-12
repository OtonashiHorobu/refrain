// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/chunk_streamer.hpp>

#include <refrain/server/chunk_view.hpp>

#include <refrain/core/log.hpp>
#include <refrain/sim/world.hpp>

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace {

constexpr refrain::log::logger log_net{"net"};

} // namespace

namespace refrain::server {

chunk_streamer::chunk_streamer(refrain::net::server &listener,
                               chunk_packet_source source)
    : listener_(listener), source_(source) {}

void chunk_streamer::set_chunk_requester(chunk_requester requester) {
    requester_ = std::move(requester);
}

void chunk_streamer::chunk_ready(std::int32_t x, std::int32_t z) {
    for (auto &[id, stream] : streams_) {
        static_cast<void>(id);
        const auto dx{x - stream.center_x};
        const auto dz{z - stream.center_z};
        if (std::max(std::abs(dx), std::abs(dz)) > stream.radius) {
            continue;
        }
        stream.requested.erase({x, z});
        stream.known.erase({x, z});
        if (stream.queued.insert({x, z}).second) {
            stream.pending.emplace_back(x, z);
        }
    }
}

void chunk_streamer::tick(refrain::session::chunk_tracker &views) {

    const auto active_ids{listener_.active_connections()};
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
                listener_.send_forget_chunk(request.connection, position.x,
                                            position.z);
                stream.known.erase({position.x, position.z});
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
                            stream.queued.contains(key)) {
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
    // Bound the bytes queued for clients that cannot drain them. A slow
    // client keeps its pending list and resumes once the socket catches
    // up; the whole stream pauses when the aggregate backlog is large.
    constexpr std::size_t max_connection_backlog{1U * 1024U * 1024U};
    constexpr std::size_t max_total_backlog{64U * 1024U * 1024U};
    std::size_t total_backlog{0};
    for (const auto id : active_ids) {
        total_backlog += listener_.backlog(id);
    }
    if (total_backlog > max_total_backlog) {
        return;
    }
    std::size_t encode_budget{1};
    for (const auto id : active_ids) {
        auto match{streams_.find(id)};
        if (match == streams_.end()) {
            continue;
        }
        if (listener_.backlog(id) > max_connection_backlog) {
            continue;
        }
        auto &stream{match->second};
        std::vector<std::vector<std::byte>> packets;
        constexpr std::size_t batch_budget{8};
        packets.reserve(batch_budget);
        while (!stream.pending.empty() && packets.size() < batch_budget) {
            const auto [x, z]{stream.pending.front()};
            if (stream.known.contains({x, z})) {
                stream.pending.pop_front();
                continue;
            }
            const auto cache_key{std::pair{x, z}};
            auto cached{cache_.find(cache_key)};
            if (cached == cache_.end()) {
                if (encode_budget == 0) {
                    // Leave the coordinate pending: the next tick picks it
                    // up again instead of dropping it from the viewport.
                    break;
                }
                --encode_budget;
                auto encoded{refrain::server::encode_full_chunk(
                    source_, refrain::sim::chunk_pos{x, z})};
                if (!encoded) {
                    // The world does not hold this column yet. Ask the
                    // generator once; `chunk_ready` re-queues it when the
                    // column arrives.
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
                cached = cache_
                             .emplace(cache_key,
                                      cache_entry{
                                          .body = std::move(*encoded),
                                          .last_use = 0,
                                      })
                             .first;
                if (cache_.size() > max_cached_chunks) {
                    // Evict the least recently used entry.
                    auto victim{cache_.begin()};
                    for (auto candidate{cache_.begin()};
                         candidate != cache_.end(); ++candidate) {
                        if (candidate->second.last_use <
                            victim->second.last_use) {
                            victim = candidate;
                        }
                    }
                    cache_.erase(victim);
                }
            }
            cached->second.last_use = ++clock_;
            stream.pending.pop_front();
            packets.push_back(cached->second.body);
            stream.known.emplace(x, z);
            ++stream.sent;
        }
        const auto update_center{stream.center_dirty};
        if (update_center || !packets.empty()) {
            listener_.send_chunk_batch(id, stream.center_x, stream.center_z,
                                       packets, update_center);
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
