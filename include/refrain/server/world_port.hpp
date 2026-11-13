// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/contracts/services/sim_abi.h>
#include <refrain/contracts/services/storage_abi.h>
#include <refrain/contracts/services/world_abi.h>
#include <refrain/core/error.hpp>
#include <refrain/core/event.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/sim/world.hpp>
#include <refrain/world/maintenance.hpp>
#include <refrain/world/model.hpp>
#include <refrain/world/store.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::server {

/// Datapack inputs of the spawn-area fill. Kept beside the port because the
/// fill is one of the port operations a provider may own.
struct world_bootstrap_config {
    /// Datapack `data` directory the generator reads.
    std::string data_root{};
    /// Extracted structure templates; an empty root still generates
    /// procedural pieces and features.
    std::string templates_root{};
    std::int64_t seed{0};
    /// Chunk radius around the spawn chunk. Zero generates the spawn chunk
    /// only.
    std::int32_t radius{3};
    /// Generator workers; zero selects the hardware concurrency.
    std::size_t workers{0};
};

/// The active providers of the three services the runtime ticks over, plus
/// their ids. The ids decide which port implementation the runtime selects;
/// the vtables are the call surfaces a contract port uses.
///
/// A handle is null when the composition has no active provider for that
/// service. The pointers stay valid for the lifetime of the mod host, which
/// outlives the tick loop.
struct server_service_handles {
    const refrain_services_world_vtable *world{nullptr};
    void *world_self{nullptr};
    const refrain_services_sim_vtable *sim{nullptr};
    void *sim_self{nullptr};
    const refrain_services_storage_vtable *storage{nullptr};
    void *storage_self{nullptr};
    /// Active provider ids (`refrain.world.vanilla`, ...). Empty when the
    /// service is inactive.
    std::string world_provider{};
    std::string sim_provider{};
    std::string storage_provider{};
};

/// Which implementation of `world_port` the runtime uses.
enum class world_port_choice : std::uint8_t {
    /// First-party in-process provider: the runtime owns the `sim::world`
    /// directly and its hot paths take no contract hop.
    direct = 0,
    /// Any other registered provider: every port call goes through the
    /// `refrain:world` / `refrain:sim` / `refrain:storage` ABI.
    contract = 1,
};

/// The world operations the runtime performs while it boots and ticks.
///
/// This is a port, not a copy of `sim::world`: a method exists here because
/// the runtime, the console, the maintenance service or the chunk pipeline
/// calls it. Operations the runtime never performs stay on `sim::world`.
///
/// Accessors that expose process-local state (`players`, `registry`, `events`,
/// `items`, `entities`) return null when the implementation has no in-process
/// world to expose; callers treat null as "not available" rather than
/// dereferencing it. A port that cannot boot the runtime fails in `open()` or
/// `level()` with a message naming the missing contract surface.
class world_port {
  public:
    world_port() = default;
    world_port(const world_port &) = delete;
    auto operator=(const world_port &) -> world_port & = delete;
    world_port(world_port &&) = delete;
    auto operator=(world_port &&) -> world_port & = delete;
    virtual ~world_port() = default;

    // --- lifecycle ---------------------------------------------------------

    /// Open (or adopt) the world at `path`. A port owns whatever it opens and
    /// releases it in `close()`.
    [[nodiscard]] virtual auto open(std::string_view path) -> status = 0;

    /// Release the world. Idempotent; a dirty world must be flushed first.
    virtual void close() noexcept = 0;

    /// Level projection the runtime reads for seed, spawn, game mode,
    /// difficulty and the empty-world decision. It is a read: the port never
    /// exposes the level directory to a provider.
    [[nodiscard]] virtual auto level() -> result<world::level_projection> = 0;

    [[nodiscard]] virtual auto loaded_chunks() const noexcept
        -> std::size_t = 0;

    [[nodiscard]] virtual auto set_zstd_level(int level) -> status = 0;
    [[nodiscard]] virtual auto set_zstd_policy(const sim::zstd_policy &policy)
        -> status = 0;
    [[nodiscard]] virtual auto zstd_level() const noexcept -> int = 0;

    virtual void seed(std::uint64_t value) noexcept = 0;
    virtual void set_item_table(sim::item_table table) = 0;

    // --- in-process state --------------------------------------------------

    [[nodiscard]] virtual auto players() noexcept -> sim::player_table * = 0;
    [[nodiscard]] virtual auto registry() noexcept -> sim::block_registry * = 0;
    [[nodiscard]] virtual auto events() noexcept -> core::events::bus * = 0;
    [[nodiscard]] virtual auto items() const noexcept
        -> const sim::item_table * = 0;
    [[nodiscard]] virtual auto entities() const noexcept
        -> const std::map<sim::entity_id, sim::entity_state> * = 0;

    // --- chunk tickets -----------------------------------------------------

    [[nodiscard]] virtual auto add_ticket(sim::chunk_pos pos,
                                          sim::ticket_kind kind) -> status = 0;
    [[nodiscard]] virtual auto ticket_count(sim::chunk_pos pos) const noexcept
        -> std::size_t = 0;

    // --- players -----------------------------------------------------------

    [[nodiscard]] virtual auto
    add_player(sim::player_table::registration record)
        -> result<sim::entity_id> = 0;
    virtual void set_player_position(sim::player_state &player, double x,
                                     double y, double z) noexcept = 0;
    [[nodiscard]] virtual auto move_player(sim::player_state &player, double dx,
                                           double dy, double dz) -> bool = 0;
    virtual void jump(sim::player_state &player) noexcept = 0;

    // --- inventory ---------------------------------------------------------

    [[nodiscard]] virtual auto add_item(sim::player_state &player,
                                        std::string_view item,
                                        std::uint32_t count = 1) -> status = 0;
    [[nodiscard]] virtual auto remove_item(sim::player_state &player,
                                           std::string_view item,
                                           std::uint32_t count = 1) -> bool = 0;
    [[nodiscard]] virtual auto damage_item(sim::player_state &player,
                                           std::size_t index,
                                           std::uint32_t amount = 1)
        -> sim::damage_result = 0;

    // --- entities ----------------------------------------------------------

    [[nodiscard]] virtual auto spawn_entity(std::string_view type, double x,
                                            double y, double z)
        -> result<sim::entity_id> = 0;
    [[nodiscard]] virtual auto remove_entity(sim::entity_id id) -> bool = 0;

    // --- blocks ------------------------------------------------------------

    [[nodiscard]] virtual auto
    block_at(const sim::block_pos &pos) const noexcept
        -> sim::block_state_id = 0;
    [[nodiscard]] virtual auto set_block(const sim::block_pos &pos,
                                         sim::block_state_id value)
        -> status = 0;
    /// Apply a batch and publish the block event the world bus carries. The
    /// contract implementation forwards to `refrain:sim`'s `set_blocks`, which
    /// publishes the same event itself.
    [[nodiscard]] virtual auto
    set_blocks(std::span<const sim::block_change> changes)
        -> result<std::size_t> = 0;
    [[nodiscard]] virtual auto block_key(sim::block_state_id id) const
        -> std::string_view = 0;
    [[nodiscard]] virtual auto intern_block(std::string key)
        -> sim::block_state_id = 0;

    // --- chunks ------------------------------------------------------------

    /// Load (or adopt) a column and return the runtime view the packet encoder
    /// reads. Invalidated by `close()`/`rebuild_palette()`.
    [[nodiscard]] virtual auto chunk_load(const sim::chunk_pos &pos)
        -> result<sim::chunk *> = 0;

    /// Adopt a generated column into the runtime world.
    [[nodiscard]] virtual auto chunk_insert(const sim::chunk_pos &pos,
                                            sim::storage_chunk column)
        -> result<sim::chunk *> = 0;

    /// Persist one storage record and commit the store. Used by the spawn-area
    /// fill, which writes columns before the runtime world holds them.
    [[nodiscard]] virtual auto write_chunk(const sim::chunk_pos &pos,
                                           const sim::storage_chunk &column)
        -> status = 0;
    [[nodiscard]] virtual auto commit() -> status = 0;

    // --- snapshots and store maintenance -----------------------------------

    [[nodiscard]] virtual auto snapshot_pending()
        -> std::vector<sim::pending_snapshot> = 0;
    [[nodiscard]] virtual auto
    write_pending(std::span<const sim::pending_snapshot> pending)
        -> result<std::vector<sim::snapshot_entry>> = 0;
    virtual void
    mark_clean(std::span<const sim::snapshot_entry> snapshot) noexcept = 0;
    [[nodiscard]] virtual auto flush() -> status = 0;
    [[nodiscard]] virtual auto compact_stores()
        -> result<world::world_compact> = 0;
    [[nodiscard]] virtual auto compact_regions(std::uint64_t min_bytes,
                                               std::uint32_t garbage_percent)
        -> result<std::size_t> = 0;
    [[nodiscard]] virtual auto rebuild_palette(unsigned threads)
        -> result<world::palette_maintenance_report> = 0;
    [[nodiscard]] virtual auto store_valid() const noexcept -> bool = 0;

    // --- tick, hash, gameplay settings -------------------------------------

    virtual void tick() noexcept = 0;
    [[nodiscard]] virtual auto ticks() const noexcept -> std::int64_t = 0;
    [[nodiscard]] virtual auto state_hash() const -> data::digest32 = 0;

    [[nodiscard]] virtual auto read_gameplay()
        -> result<world::gameplay_settings> = 0;
    [[nodiscard]] virtual auto
    write_gameplay(const world::gameplay_settings &settings) -> status = 0;

    // --- spawn-area fill ---------------------------------------------------

    /// Generate every column of the square around `center` that the store does
    /// not already hold. Safe to repeat after an interrupted start.
    [[nodiscard]] virtual auto
    fill_spawn_area(const world_bootstrap_config &config,
                    world::chunk_pos center) -> result<std::size_t> = 0;
};

/// True when every handle belongs to the first-party in-process provider set
/// (`refrain.world.vanilla`, `refrain.sim.vanilla`, `refrain.storage.rwf`).
[[nodiscard]] auto
is_first_party_world(const server_service_handles &services) noexcept -> bool;

/// The selection rule.
///
/// The first-party providers run in this process and own a `sim::world` the
/// runtime could reuse, so the direct port keeps the tick loop on the same
/// object and the same store with no per-operation ABI hop. Any other
/// composition is a different implementation: it must be reached through the
/// contract, because there is no in-process object to borrow.
[[nodiscard]] auto
select_world_port(const server_service_handles &services) noexcept
    -> world_port_choice;

/// Build the port selected by `select_world_port`. The direct port owns the
/// `sim::world` it opens, so the caller keeps the returned port alive for as
/// long as the tick loop runs.
///
/// Cost of the two choices:
///  * direct: no ABI hop and no handle validation per operation; the port call
///    itself is one virtual dispatch, which devirtualization cannot remove
///    across the public boundary. The block, chunk and snapshot paths are
///    batched, so that is one dispatch per batch, not per block.
///  * contract: one ABI call per port operation plus the provider's own
///    bookkeeping. Chunk payloads do not cross `refrain:world` v1, so a
///    contract port cannot serve the packet encoder.
[[nodiscard]] auto make_world_port(const server_service_handles &services)
    -> std::unique_ptr<world_port>;

/// Wrap an already-open `sim::world` in the direct port. The caller keeps the
/// world alive for at least as long as the returned port; `open()` on it fails
/// because the port already holds a world.
[[nodiscard]] auto make_borrowed_world_port(sim::world &world)
    -> std::unique_ptr<world_port>;

} // namespace refrain::server
