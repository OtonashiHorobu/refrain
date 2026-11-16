// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Sky and block light for the loaded columns of one world.
//
// The engine keeps the authoritative light arrays in each column's storage
// record (`light_sections`), so the chunk codec, the world writer and the
// `level_chunk_with_light` encoder all read the same bytes without a copy. It
// initialises a column when it joins the loaded set, propagates across column
// and section borders, and updates light incrementally after a block edit.
//
// Model:
//  * light levels are 0..15; a block's `dampening` decides how much a path
//    loses when it enters the block: 0 loses nothing, 1 loses one level and 15
//    stops the path;
//  * a sky light source is a cell that sees the sky: every cell above the
//    highest light-dampening block of its column starts at 15;
//  * a block light source is a block with a positive emission level.
//
// The final level of a cell is the maximum over all paths from a source of
// `source level - sum of dampening`; propagation is bounded because every
// step costs at least one level.
//
// Per-block light behaviour is approximated from the canonical block key: the
// game derives emission and dampening from code, and the shipped content
// tables carry neither. The approximation is exact for the blocks the tests
// and the acceptance criteria name (air, water, leaves, stone, torch,
// glowstone, lava and the common plants); every other block falls back to
// opaque. A TODO at the table marks the missing content source.

#pragma once

#include <refrain/world/model.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace refrain::sim {

class world;
struct chunk;

/// Highest light level a cell or a source can carry.
inline constexpr std::uint8_t max_light_level = 15;

/// Light layer a value belongs to.
enum class light_layer : std::uint8_t {
    sky = 0,
    block = 1,
};

/// One light section the engine changed since the changes were last drained.
///
/// The order of a drained batch is the section order: ascending column
/// position, then ascending section Y.
struct light_section_change {
    ::refrain::world::chunk_pos pos{};
    std::int8_t section_y{0};

    auto operator<=>(const light_section_change &) const = default;
};

/// Emission and sky dampening of one block state.
struct block_light_property {
    /// Block light level the block emits; 0 when it emits nothing.
    std::uint8_t emission{0};
    /// Light levels a path loses when it enters the block: 0 (a full source
    /// cell for sky light, no loss), 1..14 (partial) or 15 (opaque).
    std::uint8_t dampening{15};
};

/// Light property of a canonical block key (`minecraft:oak_leaves` or
/// `minecraft:redstone_lamp[lit=true]`). Unknown keys are opaque.
[[nodiscard]] auto block_light_property_for(std::string_view key)
    -> block_light_property;

/// Sky and block light engine for one `world`.
///
/// The engine is owned by the world and only touched from that world's thread.
/// Column joins are initialised synchronously (a column a client may receive
/// this tick must already carry light); block edits queue a bounded update
/// that `run()` processes.
class light_engine {
  public:
    explicit light_engine(world &owner);
    light_engine(const light_engine &) = delete;
    auto operator=(const light_engine &) -> light_engine & = delete;
    light_engine(light_engine &&other) noexcept;
    auto operator=(light_engine &&other) noexcept -> light_engine &;
    ~light_engine();

    /// Rebuild the internal index after the owned `world` moved. The light
    /// arrays travel with the storage records, so nothing is recomputed.
    void attach(world &owner);

    /// A column joined the loaded set: allocate its light layers, compute sky
    /// and block light and propagate into the loaded neighbours.
    void column_loaded(chunk &column);

    /// A column is about to leave the loaded set. The column's light arrays are
    /// dropped with it; the loaded neighbours keep their values until the
    /// column is lit again.
    void column_unloaded(const ::refrain::world::chunk_pos &pos);

    /// A block at `pos` changed. Queues the node for `run()`.
    void block_changed(const ::refrain::world::block_pos &pos);

    /// Process queued updates in a deterministic order. `budget` bounds the
    /// queue entries one call processes; returns the number processed. The
    /// remaining work stays queued for the next call.
    auto run(std::size_t budget = default_budget) -> std::size_t;

    /// Queue entries still waiting for `run()`.
    [[nodiscard]] auto pending() const noexcept -> bool;
    [[nodiscard]] auto pending_nodes() const noexcept -> std::size_t;

    /// Drain the changed light sections in section order.
    auto take_changes() -> std::vector<light_section_change>;

    /// Current sky/block light level of a cell; 0 outside the loaded set.
    [[nodiscard]] auto
    sky_light_at(const ::refrain::world::block_pos &pos) const -> std::uint8_t;
    [[nodiscard]] auto
    block_light_at(const ::refrain::world::block_pos &pos) const
        -> std::uint8_t;

    /// Largest number of queue entries one default `run()` processes. A
    /// single block edit relaxes at most a few thousand cells; column loads
    /// process their increase queue until it is empty.
    static constexpr std::size_t default_budget = 1U << 16U;

  private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace refrain::sim
