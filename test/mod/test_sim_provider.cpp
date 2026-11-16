// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/builtin/image.hpp>
#include <refrain/contracts/abi.h>
#include <refrain/contracts/services/sim.hpp>
#include <refrain/contracts/services/storage.hpp>
#include <refrain/mod/discovery.hpp>
#include <refrain/mod/gamepack.hpp>
#include <refrain/mod/host.hpp>
#include <refrain/sim/player.hpp>
#include <refrain/world/store.hpp>

#include <boost/ut.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>

namespace {
using namespace boost::ut;

namespace mod = refrain::mod;

namespace sim = refrain::services::sim;
namespace storage_service = refrain::services::storage;
namespace world = refrain::world;

[[nodiscard]] auto builtin_bytes() -> std::span<const std::byte> {
    return std::span<const std::byte>{refrain::builtin::image_data(),
                                      refrain::builtin::image_size()};
}

[[nodiscard]] auto load_plan() -> refrain::result<refrain::mod::composition> {
    auto spec{refrain::mod::load_gamepack_spec(
        std::string{REFRAIN_SOURCE_DIR} + "/gamepack/gamepack.toml", "server")};
    if (!spec) {
        return std::unexpected{spec.error()};
    }
    return refrain::mod::preview_composition(*spec);
}

[[maybe_unused]] const suite tests = [] {
    "sim/refrain.sim.vanilla registers and serves tick/hash"_test = [] {
        auto plan{load_plan()};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        refrain::mod::host host{};
        expect(static_cast<bool>(host.load(*plan, builtin_bytes())));
        const auto info{host.services().active_provider("refrain:sim")};
        expect(info.has_value());
        if (!info) {
            return;
        }
        expect(info->id == "refrain.sim.vanilla");
        const auto *api{
            static_cast<const refrain::mod::provider_api *>(info->instance)};
        expect(api != nullptr);
        if (api == nullptr) {
            return;
        }
        expect(api->abi_major == REFRAIN_SERVICES_SIM_ABI_MAJOR);
        const sim::provider provider{
            *static_cast<const refrain_services_sim_vtable *>(api->vtable),
            api->self};

        const auto stamp{
            std::chrono::steady_clock::now().time_since_epoch().count()};
        const auto directory{std::filesystem::temp_directory_path() /
                             ("refrain-sim-provider-" + std::to_string(stamp))};
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        {
            auto created{world::world_store::create(directory.string())};
            expect(created.has_value());
            if (!created) {
                std::filesystem::remove_all(directory, error);
                return;
            }
        }
        // The runtime opens the level directory through `refrain:storage` and
        // hands the handle to the sim; the sim never opens it itself.
        const auto storage_info{
            host.services().active_provider("refrain:storage")};
        expect(storage_info.has_value());
        if (!storage_info) {
            std::filesystem::remove_all(directory, error);
            return;
        }
        const auto *storage_api{static_cast<const refrain::mod::provider_api *>(
            storage_info->instance)};
        expect(storage_api != nullptr);
        if (storage_api == nullptr) {
            std::filesystem::remove_all(directory, error);
            return;
        }
        const storage_service::provider storage{
            *static_cast<const refrain_services_storage_vtable *>(
                storage_api->vtable),
            storage_api->self};
        auto storage_config{storage_service::make_world_open_config()};
        const std::string path{directory.string()};
        storage_config.world_path = refrain_str{
            .data = reinterpret_cast<const std::uint8_t *>(path.data()),
            .len = path.size(),
        };
        storage_config.gamepack_id = refrain_str{};
        storage_config.flags = 0;
        auto store{storage.open_world(storage_config)};
        expect(store.has_value());
        if (!store) {
            std::filesystem::remove_all(directory, error);
            return;
        }

        auto config{sim::make_sim_config()};
        config.storage = *store;
        config.seed = 42;
        auto handle{provider.open_world(config)};
        expect(handle.has_value());
        if (!handle) {
            std::filesystem::remove_all(directory, error);
            return;
        }
        expect(provider.ticks(*handle) == 0);
        expect(provider.tick(*handle).has_value());
        expect(provider.tick(*handle).has_value());
        expect(provider.ticks(*handle) == 2);
        const auto hash{provider.state_hash(*handle)};
        expect(!hash.empty());
        expect(provider.close_world(*handle).has_value());
        expect(!provider.tick(*handle).has_value());
        expect(storage.close_world(refrain::core::storage_world{store->world})
                   .has_value());
        std::filesystem::remove_all(directory, error);
    };

    "sim/refrain.sim.vanilla serves the player table over the ABI"_test = [] {
        auto plan{load_plan()};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        refrain::mod::host host{};
        expect(static_cast<bool>(host.load(*plan, builtin_bytes())));
        const auto sim_info{host.services().active_provider("refrain:sim")};
        const auto storage_info{
            host.services().active_provider("refrain:storage")};
        expect(sim_info.has_value());
        expect(storage_info.has_value());
        if (!sim_info || !storage_info || sim_info->instance == nullptr ||
            storage_info->instance == nullptr) {
            return;
        }
        const auto *sim_api{static_cast<const refrain::mod::provider_api *>(
            sim_info->instance)};
        const auto *storage_api{static_cast<const refrain::mod::provider_api *>(
            storage_info->instance)};
        expect(sim_api != nullptr);
        expect(storage_api != nullptr);
        if (sim_api == nullptr || storage_api == nullptr) {
            return;
        }
        const sim::provider provider{
            *static_cast<const refrain_services_sim_vtable *>(sim_api->vtable),
            sim_api->self};
        const storage_service::provider storage{
            *static_cast<const refrain_services_storage_vtable *>(
                storage_api->vtable),
            storage_api->self};

        const auto stamp{
            std::chrono::steady_clock::now().time_since_epoch().count()};
        const auto directory{std::filesystem::temp_directory_path() /
                             ("refrain-sim-players-" + std::to_string(stamp))};
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        {
            auto created{world::world_store::create(directory.string())};
            expect(created.has_value());
            if (!created) {
                std::filesystem::remove_all(directory, error);
                return;
            }
        }
        auto storage_config{storage_service::make_world_open_config()};
        const std::string path{directory.string()};
        storage_config.world_path = refrain_str{
            .data = reinterpret_cast<const std::uint8_t *>(path.data()),
            .len = path.size(),
        };
        auto store{storage.open_world(storage_config)};
        expect(store.has_value());
        if (!store) {
            std::filesystem::remove_all(directory, error);
            return;
        }
        auto config{sim::make_sim_config()};
        config.storage = *store;
        config.seed = 42;
        auto handle{provider.open_world(config)};
        expect(handle.has_value());
        if (!handle) {
            std::filesystem::remove_all(directory, error);
            return;
        }

        expect(provider.player_count(*handle) == 0U);

        const auto register_player = [&provider,
                                      &handle](std::uint64_t connection,
                                               std::string_view name,
                                               std::uint32_t slots) {
            const std::array<std::uint8_t, 16> profile{
                static_cast<std::uint8_t>(connection),
                1,
                2,
                3,
                4,
                5,
                6,
                7,
                8,
                9,
                10,
                11,
                12,
                13,
                14,
                15};
            auto registration{sim::make_player_registration()};
            registration.connection = connection;
            registration.profile_id = refrain::contracts::to_borrowed(
                std::span<const std::uint8_t>{profile.data(), profile.size()});
            registration.name = refrain::contracts::to_borrowed(name);
            registration.world = refrain::contracts::to_borrowed("overworld");
            registration.x = 1.5;
            registration.y = 64.0;
            registration.z = -2.5;
            registration.game_mode = 1;
            registration.inventory_slots = slots;
            return provider.add_player(*handle, registration);
        };
        const auto console{register_player(::refrain::sim::synthetic_connection,
                                           "@console", 0)};
        const auto alice{register_player(7, "Alice", 36)};
        expect(console.has_value());
        expect(alice.has_value());
        if (!console || !alice) {
            static_cast<void>(provider.close_world(*handle));
            std::filesystem::remove_all(directory, error);
            return;
        }
        expect(*console == 1U);
        expect(*alice == 2U);
        expect(provider.player_count(*handle) == 2U);

        // The indexed accessor follows the state hash's ascending entity id
        // order.
        const auto first{provider.player_at(*handle, 0U)};
        expect(first.has_value());
        if (first) {
            expect(first->connection == ::refrain::sim::synthetic_connection);
            expect(refrain::contracts::as_string_view(first->name) ==
                   "@console");
            expect(refrain::contracts::as_string_view(first->world) ==
                   "overworld");
            expect(first->entity_id == *console);
            expect(first->x == 1.5);
            expect(first->y == 64.0);
            expect(first->z == -2.5);
            expect(first->game_mode == 1U);
            expect(first->health == 20.0F);
            expect(first->view_distance == 10);
            expect(refrain::contracts::as_bytes(first->profile_id).size() ==
                   16U);
        }
        const auto second{provider.player_at(*handle, 1U)};
        expect(second.has_value());
        if (second) {
            expect(second->connection == 7U);
            expect(refrain::contracts::as_string_view(second->name) == "Alice");
        }
        expect(!provider.player_at(*handle, 2U).has_value());

        const auto by_connection{provider.player_by_connection(*handle, 7U)};
        expect(by_connection.has_value());
        if (by_connection) {
            expect(by_connection->entity_id == *alice);
        }
        expect(!provider.player_by_connection(*handle, 8U).has_value());

        // Two connections cannot share a player, and a player needs a name.
        const auto duplicate{register_player(7, "Dup", 0)};
        expect(!duplicate.has_value());
        if (!duplicate) {
            expect(duplicate.error() == sim::sim_error::invalid_argument);
        }
        expect(provider.add_player(*handle, sim::make_player_registration())
                   .has_value() == false);

        // The connection-addressed writes reach the record the projection
        // reads back.
        expect(
            provider
                .set_player_transform(*handle, 7U, 4.0, 65.0, 6.0, 90.0F, -5.0F)
                .has_value());
        const auto turned{provider.player_by_connection(*handle, 7U)};
        expect(turned.has_value());
        if (turned) {
            expect(turned->x == 4.0);
            expect(turned->yaw == 90.0F);
        }
        expect(provider.set_player_position(*handle, 7U, 0.5, 70.0, 0.5)
                   .has_value());
        expect(provider.set_player_view_distance(*handle, 7U, 6).has_value());
        const auto moved{provider.move_player(*handle, 7U, 1.0, 0.0, 0.0)};
        expect(moved.has_value());
        if (moved) {
            expect(*moved);
        }
        const auto after_move{provider.player_by_connection(*handle, 7U)};
        expect(after_move.has_value());
        if (after_move) {
            expect(after_move->x == 1.5);
            expect(after_move->view_distance == 6);
        }
        expect(provider.jump(*handle, 7U).has_value());
        expect(
            !provider
                 .set_player_transform(*handle, 8U, 0.0, 0.0, 0.0, 0.0F, 0.0F)
                 .has_value());

        // The state hash is a view onto the provider's buffer, and the next
        // call may rebuild it; copy before mutating the player table.
        const auto before_leave{std::string{provider.state_hash(*handle)}};
        expect(provider.remove_player(*handle, 7U) != 0);
        expect(provider.remove_player(*handle, 7U) == 0);
        expect(provider.player_count(*handle) == 1U);
        expect(provider.player_by_connection(*handle, 7U).has_value() == false);
        const auto after_leave{std::string{provider.state_hash(*handle)}};
        expect(before_leave != after_leave);

        static_cast<void>(provider.close_world(*handle));
        std::filesystem::remove_all(directory, error);
    };

    "sim/refrain.sim.vanilla refuses a store the caller did not open"_test =
        [] {
            auto plan{load_plan()};
            expect(static_cast<bool>(plan));
            if (!plan) {
                return;
            }
            refrain::mod::host host{};
            expect(static_cast<bool>(host.load(*plan, builtin_bytes())));
            const auto info{host.services().active_provider("refrain:sim")};
            expect(info.has_value());
            if (!info || info->instance == nullptr) {
                return;
            }
            const auto *api{static_cast<const refrain::mod::provider_api *>(
                info->instance)};
            expect(api != nullptr);
            if (api == nullptr) {
                return;
            }
            const sim::provider provider{
                *static_cast<const refrain_services_sim_vtable *>(api->vtable),
                api->self};
            auto config{sim::make_sim_config()};
            config.storage = storage_service::make_storage_handle();
            config.seed = 42;
            const auto handle{provider.open_world(config)};
            expect(!handle.has_value());
            if (!handle) {
                expect(handle.error() == sim::sim_error::storage_unavailable);
            }
        };
};

} // namespace
