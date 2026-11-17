// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Server-authoritative block breaking.
//
// The network layer reports the client's block-break actions and the selected
// hotbar slot; this service owns the one state record per connection that
// decides how far a break has progressed. It reads the destroy time and tool
// rules from the content mining tables, advances the progress on the server
// tick, sends the destroy-stage animation to nearby players, writes air when a
// block breaks and charges the tool its durability. Drops, item entities and
// neighbour updates belong to other subsystems; the service only reports that
// a block changed.

#pragma once

#include <refrain/data/mining.hpp>
#include <refrain/server/world_port.hpp>
#include <refrain/sim/item.hpp>
#include <refrain/sim/player.hpp>
#include <refrain/sim/world.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace refrain::server {

namespace mining {

/// Height of a standing player's eyes above its feet, as the reference uses it
/// for the eye position and the block interaction range check.
inline constexpr double eye_height = 1.62;

/// Block interaction range of the reference's player attribute plus the
/// one-block buffer its break handler allows when it checks the reach.
inline constexpr double interaction_range = 4.5 + 1.0;

/// Creative mode adds 0.5 to the block interaction range attribute.
inline constexpr double creative_interaction_range = interaction_range + 0.5;

/// Block name of a canonical block-state key: `minecraft:oak_log[axis=y]`
/// becomes `minecraft:oak_log`. Mining records are keyed by block name, not by
/// state.
[[nodiscard]] auto block_name(std::string_view key) noexcept
    -> std::string_view;

/// The reference's `hasCorrectToolForDrops`: a block that does not require a
/// correct tool answers true for bare hands as well, otherwise the tool's
/// family and harvest level decide.
[[nodiscard]] auto correct_tool(const data::block_mining &block,
                                const data::tool_mining *tool) noexcept -> bool;

/// Destroy progress of one tick before the elapsed-tick multiplier:
/// `speed / hardness / (correct ? 30 : 100)`. An unbreakable block (negative
/// hardness) is always zero; a zero-hardness block is infinite, so it breaks on
/// the first tick.
[[nodiscard]] auto per_tick_progress(const data::block_mining &block,
                                     const data::tool_mining *tool) noexcept
    -> float;

/// Accumulated progress after `elapsed` full ticks. The reference adds one to
/// the elapsed count: the tick a break starts on already counts as one.
[[nodiscard]] auto accumulated_progress(float per_tick,
                                        std::int64_t elapsed) noexcept -> float;

/// Destroy-stage packet value: the progress truncated to a tenth. Ten means
/// the client removes the animation.
[[nodiscard]] auto stage_of(float progress) noexcept -> std::int32_t;

/// True when the block cell lies within `range` of the eye position, measured
/// like the reference's `AABB.distanceToSqr` (the nearest point of the block
/// box, not its centre).
[[nodiscard]] auto within_reach(double eye_x, double eye_y, double eye_z,
                                const sim::block_pos &pos,
                                double range) noexcept -> bool;

} // namespace mining

/// One connection's break state. The service owns one of these per connection
/// that has a break in flight.
struct mining_progress {
    /// A break is in progress at `pos`, started on `start_tick`.
    bool destroying{false};
    sim::block_pos pos{};
    /// Server tick the break started on. Progress is the per-tick fraction
    /// times the elapsed count plus one, so this is the only clock the break
    /// reads.
    std::int64_t start_tick{0};
    /// Last stage value sent to observers; -1 before the first send.
    std::int32_t last_stage{-1};
    /// Game mode and hotbar slot captured when the break started. A change to
    /// either drops the break instead of finishing it with another rule set.
    std::uint32_t game_mode{sim::game_mode_survival};
    std::uint8_t tool_slot{0};
    /// Identity of the player that owns the break. The stage animation needs
    /// it after the player record is gone (a disconnect still clears the
    /// animation for the observers).
    sim::entity_id entity{0};
    std::string world{};
    /// A stop that arrived before the client-visible completion threshold. The
    /// block keeps accumulating progress without a new start packet until it
    /// breaks.
    bool delayed{false};
    sim::block_pos delayed_pos{};
    std::int64_t delayed_start_tick{0};
};

/// Server-authoritative break progress, stage animation and tool wear.
///
/// The service is single-threaded: the gameplay callbacks and `tick()` run on
/// the simulation thread, in the tick order `start()`/`stop()`/`abort()` (as
/// the network layer delivers its packets) then `tick()`. Every iteration that
/// observes the state runs in a state-derived order: breaks by ascending
/// connection and observers by ascending entity id.
class mining_service {
  public:
    /// Outbound packets of the break state machine. Empty callbacks are valid:
    /// the rules still apply, only the animation or the correction packet is
    /// skipped.
    struct outbound {
        /// One destroy-stage animation for one observer connection. The entity
        /// id is the player that mines and the stage is the packet's progress
        /// field: `0..9` animate, any other value clears the animation.
        std::function<void(std::uint64_t connection, std::int32_t entity_id,
                           const sim::block_pos &pos, std::int32_t stage)>
            destroy_stage;
        /// Authoritative state of a block the client predicted differently.
        std::function<void(std::uint64_t connection, const sim::block_pos &pos,
                           sim::block_state_id state)>
            block_state;
    };

    /// `blocks` and `tools` are the content mining tables, `world` the mutable
    /// world port and `players` its in-process player table. All four outlive
    /// the service.
    mining_service(const data::block_mining_table &blocks,
                   const data::item_tool_table &tools, world_port &world,
                   sim::player_table &players, outbound out = {});

    /// Advance every active break by one server tick. Call it once per tick,
    /// after the play packets of the tick were delivered and before the block
    /// updates of the tick are broadcast, so a break that completes here is
    /// announced in the same tick.
    void tick();

    /// START_DESTROY_BLOCK. Starts a break, or breaks the block immediately
    /// when the rules allow it (creative, or a block whose destroy time is
    /// zero).
    void start(std::uint64_t connection, const sim::block_pos &pos);

    /// STOP_DESTROY_BLOCK: the client reports the block as finished. The break
    /// completes when the accumulated progress reaches the reference's 0.7
    /// threshold; otherwise it keeps accumulating without a new start packet.
    void stop(std::uint64_t connection, const sim::block_pos &pos);

    /// ABORT_DESTROY_BLOCK: drop the progress without breaking the block.
    void abort(std::uint64_t connection, const sim::block_pos &pos);

    /// Drop the state of one connection and clear its stage animation from the
    /// observers. The runtime calls it on disconnect, teleport and respawn; a
    /// game-mode change is detected by `tick()` itself.
    void reset(std::uint64_t connection);

    /// Drop every break state. Used at shutdown.
    void clear();

    /// Break state of one connection, or null when it has none. The returned
    /// pointer is valid until the next call that mutates the service.
    [[nodiscard]] auto progress_of(std::uint64_t connection) const noexcept
        -> const mining_progress *;

    /// Number of connections with a break in flight.
    [[nodiscard]] auto active_breaks() const noexcept -> std::size_t {
        return states_.size();
    }

  private:
    [[nodiscard]] auto player(std::uint64_t connection) const noexcept
        -> sim::player_state *;
    [[nodiscard]] auto mining_table(sim::block_state_id state) const noexcept
        -> const data::block_mining &;
    [[nodiscard]] auto held_tool(const sim::player_state &player,
                                 std::uint8_t slot) const noexcept
        -> const data::tool_mining *;

    /// Send the stage animation for one block to every other player in the
    /// same level within the reference's 32-block radius.
    void send_stage(sim::entity_id miner, std::string_view world,
                    const sim::block_pos &pos, std::int32_t stage);
    /// Clear a stage animation of the player that owns the break.
    void clear_stage(const mining_progress &state, const sim::block_pos &pos);
    /// Tell one client the authoritative state of a block.
    void send_block_state(std::uint64_t connection, const sim::block_pos &pos,
                          sim::block_state_id state);

    /// Break one block: write air through the world port and charge the tool
    /// its durability when the caller asks for it. Returns false when the
    /// world refused the write.
    auto break_block(sim::player_state &actor, const sim::block_pos &pos,
                     const data::block_mining &block, std::uint8_t tool_slot,
                     bool damage_tool) -> bool;

    /// Advance one active or delayed break by one tick. Returns true when the
    /// break ended.
    auto advance(std::uint64_t connection, sim::player_state &actor,
                 mining_progress &state, const sim::block_pos &pos,
                 std::int64_t start_tick) -> bool;

    const data::block_mining_table &blocks_;
    const data::item_tool_table &tools_;
    world_port &world_;
    sim::player_table &players_;
    outbound out_{};
    /// Active breaks keyed by connection. The map order is the iteration
    /// order, which keeps the packet order deterministic.
    std::map<std::uint64_t, mining_progress> states_{};
    /// Ticks the service has advanced. Breaks store the tick they started on
    /// and derive their progress from the difference.
    std::int64_t ticks_{0};
};

} // namespace refrain::server
