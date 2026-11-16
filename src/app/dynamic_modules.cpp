// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/app/dynamic_modules.hpp>

#include <refrain/core/semver.hpp>
#include <refrain/core/version.hpp>
#include <refrain/mod/mod_ref.hpp>
#include <refrain/platform/process.hpp>

#include <fmt/format.h>

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::app {

namespace fs = std::filesystem;
namespace mod = refrain::mod;

namespace {

/// Process-lifetime state of the loaded modules. They are never unloaded: the
/// code is part of the binary's composition and stays valid for the same
/// reason linked-in static mod code does.
struct dynamic_module_state {
    std::mutex mutex{};
    bool registered{false};
    std::vector<std::unique_ptr<loaded_dynamic_module>> modules{};
};

[[nodiscard]] auto state() -> dynamic_module_state & {
    static dynamic_module_state instance{};
    return instance;
}

} // namespace

auto dynamic_module_directory() -> result<std::string> {
    auto executable{refrain::platform::executable_path()};
    if (!executable) {
        return std::unexpected{std::move(executable.error())};
    }
    const fs::path directory{fs::path{*executable}.parent_path() / "modules"};
    return directory.string();
}

auto load_dynamic_module(std::string_view path)
    -> result<loaded_dynamic_module> {
    auto library{refrain::platform::dynamic_library::load(path)};
    if (!library) {
        const bool symbol_problem{library.error().message.find(
                                      "undefined symbol") != std::string::npos};
        return std::unexpected{make_error(
            error_code::mod_error,
            symbol_problem
                ? fmt::format(
                      "failed to load dynamic mod module '{}': {}; the module "
                      "calls a runtime symbol the host executable does not "
                      "export, so it must be built into the host or listed in "
                      "another profile's dynamic set until the shared runtime "
                      "library exists",
                      path, library.error().message)
                : fmt::format("failed to load dynamic mod module '{}': {}",
                              path, library.error().message))};
    }
    auto entry{library->symbol_as<mod::dynamic_module_entry_fn>(
        mod::dynamic_module_entry_symbol)};
    if (!entry) {
        return std::unexpected{make_error(
            error_code::mod_error,
            fmt::format("dynamic mod module '{}' does not export '{}'", path,
                        mod::dynamic_module_entry_symbol))};
    }
    const auto *descriptor{(*entry)()};
    if (descriptor == nullptr) {
        return std::unexpected{make_error(
            error_code::mod_error,
            fmt::format("dynamic mod module '{}' returned a null descriptor",
                        path))};
    }
    if (!mod::is_valid_mod_id(descriptor->id)) {
        return std::unexpected{make_error(
            error_code::mod_error,
            fmt::format("dynamic mod module '{}' declares invalid mod id '{}'",
                        path, descriptor->id))};
    }
    if (!refrain::core::semver::parse(descriptor->version)) {
        return std::unexpected{make_error(
            error_code::mod_error,
            fmt::format(
                "dynamic mod '{}' in '{}' declares invalid version '{}'",
                descriptor->id, path, descriptor->version))};
    }
    if (descriptor->abi == 0U) {
        return std::unexpected{
            make_error(error_code::mod_error,
                       fmt::format("dynamic mod '{}' in '{}' declares abi 0",
                                   descriptor->id, path))};
    }
    if (descriptor->factory == nullptr) {
        return std::unexpected{make_error(
            error_code::mod_error,
            fmt::format("dynamic mod '{}' in '{}' exports no factory",
                        descriptor->id, path))};
    }
    loaded_dynamic_module loaded{};
    loaded.id = std::string{descriptor->id};
    loaded.path = std::string{path};
    loaded.descriptor = *descriptor;
    loaded.library.emplace(std::move(*library));
    return loaded;
}

auto register_dynamic_modules() -> status {
    auto &shared{state()};
    const std::lock_guard lock{shared.mutex};
    if (shared.registered) {
        return status_ok();
    }
    if (version::dynamic_modules().empty()) {
        shared.registered = true;
        return status_ok();
    }
    auto directory{dynamic_module_directory()};
    if (!directory) {
        return std::unexpected{std::move(directory.error())};
    }
    auto &registry{mod::static_mod_registry::global()};
    if (registry.is_frozen()) {
        return std::unexpected{make_error(
            error_code::permission_denied,
            fmt::format("cannot register dynamic modules for profile '{}' "
                        "after the static mod registry was frozen",
                        version::mod_profile))};
    }
    std::vector<std::unique_ptr<loaded_dynamic_module>> loaded{};
    loaded.reserve(version::dynamic_modules().size());
    for (const auto &record : version::dynamic_modules()) {
        const fs::path path{fs::path{*directory} / record.filename};
        auto module{load_dynamic_module(path.string())};
        if (!module) {
            return std::unexpected{std::move(module.error())};
        }
        if (module->id != record.id) {
            return std::unexpected{make_error(
                error_code::mod_error,
                fmt::format("dynamic module '{}' declares mod '{}' but the "
                            "build profile '{}' expects '{}'",
                            path.string(), module->id, version::mod_profile,
                            record.id))};
        }
        if (registry.find(module->id) != nullptr) {
            return std::unexpected{make_error(
                error_code::already_exists,
                fmt::format("dynamic mod '{}' is registered more than once",
                            module->id))};
        }
        auto added{registry.add(
            mod::static_mod_descriptor{.id = module->descriptor.id,
                                       .version = module->descriptor.version,
                                       .abi = module->descriptor.abi,
                                       .factory = module->descriptor.factory})};
        if (!added) {
            return std::unexpected{std::move(added.error())};
        }
        loaded.push_back(
            std::make_unique<loaded_dynamic_module>(std::move(*module)));
    }
    shared.modules = std::move(loaded);
    shared.registered = true;
    return status_ok();
}

} // namespace refrain::app
