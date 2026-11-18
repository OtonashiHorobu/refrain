// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Sky and block light for the loaded columns of one world.
//
// The engine keeps the authoritative light arrays in each column's storage
// record (`light_sections`), so the chunk codec, the world writer and the
// `level_chunk_with_light` encoder all read the same bytes without a copy. It
// initialises a column when it joins the loaded set - reusing the persisted
// arrays when the record marks them correct, relighting from the blocks
// otherwise - propagates across column and section borders, and updates light
// incrementally after a block edit.
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
// Per-block light behaviour comes from the content data: the vanilla table
// container carries, for every block state, the block light level the state
// emits, the light levels a path loses when it enters the state, and the six
// face shapes the reference lighting engine uses for shape-based occlusion.
// A state the table does not carry - a modded state, or a table imported
// without the block light report - falls back to the documented default:
// opaque, emitting nothing. The table is never guessed from a block name.

#pragma once

#include <refrain/world/model.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace refrain::data {
class vanilla_tables;
} // namespace refrain::data

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

/// One box of a light occlusion face shape, in block units. A box may extend
/// beyond its cell: a piston head's arm does.
struct block_light_box {
    std::array<double, 6> bounds{};
};

/// Face order of a light occlusion shape: the order the content report uses.
enum class light_face : std::uint8_t {
    down = 0,
    up = 1,
    north = 2,
    south = 3,
    west = 4,
    east = 5,
};

/// Face shapes of one block state, in `light_face` order.
///
/// The reference lighting engine slices the state's occlusion shape at the
/// cell boundary for each direction; the content table carries those six face
/// shapes. The lighting engine tests shape occlusion by asking whether the
/// union of two face shapes covers the whole cell.
struct block_light_shape {
    std::array<std::vector<block_light_box>, 6> faces{};
};

/// Light behaviour of one block state.
struct block_light_property {
    /// Block light level the state emits; 0 when it emits nothing.
    std::uint8_t emission{0};
    /// Light levels a path loses when it enters the state: 0 (a full source
    /// cell for sky light, no loss), 1..14 (partial) or 15 (opaque).
    std::uint8_t dampening{15};
    /// Face shapes of the state, or null when the state has no shape for light
    /// occlusion. Only states that occlude and ask the lighting engine to use
    /// their shape carry one.
    const block_light_shape *shape{nullptr};
};

/// Light table of one content version: canonical block-state key -> property.
///
/// `from_tables` copies the per-state records and occlusion shapes out of an
/// opened vanilla table container; tests fill one directly. A table that does
/// not carry a key - a modded state, or a container imported without the
/// block light report - answers `find` with null, and the light engine uses
/// the documented fallback (opaque, emitting nothing) for that state.
class block_light_table {
  public:
    block_light_table() = default;
    block_light_table(const block_light_table &) = delete;
    auto operator=(const block_light_table &) -> block_light_table & = delete;
    block_light_table(block_light_table &&other) noexcept = default;
    auto operator=(block_light_table &&other) noexcept
        -> block_light_table & = default;
    ~block_light_table() = default;

    /// Copy the light records of an opened vanilla table. A container without
    /// light sections produces an empty table.
    [[nodiscard]] static auto from_tables(const data::vanilla_tables &tables)
        -> block_light_table;

    /// Property of a canonical key, or null when the table does not carry it.
    [[nodiscard]] auto find(std::string_view key) const
        -> const block_light_property *;

    [[nodiscard]] auto empty() const noexcept -> bool {
        return properties_.empty();
    }
    [[nodiscard]] auto size() const noexcept -> std::size_t {
        return properties_.size();
    }
    [[nodiscard]] auto shape_count() const noexcept -> std::size_t {
        return shapes_.size();
    }

    /// Append one occlusion shape and return it. The pointer stays valid for
    /// the lifetime of the table.
    auto append_shape(block_light_shape shape) -> const block_light_shape *;

    /// Add or replace the property of one canonical key.
    void insert(std::string key, block_light_property property);

  private:
    /// Lookup that accepts a `string_view` key without allocating.
    struct key_hash {
        using is_transparent = void;
        [[nodiscard]] auto operator()(std::string_view value) const noexcept
            -> std::size_t {
            return std::hash<std::string_view>{}(value);
        }
        [[nodiscard]] auto operator()(const std::string &value) const noexcept
            -> std::size_t {
            return std::hash<std::string_view>{}(value);
        }
    };

    std::deque<block_light_shape> shapes_{};
    std::unordered_map<std::string, block_light_property, key_hash,
                       std::equal_to<>>
        properties_{};
};

/// Observation counters of one light engine.
///
/// The counters describe work the engine already did. They never decide which
/// cells are lit, the order light propagates or how much work one drain call
/// processes, and the engine is single-threaded, so they are plain integers
/// rather than atomics. A snapshot covers the window since the last
/// `reset_stats()`.
struct light_stats {
    /// Columns whose initial light pass ran: a column joined the loaded set
    /// without correct persisted light, or an edit forced a full relight.
    std::uint64_t columns_lit{0};
    /// Columns that joined the loaded set with persisted light reused as-is:
    /// the stored arrays were complete and the initial light pass did not run.
    std::uint64_t columns_trusted{0};
    /// The subset of the drains below that a column's initial light pass ran:
    /// calls, queue entries processed and wall time. A column load drains once,
    /// so this is one call per `columns_lit` in the common case.
    std::uint64_t initial_drain_calls{0};
    std::uint64_t initial_nodes{0};
    std::uint64_t initial_ns{0};
    /// Every drain (the initial light pass, block edits and the tick
    /// backstop): calls, queue entries processed and wall time.
    std::uint64_t drain_calls{0};
    std::uint64_t drain_nodes{0};
    std::uint64_t drain_ns{0};
    /// Block edits reported to `block_changed()`.
    std::uint64_t block_edits{0};
    /// Largest queue length observed at a drain boundary or sampled while a
    /// drain processes entries.
    std::uint64_t pending_peak{0};
    /// Queue entries still waiting when the snapshot was taken.
    std::uint64_t pending{0};
};

/// Sky and block light engine for one `world`.
///
/// The engine is owned by the world and only touched from that world's thread.
/// Column joins are initialised synchronously (a column a client may receive
/// this tick must already carry light); block edits queue a bounded update
/// that `run()` processes. `stats()` exposes the counters a workload reads
/// between phases; they are observation only.
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

    /// A column joined the loaded set. When the storage record marks its light
    /// correct and the stored layer set is complete, the persisted nibble
    /// arrays are adopted and no light is propagated; any other record is
    /// zeroed and lit from its blocks, and the column's light-correct marker is
    /// set once that pass has completed.
    void column_loaded(chunk &column);

    /// The world's light table changed: drop the cached per-state properties.
    /// Light arrays are not recomputed by this call; `world` relights its
    /// columns after installing a table.
    void reload_properties();

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

    /// Observation counters since the last `reset_stats()`. Read a snapshot
    /// before and after a phase and subtract the two; the counters never
    /// affect lighting.
    [[nodiscard]] auto stats() const noexcept -> light_stats;

    /// Clear every counter. Queued work is not touched: `pending` in the next
    /// snapshot still reports the queue, while `pending_peak` starts over.
    void reset_stats() noexcept;

  private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace refrain::sim
