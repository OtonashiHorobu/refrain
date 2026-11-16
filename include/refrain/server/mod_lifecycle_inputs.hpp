// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <optional>
#include <string>
#include <vector>

namespace refrain::server {

// Mod catalog inputs the composition root resolved for the running server.
//
// The hot lifecycle re-resolves the installed catalog when a change is staged,
// so it must use the same content-store roots and the same effective
// auto-scan policy the running composition was resolved with. The frozen
// `refrain:runtime` contract carries only the content-store path (the store
// periodic GC targets), and the first-party runtime provider runs in this
// process: the composition root therefore hands the resolved roots and scan
// policy over here instead of widening the contract.
struct mod_lifecycle_inputs {
    /// Content-store roots whose install receipts form the external catalog,
    /// in composition-root priority order. Empty keeps the catalog
    /// builtin-only.
    std::vector<std::string> store_roots{};
    /// Effective auto-scan policy after the CLI overrides, so a staged catalog
    /// resolves exactly like the next server start would.
    bool auto_scan{true};
};

/// Publish the inputs for the in-process runtime provider.
///
/// Exactly one server runs per process: the startup path publishes once,
/// before the runtime starts, and the runtime takes the handoff when it
/// assembles the mod management service.
void publish_mod_lifecycle_inputs(mod_lifecycle_inputs inputs);

/// Take the published inputs. The handoff is consumed once, so a second
/// `run_server` call in the same process falls back to the options it was
/// given instead of reusing a previous server's catalog.
[[nodiscard]] auto take_mod_lifecycle_inputs()
    -> std::optional<mod_lifecycle_inputs>;

/// Forget the published inputs (test hygiene: the handoff is process-wide).
void clear_mod_lifecycle_inputs() noexcept;

} // namespace refrain::server
