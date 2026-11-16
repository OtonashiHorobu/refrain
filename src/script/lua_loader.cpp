// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/script/loaders.hpp>

#include <refrain/contracts/services/mod_loader.hpp>
#include <refrain/mod/loader.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

namespace refrain::script::lua {
namespace {

namespace loader_abi = refrain::services::mod_loader;
namespace contracts = refrain::contracts;

using contract_capabilities =
    refrain_services_mod_loader_mod_loader_capabilities;
using contract_entry = refrain_services_mod_loader_mod_entry_manifest;
using contract_handle = refrain_services_mod_loader_handle_mod_instance;
using contract_reason = refrain_services_mod_loader_mod_unload_reason;
using contract_request = refrain_services_mod_loader_mod_load_request;

/// The provider state only has to be a non-null, stable identity: a loader that
/// can never own an instance has nothing else to remember.
struct provider_state {};

[[nodiscard]] auto view(const refrain_str &text) noexcept -> std::string_view {
    return contracts::as_string_view(text);
}

[[nodiscard]] auto report(std::string message) -> refrain_status {
    refrain::mod::set_loader_diagnostic(std::move(message));
    return static_cast<refrain_status>(
        loader_abi::mod_loader_error::unsupported);
}

/// The refusal names the loader, the manifest field that would select it and
/// the missing runtime, so an operator sees why the package cannot run instead
/// of a bare "loader not found".
[[nodiscard]] auto unavailable(const contract_entry &entry) -> refrain_status {
    const auto mod_id{view(entry.mod_id)};
    const auto mod_name{mod_id.empty() ? std::string{"?"}
                                       : std::string{mod_id}};
    return report(fmt::format(
        "mod '{}': the lua loader is unavailable because this host links no "
        "Lua runtime; the manifest declares [entry].loader = \"lua\"",
        mod_name));
}

/// Validate the little the loader can check without a runtime: the id it is
/// asked to handle and the entry fields reserved to other first-party loaders.
[[nodiscard]] auto check_entry(const contract_entry &entry)
    -> std::optional<refrain_status> {
    if (entry.struct_size <
        REFRAIN_SERVICES_MOD_LOADER_MOD_ENTRY_MANIFEST_SIZE) {
        refrain::mod::set_loader_diagnostic(
            fmt::format("lua loader: entry struct_size {} is smaller than the "
                        "contract's {}",
                        entry.struct_size,
                        REFRAIN_SERVICES_MOD_LOADER_MOD_ENTRY_MANIFEST_SIZE));
        return static_cast<refrain_status>(
            loader_abi::mod_loader_error::invalid_argument);
    }
    const auto loader{view(entry.loader)};
    if (loader != "lua") {
        refrain::mod::set_loader_diagnostic(
            fmt::format("loader provider for instance 'lua' was asked to "
                        "handle loader '{}'",
                        loader));
        return static_cast<refrain_status>(
            loader_abi::mod_loader_error::version_mismatch);
    }
    const auto mod_id{view(entry.mod_id)};
    const auto mod_name{mod_id.empty() ? std::string{"?"}
                                       : std::string{mod_id}};
    // The Lua loader has no payload field of its own yet; until it does, the
    // shared entry keys of the other first-party loaders are refused instead
    // of being silently ignored.
    // TODO: define the Lua guest payload field (the script member path) and
    // validate it here when the first-party Lua runtime lands.
    if (!view(entry.native_path).empty()) {
        refrain::mod::set_loader_diagnostic(fmt::format(
            "mod '{}': [entry].native is not part of the lua entry; the native "
            "loader owns shared libraries",
            mod_name));
        return static_cast<refrain_status>(
            loader_abi::mod_loader_error::invalid_argument);
    }
    if (!view(entry.wasm_path).empty()) {
        refrain::mod::set_loader_diagnostic(fmt::format(
            "mod '{}': [entry].wasm is not part of the lua entry; the wasm "
            "loader owns WebAssembly modules",
            mod_name));
        return static_cast<refrain_status>(
            loader_abi::mod_loader_error::invalid_argument);
    }
    return std::nullopt;
}

void bridge_capabilities(void *self, contract_capabilities *out) {
    static_cast<void>(self);
    if (out == nullptr) {
        return;
    }
    *out = contract_capabilities{};
    out->struct_size = REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_CAPABILITIES_SIZE;
    out->loader_id = contracts::to_borrowed(std::string_view{"lua"});
    out->abi_major = REFRAIN_SERVICES_MOD_LOADER_ABI_MAJOR;
    // The lua loader would be a sandboxed clean loader, exactly like the wasm
    // one; it declares that contract even while it refuses every package so a
    // host never records a weaker guarantee than the real loader will have.
    out->unload = REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_UNLOAD_CLEAN;
    out->sandboxed = 1;
    out->supports_hot_install = 0;
}

auto bridge_validate(void *self, const contract_entry *entry)
    -> refrain_status {
    if (self == nullptr || entry == nullptr) {
        refrain::mod::set_loader_diagnostic(
            "lua loader validate called without a provider or an entry");
        return static_cast<refrain_status>(
            loader_abi::mod_loader_error::invalid_argument);
    }
    if (auto checked{check_entry(*entry)}) {
        return *checked;
    }
    return unavailable(*entry);
}

auto bridge_load(void *self, const contract_request *request,
                 contract_handle *out) -> refrain_status {
    if (self == nullptr || request == nullptr || out == nullptr) {
        refrain::mod::set_loader_diagnostic(
            "lua loader load called without a provider, a request or a result "
            "slot");
        return static_cast<refrain_status>(
            loader_abi::mod_loader_error::invalid_argument);
    }
    *out = REFRAIN_SERVICES_MOD_LOADER_HANDLE_MOD_INSTANCE_INVALID;
    if (auto checked{check_entry(request->manifest)}) {
        return *checked;
    }
    return unavailable(request->manifest);
}

auto bridge_unload(void *self, contract_handle instance, contract_reason reason)
    -> refrain_status {
    static_cast<void>(instance);
    static_cast<void>(reason);
    if (self == nullptr) {
        refrain::mod::set_loader_diagnostic(
            "lua loader unload called without a provider");
        return static_cast<refrain_status>(
            loader_abi::mod_loader_error::invalid_argument);
    }
    refrain::mod::set_loader_diagnostic(
        "lua loader owns no instance: no Lua runtime is linked into this host");
    return static_cast<refrain_status>(loader_abi::mod_loader_error::not_found);
}

auto bridge_reload(void *self, contract_handle instance,
                   const contract_request *request, contract_handle *out)
    -> refrain_status {
    static_cast<void>(instance);
    if (self == nullptr || request == nullptr || out == nullptr) {
        refrain::mod::set_loader_diagnostic(
            "lua loader reload called without a provider, a request or a "
            "result slot");
        return static_cast<refrain_status>(
            loader_abi::mod_loader_error::invalid_argument);
    }
    *out = REFRAIN_SERVICES_MOD_LOADER_HANDLE_MOD_INSTANCE_INVALID;
    if (auto checked{check_entry(request->manifest)}) {
        return *checked;
    }
    return unavailable(request->manifest);
}

[[nodiscard]] auto vtable() noexcept
    -> const refrain_services_mod_loader_vtable & {
    static const refrain_services_mod_loader_vtable table{
        .struct_size = REFRAIN_SERVICES_MOD_LOADER_VTABLE_SIZE,
        .abi_major = REFRAIN_SERVICES_MOD_LOADER_ABI_MAJOR,
        .abi_minor = REFRAIN_SERVICES_MOD_LOADER_ABI_MINOR,
        .abi_patch = REFRAIN_SERVICES_MOD_LOADER_ABI_PATCH,
        .capabilities = &bridge_capabilities,
        .validate = &bridge_validate,
        .load = &bridge_load,
        .unload = &bridge_unload,
        .reload = &bridge_reload,
    };
    return table;
}

} // namespace

auto default_mod_loader_provider() noexcept -> mod_loader_provider {
    static provider_state state{};
    return mod_loader_provider{
        .vtable = &vtable(),
        .self = &state,
        .abi_major = REFRAIN_SERVICES_MOD_LOADER_ABI_MAJOR,
    };
}

} // namespace refrain::script::lua
