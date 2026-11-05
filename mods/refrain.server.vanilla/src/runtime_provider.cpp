// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// `refrain:runtime` provider for the internal `refrain.server.vanilla` mod.
// This adapter converts the generated `runtime_config` ABI struct into
// `app::server_run_options` and calls the in-process `app::run_server`. The
// tick loop still uses the C++ app API directly; routing its internals through
// `refrain:world`/`refrain:sim`/`refrain:storage` is not implemented here.

#include <refrain/app/server_runtime.hpp>
#include <refrain/contracts/services/runtime_abi.h>
#include <refrain/kernel/static_mod.hpp>
#include <refrain/mod/host.hpp>

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
make_options(const refrain_services_runtime_runtime_config &config)
    -> refrain::app::server_run_options {
    refrain::app::server_run_options options{};
    options.world_path = string_of(config.world_path);
    options.server_properties_path = string_of(config.server_properties);
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
    static auto run(void *self,
                    const refrain_services_runtime_runtime_config *config,
                    std::int32_t *out_value) -> refrain_status {
        if (self == nullptr || config == nullptr || out_value == nullptr ||
            config->struct_size !=
                REFRAIN_SERVICES_RUNTIME_RUNTIME_CONFIG_SIZE) {
            return REFRAIN_SERVICES_RUNTIME_RUNTIME_ERROR_INVALID_CONFIG;
        }
        auto options{make_options(*config)};
        if (options.world_path.empty()) {
            return REFRAIN_SERVICES_RUNTIME_RUNTIME_ERROR_INVALID_CONFIG;
        }
        // `run_server` reports startup/shutdown failures through the log and
        // returns the process exit code; the contract forwards that code
        // unchanged (0 = clean stop), so the host never reinterprets it.
        *out_value =
            static_cast<std::int32_t>(refrain::app::run_server(options));
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
