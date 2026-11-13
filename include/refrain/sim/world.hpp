// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/contracts/services/sim.hpp>
#include <refrain/core/error.hpp>
#include <refrain/core/event.hpp>

#include <functional>
#include <refrain/data/checksum.hpp>
#include <refrain/sim/player.hpp>
#include <refrain/sim/rng.hpp>
#include <refrain/world/maintenance.hpp>
#include <refrain/world/model.hpp>
#include <refrain/world/store.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace refrain::sim {

using block_pos = ::refrain::world::block_pos;
using chunk_pos = ::refrain::world::chunk_pos;
using storage_chunk = ::refrain::world::chunk;
using storage_section = ::refrain::world::section_record;
using store_type = ::refrain::world::world_store;

/// Runtime block-state id.
///
/// `block_registry` assigns ids in first-seen order; they are
/// process-local and never written to RWF (the storage palette keeps canonical
/// `minecraft:...` keys). The protocol layer maps these ids to the vanilla
/// registry; no persistent format depends on them.
using block_state_id = std::uint32_t;

inline constexpr block_state_id air_block = 0;

/// Chunk ticket kinds used by the server. Only the count matters today;
/// the kind is retained for diagnostics and the future unload policy.
enum class ticket_kind : std::uint8_t {
    player = 0,
    spawn,
    forced,
    portal,
};

/// Canonical state key -> runtime id. Stable for the lifetime of a world.
///
/// Thread-safe: the IO worker encodes snapshots (read-only `find`/`key`) while
/// the sim thread may still intern a key from a scripted `place`. Keys live in
/// a deque so a returned `string_view` stays valid across later interning
/// (including short SSO strings), and ids are never removed.
class block_registry {
  public:
    block_registry();
    block_registry(block_registry &&other) noexcept;
    auto operator=(block_registry &&other) noexcept -> block_registry &;
    block_registry(const block_registry &) = delete;
    auto operator=(const block_registry &) -> block_registry & = delete;
    ~block_registry() = default;

    /// Intern `key` (e.g. `minecraft:stone` or `minecraft:oak_log[axis=y]`);
    /// returns the existing id when the key was seen before.
    [[nodiscard]] auto intern(std::string key) -> block_state_id;

    [[nodiscard]] auto find(std::string_view key) const
        -> std::optional<block_state_id>;

    /// Canonical key for `id`; empty for an unknown id.
    [[nodiscard]] auto key(block_state_id id) const -> std::string_view;

    [[nodiscard]] auto size() const -> std::size_t;

  private:
    struct transparent_hash {
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

    std::unordered_map<std::string, block_state_id, transparent_hash,
                       std::equal_to<>>
        ids_{};
    std::deque<std::string> keys_{};
    mutable std::shared_mutex mutex_{};
};

/// Minimal entity: type key plus position/velocity. It is deliberately
/// not a LivingEntity model; the sim needs a deterministic entity tick and
/// state hash, not AI/pathfinding.
struct entity_state {
    entity_id id{0};
    std::string type{};
    double x{0.0};
    double y{0.0};
    double z{0.0};
    double vx{0.0};
    double vy{0.0};
    double vz{0.0};
    bool on_ground{false};
};

/// Minimal player state. Collision/physics is not implemented yet; the scripted
/// driver only needs a deterministic position and orientation.

/// One runtime section. Uniform sections (air/stone/water columns) do not
/// allocate the 4096-entry block array; the storage-record base keeps biome
/// and other non-block fields so a flush only replaces the block encoding.
struct section {
    std::int8_t y{0};
    bool uniform{true};
    block_state_id value{air_block};
    std::vector<block_state_id> blocks{};
    storage_section base{};
    /// Set by a runtime edit; cleared only after the chunk snapshot is safely
    /// written. `encode_chunk()` rebuilds only dirty sections and copies the
    /// decoded record of clean ones verbatim; the chunk record itself is still
    /// written as one unit (no record-level delta yet).
    bool dirty{false};
};

/// Adaptive zstd levels by horizontal chunk distance from the player. Distance
/// is Chebyshev (`max(|dx|, |dz|)`), matching Minecraft view-distance
/// semantics: near chunks are player-visible and rewritten often, so they use
/// the fastest level; far chunks trade CPU for size.
inline constexpr int near_zstd_level = 1;
inline constexpr int mid_zstd_level = 3;
inline constexpr int far_zstd_level = 6;
inline constexpr std::int32_t near_chunk_radius = 2;
inline constexpr std::int32_t mid_chunk_radius = 8;

/// Store level used when an unload writes a dirty column. Unloading runs on the
/// simulation thread inside a tick, so the fastest level matters more than the
/// record size; the level is not part of the stored format, only of that one
/// write.
inline constexpr int eviction_zstd_level = 1;

/// Distance-adaptive zstd policy. The defaults are a measured-phase heuristic,
/// not a format contract; tests/operators may override them without changing
/// bytes (the zstd level is not stored in RWF).
struct zstd_policy {
    int near_level{near_zstd_level};
    int mid_level{mid_zstd_level};
    int far_level{far_zstd_level};
    std::int32_t near_radius{near_chunk_radius};
    std::int32_t mid_radius{mid_chunk_radius};
};

/// Immutable encoded chunk snapshot used by the background writer.
struct snapshot_entry {
    chunk_pos pos{};
    storage_chunk chunk{};
    std::uint64_t revision{0};
    /// zstd level chosen by `compression_level_for()` when the snapshot was
    /// encoded; `write_snapshot()` forwards it to `world_store::write_chunk`.
    int level{::refrain::world::rwf::default_zstd_level};
};

/// A loaded chunk column. `storage` is the original decoded record; it keeps
/// heightmaps/light/block entities/extensions intact across a runtime edit.
struct chunk {
    chunk_pos pos{};
    std::int32_t y_pos{0};
    std::vector<section> sections{};
    storage_chunk storage{};
    bool dirty{false};
    std::uint64_t revision{0};
};

/// Detached runtime copy of one dirty chunk. `snapshot_pending()` produces
/// these on the sim thread; the IO worker turns them into an encoded
/// `snapshot_entry` (`write_pending()`), so TLV encoding and all store writes
/// stay off the tick thread. The copy is deep, so edits made after the
/// snapshot do not leak into the queued batch.
struct pending_snapshot {
    chunk_pos pos{};
    chunk column{};
    std::uint64_t revision{0};
    int level{::refrain::world::rwf::default_zstd_level};
};

/// One loaded column that holds no ticket, as seen by a residency sweep.
struct resident_column {
    chunk_pos pos{};
    /// True when the column holds edits that are not in the store yet.
    bool dirty{false};

    friend auto operator==(const resident_column &, const resident_column &)
        -> bool = default;
};

/// Overworld runtime view built on `refrain::world::world_store`.
///
/// The sim is single-threaded (the sim thread owns every mutable field); reads
/// and writes must not be called concurrently. Chunks are loaded on demand and
/// stay resident until the caller unloads them: a chunk is kept while a ticket
/// is held on it, and `unload_chunk()`/`unload_chunks()` drop it after flushing
/// a dirty column to the store. Re-reading an unloaded column loads it from the
/// store again; the world never generates terrain itself. The residency policy
/// (which columns are worth keeping) belongs to the server runtime, which
/// derives it from the connection viewports and the ticket set.
///
/// `state_hash()` covers the loaded chunks, so a replay has to unload the same
/// columns at the same ticks. Callers must iterate resident columns through
/// `chunk_positions()`, whose order is the position order, and never through a
/// container whose iteration order is insertion or hash order.
///
/// One applied block mutation, as observed by a batch hook.
struct block_change {
    block_pos pos{};
    block_state_id old_state{air_block};
    block_state_id new_state{air_block};
};

/// Published once per `world::set_blocks()` batch (notification only). The
/// span is valid only for the duration of the publish call.
struct block_batch_event {
    std::span<const block_change> changes{};
};

/// Published once per `world::tick()`, after the player/RNG step and before the
/// next tick. Notification-only; handlers must not throw or block.
///
/// Declared in `sdk/idl/refrain/services/sim.ridl` so subscribers and
/// mods share one layout/id (`refrain:event/refrain/services/sim/tick_event`);
/// the runtime alias keeps the `refrain::sim` spelling.
using tick_event = ::refrain::services::sim::tick_event;

class world {
  public:
    world(world &&) noexcept;
    auto operator=(world &&) noexcept -> world &;
    ~world();

    world(const world &) = delete;
    auto operator=(const world &) -> world & = delete;

    [[nodiscard]] static auto open(std::string_view path) -> result<world>;

    /// Build a world view over a store the caller already opened.
    ///
    /// The store is borrowed: it must outlive the returned world and stays
    /// open when the world is destroyed, so the caller owns its lifetime.
    /// `flush()` and the snapshot writer still write through it;
    /// `rebuild_palette_in_place()` is refused because it replaces the store
    /// object.
    [[nodiscard]] static auto attach(store_type &store) -> world;

    [[nodiscard]] auto registry() noexcept -> block_registry &;
    [[nodiscard]] auto registry() const noexcept -> const block_registry &;

    /// Load the chunk if needed. Fails with `not_found` when the imported
    /// world has no such chunk.
    ///
    /// TODO: a chunk that is absent from the store is never generated, so the
    /// server cannot serve a world it did not import; the runtime generator
    /// needs an insertion path here.
    [[nodiscard]] auto ensure_chunk(const chunk_pos &pos) -> result<chunk *>;

    /// Adopt a generated column into the runtime world.
    ///
    /// The column is validated and decoded like a stored chunk, marked dirty so
    /// the snapshot writer persists it, and returned so the caller can add a
    /// ticket. Fails with `already_exists` when the chunk is already loaded
    /// and with `parse_error` when the record is invalid.
    [[nodiscard]] auto insert_chunk(const chunk_pos &pos,
                                    storage_chunk generated) -> result<chunk *>;

    [[nodiscard]] auto find_chunk(const chunk_pos &pos) noexcept -> chunk *;
    [[nodiscard]] auto find_chunk(const chunk_pos &pos) const noexcept
        -> const chunk *;

    /// Air for an unloaded chunk or an absent section.
    [[nodiscard]] auto block_at(const block_pos &pos) const noexcept
        -> block_state_id;

    /// Set a block and mark the chunk dirty. The chunk must already exist in
    /// the imported world; Y must be inside the model's section bounds.
    [[nodiscard]] auto set_block(const block_pos &pos, block_state_id value)
        -> status;

    /// Apply a batch, publish one `block_batch_event` with the changes that
    /// actually modified the world, and return their count.
    [[nodiscard]] auto set_blocks(std::span<const block_change> changes)
        -> result<std::size_t>;

    /// Add one ticket and load the chunk. Multiple tickets on the same chunk
    /// are counted; the chunk stays loaded until the count reaches zero.
    [[nodiscard]] auto add_ticket(const chunk_pos &pos, ticket_kind kind)
        -> status;

    /// Record one ticket without requiring the column to exist. This is what a
    /// viewport pin uses: the columns of a viewport are pinned before they are
    /// generated, and a column that arrives later is already resident. A
    /// missing column is not an error here; it is simply pinned.
    [[nodiscard]] auto hold_ticket(const chunk_pos &pos, ticket_kind kind)
        -> status;

    /// Remove one ticket. Fails with `not_found` when no ticket is held.
    [[nodiscard]] auto remove_ticket(const chunk_pos &pos) -> status;

    [[nodiscard]] auto ticket_count(const chunk_pos &pos) const noexcept
        -> std::uint32_t;

    /// Flush one loaded column when dirty and drop it from memory. The record
    /// is written to the store before the column is erased, so a later load
    /// from the same world reads the edits back. `commit` publishes the store
    /// index (an fsync); pass false when the caller batches several unloads
    /// between the normal commit points. Fails with `not_found` when the column
    /// is not loaded, and with `invalid_argument` while a ticket still holds
    /// it; a failed flush keeps the column resident and dirty.
    [[nodiscard]] auto unload_chunk(const chunk_pos &pos, bool commit = true)
        -> status;

    /// Unload several columns as one batch: every dirty column is encoded and
    /// written, then all of them are erased. Every position must be loaded and
    /// free of tickets; otherwise nothing is unloaded and the first offending
    /// position is named in the error. Positions are processed in the order
    /// given, so pass them sorted. A single commit is made for the whole batch
    /// when `commit` is true.
    [[nodiscard]] auto unload_chunks(std::span<const chunk_pos> positions,
                                     bool commit = true) -> status;

    /// Loaded columns that hold no ticket, in ascending position order. This is
    /// the residency sweep's input; it is also the order a batch unload uses.
    [[nodiscard]] auto unticketed_chunks() const
        -> std::vector<resident_column>;

    /// Loaded column positions in ascending position order. This is the order
    /// a residency sweep uses to decide what to unload.
    [[nodiscard]] auto chunk_positions() const -> std::vector<chunk_pos>;

    /// Adaptive zstd level for a chunk, based on its horizontal (Chebyshev)
    /// chunk distance from the player: `near_zstd_level` up to
    /// `near_chunk_radius`, `mid_zstd_level` up to `mid_chunk_radius`,
    /// `far_zstd_level` beyond. Snapshot encoding attaches the result to each
    /// `snapshot_entry`; tick behaviour never depends on compression.
    [[nodiscard]] auto
    compression_level_for(const chunk_pos &pos) const noexcept -> int;

    /// Replace the adaptive zstd policy. Invalid levels/radii are rejected
    /// (`invalid_argument`) and leave the previous policy intact.
    [[nodiscard]] auto set_zstd_policy(const zstd_policy &policy) -> status;
    [[nodiscard]] auto zstd_policy_value() const noexcept
        -> const zstd_policy & {
        return zstd_policy_;
    }

    /// Encode every dirty chunk into an immutable snapshot without touching
    /// the store. Runs on the sim thread; the returned batch can be written by
    /// an IO worker through `write_snapshot()`. Each entry carries the
    /// adaptive zstd level for the player position at snapshot time.
    [[nodiscard]] auto snapshot_dirty() -> result<std::vector<snapshot_entry>>;

    /// Copy every dirty chunk without encoding it. Runs on the sim thread and
    /// is what the server queues to its `io-writer`; encoding then happens on
    /// the worker via `write_pending()`. A dirty chunk is copied at most once
    /// per interval (the writer rejects a second in-flight batch).
    [[nodiscard]] auto snapshot_pending() -> std::vector<pending_snapshot>;

    /// Encode one detached chunk snapshot. Uses only the immutable block
    /// registry and the chunk copy, so it is safe on the IO worker.
    [[nodiscard]] auto encode_snapshot(const pending_snapshot &pending)
        -> result<snapshot_entry>;

    /// Encode `pending`, write the resulting batch and commit the store,
    /// returning the encoded entries so the sim thread can `mark_clean()` them
    /// after the worker finishes. This is the IO-worker entry point.
    [[nodiscard]] auto write_pending(std::span<const pending_snapshot> pending)
        -> result<std::vector<snapshot_entry>>;

    /// Write a snapshot batch and commit the world store. Thread-safe with
    /// respect to `ensure_chunk()` reads (the store has its own mutex); does
    /// not clear dirty flags, so a failed/duplicated write is retried by the
    /// next flush.
    [[nodiscard]] auto write_snapshot(std::span<const snapshot_entry> snapshot)
        -> status;

    /// Clear dirty flags for a successfully written batch. Must run on the sim
    /// thread (it mutates the runtime chunks); a newer edit keeps its revision
    /// and stays dirty for the next snapshot.
    void mark_clean(std::span<const snapshot_entry> snapshot) noexcept;

    /// Compact the aux object store and blob store (all dimensions).
    [[nodiscard]] auto compact_stores()
        -> result<::refrain::world::world_compact>;

    /// Compact overworld region files whose garbage reaches the thresholds.
    /// Serialized with `write_snapshot()`/`ensure_chunk()` by the store mutex.
    [[nodiscard]] auto compact_regions(std::uint64_t min_bytes,
                                       unsigned min_percent)
        -> result<std::size_t>;

    /// Quiesced in-place palette GC.
    ///
    /// Must run on the sim thread. The caller must first drain every
    /// asynchronous writer (`snapshot_writer::drain()`); this method then
    /// `flush()`es, releases the world store (closing all region handles),
    /// rebuilds the palette through `world::rebuild_palette_in_place`, reopens
    /// the store, restores the store's zstd level and the adaptive
    /// `zstd_policy_`, and reloads the chunks that were loaded. Tickets are
    /// kept; a ticketed column that is not resident is loaded on demand after
    /// the rebuild, because a viewport pin may cover a column the store does
    /// not hold yet. `state_hash()` is unchanged when every reload succeeds.
    ///
    /// On failure the original world is reopened and reloaded when possible
    /// and the maintenance error is returned. If the reopen itself fails, a
    /// combined error is returned and `store_` is left invalid: the caller must
    /// stop using this world (no further tick/save).
    [[nodiscard]] auto rebuild_palette_in_place(unsigned threads = 1,
                                                int zstd_level = 3)
        -> result<::refrain::world::palette_maintenance_report>;

    /// Write every dirty chunk and commit the world store (group commit).
    [[nodiscard]] auto flush() -> status;

    /// Drop every loaded chunk. Dirty chunks must be flushed first.
    void unload_all() noexcept;

    /// Advance one simulation tick. The per-tick RNG draw is intentionally part
    /// of the contract: replay hashes include both the tick counter and the RNG
    /// state, so a changed draw count is visible immediately.
    void tick() noexcept;

    using timer_id = std::uint64_t;

    /// Run `fn` on a later tick. `delay == 0` runs at the end of the current
    /// tick; equal due ticks run in scheduling order. Returns a cancellation
    /// id.
    auto schedule_after(std::int64_t delay, std::function<void()> fn)
        -> timer_id;

    /// Cancel a pending timer; false when it already ran or was cancelled.
    auto cancel_timer(timer_id id) -> bool;

    /// Event bus for sim-thread notification events (tick, later block hooks).
    [[nodiscard]] auto events() noexcept -> core::events::bus & {
        return events_;
    }
    [[nodiscard]] auto ticks() const noexcept -> std::int64_t { return tick_; }

    void seed(std::uint64_t value) noexcept;
    [[nodiscard]] auto random() noexcept -> rng &;
    [[nodiscard]] auto random() const noexcept -> const rng &;

    /// Player records, addressed by entity id. The table is the one
    /// authoritative player model: the console and the session layer read and
    /// mutate these records, never a copy.
    [[nodiscard]] auto players() noexcept -> player_table &;
    [[nodiscard]] auto players() const noexcept -> const player_table &;

    /// Register a player record and allocate its entity id.
    [[nodiscard]] auto add_player(player_table::registration record)
        -> result<entity_id>;

    /// Remove the player owned by `connection`; false when it is unknown.
    [[nodiscard]] auto remove_player(std::uint64_t connection) -> bool;

    [[nodiscard]] auto player(entity_id id) noexcept -> player_state *;
    [[nodiscard]] auto player(entity_id id) const noexcept
        -> const player_state *;

    /// Re-seat `player` at `(x, y, z)` and clear its velocity.
    void set_player_position(player_state &player, double x, double y,
                             double z) noexcept;

    /// Collision-aware displacement of the player's feet. Returns true when at
    /// least one axis moved. Y is resolved first, then X/Z (vanilla order).
    [[nodiscard]] auto move_player(player_state &player, double dx, double dy,
                                   double dz) -> bool;

    void jump(player_state &player) noexcept;
    [[nodiscard]] auto block_solid(block_state_id id) const noexcept -> bool;

    /// Install the per-item stack sizes and max damage read from the item
    /// registry data. The table is copied; without one the inventory falls
    /// back to the default stack limit and no item is damageable.
    void set_item_table(item_table items);
    [[nodiscard]] auto items() const noexcept -> const item_table & {
        return items_;
    }

    /// Basic inventory mutation. Stacks are capped at the item's own stack
    /// limit; adding more fills existing partial stacks first and then appends
    /// full stacks (deterministic order). `item_count` sums every stack,
    /// `remove_item` consumes across stacks in order.
    [[nodiscard]] auto add_item(player_state &player, std::string_view item,
                                std::uint32_t count = 1) -> status;
    [[nodiscard]] auto remove_item(player_state &player, std::string_view item,
                                   std::uint32_t count = 1) -> bool;
    [[nodiscard]] auto item_count(const player_state &player,
                                  std::string_view item) const noexcept
        -> std::uint32_t;

    /// Apply one use to the stack in `player.inventory[index]`. A destroyed
    /// stack leaves an empty slot rather than removing the slot, so inventory
    /// indices stay stable for the menu that is open on it.
    [[nodiscard]] auto damage_item(player_state &player, std::size_t index,
                                   std::uint32_t amount = 1) -> damage_result;

    /// Spawn an entity of `type` at `(x, y, z)` (feet position). The id is
    /// process-local and deterministic for the same spawn order; it shares the
    /// player id sequence so a client never sees two entities with one id.
    [[nodiscard]] auto spawn_entity(std::string_view type, double x, double y,
                                    double z) -> result<entity_id>;

    /// Remove one entity; false when the id is unknown.
    [[nodiscard]] auto remove_entity(entity_id id) -> bool;

    /// Entity lookup/iteration for console, scripts and the state hash.
    [[nodiscard]] auto entities() const noexcept
        -> const std::map<entity_id, entity_state> & {
        return entities_;
    }
    [[nodiscard]] auto entity(entity_id id) noexcept -> entity_state *;
    [[nodiscard]] auto entity(entity_id id) const noexcept
        -> const entity_state *;

    /// Apply one deterministic gravity/floor tick to every entity (called by
    /// `tick()` before the tick counter advances).
    void tick_entities() noexcept;

    /// Deterministic BLAKE3 over the loaded chunks (sorted by position), every
    /// section's block ids, every player record in ascending entity id order
    /// and the tick counter. The player record fields hashed are the entity id,
    /// position/velocity, rotation, ground flag, game mode, health and
    /// inventory; identity fields (connection, profile uuid, name, world) are
    /// not simulation state. Replay compares this value across runs and thread
    /// counts; the canonical byte order is part of the test contract, not a
    /// saved format.
    [[nodiscard]] auto state_hash() const -> data::digest32;
    [[nodiscard]] auto loaded_chunks() const noexcept -> std::size_t {
        return chunks_.size();
    }

    [[nodiscard]] auto store() noexcept -> store_type &;
    [[nodiscard]] auto store() const noexcept -> const store_type &;

  private:
    struct chunk_order {
        [[nodiscard]] auto operator()(const chunk_pos &left,
                                      const chunk_pos &right) const noexcept
            -> bool {
            if (left.x != right.x) {
                return left.x < right.x;
            }
            return left.z < right.z;
        }
    };

    struct borrowed_tag {};

    explicit world(store_type store);
    explicit world(store_type &store, borrowed_tag);

    /// The store this view runs on: the owned one, or the borrowed one.
    [[nodiscard]] auto store_ref() noexcept -> store_type &;
    [[nodiscard]] auto store_ref() const noexcept -> const store_type &;

    [[nodiscard]] auto decode_chunk(storage_chunk storage, const chunk_pos &pos)
        -> result<chunk>;
    [[nodiscard]] auto encode_chunk(const chunk &value)
        -> result<storage_chunk>;
    /// The record a flush writes: the decoded record while the column has
    /// never been edited, the rebuilt encoding otherwise.
    [[nodiscard]] auto store_record(const chunk &value)
        -> result<storage_chunk>;
    /// Write encoded entries and optionally publish the store index.
    [[nodiscard]] auto write_entries(std::span<const snapshot_entry> entries,
                                     bool commit) -> status;
    [[nodiscard]] static auto section_index(std::int32_t y) noexcept
        -> std::int32_t;
    [[nodiscard]] static auto local_index(std::int32_t y, std::int32_t z,
                                          std::int32_t x) noexcept
        -> std::size_t;
    /// One player's gravity/vertical collision step; `tick()` applies it to
    /// every record in ascending entity id order.
    void tick_player(player_state &player) noexcept;

    store_type store_;
    /// Non-null when the view borrows a store it does not own; `store_` is
    /// then default-constructed and unused.
    store_type *borrowed_{nullptr};
    mutable std::mutex store_mutex_{};
    block_registry registry_{};
    std::map<chunk_pos, chunk, chunk_order> chunks_{};
    std::map<chunk_pos, std::uint32_t, chunk_order> tickets_{};
    player_table players_{};
    item_table items_{};
    zstd_policy zstd_policy_{};
    std::map<entity_id, entity_state> entities_{};
    struct scheduled_task {
        std::int64_t due{0};
        timer_id id{0};
        std::function<void()> fn{};
    };

    core::events::bus events_{};
    std::vector<scheduled_task> timers_{};
    timer_id next_timer_id_{1};
    rng rng_{};
    std::int64_t tick_{0};
};

} // namespace refrain::sim
