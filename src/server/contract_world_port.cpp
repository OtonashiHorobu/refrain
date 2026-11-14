// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Contract implementation of `server::world_port` over the generated
// `refrain:world`, `refrain:sim` and `refrain:storage` ABIs. Kept in its own
// translation unit so the direct path and the selection rule stay small.

#include <refrain/server/world_port.hpp>

#include <refrain/contracts/abi.hpp>
#include <refrain/contracts/services/sim.hpp>
#include <refrain/contracts/services/storage.hpp>
#include <refrain/contracts/services/world.hpp>
#include <refrain/core/error.hpp>
#include <refrain/core/log.hpp>
#include <refrain/server/world_bootstrap.hpp>
#include <refrain/world/chunk_codec.hpp>

#include <fmt/format.h>

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

[[nodiscard]] auto contract_error(std::string_view operation,
                                  std::string_view reason) -> refrain::error {
    return refrain::make_error(
        refrain::error_code::unsupported,
        fmt::format("contract world port cannot serve {}: {}", operation,
                    reason));
}

[[nodiscard]] auto contract_status(std::string_view operation,
                                   std::string_view reason) -> refrain::status {
    return std::unexpected{contract_error(operation, reason)};
}

/// A provider that answers an optional call with an error is reporting a real
/// failure: the empty slot is how a provider declares a setting does not
/// apply, so an error status from a filled slot is never a skip.
[[nodiscard]] auto provider_failure(std::string_view call, std::uint32_t code)
    -> refrain::status {
    const auto rejected{
        code == static_cast<std::uint32_t>(
                    refrain::services::world::world_error::out_of_bounds)};
    return std::unexpected{refrain::make_error(
        rejected ? refrain::error_code::invalid_argument
                 : refrain::error_code::io_error,
        fmt::format("refrain:world {} failed ({})", call, code))};
}

constexpr refrain::log::logger log_world{"world"};

/// `refrain:world` v1 carries section metadata but no block-array payload, so
/// a contract port cannot hand the packet encoder a runtime column. Every
/// operation that needs block payloads names the same gap.
constexpr std::string_view chunk_payload_gap{
    "refrain:world v1 exposes section metadata, not block arrays"};

} // namespace

namespace refrain::server {

namespace abi = refrain::contracts;

namespace {
/// The contract path: every operation is an ABI call on the active
/// `refrain:world`, `refrain:sim` and `refrain:storage` providers.
///
/// The port opens `refrain:storage` once and hands the resulting handle to the
/// world and sim providers, so one store serves the whole composition however
/// it is composed: a provider never opens the level directory itself, and
/// mixing a replacement with a first-party provider no longer double-opens.
class contract_world_port final : public world_port {
  public:
    explicit contract_world_port(server_service_handles handles)
        : handles_{std::move(handles)} {}

    auto open(std::string_view path) -> status override {
        if (handles_.world == nullptr || handles_.sim == nullptr ||
            handles_.storage == nullptr) {
            return std::unexpected{make_error(
                error_code::not_found, "the composition has no active world, "
                                       "sim or storage provider")};
        }
        // The generated wrapper reads slots past the header without checking
        // the table length, so a provider built against an older layout is
        // rejected here by its declared size instead of running a stale table.
        if (handles_.world->struct_size < REFRAIN_SERVICES_WORLD_VTABLE_SIZE) {
            return std::unexpected{make_error(
                error_code::version_mismatch,
                fmt::format("refrain:world provider vtable is {} bytes, the "
                            "runtime needs the {}.{}.{} layout ({} bytes)",
                            handles_.world->struct_size,
                            REFRAIN_SERVICES_WORLD_ABI_MAJOR,
                            REFRAIN_SERVICES_WORLD_ABI_MINOR,
                            REFRAIN_SERVICES_WORLD_ABI_PATCH,
                            REFRAIN_SERVICES_WORLD_VTABLE_SIZE))};
        }
        if (opened_) {
            return std::unexpected{
                make_error(error_code::already_exists,
                           "contract world port already open")};
        }
        // One store, opened once: the world and sim providers receive the
        // handle instead of opening the directory themselves.
        auto storage{storage_provider().open_world(make_storage_config(path))};
        if (!storage) {
            return std::unexpected{make_error(
                error_code::io_error,
                fmt::format("refrain:storage open_world failed ({})",
                            static_cast<std::uint32_t>(storage.error())))};
        }
        storage_world_ = core::storage_world{storage->world};
        auto sim{sim_provider().open_world(make_sim_config(*storage))};
        if (!sim) {
            static_cast<void>(storage_provider().close_world(storage_world_));
            storage_world_ = {};
            return std::unexpected{make_error(
                error_code::io_error,
                fmt::format("refrain:sim open_world failed ({})",
                            static_cast<std::uint32_t>(sim.error())))};
        }
        sim_world_ = *sim;
        if (auto opened{
                world_provider().open_world(make_world_config(*storage))};
            !opened) {
            static_cast<void>(sim_provider().close_world(sim_world_));
            sim_world_ = {};
            static_cast<void>(storage_provider().close_world(storage_world_));
            storage_world_ = {};
            return std::unexpected{make_error(
                error_code::io_error,
                fmt::format("refrain:world open_world failed ({})",
                            static_cast<std::uint32_t>(opened.error())))};
        }
        world_open_ = true;
        opened_ = true;
        return status_ok();
    }

    void close() noexcept override {
        if (!opened_) {
            return;
        }
        opened_ = false;
        // The providers hold views over the storage handle, so they close
        // before the store they were given does.
        if (world_open_) {
            static_cast<void>(world_provider().close_world());
            world_open_ = false;
        }
        if (sim_world_ != services::sim::sim_world{}) {
            static_cast<void>(sim_provider().close_world(sim_world_));
            sim_world_ = {};
        }
        if (storage_world_ != core::storage_world{}) {
            static_cast<void>(storage_provider().close_world(storage_world_));
            storage_world_ = {};
        }
    }

    auto level() -> result<world::level_projection> override {
        if (!world_open_) {
            return std::unexpected{
                contract_error("level", "the world is not open")};
        }
        if (world_provider().table().level == nullptr) {
            // A table with the current length may still leave an optional slot
            // empty; name the gap instead of calling through null.
            return std::unexpected{contract_error(
                "level", "the provider does not implement the level "
                         "projection")};
        }
        auto projected{world_provider().level()};
        if (!projected) {
            if (projected.error() ==
                services::world::world_error::not_implemented) {
                return std::unexpected{contract_error(
                    "level", "the provider does not implement the level "
                             "projection")};
            }
            return std::unexpected{make_error(
                error_code::io_error,
                fmt::format("refrain:world level failed ({})",
                            static_cast<std::uint32_t>(projected.error())))};
        }
        world::level_info record{};
        record.data_version = projected->level.data_version;
        record.seed = projected->level.seed;
        record.spawn_x = projected->level.spawn_x;
        record.spawn_y = projected->level.spawn_y;
        record.spawn_z = projected->level.spawn_z;
        record.time = projected->level.time;
        // The ABI string is borrowed for the duration of the call, so own it
        // before the provider can reuse its buffer.
        record.level_name =
            std::string{abi::as_string_view(projected->level.level_name)};
        record.game_mode = projected->level.game_mode;
        record.hardcore = projected->level.hardcore != 0;
        record.difficulty = projected->level.difficulty;
        record.difficulty_locked = projected->level.difficulty_locked != 0;
        return world::level_projection{.level = std::move(record),
                                       .has_chunks =
                                           projected->has_chunks != 0};
    }

    auto loaded_chunks() const noexcept -> std::size_t override { return 0; }

    auto set_zstd_level(int level) -> status override {
        if (!world_open_) {
            return contract_status("set_zstd_level", "the world is not open");
        }
        if (world_provider().table().set_zstd_level == nullptr) {
            // An empty slot means the provider does not apply compression
            // settings: nothing to configure, and the requested level does not
            // take effect. Report that once so an operator who pinned a level
            // learns the provider ignored it.
            if (!zstd_level_skip_reported_) {
                log_world.warn("the provider applies its own compression "
                               "policy, so zstd level {} is not applied",
                               level);
                zstd_level_skip_reported_ = true;
            }
            return status_ok();
        }
        auto configured{world_provider().set_zstd_level(level)};
        if (!configured) {
            return provider_failure(
                "set_zstd_level",
                static_cast<std::uint32_t>(configured.error()));
        }
        return status_ok();
    }

    auto set_zstd_policy(const sim::zstd_policy &policy) -> status override {
        if (!world_open_) {
            return contract_status("set_zstd_policy", "the world is not open");
        }
        if (world_provider().table().set_zstd_policy == nullptr) {
            if (!zstd_policy_skip_reported_) {
                log_world.warn("the provider applies its own compression "
                               "policy, so the zstd policy is not applied");
                zstd_policy_skip_reported_ = true;
            }
            return status_ok();
        }
        auto abi_policy{services::world::make_zstd_policy()};
        abi_policy.near_level = policy.near_level;
        abi_policy.mid_level = policy.mid_level;
        abi_policy.far_level = policy.far_level;
        abi_policy.near_radius = policy.near_radius;
        abi_policy.mid_radius = policy.mid_radius;
        auto configured{world_provider().set_zstd_policy(abi_policy)};
        if (!configured) {
            return provider_failure(
                "set_zstd_policy",
                static_cast<std::uint32_t>(configured.error()));
        }
        return status_ok();
    }

    auto zstd_level() const noexcept -> std::optional<int> override {
        if (!world_open_ || world_provider().table().zstd_level == nullptr) {
            // A provider that does not apply compression settings has no
            // effective level to report; absence of the setting is not a
            // failure.
            return std::nullopt;
        }
        return static_cast<int>(world_provider().zstd_level());
    }

    void seed(std::uint64_t) noexcept override {}

    void set_item_table(sim::item_table) override {}

    auto players() noexcept -> sim::player_table * override { return nullptr; }

    auto registry() noexcept -> sim::block_registry * override {
        return nullptr;
    }

    auto events() noexcept -> core::events::bus * override { return nullptr; }

    auto items() const noexcept -> const sim::item_table * override {
        return nullptr;
    }

    auto entities() const noexcept
        -> const std::map<sim::entity_id, sim::entity_state> * override {
        return nullptr;
    }

    auto add_ticket(sim::chunk_pos, sim::ticket_kind) -> status override {
        // TODO: `refrain:world` v1 has no ticket call; the runtime keeps its
        // spawn tickets in the direct port only.
        return contract_status("add_ticket",
                               "refrain:world v1 has no ticket call");
    }

    auto ticket_count(sim::chunk_pos) const noexcept -> std::size_t override {
        return 0;
    }

    auto add_player(sim::player_table::registration)
        -> result<sim::entity_id> override {
        // TODO: `refrain:sim` v1 exposes no player table; a replacement
        // provider cannot register the console player.
        return std::unexpected{contract_error(
            "add_player", "refrain:sim v1 exposes no player table")};
    }

    void set_player_position(sim::player_state &, double, double,
                             double) noexcept override {}

    auto move_player(sim::player_state &, double, double, double)
        -> bool override {
        return false;
    }

    void jump(sim::player_state &) noexcept override {}

    auto add_item(sim::player_state &, std::string_view, std::uint32_t)
        -> status override {
        return contract_status("add_item",
                               "refrain:sim v1 exposes no player inventory");
    }

    auto remove_item(sim::player_state &, std::string_view, std::uint32_t)
        -> bool override {
        return false;
    }

    auto damage_item(sim::player_state &, std::size_t, std::uint32_t)
        -> sim::damage_result override {
        return sim::damage_result::unchanged;
    }

    auto spawn_entity(std::string_view type, double x, double y, double z)
        -> result<sim::entity_id> override {
        if (sim_world_ == services::sim::sim_world{}) {
            return std::unexpected{
                contract_error("spawn_entity", "the sim world is not open")};
        }
        auto spawned{sim_provider().spawn_entity(sim_world_, type, x, y, z)};
        if (!spawned) {
            return std::unexpected{make_error(
                error_code::io_error,
                fmt::format("refrain:sim spawn_entity failed ({})",
                            static_cast<std::uint32_t>(spawned.error())))};
        }
        return *spawned;
    }

    auto remove_entity(sim::entity_id id) -> bool override {
        return sim_world_ != services::sim::sim_world{} &&
               sim_provider().remove_entity(sim_world_, id);
    }

    auto block_at(const sim::block_pos &pos) const noexcept
        -> sim::block_state_id override {
        if (!world_open_) {
            return sim::air_block;
        }
        auto abi_pos{core::make_block_pos()};
        abi_pos.x = pos.x;
        abi_pos.y = pos.y;
        abi_pos.z = pos.z;
        return world_provider().block_at(abi_pos);
    }

    auto set_block(const sim::block_pos &pos, sim::block_state_id value)
        -> status override {
        const sim::block_change change{
            .pos = pos, .old_state = block_at(pos), .new_state = value};
        auto applied{set_blocks(std::span{&change, 1})};
        if (!applied) {
            return std::unexpected{applied.error()};
        }
        return status_ok();
    }

    auto set_blocks(std::span<const sim::block_change> changes)
        -> result<std::size_t> override {
        if (sim_world_ == services::sim::sim_world{}) {
            return std::unexpected{
                contract_error("set_blocks", "the sim world is not open")};
        }
        std::vector<core::block_change> batch{};
        batch.reserve(changes.size());
        for (const auto &change : changes) {
            auto abi_change{core::make_block_change()};
            abi_change.pos.x = change.pos.x;
            abi_change.pos.y = change.pos.y;
            abi_change.pos.z = change.pos.z;
            abi_change.old_state = change.old_state;
            abi_change.new_state = change.new_state;
            batch.push_back(abi_change);
        }
        auto applied{sim_provider().set_blocks(sim_world_, batch)};
        if (!applied) {
            return std::unexpected{make_error(
                error_code::io_error,
                fmt::format("refrain:sim set_blocks failed ({})",
                            static_cast<std::uint32_t>(applied.error())))};
        }
        return static_cast<std::size_t>(*applied);
    }

    auto block_key(sim::block_state_id) const -> std::string_view override {
        // TODO: `refrain:world`/`refrain:sim` v1 carry runtime state ids, not
        // the canonical key table; a contract port cannot name a block.
        return {};
    }

    auto intern_block(std::string) -> sim::block_state_id override {
        // TODO: the same missing registry mapping means a command cannot
        // resolve `minecraft:stone` to a provider state id.
        return sim::air_block;
    }

    auto chunk_load(const sim::chunk_pos &) -> result<sim::chunk *> override {
        return std::unexpected{contract_error("chunk_load", chunk_payload_gap)};
    }

    auto chunk_insert(const sim::chunk_pos &, sim::storage_chunk)
        -> result<sim::chunk *> override {
        // TODO: `refrain:world` v1 has no insert call, and `refrain:storage`
        // can persist a column but cannot adopt it into the provider's live
        // world, so streamed terrain cannot reach a contract port.
        return std::unexpected{contract_error(
            "chunk_insert", "refrain:world v1 has no adopt/insert call")};
    }

    auto write_chunk(const sim::chunk_pos &pos,
                     const sim::storage_chunk &column) -> status override {
        if (storage_world_ == core::storage_world{}) {
            return contract_status("write_chunk",
                                   "the storage world is not open");
        }
        auto payload{world::encode_chunk(
            column, nullptr, true, world::chunk_palette_mode::inline_keys)};
        if (!payload) {
            return std::unexpected{std::move(payload.error())};
        }
        auto key{services::storage::make_chunk_key()};
        key.pos.x = pos.x;
        key.pos.z = pos.z;
        key.format_version = world::chunk_format_version;
        const auto bytes{std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t *>(payload->data()),
            payload->size()}};
        auto written{
            storage_provider().write_chunk(storage_world_, key, bytes)};
        if (!written) {
            return std::unexpected{make_error(
                error_code::io_error,
                fmt::format("refrain:storage write_chunk failed ({})",
                            static_cast<std::uint32_t>(written.error())))};
        }
        return status_ok();
    }

    auto commit() -> status override {
        if (storage_world_ == core::storage_world{}) {
            return contract_status("commit", "the storage world is not open");
        }
        auto synced{storage_provider().sync(storage_world_)};
        if (!synced) {
            return std::unexpected{make_error(
                error_code::io_error,
                fmt::format("refrain:storage sync failed ({})",
                            static_cast<std::uint32_t>(synced.error())))};
        }
        return status_ok();
    }

    auto snapshot_pending() -> std::vector<sim::pending_snapshot> override {
        // TODO: the contracts expose block batches and chunk writes but no
        // dirty-chunk snapshot/revision lifecycle, so a contract port cannot
        // hand autosave a batch yet; a provider that grows a snapshot call
        // replaces this empty result.
        return {};
    }

    auto write_pending(std::span<const sim::pending_snapshot>)
        -> result<std::vector<sim::snapshot_entry>> override {
        return std::unexpected{contract_error(
            "write_pending",
            "refrain:sim/refrain:storage v1 have no snapshot encoding call")};
    }

    void mark_clean(std::span<const sim::snapshot_entry>) noexcept override {}

    auto flush() -> status override { return commit(); }

    auto compact_stores() -> result<world::world_compact> override {
        return std::unexpected{contract_error(
            "compact_stores",
            "refrain:storage v1 has no aux-store compaction call")};
    }

    auto compact_regions(std::uint64_t, std::uint32_t)
        -> result<std::size_t> override {
        return std::unexpected{
            contract_error("compact_regions",
                           "refrain:storage v1 has no region compaction call")};
    }

    auto rebuild_palette(unsigned)
        -> result<world::palette_maintenance_report> override {
        return std::unexpected{
            contract_error("rebuild_palette",
                           "refrain:storage v1 has no palette-rebuild call")};
    }

    auto store_valid() const noexcept -> bool override {
        return opened_ && storage_world_ != core::storage_world{} &&
               storage_provider().is_open(storage_world_);
    }

    void tick() noexcept override {
        if (sim_world_ != services::sim::sim_world{}) {
            static_cast<void>(sim_provider().tick(sim_world_));
        }
    }

    auto ticks() const noexcept -> std::int64_t override {
        return sim_world_ == services::sim::sim_world{}
                   ? 0
                   : sim_provider().ticks(sim_world_);
    }

    auto state_hash() const -> data::digest32 override {
        if (sim_world_ == services::sim::sim_world{}) {
            return {};
        }
        const auto hex{sim_provider().state_hash(sim_world_)};
        auto parsed{data::parse_digest(hex)};
        if (!parsed) {
            return {};
        }
        return *parsed;
    }

    auto read_gameplay() -> result<world::gameplay_settings> override {
        return std::unexpected{contract_error(
            "read_gameplay",
            "refrain:world v1 exposes no gameplay-settings projection")};
    }

    auto write_gameplay(const world::gameplay_settings &) -> status override {
        return contract_status(
            "write_gameplay",
            "refrain:world v1 exposes no gameplay-settings projection");
    }

    auto fill_spawn_area(const world_bootstrap_config &config,
                         world::chunk_pos center)
        -> result<std::size_t> override {
        return generate_spawn_area(*this, config, center);
    }

  private:
    [[nodiscard]] auto sim_provider() const -> services::sim::provider {
        return {*handles_.sim, handles_.sim_self};
    }

    [[nodiscard]] auto world_provider() const -> services::world::provider {
        return {*handles_.world, handles_.world_self};
    }

    [[nodiscard]] auto storage_provider() const -> services::storage::provider {
        return {*handles_.storage, handles_.storage_self};
    }

    [[nodiscard]] static auto
    make_sim_config(const services::storage::storage_handle &storage)
        -> services::sim::sim_config {
        auto config{services::sim::make_sim_config()};
        config.storage = storage;
        config.seed = 0;
        return config;
    }

    [[nodiscard]] static auto
    make_world_config(const services::storage::storage_handle &storage)
        -> services::world::world_config {
        auto config{services::world::make_world_config()};
        config.storage = storage;
        config.seed = 0;
        return config;
    }

    [[nodiscard]] static auto make_storage_config(std::string_view path)
        -> services::storage::world_open_config {
        auto config{services::storage::make_world_open_config()};
        config.world_path = abi::to_borrowed(path);
        config.gamepack_id = abi::to_borrowed("refrain");
        config.flags = 0;
        return config;
    }

    server_service_handles handles_{};
    services::sim::sim_world sim_world_{};
    core::storage_world storage_world_{};
    bool world_open_{false};
    bool opened_{false};
    /// One line per skipped compression setting: the first call names the
    /// setting, later calls stay silent.
    bool zstd_level_skip_reported_{false};
    bool zstd_policy_skip_reported_{false};
};

} // namespace

namespace detail {

auto make_contract_world_port(const server_service_handles &services)
    -> std::unique_ptr<world_port> {
    return std::make_unique<contract_world_port>(services);
}

} // namespace detail

} // namespace refrain::server
