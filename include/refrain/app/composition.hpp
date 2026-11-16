// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/mod/discovery.hpp>
#include <refrain/mod/gamepack.hpp>
#include <refrain/mod/host.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace refrain::app {

// Composition + activation service: the one startup path shared
// by `refrain-server` and `refrain-tools mods load`. It owns the pipeline order
//   catalog (builtin image + installed receipts) → resolve → select → load →
//   activate
// so neither main re-implements it.
//
// The external half of the catalog is read from content-store install receipts
// (`mod::scan_installed`); a `mods/<id>/` directory is not a load source.

struct server_mods_options {
    /// Content store roots whose install receipts form the external mod
    /// catalog. Empty keeps the composition builtin-only.
    std::span<const std::string> installed_stores{};
    /// Embedded/loaded built-in RFSI image (empty = no builtin seed).
    std::span<const std::byte> builtin_image{};
    /// `[providers]` keys the host cannot start without.
    std::span<const std::string_view> required_services{};
    /// Override `gamepack.auto_scan` (`--auto-scan` / `--no-auto-scan`).
    std::optional<bool> auto_scan{};
    /// Catalog options (e.g. `require_mods_dir` for explicit store roots).
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
[[nodiscard]] auto resolve_server_mods(const mod::gamepack_spec &spec,
                                       const server_mods_options &options)
    -> result<mod::composition>;

/// Load and activate every planned provider fail-closed against `image`.
/// `composition` is read-only here: the caller keeps it for the lock stage.
[[nodiscard]] auto activate_server_mods(mod::host &host,
                                        const mod::composition &composition,
                                        std::span<const std::byte> image)
    -> status;

/// Register the first-party script loader providers on `host` before
/// activation: `wasm` (the sandboxed WebAssembly loader) and `lua` (an
/// explicit fail-closed provider while no Lua runtime is linked). Idempotent
/// per loader id: an id that already has a provider registered, for example one
/// an embedder or a test installed, is left alone. Failures are the host's
/// registration errors.
[[nodiscard]] auto register_script_loaders(mod::host &host) -> status;

/// Resolve + activate (the `mods load` / test path).
[[nodiscard]] auto load_server_mods(const mod::gamepack_spec &spec,
                                    const server_mods_options &options)
    -> result<std::unique_ptr<server_mods>>;

} // namespace refrain::app
