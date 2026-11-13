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
    /// Wall-clock time one stream pass may spend draining client queues.
    ///
    /// A tick lasts 50 ms and the simulation, the column generator and the
    /// entity broadcast share it. Four milliseconds is one twelfth of a tick:
    /// a burst of viewport requests drains over several ticks instead of
    /// overrunning one, while a cheap encode does not leave the rest of the
    /// tick idle. The drain stops between packets, so an encode already
    /// running when the budget expires finishes; everything still queued
    /// stays pending, in order, for the next tick.
    static constexpr std::uint64_t tick_budget_ns{4'000'000};

    /// Encode one column. Production always reads the world through
    /// `encode_full_chunk`; a test installs its own encoder to control the
    /// cost of one encode and observe the drain.
    using chunk_encoder = std::function<result<std::vector<std::byte>>(
        const chunk_packet_source &source, refrain::sim::chunk_pos pos)>;

    chunk_streamer(play_gateway &gateway, chunk_packet_source source);

    /// Log the encoded-cache hit and miss totals at shutdown. The ratio is the
    /// cache hit rate; `cache_hits()` and `cache_misses()` expose the same
    /// counters to a caller that reports them earlier.
    ~chunk_streamer();

    /// Drain the tracker's viewport requests and send the pending chunks of
    /// every connection until the tick budget expires. A connection that
    /// cannot drain its socket keeps its pending list and resumes where it
    /// stopped; the whole stream pauses when the aggregate backlog is large.
    /// The transport closes a connection whose write queue grows past its
    /// 16 MiB cap, so a client that stops reading is dropped instead of
    /// buffered without limit.
    void tick(refrain::session::chunk_tracker &views);

    /// Ask an outside generator for a column the world does not hold yet.
    /// Returns false when the request cannot be queued.
    using chunk_requester = std::function<bool(std::int32_t x, std::int32_t z)>;
    void set_chunk_requester(chunk_requester requester);

    /// Replace the encoder. Only tests call this; the runtime keeps the
    /// world-backed encoder.
    void set_encoder(chunk_encoder encoder);

    /// A requested column is now in the world: enqueue it for every viewer
    /// whose viewport contains it.
    void chunk_ready(std::int32_t x, std::int32_t z);

    /// Bodies served from the encoded cache and lookups that had to encode,
    /// since construction. The ratio is the cache hit rate; it approaches one
    /// when another connection joins a viewport that is already resident.
    [[nodiscard]] auto cache_hits() const noexcept -> std::uint64_t {
        return cache_hits_;
    }
    [[nodiscard]] auto cache_misses() const noexcept -> std::uint64_t {
        return cache_misses_;
    }

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

    /// The largest view distance the game's configuration accepts (the
    /// protocol range is 2..32). The capacity below is capped at the viewport
    /// of this distance.
    static constexpr std::int32_t max_view_radius{32};

    /// Capacity of the encoded cache for one viewport of `radius`.
    ///
    /// The bound follows the streamed view distance instead of a fixed entry
    /// count: the reusable set is exactly the chunks one viewport holds, so a
    /// second connection at the same distance hits every body. A fixed 256
    /// would sit below the 441 chunks of a distance-10 viewport and evict
    /// bodies the next viewer needs, which turns the cache into pure churn.
    /// Two connections with different centres share nothing, so the size
    /// follows one viewport rather than the number of connections. The result
    /// is capped at the viewport of `max_view_radius` so a misconfigured
    /// distance cannot pin an unbounded cache; a distance above the protocol
    /// ceiling still streams, it just stops growing the cache. The cap is
    /// enforced when a body is inserted, so a radius shrink frees the excess
    /// as new bodies arrive instead of paying a burst of evictions in one
    /// tick.
    [[nodiscard]] static auto cache_capacity(std::int32_t radius)
        -> std::size_t;

    /// Send the pending chunks of one connection, up to one wire batch, until
    /// the deadline passes. Returns true when the deadline stopped the drain.
    auto drain(std::uint64_t connection, stream_state &stream,
               std::int64_t deadline_ns) -> bool;

    void evict_least_recently_used();

    [[nodiscard]] auto encode(refrain::sim::chunk_pos pos)
        -> result<std::vector<std::byte>>;

    play_gateway &gateway_;
    chunk_packet_source source_{};
    chunk_encoder encoder_{};
    std::unordered_map<std::uint64_t, stream_state> streams_{};
    std::map<std::pair<std::int32_t, std::int32_t>, cache_entry> cache_{};
    std::size_t cache_capacity_{1};
    std::uint64_t cache_hits_{0};
    std::uint64_t cache_misses_{0};
    std::uint64_t clock_{0};
    chunk_requester requester_{};
};

} // namespace refrain::server
