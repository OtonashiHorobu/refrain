// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/server/chunk_packet.hpp>
#include <refrain/server/play_gateway.hpp>
#include <refrain/session/chunk_tracker.hpp>
#include <refrain/sim/world.hpp>

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
///
/// A column's generated entities follow its chunk packet: when a column is sent
/// to a connection the streamer announces every entity the column carries with
/// the normal `add_entity` packet, and when the column leaves the viewport it
/// removes those ids before the client forgets the chunk. The entity source is
/// the simulation's column entity table, so the announced ids are the ids the
/// world spawned and a connation can never receive the same entity twice.
///
/// A column the world does not hold is requested from the generator once and
/// stays out of the encoded queue until the generator answers. When a task
/// fails, `chunk_failed()` puts the column back in a re-requestable state up to
/// a fixed number of attempts; after that the streamer stops asking for it
/// rather than re-requesting once per tick.
///
/// A moving player's accepted centre history also drives a look-ahead band:
/// the columns of the predicted viewport that the accepted viewport does not
/// contain yet are requested from the generator and reported through
/// `prefetch_columns()`, so the chunk pipeline can pin them. The band is
/// recomputed every tick from the estimate, never from a client-reported
/// velocity, and it is rebuilt when the estimate moves; a turn or a stop
/// clears it, which is what bounds both the generation requests and the
/// residency claim. The band never enters the packet queue: the client is
/// sent exactly its accepted viewport.
class chunk_streamer {
  public:
    /// One connection's viewport, in the form the residency policy consumes:
    /// every column with Chebyshev distance at most `radius` from the centre is
    /// expected to stay resident while the connection can see it.
    struct viewport {
        std::uint64_t connection{0};
        std::int32_t chunk_x{0};
        std::int32_t chunk_z{0};
        std::int32_t radius{0};

        friend auto operator==(const viewport &, const viewport &)
            -> bool = default;
    };

    /// Attempts one column is given before the streamer stops requesting it.
    /// A generator that keeps failing cannot make the streamer loop: the column
    /// is left unserved for this viewport visit, and a later viewport change
    /// gives it a fresh allowance.
    static constexpr std::uint32_t max_chunk_attempts{3};

    /// Columns one connection may ask the generator for outside its accepted
    /// viewport. A look-ahead band is at most two chunks wide per moved axis,
    /// so the cap only bites for a view distance far above the protocol
    /// default; it is what keeps a prediction that outruns the player from
    /// growing generation without bound.
    static constexpr std::size_t max_prefetch_columns{64};

    /// Columns of the whole stream, across connections, that residency keeps
    /// resident while they wait for the viewport. Kept beside the per-stream
    /// cap so a join storm cannot pin one predicted band per connection.
    static constexpr std::size_t max_total_prefetch_columns{256};

    /// New prefetch requests one connection may issue per tick. The band is
    /// recomputed every tick, so a request that failed because the queue was
    /// full is retried without ever issuing a burst in one tick.
    static constexpr std::size_t max_prefetch_requests_per_tick{8};

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

    /// Bytes one connection may have queued before the streamer leaves it
    /// alone for a tick, and the aggregate above which the whole stream pauses.
    ///
    /// The first starves only the chunk stream, so a client that resumes
    /// reading catches up instead of growing its queue; the second stops
    /// generating and encoding while every client is behind, because that work
    /// would be wasted. Both are work bounds, not wire limits: the transport
    /// keeps its own per-connection cap and drops a client that passes it, and
    /// the generator pauses at the same aggregate.
    struct backlog_limits {
        std::size_t max_connection_bytes{1U * 1024U * 1024U};
        std::size_t max_total_bytes{64U * 1024U * 1024U};

        /// Build from configured counts. Zero or a negative count would pause
        /// the stream for good, so it is raised to one byte.
        [[nodiscard]] static constexpr auto
        from_config(std::int64_t connection_bytes, std::int64_t total_bytes)
            -> backlog_limits {
            return backlog_limits{
                .max_connection_bytes = at_least_one(connection_bytes),
                .max_total_bytes = at_least_one(total_bytes),
            };
        }

        [[nodiscard]] constexpr auto clamped() const -> backlog_limits {
            return backlog_limits{
                .max_connection_bytes =
                    max_connection_bytes < 1U ? 1U : max_connection_bytes,
                .max_total_bytes = max_total_bytes < 1U ? 1U : max_total_bytes,
            };
        }

      private:
        [[nodiscard]] static constexpr auto at_least_one(std::int64_t value)
            -> std::size_t {
            if (value < 1) {
                return 1U;
            }
            return static_cast<std::size_t>(value);
        }
    };

    chunk_streamer(play_gateway &gateway, chunk_packet_source source,
                   backlog_limits limits);

    /// Same, with the default limits. This is a delegating constructor rather
    /// than a default argument because the nested type cannot be aggregate-
    /// initialized in the enclosing class's parameter list.
    chunk_streamer(play_gateway &gateway, chunk_packet_source source)
        : chunk_streamer(gateway, source, backlog_limits{}) {}

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

    /// Bind the source of the entities a column carries. The runtime binds the
    /// simulation column table; a streamer without a source announces no
    /// entities, which is also the contract-world-port case (its provider keeps
    /// the columns and hands out no entity records).
    using chunk_entity_source =
        std::function<std::vector<sim::chunk_entity>(sim::chunk_pos)>;
    void set_entity_source(chunk_entity_source source);

    /// A requested column is now in the world: enqueue it for every viewer
    /// whose viewport contains it.
    void chunk_ready(std::int32_t x, std::int32_t z);

    /// One viewport per connection that has a stream, ordered by connection id.
    /// The residency policy uses this to derive the set of columns a connection
    /// keeps resident.
    [[nodiscard]] auto viewports() const -> std::vector<viewport>;

    /// Columns outside every accepted viewport that the look-ahead currently
    /// asks the generator for, ascending by connection and position. These are
    /// pre-generated, never sent: the residency policy pins them while the
    /// estimate lasts so the work is not thrown away by the next sweep.
    [[nodiscard]] auto prefetch_columns() const -> std::vector<sim::chunk_pos>;

    /// Connections whose client already received the column, ascending by
    /// connection id. Used to fan out block and light updates.
    [[nodiscard]] auto connections_with_chunk(std::int32_t x,
                                              std::int32_t z) const
        -> std::vector<std::uint64_t>;

    /// The encoder inputs the streamer was attached with.
    [[nodiscard]] auto source() const noexcept -> const chunk_packet_source & {
        return source_;
    }

    /// A generation task for `(x, z)` failed. Clears the in-flight marker of
    /// every connection that asked for the column and queues it again while the
    /// attempt allowance lasts; a column that keeps failing is abandoned for
    /// this viewport visit instead of being requested once per tick.
    void chunk_failed(std::int32_t x, std::int32_t z);

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
        /// Entity ids announced to this client, keyed by the column that owns
        /// them. The entry is what a viewport change removes, so a client never
        /// keeps a stale copy of an entity whose chunk it dropped.
        std::map<std::pair<std::int32_t, std::int32_t>,
                 std::vector<std::uint32_t>>
            entity_ids{};
        std::set<std::pair<std::int32_t, std::int32_t>> queued{};
        /// Columns already handed to the generator; they stay out of the
        /// queue until `chunk_ready` reports them.
        std::set<std::pair<std::int32_t, std::int32_t>> requested{};
        /// Failed requests per column since the viewport last changed. A column
        /// that reaches `max_chunk_attempts` moves to `abandoned`.
        std::map<std::pair<std::int32_t, std::int32_t>, std::uint32_t>
            attempts{};
        /// Columns dropped from the queue after their attempt allowance: the
        /// viewport rebuild skips them until they arrive from elsewhere.
        std::set<std::pair<std::int32_t, std::int32_t>> abandoned{};
        std::deque<std::pair<std::int32_t, std::int32_t>> pending{};
        /// The look-ahead band of the current tick: columns outside this
        /// viewport that generation is asked for. Recomputed from the accepted
        /// centre history every tick, so a turn or a stop clears it instead of
        /// leaving a stale request behind.
        std::set<std::pair<std::int32_t, std::int32_t>> prefetch{};
        /// Prefetch columns the generator was already asked for. A column that
        /// is already wanted or in flight is not asked twice; the entry is
        /// dropped when the column leaves the band.
        std::set<std::pair<std::int32_t, std::int32_t>> prefetch_asked{};
        /// Look-ahead centre the current band was built for. The band is
        /// rebuilt only when the estimate moves, so a still camera costs one
        /// comparison per tick instead of a square walk.
        std::int32_t prefetch_center_x{0};
        std::int32_t prefetch_center_z{0};
        std::int32_t prefetch_radius{-1};
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

    /// Recompute one connection's look-ahead band and ask the generator for
    /// the columns of it that are not resident. The band follows the accepted
    /// centre history only; the packet queue is untouched, so a prefetched
    /// column cannot reach the client before its accepted viewport contains
    /// it.
    void update_prefetch(std::uint64_t connection, stream_state &stream,
                         const refrain::session::chunk_tracker &views);

    /// Send the pending chunks of one connection, up to one wire batch, until
    /// the deadline passes. Returns true when the deadline stopped the drain.
    auto drain(std::uint64_t connection, stream_state &stream,
               std::int64_t deadline_ns) -> bool;

    void evict_least_recently_used();

    [[nodiscard]] auto encode(refrain::sim::chunk_pos pos)
        -> result<std::vector<std::byte>>;

    /// Encode the spawn packets of one column's entities, in the column's
    /// canonical order, and record their ids on the connection's stream. A
    /// type the protocol registry does not know is skipped; the storage record
    /// stays intact, so a later build with the type table can serve it.
    void encode_column_entities(std::uint64_t connection, stream_state &stream,
                                std::int32_t x, std::int32_t z);

    /// Encode one `remove_entities` body for the ids and send it.
    void remove_column_entities(std::uint64_t connection, stream_state &stream,
                                std::int32_t x, std::int32_t z);

    play_gateway &gateway_;
    chunk_packet_source source_{};
    chunk_encoder encoder_{};
    std::unordered_map<std::uint64_t, stream_state> streams_{};
    std::map<std::pair<std::int32_t, std::int32_t>, cache_entry> cache_{};
    std::size_t cache_capacity_{1};
    backlog_limits limits_{};
    std::uint64_t cache_hits_{0};
    std::uint64_t cache_misses_{0};
    std::uint64_t clock_{0};
    chunk_requester requester_{};
    chunk_entity_source entity_source_{};
};

} // namespace refrain::server
