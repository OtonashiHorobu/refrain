// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace refrain::mod {

/// Host-observed install tier for a mod tree or package. The numeric order
/// *is* the priority: a higher value wins when the same mod id appears in
/// several roots. Manifests can never declare or change their
/// origin; only the host observes where the bytes came from.
enum class mod_origin : std::uint8_t {
    builtin = 0,     ///< shipped with the engine/edition (default vanilla pack)
    first_party = 1, ///< official but optional modules
    user = 2,        ///< user-installed mods (including third-party)
};

/// Priority rank (higher wins): `user` > `first_party` > `builtin`.
[[nodiscard]] constexpr auto origin_rank(mod_origin origin) noexcept
    -> std::uint8_t {
    return static_cast<std::uint8_t>(origin);
}

/// `builtin` | `first_party` | `user` (lock/receipt/TOML spelling).
[[nodiscard]] constexpr auto to_string(mod_origin origin) noexcept
    -> std::string_view {
    switch (origin) {
    case mod_origin::builtin:
        return "builtin";
    case mod_origin::first_party:
        return "first_party";
    case mod_origin::user:
        return "user";
    }
    return "user";
}

/// Parse the TOML/lock/receipt spelling; `std::nullopt` for an unknown origin.
[[nodiscard]] constexpr auto parse_mod_origin(std::string_view text) noexcept
    -> std::optional<mod_origin> {
    if (text == "builtin") {
        return mod_origin::builtin;
    }
    if (text == "first_party") {
        return mod_origin::first_party;
    }
    if (text == "user") {
        return mod_origin::user;
    }
    return std::nullopt;
}

} // namespace refrain::mod
