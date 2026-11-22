// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/mod/discovery.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::app {

// Build-time mod profiles.
//
// A profile (`profiles/<name>.toml`) declares the first-party mods that exist
// in a binary, the mods built as shared modules and the default gamepack. CMake
// resolves the same file at configure time; the tools below read it without a
// build so `mods check --profile` and `gamepack check --profile` can validate a
// profile/gamepack pair before anything is compiled.
//
// Schema:
//
//   name = "vanilla"                  # must match the file stem
//   description = "..."              # optional
//   gamepack = "gamepack/gamepack.toml"
//   mods = [ "refrain.content.vanilla", ... ]
//   dynamic = [ "refrain.data.vanilla" ]        # optional
//   build_id = "0123456789abcdef"    # written by the build, optional here
//   link = "mixed"                   # written by the build, optional here

struct mod_profile_spec {
    std::string name{};
    std::string description{};
    /// Gamepack path as declared by the profile (repo-relative).
    std::string gamepack{};
    /// `gamepack` resolved against the profile root; the CLI reads this file.
    std::string gamepack_path{};
    /// First-party mod ids, sorted and unique.
    std::vector<std::string> mods{};
    /// Mods built as shared modules (subset of `mods`), sorted.
    std::vector<std::string> dynamic{};
    /// Optional fields written into the resolved profile by the build.
    std::string link{"static"};
    std::string build_id{};
    /// Path the profile was read from, for diagnostics.
    std::string source{};
};

/// Load and schema-check one profile file.
[[nodiscard]] auto load_mod_profile(std::string_view path)
    -> result<mod_profile_spec>;

/// Load `profiles/<name>.toml`; `directory` defaults to `profiles`.
[[nodiscard]] auto
load_mod_profile_named(std::string_view name,
                       std::string_view directory = "profiles")
    -> result<mod_profile_spec>;

/// Findings of a profile consistency check. Errors fail the command; warnings
/// are printed but do not.
struct profile_check_result {
    std::vector<std::string> errors{};
    std::vector<std::string> warnings{};

    [[nodiscard]] auto ok() const noexcept -> bool { return errors.empty(); }
    void error(std::string message) { errors.push_back(std::move(message)); }
    void warn(std::string message) { warnings.push_back(std::move(message)); }
};

/// Check every listed mod exists under `mods_root` with a readable manifest,
/// that the `[requires]` closure is inside the profile and that every dynamic
/// mod has code (`mods/<id>/src/*.cpp`). This is the pre-build check that turns
/// a missing or mis-typed profile entry into a clear error.
[[nodiscard]] auto check_profile_mods_tree(const mod_profile_spec &profile,
                                           std::string_view mods_root)
    -> profile_check_result;

/// Check a gamepack against the profile: every pin must be built and every
/// built mod must be pinned. `gamepack_path` overrides the profile's default.
[[nodiscard]] auto check_profile_gamepack(const mod_profile_spec &profile,
                                          std::string_view gamepack_path)
    -> profile_check_result;

/// Check a built-in image contains exactly the profile's mod manifests (and
/// `mods.index.toml`). Used by CI to prove the image and the compiled sources
/// describe the same set.
[[nodiscard]] auto check_profile_image(const mod_profile_spec &profile,
                                       std::string_view image_path)
    -> profile_check_result;

/// Compare a profile file with the profile embedded in this binary (name, mod
/// ids and dynamic ids). Used by `--built` to prove that the profile, the
/// compiled sources and the built-in image describe the same set.
[[nodiscard]] auto check_profile_against_binary(const mod_profile_spec &profile)
    -> profile_check_result;

/// Ids from `ids` that this binary was not built with; empty when every id is
/// part of the build-time profile.
[[nodiscard]] auto unbuilt_mod_ids(std::span<const std::string> ids)
    -> std::vector<std::string>;

/// Diagnostic for a gamepack/mods.lock pin the binary does not contain:
/// `mod '<id>' is not built into this binary (profile=<name>, build <id>)`.
[[nodiscard]] auto format_unbuilt_mod_error(std::string_view source,
                                            std::span<const std::string> ids)
    -> std::string;

/// Reject a `mods.lock` written by a different build profile or build id.
/// Both fields are required by the lock format; a lock that does not record
/// them is refused instead of being compared against the running binary.
[[nodiscard]] auto check_lock_profile(const mod::mod_lock &lock) -> status;

/// Stable one-line profile identity, e.g.
/// `vanilla (12 mods, 0 dynamic, build e9e6e4a4906e3694)`.
[[nodiscard]] auto format_profile_identity(const mod_profile_spec &profile)
    -> std::string;

// Gamepack generation from a build profile.
//
// Generated gamepacks are starters: they pin every profile mod at the version
// its manifest declares and bind each service contract to its single declaring
// mod. Provider composition (exclusive, instanced or chain) is a deployment
// decision and is never inferred. A service with more than one candidate is a
// hard error naming the service and the candidates.

struct gamepack_gen_options {
    /// Profile name under the profile directory.
    std::string profile{};
    /// Root holding `<id>/manifest.toml` for the profile mods.
    std::string mods_dir{"mods"};
    /// Profile directory used to find `<name>.toml`.
    std::string profiles_dir{"profiles"};
    /// Destination `gamepack.toml` path.
    std::string out{};
    /// Compare with the existing file instead of writing it.
    bool check{false};
};

struct generated_gamepack {
    /// The parsed, validated form of `text`.
    mod::gamepack_spec spec{};
    /// Byte-identical TOML written for the same profile and manifests.
    std::string text{};
};

/// Build the starter gamepack text for `profile` from the manifests under
/// `mods_root`. No paths, clocks or iteration-order accidents enter `text`.
[[nodiscard]] auto generate_gamepack(const mod_profile_spec &profile,
                                     std::string_view mods_root)
    -> result<generated_gamepack>;

/// CLI entry point: load the named profile, generate and write `--out`.
[[nodiscard]] auto run_gamepack_gen(const gamepack_gen_options &options) -> int;

} // namespace refrain::app
