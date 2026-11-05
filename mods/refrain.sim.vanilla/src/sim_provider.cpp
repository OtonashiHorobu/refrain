// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// `refrain:sim` provider for the internal `refrain.sim.vanilla` mod.
//
// The simulation runs on a single sim thread, so this provider owns a table of
// open
// `sim::world` instances addressed by opaque handles. It deliberately exposes
// only lifecycle/tick/hash; game logic stays inside the provider and is not
// reachable through the C ABI.

#include <refrain/contracts/services/sim_abi.h>

#include <refrain/core/error.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/mod/host.hpp>
#include <refrain/mod/static_mod.hpp>
#include <refrain/sim/world.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace sim = refrain::sim;

[[nodiscard]] auto to_str(std::string_view value) noexcept -> refrain_str {
    return refrain_str{
        .data = reinterpret_cast<const std::uint8_t *>(value.data()),
        .len = value.size(),
    };
}

[[nodiscard]] auto to_view(refrain_str value) noexcept -> std::string_view {
    if (value.data == nullptr || value.len == 0) {
        return {};
    }
    return std::string_view{reinterpret_cast<const char *>(value.data),
                            static_cast<std::size_t>(value.len)};
}

[[nodiscard]] auto to_status(const refrain::error &error) noexcept
    -> refrain_status {
    switch (error.code) {
    case refrain::error_code::not_found:
        return REFRAIN_SERVICES_SIM_SIM_ERROR_NOT_FOUND;
    case refrain::error_code::already_exists:
        return REFRAIN_SERVICES_SIM_SIM_ERROR_ALREADY_OPEN;
    case refrain::error_code::io_error:
        return REFRAIN_SERVICES_SIM_SIM_ERROR_IO_ERROR;
    case refrain::error_code::invalid_argument:
        return REFRAIN_SERVICES_SIM_SIM_ERROR_INVALID_HANDLE;
    default:
        return REFRAIN_SERVICES_SIM_SIM_ERROR_INTERNAL;
    }
}

struct sim_provider {
    std::map<std::uint64_t, sim::world> worlds{};
    std::uint64_t next_handle{1};
    std::string hash_hex{};

    [[nodiscard]] auto find(std::uint64_t handle) noexcept -> sim::world * {
        const auto found{worlds.find(handle)};
        return found == worlds.end() ? nullptr : &found->second;
    }

    static auto open_world(void *self,
                           const refrain_services_sim_sim_config *config,
                           refrain_services_sim_handle_sim_world *out_value)
        -> refrain_status {
        if (self == nullptr || config == nullptr || out_value == nullptr ||
            config->struct_size != REFRAIN_SERVICES_SIM_SIM_CONFIG_SIZE) {
            return REFRAIN_SERVICES_SIM_SIM_ERROR_INVALID_HANDLE;
        }
        const auto path{to_view(config->world_path)};
        if (path.empty()) {
            return REFRAIN_SERVICES_SIM_SIM_ERROR_INVALID_HANDLE;
        }
        auto &provider{*static_cast<sim_provider *>(self)};
        auto world{sim::world::open(path)};
        if (!world) {
            return to_status(world.error());
        }
        world->seed(static_cast<std::uint64_t>(config->seed));
        const auto handle{provider.next_handle++};
        provider.worlds.emplace(handle, std::move(*world));
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
            return to_status(flushed.error());
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
        return to_str(provider->hash_hex);
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
    };
    return table;
}

auto make_sim_vanilla(refrain::mod::host &host) -> refrain::status {
    static std::unique_ptr<sim_provider> provider{};
    provider = std::make_unique<sim_provider>();
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
