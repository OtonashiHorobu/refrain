// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/core/semver.hpp>

#include <string>
#include <string_view>

namespace refrain::mod {

// Mod identity primitives shared by gamepack manifests, mod manifests and the
// mod loader. All declared versions are SemVer 2.0.0 values.
//
// A "mod reference" pins an exact version (`id@1.2.3`); a "requirement" pins a
// SemVer range (`id@^1.0.0`). Both are plain strings on purpose: manifests keep
// the author's text for diagnostics, while validation normalizes semantics.

/// A concrete mod reference written as `id@semver`, e.g.
/// `refrain.worldgen.flat@1.0.0`. The version text is preserved verbatim for
/// diagnostics; matching is exact SemVer.
struct mod_ref {
    std::string id{};
    std::string version{};

    [[nodiscard]] auto text() const -> std::string;
    [[nodiscard]] auto parsed() const -> result<core::semver>;

    friend auto operator==(const mod_ref &, const mod_ref &) -> bool = default;
};

/// Parse/validate `id@semver`. Fails with `invalid_argument` for malformed
/// references.
[[nodiscard]] auto parse_mod_ref(std::string_view text) -> result<mod_ref>;

/// A dependency/provider requirement written as `id@range`, e.g.
/// `refrain.worldgen.flat@^1.0.0` or `refrain.sim.vanilla@>=1.0.0 <2.0.0`.
struct mod_requirement {
    std::string id{};
    std::string range{};

    [[nodiscard]] auto text() const -> std::string;

    friend auto operator==(const mod_requirement &, const mod_requirement &)
        -> bool = default;
};

[[nodiscard]] auto parse_mod_requirement(std::string_view text)
    -> result<mod_requirement>;

/// Mod ids: 1..128 characters, ASCII alphanumeric first character, then
/// alphanumerics or `. _ -`. Namespaced ids such as `refrain.world.vanilla`
/// and `com.example.rpg` are valid. Ids are case-sensitive textually but the
/// loader treats a case-only difference as a conflict (portable file systems).
inline constexpr std::size_t max_mod_id_length = 128;

[[nodiscard]] auto is_valid_mod_id(std::string_view id) noexcept -> bool;

/// Service ids (`namespace:name`, e.g. `refrain:data.packs`): exactly one
/// ':', both halves non-empty, lowercase [a-z0-9._-], no empty/`.`/`..`
/// segments. These are the ids generated from RIDL (`service_id` constants).
[[nodiscard]] auto is_valid_service_id(std::string_view id) noexcept -> bool;

} // namespace refrain::mod
