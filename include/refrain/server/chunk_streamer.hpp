// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/server/chunk_packet.hpp>
#include <refrain/server/play_gateway.hpp>
#include <refrain/session/chunk_tracker.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace refrain::server {

/// Streams the chunks one connection needs as its viewport moves.
///
/// The simulation thread owns the streamer: viewport requests come from the
/// session chunk tracker, chunks are encoded on demand and the encoded bodies
/// are shared by every connection.
class chunk_streamer {
  public:
    chunk_streamer(play_gateway &gateway, chunk_packet_source source);

    /// Drain the tracker's viewport requests and send one bounded batch per
    /// connection. A connection that cannot drain its socket keeps its pending
    /// list and resumes later; the whole stream pauses when the aggregate
    /// backlog is large.
    void tick(refrain::session::chunk_tracker &views);

    /// Ask an outside generator for a column the world does not hold yet.
    /// Returns false when the request cannot be queued.
    using chunk_requester = std::function<bool(std::int32_t x, std::int32_t z)>;
    void set_chunk_requester(chunk_requester requester);

    /// A requested column is now in the world: enqueue it for every viewer
    /// whose viewport contains it.
    void chunk_ready(std::int32_t x, std::int32_t z);

  private:
    struct stream_state {
        std::int32_t center_x{0};
        std::int32_t center_z{0};
        std::int32_t radius{0};
        bool center_dirty{false};
        bool reported{false};
        std::size_t sent{0};
        std::set<std::pair<std::int32_t, std::int32_t>> known{};
        std::set<std::pair<std::int32_t, std::int32_t>> queued{};
        /// Columns already handed to the generator; they stay out of the
        /// queue until `chunk_ready` reports them.
        std::set<std::pair<std::int32_t, std::int32_t>> requested{};
        std::deque<std::pair<std::int32_t, std::int32_t>> pending{};
    };

    /// Encoded chunk bodies survive a tick so a second player with the same
    /// viewport does not encode the chunk again. Eviction is least-recently
    /// used: a chunk that keeps being served stays resident.
    struct cache_entry {
        std::vector<std::byte> body{};
        std::uint64_t last_use{0};
    };

    static constexpr std::size_t max_cached_chunks{256};

    play_gateway &gateway_;
    chunk_packet_source source_{};
    std::unordered_map<std::uint64_t, stream_state> streams_{};
    std::map<std::pair<std::int32_t, std::int32_t>, cache_entry> cache_{};
    std::uint64_t clock_{0};
    chunk_requester requester_{};
};

} // namespace refrain::server
