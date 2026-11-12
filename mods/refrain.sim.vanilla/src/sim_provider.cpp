// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// `refrain:sim` provider for the internal `refrain.sim.vanilla` mod.
//
// The simulation runs on a single sim thread, so this provider owns a table of
// open `sim::world` instances addressed by opaque handles. Lifecycle, tick and
// hash stay provider-internal; the block and entity mutations are exposed
// because a mod must be able to change the world, and every applied mutation is
// published as a contract event.
//
// A sim world is attached to the store the runtime opened once and handed down
// in `sim_config`; the provider never opens the level directory itself.
//
// Publication runs through the host kernel event bus (the stable C ABI a
// subscriber uses) and through the world's typed bus (the in-process C++ ABI).
// Both carry the same payload bytes: the generated C++ event is layout-checked
// against the generated C struct at compile time.

#include <refrain/contracts/services/sim.hpp>

#include <refrain/contracts/abi.h>
#include <refrain/contracts/abi.hpp>
#include <refrain/contracts/host/kernel.hpp>
#include <refrain/contracts/services/sim_abi.h>
#include <refrain/core/error.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/host/abi_bridge.hpp>
#include <refrain/mod/host.hpp>
#include <refrain/mod/static_mod.hpp>
#include <refrain/sim/world.hpp>
#include <refrain/world/store.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace abi = refrain::host::abi;
namespace sim = refrain::sim;
namespace sim_service = refrain::services::sim;

/// Status rows for the codes the `refrain:sim` contract distinguishes; every
/// other error is an internal failure.
constexpr abi::status_entry sim_statuses[]{
    {refrain::error_code::not_found, REFRAIN_SERVICES_SIM_SIM_ERROR_NOT_FOUND},
    {refrain::error_code::already_exists,
     REFRAIN_SERVICES_SIM_SIM_ERROR_ALREADY_OPEN},
    {refrain::error_code::io_error, REFRAIN_SERVICES_SIM_SIM_ERROR_IO_ERROR},
    {refrain::error_code::invalid_argument,
     REFRAIN_SERVICES_SIM_SIM_ERROR_INVALID_HANDLE},
};

/// Error sink for the block mutation calls: an out-of-range position is the
/// only argument error the world reports, and the contract names it.
auto block_error(const refrain::error &error)
    -> refrain_services_sim_sim_error {
    if (error.code == refrain::error_code::invalid_argument) {
        return REFRAIN_SERVICES_SIM_SIM_ERROR_OUT_OF_BOUNDS;
    }
    return abi::to_status(error, sim_statuses,
                          REFRAIN_SERVICES_SIM_SIM_ERROR_INTERNAL);
}

auto entity_error(const refrain::error &error)
    -> refrain_services_sim_sim_error {
    if (error.code == refrain::error_code::invalid_argument) {
        return REFRAIN_SERVICES_SIM_SIM_ERROR_INVALID_ARGUMENT;
    }
    return abi::to_status(error, sim_statuses,
                          REFRAIN_SERVICES_SIM_SIM_ERROR_INTERNAL);
}

/// Center of the chunk column that contains `value` (floor division).
constexpr auto chunk_of(std::int32_t value) noexcept -> std::int32_t {
    const auto wide{static_cast<std::int64_t>(value)};
    const auto edge{static_cast<std::int64_t>(::refrain::world::section_edge)};
    return static_cast<std::int32_t>((wide - (wide < 0 ? edge - 1 : 0)) / edge);
}

struct sim_provider {
    std::map<std::uint64_t, sim::world> worlds{};
    std::uint64_t next_handle{1};
    std::string hash_hex{};
    /// Sealed host kernel provider used to publish the C ABI payload. Empty
    /// when the composition registers no kernel service, in which case the
    /// typed in-process bus still carries the events.
    const refrain_host_kernel_vtable *kernel_vtable{nullptr};
    void *kernel_self{nullptr};

    [[nodiscard]] auto find(std::uint64_t handle) noexcept -> sim::world * {
        const auto found{worlds.find(handle)};
        return found == worlds.end() ? nullptr : &found->second;
    }

    /// Hand one generated C payload to the C ABI subscribers when the
    /// composition offers the host kernel. The typed C++ event is published
    /// separately on the world bus; the payload is converted field by field so
    /// no C++ layout crosses the boundary.
    void publish_bytes(std::uint64_t type, const void *payload,
                       std::size_t size) {
        if (kernel_vtable == nullptr || kernel_self == nullptr) {
            return;
        }
        const refrain::host::kernel::provider kernel{*kernel_vtable,
                                                     kernel_self};
        kernel.event_publish(type, const_cast<void *>(payload),
                             static_cast<std::uint64_t>(size));
    }

    /// One `block_changed` event per applied change, in ascending position
    /// order so a replay subscriber sees the same sequence on every run.
    void publish_block_batch(sim::world &world,
                             const sim::block_batch_event &batch) {
        std::vector<sim::block_change> changes{batch.changes.begin(),
                                               batch.changes.end()};
        std::stable_sort(
            changes.begin(), changes.end(),
            [](const sim::block_change &left, const sim::block_change &right) {
                if (left.pos.x != right.pos.x) {
                    return left.pos.x < right.pos.x;
                }
                if (left.pos.y != right.pos.y) {
                    return left.pos.y < right.pos.y;
                }
                return left.pos.z < right.pos.z;
            });
        for (const auto &change : changes) {
            const sim_service::block_changed event{
                .x = static_cast<std::int32_t>(change.pos.x),
                .y = static_cast<std::int32_t>(change.pos.y),
                .z = static_cast<std::int32_t>(change.pos.z),
                .old_state = change.old_state,
                .new_state = change.new_state,
            };
            world.events().publish(event);
            refrain_services_sim_block_changed payload{};
            payload.x = event.x;
            payload.y = event.y;
            payload.z = event.z;
            payload.old_state = event.old_state;
            payload.new_state = event.new_state;
            publish_bytes(sim_service::block_changed_type, &payload,
                          sizeof(payload));
        }
    }

    static auto open_world(void *self,
                           const refrain_services_sim_sim_config *config,
                           refrain_services_sim_handle_sim_world *out_value)
        -> refrain_status {
        if (self == nullptr || config == nullptr || out_value == nullptr ||
            config->struct_size != REFRAIN_SERVICES_SIM_SIM_CONFIG_SIZE) {
            return REFRAIN_SERVICES_SIM_SIM_ERROR_INVALID_HANDLE;
        }
        auto *store{
            static_cast<refrain::world::world_store *>(config->storage.store)};
        if (store == nullptr) {
            // TODO: drive the chunk cache through the `refrain:storage` ABI so
            // a storage provider that keeps its store private can host this
            // sim; today the view needs the in-process store and refuses with
            // the named error instead of opening the directory itself.
            return REFRAIN_SERVICES_SIM_SIM_ERROR_STORAGE_UNAVAILABLE;
        }
        auto &provider{*static_cast<sim_provider *>(self)};
        auto world{sim::world::attach(*store)};
        world.seed(static_cast<std::uint64_t>(config->seed));
        const auto handle{provider.next_handle++};
        auto inserted{provider.worlds.emplace(handle, std::move(world))};
        // Subscribe after the move: the world's event bus is not movable, so a
        // listener registered on the temporary would be lost.
        static_cast<void>(
            inserted.first->second.events().subscribe<sim::block_batch_event>(
                [&provider, handle](const sim::block_batch_event &batch) {
                    if (auto *opened{provider.find(handle)};
                        opened != nullptr) {
                        provider.publish_block_batch(*opened, batch);
                    }
                }));
        *out_value = handle;
        return REFRAIN_STATUS_OK;
    }

    static auto close_world(void *self,
                            refrain_services_sim_handle_sim_world handle)
        -> refrain_status {
        auto *provider{static_cast<sim_provider *>(self)};
        if (provider == nullptr) {
            return REFRAIN_SERVICES_SIM_SIM_ERROR_INVALID_HANDLE;
        }
        const auto found{provider->worlds.find(handle)};
        if (found == provider->worlds.end()) {
            return REFRAIN_SERVICES_SIM_SIM_ERROR_NOT_FOUND;
        }
        if (auto flushed{found->second.flush()}; !flushed) {
            return abi::to_status(flushed.error(), sim_statuses,
                                  REFRAIN_SERVICES_SIM_SIM_ERROR_INTERNAL);
        }
        provider->worlds.erase(found);
        return REFRAIN_STATUS_OK;
    }

    static auto tick(void *self, refrain_services_sim_handle_sim_world handle)
        -> refrain_status {
        auto *provider{static_cast<sim_provider *>(self)};
        if (provider == nullptr) {
            return REFRAIN_SERVICES_SIM_SIM_ERROR_INVALID_HANDLE;
        }
        auto *world{provider->find(handle)};
        if (world == nullptr) {
            return REFRAIN_SERVICES_SIM_SIM_ERROR_NOT_FOUND;
        }
        world->tick();
        return REFRAIN_STATUS_OK;
    }

    static auto ticks(void *self, refrain_services_sim_handle_sim_world handle)
        -> std::int64_t {
        auto *provider{static_cast<sim_provider *>(self)};
        if (provider == nullptr) {
            return -1;
        }
        auto *world{provider->find(handle)};
        return world == nullptr ? -1 : world->ticks();
    }

    static auto
    state_hash(void *self,
               refrain_services_sim_handle_sim_world handle) noexcept
        -> refrain_str {
        auto *provider{static_cast<sim_provider *>(self)};
        if (provider == nullptr) {
            return {};
        }
        auto *world{provider->find(handle)};
        if (world == nullptr) {
            return {};
        }
        provider->hash_hex = refrain::data::to_hex(world->state_hash());
        return abi::to_borrowed(provider->hash_hex);
    }

    static auto set_blocks(void *self,
                           refrain_services_sim_handle_sim_world handle,
                           refrain_core_slice_block_change changes,
                           std::uint32_t *out_value) -> refrain_status {
        if (out_value == nullptr) {
            return REFRAIN_SERVICES_SIM_SIM_ERROR_INVALID_ARGUMENT;
        }
        auto *provider{static_cast<sim_provider *>(self)};
        if (provider == nullptr) {
            return REFRAIN_SERVICES_SIM_SIM_ERROR_INVALID_HANDLE;
        }
        auto *world{provider->find(handle)};
        if (world == nullptr) {
            return REFRAIN_SERVICES_SIM_SIM_ERROR_NOT_FOUND;
        }
        if (changes.data == nullptr && changes.len != 0) {
            return REFRAIN_SERVICES_SIM_SIM_ERROR_INVALID_ARGUMENT;
        }
        // The contract carries `block_pos` as three 32-bit words; the runtime
        // keeps Y as a signed 16-bit coordinate. Convert field by field instead
        // of reinterpreting the slice.
        std::vector<sim::block_change> batch{};
        // `world::set_blocks` reads the previous state without loading a
        // column, so an unloaded column would look like air and the change
        // would be dropped. Load every touched column first.
        std::set<sim::chunk_pos> columns{};
        for (std::uint64_t index{0}; index < changes.len; ++index) {
            const auto &change{changes.data[index]};
            columns.insert(
                sim::chunk_pos{chunk_of(change.pos.x), chunk_of(change.pos.z)});
        }
        for (const auto &column : columns) {
            if (auto loaded{world->ensure_chunk(column)}; !loaded) {
                return block_error(loaded.error());
            }
        }
        batch.reserve(static_cast<std::size_t>(changes.len));
        for (std::uint64_t index{0}; index < changes.len; ++index) {
            const auto &change{changes.data[index]};
            batch.push_back(sim::block_change{
                .pos = sim::block_pos{change.pos.x,
                                      static_cast<std::int16_t>(change.pos.y),
                                      change.pos.z},
                .old_state = change.old_state,
                .new_state = change.new_state});
        }
        const auto applied{world->set_blocks(batch)};
        if (!applied) {
            return block_error(applied.error());
        }
        *out_value = static_cast<std::uint32_t>(*applied);
        return REFRAIN_STATUS_OK;
    }

    static auto spawn_entity(void *self,
                             refrain_services_sim_handle_sim_world handle,
                             refrain_str type, double x, double y, double z,
                             std::uint64_t *out_value) -> refrain_status {
        if (out_value == nullptr) {
            return REFRAIN_SERVICES_SIM_SIM_ERROR_INVALID_ARGUMENT;
        }
        auto *provider{static_cast<sim_provider *>(self)};
        if (provider == nullptr) {
            return REFRAIN_SERVICES_SIM_SIM_ERROR_INVALID_HANDLE;
        }
        auto *world{provider->find(handle)};
        if (world == nullptr) {
            return REFRAIN_SERVICES_SIM_SIM_ERROR_NOT_FOUND;
        }
        const auto type_view{abi::to_view(type)};
        auto id{world->spawn_entity(type_view, x, y, z)};
        if (!id) {
            return entity_error(id.error());
        }
        const sim_service::entity_spawned event{
            .id = *id, .type = type_view, .x = x, .y = y, .z = z};
        world->events().publish(event);
        refrain_services_sim_entity_spawned payload{};
        payload.id = *id;
        payload.type = refrain::contracts::to_borrowed(type_view);
        payload.x = x;
        payload.y = y;
        payload.z = z;
        provider->publish_bytes(sim_service::entity_spawned_type, &payload,
                                sizeof(payload));
        *out_value = *id;
        return REFRAIN_STATUS_OK;
    }

    static auto remove_entity(void *self,
                              refrain_services_sim_handle_sim_world handle,
                              std::uint64_t id) -> std::uint8_t {
        auto *provider{static_cast<sim_provider *>(self)};
        if (provider == nullptr) {
            return 0;
        }
        auto *world{provider->find(handle)};
        if (world == nullptr || !world->remove_entity(id)) {
            return 0;
        }
        world->events().publish(sim_service::entity_removed{.id = id});
        refrain_services_sim_entity_removed payload{};
        payload.id = id;
        provider->publish_bytes(sim_service::entity_removed_type, &payload,
                                sizeof(payload));
        return 1;
    }
};

auto sim_vtable() -> const refrain_services_sim_vtable & {
    static const refrain_services_sim_vtable table{
        .struct_size = REFRAIN_SERVICES_SIM_VTABLE_SIZE,
        .abi_major = REFRAIN_SERVICES_SIM_ABI_MAJOR,
        .abi_minor = REFRAIN_SERVICES_SIM_ABI_MINOR,
        .abi_patch = REFRAIN_SERVICES_SIM_ABI_PATCH,
        .open_world = &sim_provider::open_world,
        .close_world = &sim_provider::close_world,
        .tick = &sim_provider::tick,
        .ticks = &sim_provider::ticks,
        .state_hash = &sim_provider::state_hash,
        .set_blocks = &sim_provider::set_blocks,
        .spawn_entity = &sim_provider::spawn_entity,
        .remove_entity = &sim_provider::remove_entity,
    };
    return table;
}

auto make_sim_vanilla(refrain::mod::host &host) -> refrain::status {
    static std::unique_ptr<sim_provider> provider{};
    provider = std::make_unique<sim_provider>();
    // The kernel service is sealed and registered before any mod factory runs;
    // when the embedding registers it, events also reach C ABI subscribers.
    if (const auto kernel{
            host.services().active_provider("refrain:host.kernel")};
        kernel && kernel->instance != nullptr) {
        const auto *api{
            static_cast<const refrain::mod::provider_api *>(kernel->instance)};
        if (api->vtable != nullptr) {
            const auto *vtable{
                static_cast<const refrain_host_kernel_vtable *>(api->vtable)};
            if (vtable->struct_size == REFRAIN_HOST_KERNEL_VTABLE_SIZE) {
                provider->kernel_vtable = vtable;
                provider->kernel_self = api->self;
            }
        }
    }
    const refrain::mod::provider_api api{
        .vtable = &sim_vtable(),
        .self = provider.get(),
        .abi_major = REFRAIN_SERVICES_SIM_ABI_MAJOR,
    };
    auto status{host.register_provider("refrain:sim", api, "1.0.0")};
    if (!status) {
        provider.reset();
        return status;
    }
    return refrain::status_ok();
}

} // namespace

REFR_MOD_STATIC("refrain.sim.vanilla", "1.0.0", 1, make_sim_vanilla);
