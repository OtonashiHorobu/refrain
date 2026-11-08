// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// `refrain:runtime` provider for the internal `refrain.server.vanilla` mod.
// This adapter converts the generated `runtime_config` ABI struct into
// `server::server_run_options` and calls the in-process `server::run_server`.
// The tick loop still uses the C++ app API directly; routing its internals
// through `refrain:world`/`refrain:sim`/`refrain:storage` is not implemented
// here.

#include <refrain/contracts/abi.hpp>
#include <refrain/contracts/services/net_protocol.hpp>
#include <refrain/contracts/services/runtime_abi.h>
#include <refrain/contracts/services/transport.hpp>
#include <refrain/core/config.hpp>
#include <refrain/core/log.hpp>
#include <refrain/mod/host.hpp>
#include <refrain/mod/static_mod.hpp>
#include <refrain/server/provider_protocol.hpp>
#include <refrain/server/provider_transport.hpp>
#include <refrain/server/server_runtime.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace {

[[nodiscard]] auto to_view(refrain_str value) noexcept -> std::string_view {
    if (value.data == nullptr || value.len == 0) {
        return {};
    }
    return std::string_view{reinterpret_cast<const char *>(value.data),
                            static_cast<std::size_t>(value.len)};
}

[[nodiscard]] auto string_of(refrain_str value) -> std::string {
    return std::string{to_view(value)};
}

/// ABI config -> app options. Both structs carry the same fields by design;
/// keep this the only translation point so a new CLI option cannot
/// silently diverge between the contract and the runtime.
[[nodiscard]] auto
make_options(const refrain_services_runtime_runtime_config &config,
             const refrain::core::config::view &network,
             const refrain::core::config::view &world,
             const refrain::core::config::view &storage,
             const refrain::core::config::view &runtime,
             refrain::core::config::store *config_store)
    -> refrain::server::server_run_options {
    refrain::server::server_run_options options{};
    options.world_path = string_of(config.world_path);
    options.network = network;
    options.world = world;
    options.storage = storage;
    options.runtime = runtime;
    options.config = config_store;
    options.ticks = config.ticks;
    options.command_file = string_of(config.command_file);
    options.script_file = string_of(config.script_file);
    options.hash_every = config.hash_every;
    options.world_zstd_level = config.world_zstd_level;
    options.zstd_near_level = config.zstd_near_level;
    options.zstd_mid_level = config.zstd_mid_level;
    options.zstd_far_level = config.zstd_far_level;
    options.zstd_near_radius = config.zstd_near_radius;
    options.zstd_mid_radius = config.zstd_mid_radius;
    options.watchdog_timeout_ms = config.watchdog_timeout_ms;
    options.autosave_interval_ticks = config.autosave_interval_ticks;
    options.compact_interval_ticks = config.compact_interval_ticks;
    options.compact_min_bytes = config.compact_min_bytes;
    options.compact_garbage_percent = config.compact_garbage_percent;
    options.store_compact_interval_ticks = config.store_compact_interval_ticks;
    options.content_store = string_of(config.content_store);
    options.content_gc_apply = config.content_gc_apply;
    options.content_gc_keep_days = config.content_gc_keep_days;
    options.content_gc_budget_ms = config.content_gc_budget_ms;
    options.content_gc_interval_ticks = config.content_gc_interval_ticks;
    return options;
}

struct runtime_provider {
    refrain::core::config::view network{};
    refrain::core::config::view world{};
    refrain::core::config::view storage{};
    refrain::core::config::view runtime{};
    refrain::core::config::store *config_store{nullptr};
    refrain::mod::host *host{nullptr};

    static auto run(void *self,
                    const refrain_services_runtime_runtime_config *config,
                    std::int32_t *out_value) -> refrain_status {
        if (self == nullptr || config == nullptr || out_value == nullptr ||
            config->struct_size !=
                REFRAIN_SERVICES_RUNTIME_RUNTIME_CONFIG_SIZE) {
            return REFRAIN_SERVICES_RUNTIME_RUNTIME_ERROR_INVALID_CONFIG;
        }
        auto *provider{static_cast<runtime_provider *>(self)};
        auto options{make_options(*config, provider->network, provider->world,
                                  provider->storage, provider->runtime,
                                  provider->config_store)};
        if (provider->host != nullptr) {
            const auto protocol{provider->host->services().active_provider(
                "refrain:net.protocol", "java_777")};
            if (!protocol || protocol->instance == nullptr) {
                return REFRAIN_SERVICES_RUNTIME_RUNTIME_ERROR_INVALID_CONFIG;
            }
            const auto *protocol_api{
                static_cast<const refrain::mod::provider_api *>(
                    protocol->instance)};
            if (protocol_api->vtable == nullptr) {
                return REFRAIN_SERVICES_RUNTIME_RUNTIME_ERROR_INVALID_CONFIG;
            }
            const refrain::services::net_protocol::provider protocol_service{
                *static_cast<const refrain_services_net_protocol_vtable *>(
                    protocol_api->vtable),
                protocol_api->self};
            auto descriptor{
                refrain::services::net_protocol::make_protocol_descriptor()};
            protocol_service.descriptor(descriptor);
            if (descriptor.canonical_version != 777U ||
                descriptor.transport_protocol != 0U ||
                descriptor.requires_stream == 0) {
                return REFRAIN_SERVICES_RUNTIME_RUNTIME_ERROR_INVALID_CONFIG;
            }
            refrain::log::info(
                "protocol: {} {} over {}",
                refrain::contracts::as_string_view(descriptor.family),
                descriptor.canonical_version,
                refrain::contracts::as_string_view(
                    descriptor.accepted_versions));
            options.protocol = std::shared_ptr<refrain::net::protocol_codec>{
                refrain::server::make_provider_protocol(
                    std::make_unique<refrain::services::net_protocol::provider>(
                        *static_cast<const refrain_services_net_protocol_vtable
                                         *>(protocol_api->vtable),
                        protocol_api->self))};
            const auto transport{provider->host->services().active_provider(
                "refrain:transport", "asio_tcp")};
            if (transport) {
                refrain::log::info("transport: {}", transport->id);
            }
            if (transport && transport->instance != nullptr) {
                const auto *api{static_cast<const refrain::mod::provider_api *>(
                    transport->instance)};
                if (api->vtable != nullptr) {
                    const auto *vtable{
                        static_cast<const refrain_services_transport_vtable *>(
                            api->vtable)};
                    void *transport_self{api->self};
                    options.make_transport =
                        [vtable, transport_self](
                            refrain::net::transport_handler handler,
                            std::size_t)
                        -> refrain::result<
                            std::unique_ptr<refrain::net::transport>> {
                        auto transport_service{std::make_unique<
                            refrain::services::transport::provider>(
                            *vtable, transport_self)};
                        return refrain::server::make_provider_transport(
                            std::move(transport_service), std::move(handler));
                    };
                }
            }
        }
        if (options.world_path.empty()) {
            return REFRAIN_SERVICES_RUNTIME_RUNTIME_ERROR_INVALID_CONFIG;
        }
        // `run_server` reports startup/shutdown failures through the log and
        // returns the process exit code; the contract forwards that code
        // unchanged (0 = clean stop), so the host never reinterprets it.
        *out_value =
            static_cast<std::int32_t>(refrain::server::run_server(options));
        return REFRAIN_STATUS_OK;
    }
};

auto runtime_vtable() -> const refrain_services_runtime_vtable & {
    static const refrain_services_runtime_vtable table{
        .struct_size = REFRAIN_SERVICES_RUNTIME_VTABLE_SIZE,
        .abi_major = REFRAIN_SERVICES_RUNTIME_ABI_MAJOR,
        .abi_minor = REFRAIN_SERVICES_RUNTIME_ABI_MINOR,
        .abi_patch = REFRAIN_SERVICES_RUNTIME_ABI_PATCH,
        .run = &runtime_provider::run,
    };
    return table;
}

auto make_server_vanilla(refrain::mod::host &host) -> refrain::status {
    static std::unique_ptr<runtime_provider> provider{};
    provider = std::make_unique<runtime_provider>();
    provider->host = &host;
    if (auto *store{host.config()}; store != nullptr) {
        provider->config_store = store;
        if (auto view{store->module("network")}) {
            provider->network = *view;
        }
        if (auto view{store->module("world")}) {
            provider->world = *view;
        }
        if (auto view{store->module("storage")}) {
            provider->storage = *view;
        }
        if (auto view{store->module("runtime")}) {
            provider->runtime = *view;
        }
    }
    const refrain::mod::provider_api api{
        .vtable = &runtime_vtable(),
        .self = provider.get(),
        .abi_major = REFRAIN_SERVICES_RUNTIME_ABI_MAJOR,
    };
    auto status{host.register_provider("refrain:runtime", api, "1.0.0")};
    if (!status) {
        provider.reset();
        return status;
    }
    return refrain::status_ok();
}

} // namespace

REFR_MOD_STATIC("refrain.server.vanilla", "1.0.0", 1, make_server_vanilla);
