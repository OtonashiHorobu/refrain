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
#include <string>
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

    /// The work bounds one broadcaster applies per tick.
    ///
    /// Every field defaults to the constant above it and the runtime overrides
    /// them from the `network.broadcast` configuration section. All four are
    /// work units, not wire limits: the byte fields measure encoded player-list
    /// fan-out handed to the transport, where the compression cost lives, and
    /// the pair and spawn counts bound the visibility sweep. A value below one
    /// would stop its pass from making progress, so `clamped()` raises it to
    /// one and the constructor applies that normalization to every instance.
    struct budgets {
        /// Defaults to `max_info_bytes_per_tick`.
        std::size_t max_info_bytes_per_tick{
            entity_broadcaster::max_info_bytes_per_tick};
        /// Defaults to `max_queued_info_bytes`.
        std::size_t max_queued_info_bytes{
            entity_broadcaster::max_queued_info_bytes};
        /// Defaults to `max_visibility_pairs_per_tick`.
        std::size_t max_visibility_pairs_per_tick{
            entity_broadcaster::max_visibility_pairs_per_tick};
        /// Defaults to `max_spawns_per_tick`.
        std::size_t max_spawns_per_tick{
            entity_broadcaster::max_spawns_per_tick};

        /// Build the effective budgets from configured counts. A negative or
        /// zero count has no meaning as a work unit and would stall the pass,
        /// so it becomes one.
        [[nodiscard]] static constexpr auto
        from_config(std::int64_t info_bytes_per_tick,
                    std::int64_t queued_info_bytes,
                    std::int64_t visibility_pairs_per_tick,
                    std::int64_t spawns_per_tick) -> budgets {
            return budgets{
                .max_info_bytes_per_tick = non_negative(info_bytes_per_tick),
                .max_queued_info_bytes = non_negative(queued_info_bytes),
                .max_visibility_pairs_per_tick =
                    non_negative(visibility_pairs_per_tick),
                .max_spawns_per_tick = non_negative(spawns_per_tick),
            }
                .clamped();
        }

        /// This budget with every field raised to at least one.
        [[nodiscard]] constexpr auto clamped() const -> budgets {
            return budgets{
                .max_info_bytes_per_tick =
                    at_least_one(max_info_bytes_per_tick),
                .max_queued_info_bytes = at_least_one(max_queued_info_bytes),
                .max_visibility_pairs_per_tick =
                    at_least_one(max_visibility_pairs_per_tick),
                .max_spawns_per_tick = at_least_one(max_spawns_per_tick),
            };
        }

      private:
        /// A count below zero cannot size a queue; zero is the sentinel that
        /// `clamped()` raises to the minimum.
        [[nodiscard]] static constexpr auto non_negative(std::int64_t value)
            -> std::size_t {
            return value < 0 ? std::size_t{0} : static_cast<std::size_t>(value);
        }

        [[nodiscard]] static constexpr auto at_least_one(std::size_t value)
            -> std::size_t {
            return value < 1U ? 1U : value;
        }
    };

    /// `tracking_chunks` is the effective entity tracking range, already
    /// converted from blocks to chunks by the caller. `limits` bounds the work
    /// one tick may spend; values below one are raised to one.
    entity_broadcaster(play_gateway &gateway, std::int32_t tracking_chunks,
                       budgets limits);

    /// Same, with every work bound at its default value.
    entity_broadcaster(play_gateway &gateway, std::int32_t tracking_chunks)
        : entity_broadcaster(gateway, tracking_chunks, budgets{}) {}

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

    /// Ticks between two full visibility reconciles (thirty seconds at twenty
    /// ticks per second). The per-tick pass is incremental and reacts to a
    /// chunk change, a join or leave and a view-distance change; a pair whose
    /// distance test changed while both players stayed inside their chunks is
    /// picked up by this pass at worst. The pass is spread over as many ticks
    /// as the per-tick pair and spawn budgets need and then waits again.
    static constexpr std::int64_t full_reconcile_interval{600};

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

    /// Where a player stood at the previous tick. A change in any field means
    /// every visibility pair that involves the player is re-evaluated.
    ///
    /// The world is owned here rather than borrowed from the session record:
    /// the projection is rebuilt in place on every tick, so a view into it
    /// names a freed buffer by the time the next comparison runs, and a
    /// dimension change would read as "no change" whenever the new name fits
    /// the old allocation.
    struct entity_anchor {
        std::string world{};
        std::int64_t chunk_x{0};
        std::int64_t chunk_z{0};
        std::int32_t view_distance{0};
        friend auto operator==(const entity_anchor &, const entity_anchor &)
            -> bool = default;
    };

    /// Compare the snapshot against the previous anchors and queue the players
    /// whose visibility relation may have changed.
    void collect_visibility_changes();
    /// Re-evaluate every pair of one viewer: what it stops seeing and what it
    /// starts seeing.
    void refresh_viewer(std::size_t viewer_position);
    /// Re-evaluate what every viewer sees of one entity that changed chunk.
    void refresh_entity(std::uint64_t connection);
    /// True when this tick has spent its pair or spawn budget.
    [[nodiscard]] auto visibility_budget_spent() const -> bool {
        return pair_work_ >= limits_.max_visibility_pairs_per_tick ||
               spawn_work_ >= limits_.max_spawns_per_tick;
    }
    /// Visit the next window of the rotating full reconcile.
    void sweep_viewers();
    /// Encode the queued visibility transitions into one body per viewer.
    void flush_visibility();
    /// Encode the movement of this tick and append each mover's body to the
    /// viewers that track it.
    void flush_movement();

    play_gateway &gateway_;
    std::int32_t tracking_chunks_{0};
    budgets limits_{};
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
    /// The snapshot position indexed by connection: the visibility pass
    /// resolves a tracked connection without scanning the session table or
    /// projecting the record again.
    std::unordered_map<std::uint64_t, std::size_t> snapshot_by_connection_{};
    std::vector<visibility_entity> entities_{};
    std::vector<std::size_t> candidates_{};
    std::vector<std::uint64_t> despawns_{};
    /// Previous-tick position of every player in play.
    std::unordered_map<std::uint64_t, entity_anchor> anchors_{};
    /// Connections to re-evaluate this tick; a set deduplicates a player that
    /// both moved and changed its view distance. `dirty_entities_` keeps the
    /// anchor the player left so the old viewport can be queried too.
    std::unordered_set<std::uint64_t> dirty_viewers_{};
    std::unordered_set<std::uint64_t> dirty_entities_{};
    std::vector<std::uint64_t> dirty_viewer_queue_{};
    std::vector<std::uint64_t> dirty_entity_queue_{};
    /// One body per viewer for the visibility pass, reused across ticks.
    std::vector<std::vector<std::byte>> outbox_{};
    /// Entity ids a viewer stops seeing this tick, sorted before encoding.
    std::vector<std::vector<std::uint32_t>> removals_{};
    /// Snapshot positions a viewer starts seeing this tick, in entity order.
    std::vector<std::vector<std::size_t>> spawns_{};
    /// Reverse of `visible_`: the viewers that track one entity, with the
    /// snapshot position of each viewer as a hint. The movement fan-out walks
    /// exactly the recipients of a mover instead of the whole chunk window and
    /// a membership test per candidate, and the hint turns the per-recipient
    /// connection lookup into an indexed read while the population is stable.
    std::unordered_map<std::uint64_t,
                       std::unordered_map<std::uint64_t, std::size_t>>
        tracked_by_{};
    /// Pair checks and spawns spent by the current tick.
    std::size_t pair_work_{0};
    std::size_t spawn_work_{0};
    /// Tick counter and the state of the periodic full reconcile.
    std::int64_t tick_index_{0};
    bool reconciling_{false};
    std::size_t reconcile_remaining_{0};
};

} // namespace refrain::server
