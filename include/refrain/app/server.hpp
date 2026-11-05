// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/kernel/gamepack.hpp>
#include <refrain/mod/discovery.hpp>
#include <refrain/mod/host.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace refrain::app {

// Composition + activation service: the one startup path shared
// by `refrain-server` and `refrain-tools mods load`. It owns the pipeline order
//   catalog (builtin image + discovery roots) → resolve → select → load →
//   activate
// so neither main re-implements it.

struct server_mods_options {
    /// `--mods` instance roots (origin=user), already tagged by the caller.
    std::span<const mod::discovery_root> roots{};
    /// Embedded/loaded built-in RFSI image (empty = no builtin seed).
    std::span<const std::byte> builtin_image{};
    /// `[providers]` keys the host cannot start without.
    std::span<const std::string_view> required_services{};
    /// Override `gamepack.auto_scan` (`--auto-scan` / `--no-auto-scan`).
    std::optional<bool> auto_scan{};
    /// Discovery options (e.g. `require_mods_dir` for explicit `--mods`).
    mod::discover_options discovery{};
};

/// One loaded server composition: the resolved plan plus the activated host.
struct server_mods {
    mod::composition composition{};
    mod::host host{};
};

/// Resolve only (catalog + lock + providers). `refrain-server` uses this before
/// `--dump-gamepack` because the dump path must not require every planned mod
/// to exist in the built-in image.
[[nodiscard]] auto resolve_server_mods(const kernel::gamepack_spec &spec,
                                       const server_mods_options &options)
    -> result<mod::composition>;

/// Load and activate every planned provider fail-closed against `image`.
/// `composition` is read-only here: the caller keeps it for the lock stage.
[[nodiscard]] auto activate_server_mods(mod::host &host,
                                        const mod::composition &composition,
                                        std::span<const std::byte> image)
    -> status;

/// Resolve + activate (the `mods load` / test path).
[[nodiscard]] auto load_server_mods(const kernel::gamepack_spec &spec,
                                    const server_mods_options &options)
    -> result<std::unique_ptr<server_mods>>;

} // namespace refrain::app
