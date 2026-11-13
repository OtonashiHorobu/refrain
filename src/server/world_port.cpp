// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/world_port.hpp>

#include <refrain/core/error.hpp>
#include <refrain/server/world_bootstrap.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "world_port_impl.hpp"

namespace {

constexpr std::string_view first_party_world{"refrain.world.vanilla"};
constexpr std::string_view first_party_sim{"refrain.sim.vanilla"};
constexpr std::string_view first_party_storage{"refrain.storage.rwf"};

} // namespace

namespace refrain::server {

// --- selection --------------------------------------------------------------

auto is_first_party_world(const server_service_handles &services) noexcept
    -> bool {
    return services.world != nullptr && services.sim != nullptr &&
           services.storage != nullptr &&
           services.world_provider == first_party_world &&
           services.sim_provider == first_party_sim &&
           services.storage_provider == first_party_storage;
}

auto select_world_port(const server_service_handles &services) noexcept
    -> world_port_choice {
    return is_first_party_world(services) ? world_port_choice::direct
                                          : world_port_choice::contract;
}

// --- direct port ------------------------------------------------------------

namespace {

/// The first-party path: the runtime owns the `sim::world` and every port call
/// lands on it unchanged. Also usable as a borrowed wrapper around a world a
/// test already opened.
class direct_world_port final : public world_port {
  public:
    direct_world_port() = default;
    explicit direct_world_port(sim::world &world) noexcept : world_{&world} {}

    auto open(std::string_view path) -> status override {
        if (world_ != nullptr) {
            return std::unexpected{
                make_error(error_code::already_exists,
                           "direct world port already holds an open world")};
        }
        auto opened{sim::world::open(path)};
        if (!opened) {
            return std::unexpected{std::move(opened.error())};
        }
        owned_.emplace(std::move(*opened));
        world_ = &*owned_;
        return status_ok();
    }

    void close() noexcept override {
        world_ = nullptr;
        owned_.reset();
    }

    auto level() -> result<world::level_projection> override {
        return world().store().project_level();
    }

    auto loaded_chunks() const noexcept -> std::size_t override {
        return world().loaded_chunks();
    }

    auto set_zstd_level(int level) -> status override {
        return world().store().set_zstd_level(level);
    }

    auto set_zstd_policy(const sim::zstd_policy &policy) -> status override {
        return world().set_zstd_policy(policy);
    }

    auto zstd_level() const noexcept -> std::optional<int> override {
        return world_ == nullptr
                   ? std::nullopt
                   : std::optional<int>{world().store().zstd_level()};
    }

    void seed(std::uint64_t value) noexcept override { world().seed(value); }

    void set_item_table(sim::item_table table) override {
        world().set_item_table(std::move(table));
    }

    auto players() noexcept -> sim::player_table * override {
        return world_ == nullptr ? nullptr : &world().players();
    }

    auto registry() noexcept -> sim::block_registry * override {
        return world_ == nullptr ? nullptr : &world().registry();
    }

    auto events() noexcept -> core::events::bus * override {
        return world_ == nullptr ? nullptr : &world().events();
    }

    auto items() const noexcept -> const sim::item_table * override {
        return world_ == nullptr ? nullptr : &world().items();
    }

    auto entities() const noexcept
        -> const std::map<sim::entity_id, sim::entity_state> * override {
        return world_ == nullptr ? nullptr : &world().entities();
    }

    auto add_ticket(sim::chunk_pos pos, sim::ticket_kind kind)
        -> status override {
        return world().add_ticket(pos, kind);
    }

    auto ticket_count(sim::chunk_pos pos) const noexcept
        -> std::size_t override {
        return world().ticket_count(pos);
    }

    auto add_player(sim::player_table::registration record)
        -> result<sim::entity_id> override {
        return world().add_player(std::move(record));
    }

    void set_player_position(sim::player_state &player, double x, double y,
                             double z) noexcept override {
        world().set_player_position(player, x, y, z);
    }

    auto move_player(sim::player_state &player, double dx, double dy, double dz)
        -> bool override {
        return world().move_player(player, dx, dy, dz);
    }

    void jump(sim::player_state &player) noexcept override {
        world().jump(player);
    }

    auto add_item(sim::player_state &player, std::string_view item,
                  std::uint32_t count) -> status override {
        return world().add_item(player, item, count);
    }

    auto remove_item(sim::player_state &player, std::string_view item,
                     std::uint32_t count) -> bool override {
        return world().remove_item(player, item, count);
    }

    auto damage_item(sim::player_state &player, std::size_t index,
                     std::uint32_t amount) -> sim::damage_result override {
        return world().damage_item(player, index, amount);
    }

    auto spawn_entity(std::string_view type, double x, double y, double z)
        -> result<sim::entity_id> override {
        return world().spawn_entity(type, x, y, z);
    }

    auto remove_entity(sim::entity_id id) -> bool override {
        return world().remove_entity(id);
    }

    auto block_at(const sim::block_pos &pos) const noexcept
        -> sim::block_state_id override {
        return world().block_at(pos);
    }

    auto set_block(const sim::block_pos &pos, sim::block_state_id value)
        -> status override {
        return world().set_block(pos, value);
    }

    auto set_blocks(std::span<const sim::block_change> changes)
        -> result<std::size_t> override {
        auto applied{world().set_blocks(changes)};
        if (!applied) {
            return std::unexpected{std::move(applied.error())};
        }
        // The runtime publishes one typed event per applied change so an
        // in-process subscriber observes the same state the world committed.
        // `set_blocks` reports only the count, so use the batch the caller
        // supplied: it carries the previous state it read right before.
        if (auto *bus = events(); bus != nullptr && *applied > 0) {
            for (const auto &change : changes) {
                if (change.old_state == change.new_state) {
                    continue;
                }
                try {
                    bus->publish(services::sim::block_changed{
                        .x = static_cast<std::int32_t>(change.pos.x),
                        .y = static_cast<std::int32_t>(change.pos.y),
                        .z = static_cast<std::int32_t>(change.pos.z),
                        .old_state = change.old_state,
                        .new_state = change.new_state,
                    });
                } catch (...) {
                    // A listener failure must not roll back applied changes.
                }
            }
        }
        return *applied;
    }

    auto block_key(sim::block_state_id id) const -> std::string_view override {
        return world().registry().key(id);
    }

    auto intern_block(std::string key) -> sim::block_state_id override {
        return world().registry().intern(std::move(key));
    }

    auto chunk_load(const sim::chunk_pos &pos)
        -> result<sim::chunk *> override {
        auto loaded{world().ensure_chunk(pos)};
        if (!loaded) {
            return std::unexpected{std::move(loaded.error())};
        }
        return *loaded;
    }

    auto chunk_insert(const sim::chunk_pos &pos, sim::storage_chunk column)
        -> result<sim::chunk *> override {
        auto inserted{world().insert_chunk(pos, std::move(column))};
        if (!inserted) {
            return std::unexpected{std::move(inserted.error())};
        }
        return *inserted;
    }

    auto write_chunk(const sim::chunk_pos &pos,
                     const sim::storage_chunk &column) -> status override {
        return world().store().write_chunk(pos, column);
    }

    auto commit() -> status override { return world().store().commit(); }

    auto snapshot_pending() -> std::vector<sim::pending_snapshot> override {
        return world().snapshot_pending();
    }

    auto write_pending(std::span<const sim::pending_snapshot> pending)
        -> result<std::vector<sim::snapshot_entry>> override {
        return world().write_pending(pending);
    }

    void mark_clean(
        std::span<const sim::snapshot_entry> snapshot) noexcept override {
        world().mark_clean(snapshot);
    }

    auto flush() -> status override { return world().flush(); }

    auto compact_stores() -> result<world::world_compact> override {
        return world().compact_stores();
    }

    auto compact_regions(std::uint64_t min_bytes, std::uint32_t garbage_percent)
        -> result<std::size_t> override {
        return world().compact_regions(min_bytes, garbage_percent);
    }

    auto rebuild_palette(unsigned threads)
        -> result<world::palette_maintenance_report> override {
        return world().rebuild_palette_in_place(threads,
                                                world().store().zstd_level());
    }

    auto store_valid() const noexcept -> bool override {
        return world_ != nullptr && world().store().valid();
    }

    void tick() noexcept override { world().tick(); }

    auto ticks() const noexcept -> std::int64_t override {
        return world().ticks();
    }

    auto state_hash() const -> data::digest32 override {
        return world().state_hash();
    }

    auto read_gameplay() -> result<world::gameplay_settings> override {
        return world().store().read_gameplay();
    }

    auto write_gameplay(const world::gameplay_settings &settings)
        -> status override {
        return world().store().write_gameplay(settings);
    }

    auto fill_spawn_area(const world_bootstrap_config &config,
                         world::chunk_pos center)
        -> result<std::size_t> override {
        return generate_spawn_area(*this, config, center);
    }

  private:
    [[nodiscard]] auto world() -> sim::world & { return *world_; }
    [[nodiscard]] auto world() const -> const sim::world & { return *world_; }

    std::optional<sim::world> owned_{};
    sim::world *world_{nullptr};
};

} // namespace

auto make_borrowed_world_port(sim::world &world)
    -> std::unique_ptr<world_port> {
    return std::make_unique<direct_world_port>(world);
}

auto make_world_port(const server_service_handles &services)
    -> std::unique_ptr<world_port> {
    if (select_world_port(services) == world_port_choice::direct) {
        return std::make_unique<direct_world_port>();
    }
    return detail::make_contract_world_port(services);
}

} // namespace refrain::server
