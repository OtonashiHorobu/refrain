// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/builtin.hpp>
#include <refrain/contracts/abi.h>
#include <refrain/contracts/services/sim.hpp>
#include <refrain/kernel/gamepack.hpp>
#include <refrain/mod/discovery.hpp>
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

namespace sim = refrain::services::sim;
namespace world = refrain::world;

[[nodiscard]] auto builtin_bytes() -> std::span<const std::byte> {
    return std::span<const std::byte>{refrain::builtin::image_data(),
                                      refrain::builtin::image_size()};
}

[[nodiscard]] auto load_plan() -> refrain::result<refrain::mod::composition> {
    auto spec{refrain::kernel::load_gamepack_spec(
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
            auto store{world::world_store::create(directory.string())};
            expect(store.has_value());
            if (!store) {
                std::filesystem::remove_all(directory, error);
                return;
            }
            auto config{sim::make_sim_config()};
            const std::string path{directory.string()};
            config.world_path = refrain_str{
                .data = reinterpret_cast<const std::uint8_t *>(path.data()),
                .len = path.size(),
            };
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
        }
        std::filesystem::remove_all(directory, error);
    };
};

} // namespace
