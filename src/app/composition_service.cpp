// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/app/composition.hpp>

#include <refrain/app/dynamic_modules.hpp>
#include <refrain/mod/loader.hpp>
#include <refrain/script/loaders.hpp>

#include <string_view>
#include <utility>

#include <fmt/format.h>

namespace refrain::app {

auto resolve_server_mods(const mod::gamepack_spec &spec,
                         const server_mods_options &options)
    -> result<mod::composition> {
    return mod::resolve_installed_composition(
        spec, options.installed_stores,
        mod::composition_load_options{.builtin_image = options.builtin_image,
                                      .required_services =
                                          options.required_services,
                                      .discovery = options.discovery,
                                      .auto_scan = options.auto_scan});
}

auto register_script_loaders(mod::host &host) -> status {
    const auto register_one{
        [&host](std::string_view loader_id,
                const script::mod_loader_provider &provider) -> result<bool> {
            if (host.find_loader(loader_id) != nullptr) {
                return false;
            }
            auto registered{host.register_mod_loader(
                loader_id, mod::provider_api{.vtable = provider.vtable,
                                             .self = provider.self,
                                             .abi_major = provider.abi_major})};
            if (!registered) {
                auto failure{std::move(registered.error())};
                failure.message =
                    fmt::format("cannot register the '{}' mod loader: {}",
                                loader_id, failure.message);
                return std::unexpected{std::move(failure)};
            }
            return true;
        }};
    auto wasm_registered{register_one(
        mod::wasm_loader_id, script::wasm::default_mod_loader_provider())};
    if (!wasm_registered) {
        return std::unexpected{std::move(wasm_registered.error())};
    }
    if (auto lua_registered{register_one(
            mod::lua_loader_id, script::lua::default_mod_loader_provider())};
        !lua_registered) {
        return std::unexpected{std::move(lua_registered.error())};
    }
    // The script host dispatches callbacks into the guests of the wasm loader
    // this bootstrap registered. An embedder that installed its own wasm
    // loader owns the matching script host, because instance handles are
    // loader-owned.
    if (*wasm_registered) {
        if (auto registered{script::register_script_host(
                host, script::wasm::default_script_host_provider())};
            !registered) {
            auto failure{std::move(registered.error())};
            failure.message =
                fmt::format("cannot register the script callback host: {}",
                            failure.message);
            return std::unexpected{std::move(failure)};
        }
    }
    return status_ok();
}

auto activate_server_mods(mod::host &host, const mod::composition &composition,
                          std::span<const std::byte> image) -> status {
    // The script loaders are host bootstrap code: they must be active before
    // any manifest naming one is dispatched.
    if (auto registered{register_script_loaders(host)}; !registered) {
        return registered;
    }
    // Mods the build profile selected as dynamic are not linked in: load their
    // modules and register the factories before the composition loader asks the
    // static registry for each mod. Idempotent, so repeated activations (tests,
    // tooling) stay correct.
    if (auto registered{register_dynamic_modules()}; !registered) {
        return registered;
    }
    return host.load(composition, image);
}

auto load_server_mods(const mod::gamepack_spec &spec,
                      const server_mods_options &options)
    -> result<std::unique_ptr<server_mods>> {
    auto loaded{std::make_unique<server_mods>()};
    auto composition{resolve_server_mods(spec, options)};
    if (!composition) {
        return std::unexpected{std::move(composition.error())};
    }
    loaded->composition = std::move(*composition);
    if (auto activated{activate_server_mods(loaded->host, loaded->composition,
                                            options.builtin_image)};
        !activated) {
        return std::unexpected{std::move(activated.error())};
    }
    return loaded;
}

} // namespace refrain::app
