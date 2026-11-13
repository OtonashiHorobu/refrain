// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Player visibility and entity broadcast. The broadcaster owns the per-viewer
// tracking sets and the last movement it sent for each player; the gateway owns
// the connections and the encoding output.

#pragma once

#include <refrain/net/server.hpp>
#include <refrain/server/play_gateway.hpp>
#include <refrain/server/player_visibility.hpp>
#include <refrain/session/play_session.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace refrain::server {

/// Mirrors the vanilla `ChunkMap.TrackedEntity` pass for player entities.
///
/// One `tick()` emits the visibility transitions (spawn/remove) and the
/// movement packets of that tick. Lifecycle hooks (`joined`, `left`, `attack`,
/// `animation`, `play_ready`, `respawn`) emit the one-off session packets so
/// the tick loop does not need to know their wire shape.
class entity_broadcaster {
  public:
    /// `tracking_chunks` is the effective entity tracking range, already
    /// converted from blocks to chunks by the caller.
    entity_broadcaster(play_gateway &gateway, std::int32_t tracking_chunks);

    /// Send the queued join and leave updates, then the visibility
    /// transitions and the movement updates of one tick.
    void tick(const refrain::session::play_session &players);

    /// Queue one joined connection. The tab-list add and the announcement to
    /// the other players are sent by the next `tick()` as one update for the
    /// whole cohort instead of one per join.
    void joined(std::uint64_t connection);

    /// Queue one leave: drop its tracking state now, send the entity removal
    /// and the tab-list removal with the other leaves of the tick.
    void left(const refrain::session::player &value);

    /// Send the join and leave updates queued since the previous call. `tick()`
    /// calls it once at the start; a caller that batches lifecycle events
    /// itself can flush at the same point.
    void flush_lifecycle(const refrain::session::play_session &players);

    void attack(refrain::session::play_session &players,
                const refrain::net::attack_event &event);

    void animation(const refrain::session::play_session &players,
                   std::uint64_t source, std::int8_t action);

    void play_ready(const refrain::session::play_session &players,
                    std::uint64_t connection);

    void respawn(refrain::session::play_session &players,
                 std::uint64_t connection);

    /// Maximum player-list bytes one tick hands to the transport. The fan-out
    /// of a cohort is one encoded body per recipient and the transport
    /// compresses it per connection, so the bytes bound the tick's share of
    /// that work. A job that does not fit stays queued, in order, for the next
    /// tick; a client sees its tab-list entries a tick later at worst.
    static constexpr std::size_t max_info_bytes_per_tick{1U * 1024U * 1024U};

    /// Queued fan-out above this size is sent in one tick even though it blows
    /// the per-tick budget. The normal path never reaches it; the cap only
    /// keeps a sustained join rate from growing the queue without bound, and
    /// one long tick is the smaller harm then.
    static constexpr std::size_t max_queued_info_bytes{32U * 1024U * 1024U};

    /// Maximum visibility pair checks one tick performs. A dense crowd makes
    /// every viewer see every other player; checking that relation for all of
    /// them in one tick is unbounded work, so the sweep resumes from the next
    /// viewer on the following tick and still visits every viewer.
    static constexpr std::size_t max_visibility_pairs_per_tick{100'000U};

    /// Maximum spawn packets one tick encodes. A pair check is cheap, but the
    /// spawn it produces is a packet the transport compresses for the viewer;
    /// the initial fill of a dense crowd is where that volume lives.
    static constexpr std::size_t max_spawns_per_tick{8'000U};

  private:
    /// One encoded player-list body and the connections that have not
    /// received it yet.
    struct pending_info {
        std::vector<std::byte> body{};
        std::vector<std::uint64_t> recipients{};
        std::size_t next{0};
    };

    /// Send queued player-list updates until the tick's byte budget is spent.
    void drain_info_sends();
    void queue_info_send(std::vector<std::byte> body,
                         std::vector<std::uint64_t> recipients);

    play_gateway &gateway_;
    std::int32_t tracking_chunks_{0};
    player_visibility_index index_;
    std::unordered_map<std::uint64_t, refrain::net::movement_event>
        last_moves_{};
    std::unordered_map<std::uint64_t, std::unordered_set<std::uint64_t>>
        visible_{};
    /// Connections that joined since the previous tick, in delivery order.
    std::vector<std::uint64_t> pending_joins_{};
    /// Connections that left since the previous tick and the profile id to
    /// remove from the player list.
    struct pending_leave {
        std::uint64_t connection{0};
        refrain::net::uuid id{};
    };
    std::vector<pending_leave> pending_leaves_{};
    /// Entity ids a tick's leaves remove from each viewer, one packet each.
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>>
        pending_removals_{};
    /// Player-list bodies waiting for fan-out budget, oldest first.
    std::deque<pending_info> pending_info_sends_{};
    /// Bytes still queued in `pending_info_sends_`.
    std::size_t pending_info_bytes_{0};
    /// Next viewer to process in the rotating visibility sweep.
    std::size_t viewer_cursor_{0};
    /// Per-tick scratch, kept across ticks so the hot path does not allocate
    /// three vectors per player per tick.
    std::vector<const refrain::session::player *> snapshot_{};
    /// The snapshot indexed by connection: the visibility pass resolves a
    /// tracked connection without scanning the session table or projecting
    /// the record again.
    std::unordered_map<std::uint64_t, const refrain::session::player *>
        snapshot_by_connection_{};
    std::vector<visibility_entity> entities_{};
    std::vector<std::size_t> candidates_{};
    std::vector<std::uint64_t> despawns_{};
};

} // namespace refrain::server
