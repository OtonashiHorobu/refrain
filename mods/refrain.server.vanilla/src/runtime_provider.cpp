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
#include <refrain/contracts/services/session_abi.h>
#include <refrain/contracts/services/session_lifecycle.hpp>
#include <refrain/contracts/services/session_status.hpp>
#include <refrain/contracts/services/session_tab.hpp>
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
            const auto protocol_instance{
                provider->network.get_string("protocol_instance", "java_777")};
            const auto transport_instance{
                provider->network.get_string("transport_instance", "asio_tcp")};
            std::vector<std::pair<
                const refrain_services_session_status_vtable *, void *>>
                status_hooks{};
            for (const auto &instance :
                 provider->host->services().active_instances(
                     "refrain:session.status")) {
                const auto info{provider->host->services().active_provider(
                    "refrain:session.status", instance)};
                if (!info || info->instance == nullptr) {
                    continue;
                }
                const auto *api{static_cast<const refrain::mod::provider_api *>(
                    info->instance)};
                if (api->vtable != nullptr) {
                    status_hooks.emplace_back(
                        static_cast<const refrain_services_session_status_vtable
                                        *>(api->vtable),
                        api->self);
                }
            }
            if (!status_hooks.empty()) {
                const auto network_view{provider->network};
                options.status =
                    [hooks = std::move(status_hooks), network_view](
                        const refrain::net::status_request_context &context) {
                        for (const auto &[vtable, hook_self] : hooks) {
                            const refrain::services::session_status::provider
                                status_service{*vtable, hook_self};
                            auto request{refrain::services::session_status::
                                             make_status_request()};
                            request.protocol =
                                static_cast<std::uint32_t>(context.protocol);
                            request.host =
                                refrain::contracts::to_borrowed(context.host);
                            request.port = context.port;
                            request.online_players = static_cast<std::uint32_t>(
                                context.online_players);
                            auto view{refrain::services::session_status::
                                          make_status_view()};
                            if (status_service.status(request, view)) {
                                refrain::net::status_info info{};
                                info.description = std::string{
                                    refrain::contracts::as_string_view(
                                        view.description)};
                                info.online_players = static_cast<std::int32_t>(
                                    view.online_players);
                                info.max_players =
                                    static_cast<std::int32_t>(view.max_players);
                                return info;
                            }
                        }
                        refrain::net::status_info info{};
                        info.description = network_view.get_string(
                            "status.motd", "A Refrain Server");
                        info.online_players = context.online_players;
                        info.max_players = static_cast<std::int32_t>(
                            network_view.get_int64("status.max_players", 20));
                        return info;
                    };
            }
            std::vector<
                std::pair<const refrain_services_session_tab_vtable *, void *>>
                tab_hooks{};
            for (const auto &instance :
                 provider->host->services().active_instances(
                     "refrain:session.tab")) {
                const auto info{provider->host->services().active_provider(
                    "refrain:session.tab", instance)};
                if (!info || info->instance == nullptr) {
                    continue;
                }
                const auto *api{static_cast<const refrain::mod::provider_api *>(
                    info->instance)};
                if (api->vtable != nullptr) {
                    tab_hooks.emplace_back(
                        static_cast<const refrain_services_session_tab_vtable
                                        *>(api->vtable),
                        api->self);
                }
            }
            if (!tab_hooks.empty()) {
                options
                    .tab = [hooks = std::move(tab_hooks)](
                               const refrain::net::player_identity &identity) {
                    for (const auto &[vtable, hook_self] : hooks) {
                        const refrain::services::session_tab::provider
                            tab_service{*vtable, hook_self};
                        auto request{
                            refrain::services::session_tab::make_tab_request()};
                        request.connection = identity.connection;
                        request.player_name =
                            refrain::contracts::to_borrowed(identity.name);
                        auto view{
                            refrain::services::session_tab::make_tab_view()};
                        if (tab_service.tab(request, view)) {
                            return refrain::net::tab_view{
                                .header =
                                    std::string{
                                        refrain::contracts::as_string_view(
                                            view.header)},
                                .footer =
                                    std::string{
                                        refrain::contracts::as_string_view(
                                            view.footer)},
                            };
                        }
                    }
                    return refrain::net::tab_view{};
                };
            }
            std::vector<std::pair<
                const refrain_services_session_lifecycle_vtable *, void *>>
                lifecycle_hooks{};
            for (const auto &instance :
                 provider->host->services().active_instances(
                     "refrain:session.lifecycle")) {
                const auto info{provider->host->services().active_provider(
                    "refrain:session.lifecycle", instance)};
                if (!info || info->instance == nullptr) {
                    continue;
                }
                const auto *api{static_cast<const refrain::mod::provider_api *>(
                    info->instance)};
                if (api->vtable != nullptr) {
                    lifecycle_hooks.emplace_back(
                        static_cast<
                            const refrain_services_session_lifecycle_vtable *>(
                            api->vtable),
                        api->self);
                }
            }
            if (!lifecycle_hooks.empty()) {
                const auto dispatch =
                    [hooks = std::move(lifecycle_hooks)](
                        const refrain::net::player_identity &identity,
                        bool joining) {
                        for (const auto &[vtable, hook_self] : hooks) {
                            const refrain::services::session_lifecycle::provider
                                lifecycle{*vtable, hook_self};
                            auto request{refrain::services::session_lifecycle::
                                             make_player_info()};
                            request.connection = identity.connection;
                            request.name =
                                refrain::contracts::to_borrowed(identity.name);
                            request.world =
                                refrain::contracts::to_borrowed(identity.world);
                            if (joining) {
                                lifecycle.player_join(request);
                            } else {
                                lifecycle.player_leave(request);
                            }
                        }
                    };
                options.player_join =
                    [dispatch](const refrain::net::player_identity &identity) {
                        dispatch(identity, true);
                    };
                options.player_leave =
                    [dispatch](const refrain::net::player_identity &identity) {
                        dispatch(identity, false);
                    };
            }
            const auto protocol{provider->host->services().active_provider(
                "refrain:net.protocol", protocol_instance)};
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
            auto tab_codec{
                protocol_service
                    .extension<refrain::services::net_protocol::tab_codec>()};
            if (tab_codec) {
                options.encode_tab = [codec =
                                          *tab_codec](std::string_view header,
                                                      std::string_view footer)
                    -> refrain::result<std::vector<std::byte>> {
                    auto packet{
                        refrain::services::net_protocol::make_encoded_packet()};
                    auto encoded{codec.encode_tab(header, footer, packet)};
                    if (!encoded) {
                        return std::unexpected{refrain::make_error(
                            refrain::error_code::parse_error,
                            "protocol tab encoder failed")};
                    }
                    const auto bytes{refrain::contracts::as_bytes(packet.data)};
                    return std::vector<std::byte>{
                        reinterpret_cast<const std::byte *>(bytes.data()),
                        reinterpret_cast<const std::byte *>(bytes.data()) +
                            bytes.size()};
                };
            }
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
                "refrain:transport", transport_instance)};
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
                    const refrain::services::transport::provider
                        transport_inspector{*vtable, transport_self};
                    auto transport_descriptor{refrain::services::transport::
                                                  make_transport_descriptor()};
                    transport_inspector.descriptor(transport_descriptor);
                    if (transport_descriptor.protocol !=
                            descriptor.transport_protocol ||
                        transport_descriptor.reliable == 0 ||
                        transport_descriptor.ordered == 0 ||
                        (descriptor.requires_stream != 0 &&
                         transport_descriptor.message_oriented != 0)) {
                        return REFRAIN_SERVICES_RUNTIME_RUNTIME_ERROR_INVALID_CONFIG;
                    }
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

void session_descriptor(void *self,
                        refrain_services_session_session_descriptor *out) {
    static_cast<void>(self);
    if (out == nullptr) {
        return;
    }
    out->struct_size = REFRAIN_SERVICES_SESSION_SESSION_DESCRIPTOR_SIZE;
    static constexpr std::string_view family{"minecraft-java"};
    out->family = refrain::contracts::to_borrowed(family);
    static constexpr std::string_view edition{"java"};
    out->edition = refrain::contracts::to_borrowed(edition);
    static constexpr std::string_view capabilities{"tick,console"};
    out->capabilities = refrain::contracts::to_borrowed(capabilities);
}

const refrain_services_session_vtable &session_vtable() {
    static const refrain_services_session_vtable table{
        .struct_size = REFRAIN_SERVICES_SESSION_VTABLE_SIZE,
        .abi_major = REFRAIN_SERVICES_SESSION_ABI_MAJOR,
        .abi_minor = REFRAIN_SERVICES_SESSION_ABI_MINOR,
        .abi_patch = REFRAIN_SERVICES_SESSION_ABI_PATCH,
        .descriptor = &session_descriptor,
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
    const refrain::mod::provider_api session_api{
        .vtable = &session_vtable(),
        .self = provider.get(),
        .abi_major = REFRAIN_SERVICES_SESSION_ABI_MAJOR,
    };
    status = host.register_provider("refrain:session", "minecraft", session_api,
                                    "1.0.0");
    if (!status) {
        provider.reset();
        return status;
    }
    return refrain::status_ok();
}

} // namespace

REFR_MOD_STATIC("refrain.server.vanilla", "1.0.0", 1, make_server_vanilla);
