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
#include <refrain/world/store.hpp>

#include <boost/ut.hpp>

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
