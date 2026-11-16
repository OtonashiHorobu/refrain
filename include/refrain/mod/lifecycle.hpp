// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/mod/loader.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::mod {

// Hot mod lifecycle vocabulary shared by the host and the application
// lifecycle service.
//
// A mod moves through
//
//   staged -> active -> quiescing -> retired -> removed
//
//  - staged:   the package is resolved and its code/loader instance is built,
//              but it is not part of the committed composition. A failure here
//              aborts without touching the running epoch.
//  - active:   the mod's providers are in the committed registry generation and
//              callers hold generation-stamped handles.
//  - quiescing: new calls into the mod's provider slots are refused; the
//              lifecycle waits a bounded time for in-flight calls to drain.
//  - retired:  on_unload ran (or the mod was quarantined); the generation is
//              stale and the code is never called again. A native library may
//              still be mapped in the process (retire, never dlclose).
//  - removed:  the receipt and the composition record are gone; content GC may
//              collect the package blobs.

enum class mod_lifecycle_state : std::uint8_t {
    staged = 0,
    active = 1,
    quiescing = 2,
    retired = 3,
    removed = 4,
};

[[nodiscard]] auto to_string(mod_lifecycle_state state) noexcept
    -> std::string_view;

/// Why the host asked the mod to unload. The loader contract carries the same
/// distinctions; `shutdown` is also used when a staged instance is dropped
/// without ever becoming active (the process did not intend to keep it).
enum class unload_reason : std::uint8_t {
    shutdown = 0,
    reload = 1,
    uninstall = 2,
};

[[nodiscard]] auto to_string(unload_reason reason) noexcept -> std::string_view;

/// Default bounded wait for in-flight calls before a generation is retired.
/// A mod whose on_unload cannot be reached in this window is quarantined
/// instead of being called after its generation expired.
inline constexpr std::chrono::milliseconds default_drain_timeout{5000};

/// Default number of retired native generations a single mod may accumulate.
/// Every native reload keeps the old shared library mapped for the process
/// lifetime (dlclose is never called); after this many generations a reload is
/// refused and the operator must restart the server.
inline constexpr std::size_t default_native_retire_limit{8};

/// One mod's lifecycle record as reported to the composition root and audit
/// log. It is a snapshot: the host owns the authoritative state.
struct mod_lifecycle_record {
    std::string id{};
    std::string version{};
    std::string loader{};
    mod_lifecycle_state state{mod_lifecycle_state::staged};
    /// Monotonic per mod id; incremented when a reloaded generation becomes
    /// active. An installed mod starts at 1.
    std::uint64_t generation{0};
    /// Removal guarantee the loader declared when the instance was loaded.
    mod_unload_semantics unload_semantics{
        mod_unload_semantics::restart_required};
    /// True when the mod may not be called or reloaded again; a new install of
    /// the same id needs a process restart.
    bool quarantined{false};
    std::string quarantine_reason{};
};

/// Services that cannot be replaced while the server runs. The tick loop binds
/// the world/simulation/storage objects directly and the protocol codec owns
/// the connection framing; a different implementation would have to take over
/// live state that has no hand-over contract yet. A mod that provides any of
/// them is restart-required for reload and uninstall.
[[nodiscard]] auto
is_restart_required_service(std::string_view service) noexcept -> bool;

/// Short reason text for a restart-required refusal, naming the matched
/// service (for example "service 'refrain:sim' requires a restart").
[[nodiscard]] auto restart_required_reason(std::string_view service)
    -> std::string;

/// Summary of one staged (not yet committed) composition change.
struct staged_composition_change {
    std::uint64_t base_epoch{0};
    /// Mod ids that will be added.
    std::vector<std::string> added{};
    /// Mod ids whose code was rebuilt for a new generation.
    std::vector<std::string> reloaded{};
    /// Mod ids that will be retired after the commit.
    std::vector<std::string> removed{};
    /// Mod ids whose native mapping count is retired but not yet collected.
    std::size_t retired_native_generations{0};
};

/// Result of committing a staged composition at a tick boundary.
struct composition_commit {
    std::uint64_t epoch{0};
    /// BLAKE3 hex digest over the committed epoch, gamepack identity, every
    /// active mod generation and every active provider generation. Part of
    /// the audit record; a replay compares it to prove the same composition.
    std::string digest{};
    std::vector<std::string> added{};
    std::vector<std::string> reloaded{};
    std::vector<std::string> removed{};
    /// Mods whose on_unload failed or whose calls did not drain; they are never
    /// called again and need a restart to be re-installed.
    std::vector<std::string> quarantined{};
    std::size_t retired_native_generations{0};
};

} // namespace refrain::mod
