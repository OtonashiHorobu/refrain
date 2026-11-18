// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/mod/loader.hpp>

#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

namespace refrain::mod {
namespace {

/// Per-thread because the loader contract dispatches on the main thread and a
/// process-global string would race with a provider that dispatches from
/// another thread despite the contract.
thread_local std::string loader_diagnostic{};

} // namespace

void set_loader_diagnostic(std::string message) {
    loader_diagnostic = std::move(message);
}

void clear_loader_diagnostic() noexcept { loader_diagnostic.clear(); }

auto take_loader_diagnostic() -> std::string {
    auto message{std::move(loader_diagnostic)};
    loader_diagnostic.clear();
    return message;
}

auto is_host_loader_id(std::string_view id) noexcept -> bool {
    return id == builtin_loader_id || id == native_loader_id;
}

auto check_mod_loader_id(std::string_view id) -> status {
    if (!is_valid_loader_id(id)) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            fmt::format("loader id '{}' is not valid: loader ids are 1 to 128 "
                        "lowercase letters, digits, '.', '_' or '-', start "
                        "with a letter or digit and contain no empty '.' "
                        "segment",
                        id))};
    }
    if (is_first_party_loader_id(id)) {
        return std::unexpected{make_error(
            error_code::permission_denied,
            fmt::format("loader id '{}' is reserved to the host and cannot be "
                        "provided by a mod package; a package-provided loader "
                        "uses a namespaced id such as com.example.loader, "
                        "while builtin, native, wasm, lua, bukkit and the "
                        "refrain. namespace stay host-owned",
                        id))};
    }
    if (id.find('.') == std::string_view::npos) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            fmt::format("loader id '{}' is not namespaced; a package-provided "
                        "loader uses an id such as com.example.loader",
                        id))};
    }
    return status_ok();
}

} // namespace refrain::mod
