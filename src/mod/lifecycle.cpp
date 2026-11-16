// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/mod/lifecycle.hpp>

#include <array>
#include <string>
#include <string_view>

#include <fmt/format.h>

namespace refrain::mod {

auto to_string(mod_lifecycle_state state) noexcept -> std::string_view {
    switch (state) {
    case mod_lifecycle_state::staged:
        return "staged";
    case mod_lifecycle_state::active:
        return "active";
    case mod_lifecycle_state::quiescing:
        return "quiescing";
    case mod_lifecycle_state::retired:
        return "retired";
    case mod_lifecycle_state::removed:
        return "removed";
    }
    return "unknown";
}

auto to_string(unload_reason reason) noexcept -> std::string_view {
    switch (reason) {
    case unload_reason::shutdown:
        return "shutdown";
    case unload_reason::reload:
        return "reload";
    case unload_reason::uninstall:
        return "uninstall";
    }
    return "unknown";
}

auto is_restart_required_service(std::string_view service) noexcept -> bool {
    // The running tick loop owns a live world/sim/storage object graph; the
    // protocol codec owns framing state for open connections. None of them has
    // a hand-over contract, so replacing the implementation in place would
    // silently drop the live state. A gamepack `[providers]` key addresses the
    // same surface through the short form ("sim", "protocol"), so compare on
    // the name after the `refrain:` namespace.
    constexpr std::array<std::string_view, 5> restart_required{
        "sim", "world", "storage", "net.protocol", "runtime"};
    std::string_view name{service};
    constexpr std::string_view namespace_prefix{"refrain:"};
    if (name.starts_with(namespace_prefix)) {
        name.remove_prefix(namespace_prefix.size());
    }
    for (const auto blocked : restart_required) {
        if (name == blocked) {
            return true;
        }
    }
    return false;
}

auto restart_required_reason(std::string_view service) -> std::string {
    return fmt::format(
        "service '{}' is restart-required: the running tick loop holds its "
        "live state and has no hand-over contract, so the provider cannot be "
        "replaced while the server runs",
        service);
}

} // namespace refrain::mod
