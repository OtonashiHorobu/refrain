// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <cstddef>
#include <span>
#include <string_view>

namespace refrain::version {

/// Release version injected by CMake (e.g. "0.1.0-alpha").
extern const char *release_version;

/// Git branch the binary was built from (may be empty for source exports).
extern const char *branch_name;

/// Git commit hash the binary was built from (may be empty).
extern const char *commit_hash;

/// Human-readable version string, e.g. "Refrain 0.1.0-alpha [main@abc1234]".
[[nodiscard]] auto string() noexcept -> std::string_view;

// --- build-time mod profile ------------------------------------------------
//
// The build selects which first-party mods exist (profiles/<name>.toml, CMake
// cache REFRAIN_MOD_PROFILE). The generated table below is the binary's own
// record of that selection: it is what the startup preflight compares a
// gamepack or mods.lock pin against, so a composition that asks for a mod this
// binary does not contain fails with a profile diagnostic instead of a missing
// service key.

/// Profile name the binary was built with, e.g. "vanilla".
extern const char *mod_profile;

/// Identity of the resolved mod selection: 16 lowercase hex characters over
/// the profile name, link mode, mod list and default gamepack. Recorded in
/// mods.lock; locks written by different builds are refused.
extern const char *mod_build_id;

/// Repo-relative default gamepack selected by the profile.
extern const char *mod_gamepack;

/// Mod link mode this binary was built with: "static", "mixed" or "dynamic".
extern const char *mod_link_mode;

/// One first-party mod built as a shared module: mod id and the file name
/// below `<executable dir>/modules/`.
struct dynamic_module_record {
    std::string_view id{};
    std::string_view filename{};
};

/// Mod ids built into this binary (static and dynamic), sorted ascending.
[[nodiscard]] auto mod_ids() noexcept -> std::span<const std::string_view>;

/// Dynamic modules this binary expects to load at startup, sorted by id.
[[nodiscard]] auto dynamic_modules() noexcept
    -> std::span<const dynamic_module_record>;

/// True when `id` is part of this binary's mod profile.
[[nodiscard]] auto is_mod_built(std::string_view id) noexcept -> bool;

} // namespace refrain::version
