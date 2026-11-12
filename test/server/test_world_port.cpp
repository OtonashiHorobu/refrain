// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// World port routing.
//
// The tick loop reads and writes the world through `server::world_port`.
// These tests pin both sides of the seam:
//  * the direct port forwards every operation to the in-process `sim::world`
//    without changing its result;
//  * the selection rule keeps that path for the first-party provider set and
//    picks the contract port for anything else;
//  * the contract port turns each operation into the matching
//    `refrain:world` / `refrain:sim` / `refrain:storage` call, proven by an
//    instrumented provider set the test swaps in (the same ids the gamepack
//    pins, different code);
//  * the runtime refuses to boot on a contract composition with the
//    documented gap instead of ignoring the replacement.

#include <refrain/builtin/image.hpp>
#include <refrain/contracts/abi.hpp>
#include <refrain/contracts/services/sim_abi.h>
#include <refrain/contracts/services/storage_abi.h>
#include <refrain/contracts/services/world_abi.h>
#include <refrain/core/error.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/mod/discovery.hpp>
#include <refrain/mod/gamepack.hpp>
#include <refrain/mod/host.hpp>
#include <refrain/server/server_runtime.hpp>
#include <refrain/server/world_port.hpp>
#include <refrain/sim/world.hpp>
#include <refrain/world/model.hpp>
#include <refrain/world/store.hpp>

#include <boost/ut.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "support/scratch_directory.hpp"

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace abi = refrain::contracts;
namespace data = refrain::data;
namespace server = refrain::server;
namespace sim = refrain::sim;
namespace world = refrain::world;
namespace test_support = refrain::test_support;

/// Instrumented provider set. All three services share one `sim::world`, which
/// is what a correctly composed replacement would do at the process level.
struct recording_provider {
    sim::world *world{nullptr};
    std::uint64_t next_handle{1};
    std::string hash_hex{};
    /// The handle the runtime handed to each provider, so a test can prove
    /// that one store serves the whole composition.
    refrain_services_storage_storage_handle world_storage{};
    refrain_services_storage_storage_handle sim_storage{};
    refrain_core_handle_storage_world storage_world{};

    int world_opens{0};
    int world_closes{0};
    int world_block_reads{0};
    int world_block_writes{0};
    int world_chunks{0};
    int sim_opens{0};
    int sim_set_blocks{0};
    int sim_ticks{0};
    int sim_spawns{0};
    int storage_opens{0};
    int storage_writes{0};
    int storage_syncs{0};
};

[[nodiscard]] auto chunk_pos(refrain_core_chunk_pos pos) -> sim::chunk_pos {
    return sim::chunk_pos{pos.x, pos.z};
}

[[nodiscard]] auto block_pos(refrain_core_block_pos pos) -> sim::block_pos {
    return sim::block_pos{pos.x, static_cast<std::int16_t>(pos.y), pos.z};
}

refrain_status
record_world_open(void *self,
                  const refrain_services_world_world_config *config) {
    if (self == nullptr || config == nullptr ||
        config->struct_size != REFRAIN_SERVICES_WORLD_WORLD_CONFIG_SIZE) {
        return REFRAIN_SERVICES_WORLD_WORLD_ERROR_OPEN_FAILED;
    }
    auto *provider{static_cast<recording_provider *>(self)};
    ++provider->world_opens;
    provider->world_storage = config->storage;
    return REFRAIN_STATUS_OK;
}

refrain_status record_world_close(void *self) {
    ++static_cast<recording_provider *>(self)->world_closes;
    return REFRAIN_STATUS_OK;
}

refrain_status record_world_chunk_at(void *self,
                                     const refrain_core_chunk_pos *pos,
                                     refrain_core_chunk_view *out) {
    if (self == nullptr || pos == nullptr || out == nullptr) {
        return REFRAIN_SERVICES_WORLD_WORLD_ERROR_CHUNK_NOT_LOADED;
    }
    auto *provider{static_cast<recording_provider *>(self)};
    ++provider->world_chunks;
    auto *loaded{provider->world->find_chunk(chunk_pos(*pos))};
    if (loaded == nullptr) {
        return REFRAIN_SERVICES_WORLD_WORLD_ERROR_CHUNK_NOT_LOADED;
    }
    out->struct_size = REFRAIN_CORE_CHUNK_VIEW_SIZE;
    out->pos = *pos;
    out->status =
        static_cast<refrain_core_chunk_status>(loaded->storage.status);
    out->section_count = static_cast<std::uint32_t>(loaded->sections.size());
    out->sections = refrain_core_slice_chunk_section_view{};
    return REFRAIN_STATUS_OK;
}

std::uint32_t record_world_block_at(void *self,
                                    const refrain_core_block_pos *pos) {
    if (self == nullptr || pos == nullptr) {
        return sim::air_block;
    }
    auto *provider{static_cast<recording_provider *>(self)};
    ++provider->world_block_reads;
    return provider->world->block_at(block_pos(*pos));
}

std::int32_t record_world_height_at(void *, const refrain_core_block_pos *pos) {
    return pos == nullptr ? -1 : pos->y;
}

std::uint8_t record_world_is_chunk_loaded(void *self,
                                          const refrain_core_chunk_pos *pos) {
    auto *provider{static_cast<recording_provider *>(self)};
    return provider != nullptr && pos != nullptr &&
                   provider->world->find_chunk(chunk_pos(*pos)) != nullptr
               ? 1
               : 0;
}

refrain_status
record_world_apply_changes(void *self, refrain_core_slice_block_change changes,
                           std::uint32_t *out_value) {
    if (self == nullptr || out_value == nullptr) {
        return REFRAIN_SERVICES_WORLD_WORLD_ERROR_OUT_OF_BOUNDS;
    }
    auto *provider{static_cast<recording_provider *>(self)};
    ++provider->world_block_writes;
    std::vector<sim::block_change> batch{};
    for (std::uint64_t index{0}; index < changes.len; ++index) {
        const auto &change{changes.data[index]};
        batch.push_back(sim::block_change{.pos = block_pos(change.pos),
                                          .old_state = change.old_state,
                                          .new_state = change.new_state});
    }
    auto applied{provider->world->set_blocks(batch)};
    if (!applied) {
        return REFRAIN_SERVICES_WORLD_WORLD_ERROR_INTERNAL;
    }
    *out_value = static_cast<std::uint32_t>(*applied);
    return REFRAIN_STATUS_OK;
}

std::int64_t record_world_tick(void *self) {
    return static_cast<recording_provider *>(self)->world->ticks();
}

refrain_status record_sim_open(void *self,
                               const refrain_services_sim_sim_config *config,
                               refrain_services_sim_handle_sim_world *out) {
    if (self == nullptr || config == nullptr || out == nullptr ||
        config->struct_size != REFRAIN_SERVICES_SIM_SIM_CONFIG_SIZE) {
        return REFRAIN_SERVICES_SIM_SIM_ERROR_INVALID_HANDLE;
    }
    auto *provider{static_cast<recording_provider *>(self)};
    ++provider->sim_opens;
    provider->sim_storage = config->storage;
    *out = provider->next_handle++;
    return REFRAIN_STATUS_OK;
}

refrain_status record_sim_close(void *self,
                                refrain_services_sim_handle_sim_world) {
    if (self == nullptr) {
        return REFRAIN_SERVICES_SIM_SIM_ERROR_INVALID_HANDLE;
    }
    return REFRAIN_STATUS_OK;
}

refrain_status record_sim_tick(void *self,
                               refrain_services_sim_handle_sim_world world_id) {
    auto *provider{static_cast<recording_provider *>(self)};
    if (provider == nullptr || world_id == REFRAIN_HANDLE_INVALID) {
        return REFRAIN_SERVICES_SIM_SIM_ERROR_INVALID_HANDLE;
    }
    ++provider->sim_ticks;
    provider->world->tick();
    return REFRAIN_STATUS_OK;
}

std::int64_t record_sim_ticks(void *self,
                              refrain_services_sim_handle_sim_world) {
    auto *provider{static_cast<recording_provider *>(self)};
    return provider == nullptr ? -1 : provider->world->ticks();
}

refrain_str record_sim_state_hash(void *self,
                                  refrain_services_sim_handle_sim_world) {
    auto *provider{static_cast<recording_provider *>(self)};
    if (provider == nullptr) {
        return {};
    }
    provider->hash_hex = data::to_hex(provider->world->state_hash());
    return abi::to_borrowed(provider->hash_hex);
}

refrain_status record_sim_set_blocks(void *self,
                                     refrain_services_sim_handle_sim_world,
                                     refrain_core_slice_block_change changes,
                                     std::uint32_t *out) {
    if (self == nullptr || out == nullptr) {
        return REFRAIN_SERVICES_SIM_SIM_ERROR_INVALID_ARGUMENT;
    }
    auto *provider{static_cast<recording_provider *>(self)};
    ++provider->sim_set_blocks;
    std::vector<sim::block_change> batch{};
    batch.reserve(static_cast<std::size_t>(changes.len));
    for (std::uint64_t index{0}; index < changes.len; ++index) {
        const auto &change{changes.data[index]};
        batch.push_back(sim::block_change{.pos = block_pos(change.pos),
                                          .old_state = change.old_state,
                                          .new_state = change.new_state});
    }
    auto applied{provider->world->set_blocks(batch)};
    if (!applied) {
        return REFRAIN_SERVICES_SIM_SIM_ERROR_INTERNAL;
    }
    *out = static_cast<std::uint32_t>(*applied);
    return REFRAIN_STATUS_OK;
}

refrain_status record_sim_spawn_entity(void *self,
                                       refrain_services_sim_handle_sim_world,
                                       refrain_str type, double x, double y,
                                       double z, std::uint64_t *out) {
    if (self == nullptr || out == nullptr) {
        return REFRAIN_SERVICES_SIM_SIM_ERROR_INVALID_ARGUMENT;
    }
    auto *provider{static_cast<recording_provider *>(self)};
    ++provider->sim_spawns;
    auto spawned{
        provider->world->spawn_entity(abi::as_string_view(type), x, y, z)};
    if (!spawned) {
        return REFRAIN_SERVICES_SIM_SIM_ERROR_INTERNAL;
    }
    *out = *spawned;
    return REFRAIN_STATUS_OK;
}

std::uint8_t record_sim_remove_entity(void *self,
                                      refrain_services_sim_handle_sim_world,
                                      std::uint64_t id) {
    auto *provider{static_cast<recording_provider *>(self)};
    return provider != nullptr && provider->world->remove_entity(id) ? 1 : 0;
}

extern const refrain_services_storage_vtable recording_storage_vtable;

refrain_status
record_storage_open(void *self,
                    const refrain_services_storage_world_open_config *config,
                    refrain_services_storage_storage_handle *out) {
    if (self == nullptr || config == nullptr || out == nullptr ||
        config->struct_size !=
            REFRAIN_SERVICES_STORAGE_WORLD_OPEN_CONFIG_SIZE) {
        return REFRAIN_SERVICES_STORAGE_STORAGE_ERROR_OPEN_FAILED;
    }
    auto *provider{static_cast<recording_provider *>(self)};
    ++provider->storage_opens;
    provider->storage_world = provider->next_handle++;
    refrain_services_storage_storage_handle value{};
    abi::init_struct(value);
    value.world = provider->storage_world;
    value.owner.struct_size = REFRAIN_HOST_CORE_PROVIDER_REF_SIZE;
    value.owner.vtable = const_cast<void *>(
        static_cast<const void *>(&recording_storage_vtable));
    value.owner.self = provider;
    value.owner.abi_major = REFRAIN_SERVICES_STORAGE_ABI_MAJOR;
    // The recorded provider set keeps no `world_store` of its own, so the
    // in-process reference stays null; the world and sim providers record the
    // handle they were given.
    value.store = nullptr;
    *out = value;
    return REFRAIN_STATUS_OK;
}

refrain_status record_storage_close(void *self,
                                    refrain_core_handle_storage_world) {
    return self == nullptr ? REFRAIN_SERVICES_STORAGE_STORAGE_ERROR_OPEN_FAILED
                           : REFRAIN_STATUS_OK;
}

refrain_status
record_storage_read(void *, refrain_core_handle_storage_world,
                    const refrain_services_storage_chunk_key *,
                    refrain_core_chunk_buffer *,
                    refrain_services_storage_chunk_read_result *) {
    return REFRAIN_SERVICES_STORAGE_STORAGE_ERROR_NOT_FOUND;
}

refrain_status record_storage_write(void *self,
                                    refrain_core_handle_storage_world,
                                    const refrain_services_storage_chunk_key *,
                                    refrain_bytes) {
    auto *provider{static_cast<recording_provider *>(self)};
    if (provider == nullptr) {
        return REFRAIN_SERVICES_STORAGE_STORAGE_ERROR_IO_ERROR;
    }
    ++provider->storage_writes;
    return REFRAIN_STATUS_OK;
}

refrain_status record_storage_sync(void *self,
                                   refrain_core_handle_storage_world) {
    auto *provider{static_cast<recording_provider *>(self)};
    if (provider == nullptr) {
        return REFRAIN_SERVICES_STORAGE_STORAGE_ERROR_IO_ERROR;
    }
    ++provider->storage_syncs;
    return REFRAIN_STATUS_OK;
}

std::uint8_t record_storage_is_open(void *self,
                                    refrain_core_handle_storage_world) {
    return self == nullptr ? 0 : 1;
}

const refrain_services_world_vtable recording_world_vtable{
    .struct_size = REFRAIN_SERVICES_WORLD_VTABLE_SIZE,
    .abi_major = REFRAIN_SERVICES_WORLD_ABI_MAJOR,
    .abi_minor = REFRAIN_SERVICES_WORLD_ABI_MINOR,
    .abi_patch = REFRAIN_SERVICES_WORLD_ABI_PATCH,
    .open_world = &record_world_open,
    .close_world = &record_world_close,
    .chunk_at = &record_world_chunk_at,
    .block_at = &record_world_block_at,
    .height_at = &record_world_height_at,
    .is_chunk_loaded = &record_world_is_chunk_loaded,
    .apply_block_changes = &record_world_apply_changes,
    .world_tick = &record_world_tick,
};

const refrain_services_sim_vtable recording_sim_vtable{
    .struct_size = REFRAIN_SERVICES_SIM_VTABLE_SIZE,
    .abi_major = REFRAIN_SERVICES_SIM_ABI_MAJOR,
    .abi_minor = REFRAIN_SERVICES_SIM_ABI_MINOR,
    .abi_patch = REFRAIN_SERVICES_SIM_ABI_PATCH,
    .open_world = &record_sim_open,
    .close_world = &record_sim_close,
    .tick = &record_sim_tick,
    .ticks = &record_sim_ticks,
    .state_hash = &record_sim_state_hash,
    .set_blocks = &record_sim_set_blocks,
    .spawn_entity = &record_sim_spawn_entity,
    .remove_entity = &record_sim_remove_entity,
};

const refrain_services_storage_vtable recording_storage_vtable{
    .struct_size = REFRAIN_SERVICES_STORAGE_VTABLE_SIZE,
    .abi_major = REFRAIN_SERVICES_STORAGE_ABI_MAJOR,
    .abi_minor = REFRAIN_SERVICES_STORAGE_ABI_MINOR,
    .abi_patch = REFRAIN_SERVICES_STORAGE_ABI_PATCH,
    .open_world = &record_storage_open,
    .close_world = &record_storage_close,
    .read_chunk = &record_storage_read,
    .write_chunk = &record_storage_write,
    .sync = &record_storage_sync,
    .is_open = &record_storage_is_open,
};

/// One full stone section, enough for block reads and writes to be visible.
[[nodiscard]] auto flat_chunk() -> world::chunk {
    world::chunk chunk{};
    chunk.format_version = world::chunk_format_version;
    chunk.status = refrain::storage::chunk_status::full;
    chunk.y_pos = 0;
    refrain::storage::section_record section{};
    section.y = 0;
    section.kind = refrain::storage::section_kind::single_value;
    section.bits_per_entry = 0;
    section.block_count = static_cast<std::uint16_t>(world::section_volume);
    section.biome_kind = refrain::storage::section_kind::single_value;
    section.biome_bits_per_entry = 0;
    section.biome_palette.push_back("minecraft:plains");
    section.palette.push_back(refrain::storage::palette_entry{
        .key = "minecraft:stone",
        .count = static_cast<std::uint16_t>(world::section_volume),
        .state = 0,
    });
    chunk.sections.push_back(std::move(section));
    return chunk;
}

[[nodiscard]] auto open_flat_world(const std::filesystem::path &directory)
    -> std::optional<sim::world> {
    auto store{world::world_store::create(directory.string())};
    if (!store) {
        return std::nullopt;
    }
    auto chunk{flat_chunk()};
    if (!store->write_chunk({0, 0}, chunk) || !store->commit()) {
        return std::nullopt;
    }
    auto opened{sim::world::open(directory.string())};
    if (!opened) {
        return std::nullopt;
    }
    // The runtime loads columns on demand; a test that reads blocks first has
    // to load the column, otherwise `block_at` correctly reports air.
    if (!opened->ensure_chunk({0, 0})) {
        return std::nullopt;
    }
    return std::optional<sim::world>{std::move(*opened)};
}

[[nodiscard]] auto recording_handles(recording_provider &provider)
    -> server::server_service_handles {
    return server::server_service_handles{
        .world = &recording_world_vtable,
        .world_self = &provider,
        .sim = &recording_sim_vtable,
        .sim_self = &provider,
        .storage = &recording_storage_vtable,
        .storage_self = &provider,
        .world_provider = "refrain.world.vanilla",
        .sim_provider = "refrain.sim.vanilla",
        .storage_provider = "refrain.storage.rwf",
    };
}

/// The same instrumented tables under the ids the gamepack pins when a test
/// host swaps the static factory; `is_first_party_world` distinguishes the
/// implementation by id, so the contract port is selected.
[[nodiscard]] auto recording_contract_handles(recording_provider &provider)
    -> server::server_service_handles {
    auto handles{recording_handles(provider)};
    handles.world_provider = "test.recording.world";
    handles.sim_provider = "test.recording.sim";
    handles.storage_provider = "test.recording.storage";
    return handles;
}

/// The active first-party providers with ids that do not name the first-party
/// implementation, so the selection rule picks the contract port while the
/// real providers serve it.
[[nodiscard]] auto real_contract_handles(const refrain::mod::host &host)
    -> std::optional<server::server_service_handles> {
    const auto world{host.services().active_provider("refrain:world")};
    const auto sim{host.services().active_provider("refrain:sim")};
    const auto storage{host.services().active_provider("refrain:storage")};
    if (!world || !sim || !storage || world->instance == nullptr ||
        sim->instance == nullptr || storage->instance == nullptr) {
        return std::nullopt;
    }
    const auto *world_api{
        static_cast<const refrain::mod::provider_api *>(world->instance)};
    const auto *sim_api{
        static_cast<const refrain::mod::provider_api *>(sim->instance)};
    const auto *storage_api{
        static_cast<const refrain::mod::provider_api *>(storage->instance)};
    if (world_api == nullptr || sim_api == nullptr || storage_api == nullptr) {
        return std::nullopt;
    }
    return server::server_service_handles{
        .world = static_cast<const refrain_services_world_vtable *>(
            world_api->vtable),
        .world_self = world_api->self,
        .sim =
            static_cast<const refrain_services_sim_vtable *>(sim_api->vtable),
        .sim_self = sim_api->self,
        .storage = static_cast<const refrain_services_storage_vtable *>(
            storage_api->vtable),
        .storage_self = storage_api->self,
        .world_provider = "test.contract.world",
        .sim_provider = "test.contract.sim",
        .storage_provider = "test.contract.storage",
    };
}

[[maybe_unused]] const suite world_port_tests = [] {
    "world port/direct forwards block, chunk, hash and snapshot operations"_test =
        [] {
            test_support::scratch_directory directory{};
            const auto world_path{directory.path() / "world"};
            auto opened{open_flat_world(world_path)};
            expect(opened.has_value());
            if (!opened) {
                return;
            }
            auto port{server::make_borrowed_world_port(*opened)};
            expect(port != nullptr);
            if (!port) {
                return;
            }

            const sim::block_pos stone_pos{0, 2, 0};
            expect(port->block_at(stone_pos) == opened->block_at(stone_pos));
            expect(port->block_at(stone_pos) != sim::air_block);
            expect(port->block_key(port->block_at(stone_pos)) ==
                   "minecraft:stone");
            expect(port->players() != nullptr);
            expect(port->registry() != nullptr);
            expect(port->events() != nullptr);
            expect(port->items() != nullptr);
            expect(port->entities() != nullptr);
            expect(port->ticks() == 0);

            const auto loaded{port->chunk_load({0, 0})};
            expect(loaded.has_value());
            if (loaded) {
                expect(*loaded != nullptr);
                expect((*loaded)->sections.size() == 1U);
            }

            const auto interned{port->intern_block("minecraft:dirt")};
            expect(interned != sim::air_block);
            expect(port->block_key(interned) == "minecraft:dirt");

            const sim::block_change change{.pos = stone_pos,
                                           .old_state =
                                               port->block_at(stone_pos),
                                           .new_state = interned};
            auto applied{port->set_blocks(
                std::span<const sim::block_change>{&change, 1})};
            expect(applied.has_value());
            if (applied) {
                expect(*applied == 1U);
            }
            expect(port->block_at(stone_pos) == interned);
            expect(port->state_hash() == opened->state_hash());

            auto pending{port->snapshot_pending()};
            expect(!pending.empty());
            auto written{port->write_pending(pending)};
            expect(written.has_value());
            if (written) {
                port->mark_clean(*written);
            }
            expect(port->snapshot_pending().empty());
            expect(port->flush().has_value());
            expect(port->store_valid());
            expect(!port->open(world_path.string()).has_value());
        };

    "world port/selection keeps the direct port for the first-party set"_test =
        [] {
            recording_provider provider{};
            const auto direct_handles{recording_handles(provider)};
            expect(server::is_first_party_world(direct_handles));
            expect(server::select_world_port(direct_handles) ==
                   server::world_port_choice::direct);

            const auto contract_handles{recording_contract_handles(provider)};
            expect(!server::is_first_party_world(contract_handles));
            expect(server::select_world_port(contract_handles) ==
                   server::world_port_choice::contract);

            // A mixed set is a contract composition: the direct port must not
            // borrow the first-party world when another provider replaces it.
            auto mixed{contract_handles};
            mixed.sim_provider = "refrain.sim.vanilla";
            expect(server::select_world_port(mixed) ==
                   server::world_port_choice::contract);
        };

    "world port/contract routes every operation through the provider ABI"_test =
        [] {
            test_support::scratch_directory directory{};
            const auto world_path{directory.path() / "world"};
            auto opened{open_flat_world(world_path)};
            expect(opened.has_value());
            if (!opened) {
                return;
            }
            recording_provider provider{.world = &*opened};
            auto handles{recording_contract_handles(provider)};
            auto port{server::make_world_port(handles)};
            expect(port != nullptr);
            if (!port) {
                return;
            }
            expect(port->open(world_path.string()).has_value());
            expect(provider.storage_opens == 1);
            expect(provider.world_opens == 1);
            expect(provider.sim_opens == 1);
            // One store, handed down: both providers received the handle the
            // runtime opened from the level directory.
            expect(provider.world_storage.world == provider.storage_world);
            expect(provider.sim_storage.world == provider.storage_world);
            expect(provider.world_storage.owner.vtable != nullptr);
            expect(provider.sim_storage.owner.vtable != nullptr);

            const sim::block_pos stone_pos{0, 2, 0};
            expect(port->block_at(stone_pos) != sim::air_block);
            expect(provider.world_block_reads > 0);

            auto applied{port->set_block(stone_pos, sim::air_block)};
            expect(applied.has_value());
            expect(provider.sim_set_blocks == 1);
            expect(port->block_at(stone_pos) == sim::air_block);

            const auto interned{port->intern_block("minecraft:stone")};
            expect(interned == sim::air_block);
            expect(port->block_key(1) == std::string_view{});

            const auto hash{port->state_hash()};
            expect(data::to_hex(hash) == data::to_hex(opened->state_hash()));
            expect(port->ticks() == opened->ticks());
            port->tick();
            expect(provider.sim_ticks == 1);

            expect(port->flush().has_value());
            expect(provider.storage_syncs == 1);
            expect(port->store_valid());

            auto chunk{flat_chunk()};
            expect(port->write_chunk({0, 0}, chunk).has_value());
            expect(provider.storage_writes == 1);
            expect(port->commit().has_value());
            expect(provider.storage_syncs == 2);

            // The v1 contracts cannot serve the boot projection or the packet
            // encoder; the port names the gap instead of faking a value.
            const auto level{port->level()};
            expect(!level.has_value());
            if (!level) {
                expect(level.error().code == refrain::error_code::unsupported);
            }
            expect(!port->chunk_load({0, 0}).has_value());
            expect(
                !port->write_pending(std::span<const sim::pending_snapshot>{})
                     .has_value());

            port->close();
            expect(provider.world_closes == 1);
        };

    "world port/a mixed composition opens through the contract port"_test = [] {
        test_support::scratch_directory directory{};
        const auto world_path{directory.path() / "world"};
        auto opened{open_flat_world(world_path)};
        expect(opened.has_value());
        if (!opened) {
            return;
        }
        recording_provider provider{.world = &*opened};
        auto handles{recording_contract_handles(provider)};
        // One first-party id used to make the port refuse the composition,
        // because that provider would have opened its own store.
        handles.world_provider = "refrain.world.vanilla";
        auto port{server::make_world_port(handles)};
        expect(port != nullptr);
        if (!port) {
            return;
        }
        expect(port->open(world_path.string()).has_value());
        expect(provider.storage_opens == 1);
        expect(provider.world_opens == 1);
        expect(provider.sim_opens == 1);
        expect(provider.world_storage.world == provider.storage_world);
        port->close();
    };

    "world port/a second runtime over one level directory is refused"_test =
        [] {
            auto spec{refrain::mod::load_gamepack_spec(
                std::string{REFRAIN_SOURCE_DIR} + "/gamepack/gamepack.toml",
                "server")};
            expect(static_cast<bool>(spec));
            if (!spec) {
                return;
            }
            auto plan{refrain::mod::preview_composition(*spec)};
            expect(static_cast<bool>(plan));
            if (!plan) {
                return;
            }
            refrain::mod::host host{};
            const std::span<const std::byte> image{
                refrain::builtin::image_data(), refrain::builtin::image_size()};
            expect(static_cast<bool>(host.load(*plan, image)));
            const auto handles{real_contract_handles(host)};
            expect(handles.has_value());
            if (!handles) {
                return;
            }

            test_support::scratch_directory directory{};
            const auto world_path{directory.path() / "world"};
            {
                auto store{world::world_store::create(world_path.string())};
                expect(store.has_value());
                if (!store) {
                    return;
                }
                auto chunk{flat_chunk()};
                expect(static_cast<bool>(store->write_chunk({0, 0}, chunk)));
                expect(static_cast<bool>(store->commit()));
            }

            auto first{server::make_world_port(*handles)};
            expect(first != nullptr);
            if (!first) {
                return;
            }
            expect(first->open(world_path.string()).has_value());
            expect(first->store_valid());

            // A second runtime over the same directory gets no second store:
            // the storage provider already owns it.
            auto second{server::make_world_port(*handles)};
            expect(second != nullptr);
            if (second) {
                const auto reopened{second->open(world_path.string())};
                expect(!reopened.has_value());
                if (!reopened) {
                    expect(reopened.error().message.find("refrain:storage") !=
                           std::string::npos);
                }
                second->close();
            }
            // The refused open left the running runtime untouched.
            expect(first->store_valid());
            expect(first->ticks() == 0);

            first->close();
            expect(!first->store_valid());

            // The first runtime released the directory, so the next one opens
            // it.
            auto third{server::make_world_port(*handles)};
            expect(third != nullptr);
            if (third) {
                expect(third->open(world_path.string()).has_value());
                third->close();
            }
        };

    "world port/run_server refuses a contract composition without a level "
    "projection"_test = [] {
        test_support::scratch_directory directory{};
        const auto world_path{directory.path() / "world"};
        auto opened{open_flat_world(world_path)};
        expect(opened.has_value());
        if (!opened) {
            return;
        }
        recording_provider provider{.world = &*opened};
        auto handles{recording_contract_handles(provider)};
        auto port{server::make_world_port(handles)};
        expect(port != nullptr);
        if (!port) {
            return;
        }
        server::server_run_options options{};
        options.world_path = world_path.string();
        options.ticks = 1;
        // The contract port opens the provider set, then the runtime asks for
        // the level projection and stops; no listener, no tick loop.
        expect(server::run_server(options, handles, *port) == 1);
        expect(provider.world_opens == 1);
        expect(provider.sim_opens == 1);
        expect(provider.storage_opens == 1);
        expect(provider.sim_ticks == 0);
    };
};

} // namespace
