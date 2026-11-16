// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/app/mod_management.hpp>
#include <refrain/core/error.hpp>
#include <refrain/mod/host.hpp>
#include <refrain/mod/rfmod.hpp>

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::app {

// Application-level hot mod management.
//
// This is the one service the command surface (console and in-game `/mods`)
// calls: the command layer parses arguments, performs permission checks and
// prints the result, while every lifecycle decision lives here. The service
// stages changes outside the tick and publishes them at the next tick
// boundary through commit(); installing, removing or reloading a mod never
// changes the composition between two ticks.
//
// Store transactions are delegated to the install library: `install()` runs
// the verified content-store install transaction and then stages the resolved
// composition; `uninstall()` stages the removal first and only then deletes the
// receipt, so a refusal never leaves the store inconsistent with the running
// composition.

struct mod_lifecycle_options {
    /// Content store roots whose install receipts form the external catalog.
    /// Empty keeps the catalog builtin-only (no hot install/uninstall).
    std::vector<std::string> store_roots{};
    /// Store `install()` writes to; empty uses the first root.
    std::string store_root{};
    /// Host side checked when re-resolving the gamepack ("server").
    std::string side{"server"};
    /// `[providers]` keys the host cannot start without.
    std::vector<std::string> required_services{};
    mod::discover_options discovery{};
    /// Operator identity recorded with every action (console, player name).
    std::string actor{"console"};
    /// Bounded wait for in-flight calls before a generation is retired.
    std::chrono::milliseconds drain_timeout{mod::default_drain_timeout};
};

/// One staged lifecycle operation, waiting for the tick-boundary commit.
struct mod_operation {
    std::string action{}; ///< "install", "uninstall" or "reload"
    /// Change the commit will publish; empty vectors mean "no-op".
    mod::staged_composition_change change{};
    /// Operator-facing summary of what happened before the staging step (for
    /// example "installed 1.2.0 of com.example.mod from ...").
    std::string detail{};
    /// Content-collection hint for an uninstall: the receipt is gone, the blobs
    /// are reclaimed by the next mark-sweep.
    std::string gc_hint{};
};

/// Committed composition epoch after a tick-boundary commit.
struct mod_commit_result {
    mod::composition_commit commit{};
};

class mod_lifecycle {
  public:
    /// `owner` must already be loaded; the service stages changes against its
    /// committed composition. The service attaches itself as the process-wide
    /// tick-boundary target; detach with the destructor.
    explicit mod_lifecycle(mod::host &owner,
                           mod_lifecycle_options options = {});
    mod_lifecycle(const mod_lifecycle &) = delete;
    auto operator=(const mod_lifecycle &) -> mod_lifecycle & = delete;
    mod_lifecycle(mod_lifecycle &&) = delete;
    auto operator=(mod_lifecycle &&) -> mod_lifecycle & = delete;
    ~mod_lifecycle();

    /// Install `.rfmod` (the verified content-store transaction), re-resolve
    /// the catalog and stage the new composition. The receipt stays installed
    /// when the staging step fails, so the package is picked up by the next
    /// server start instead of being half-removed.
    ///
    /// Errors: `failed_precondition` when no store is configured, whatever the
    /// package verifier/installer returns, and whatever
    /// `host::stage_composition()` returns (restart-required, loader failure).
    [[nodiscard]] auto
    install(std::string_view archive_path,
            const mod::rfmod_install_options &install_options = {})
        -> result<mod_operation>;

    /// Stage the removal of the active version of `id`. On success the install
    /// receipt of the requested version is removed; the composition still
    /// references the mod until the tick-boundary commit, which is safe because
    /// the generation is only retired instructions later.
    ///
    /// `version` empty removes every installed version of the id; a version
    /// other than the active one is refused with `version_mismatch`.
    [[nodiscard]] auto uninstall(std::string_view id,
                                 std::string_view version = {})
        -> result<mod_operation>;

    /// Rebuild the active version of `id` as the next generation. Data-only and
    /// clean/retire loaders are supported; statically linked code and core
    /// providers are refused with a restart-required message.
    [[nodiscard]] auto reload(std::string_view id) -> result<mod_operation>;

    /// Re-resolve the installed catalog and stage the difference against the
    /// running composition. This is the apply half of a store change another
    /// component already committed (the command layer installs or removes the
    /// receipt first and then asks for the running composition to follow).
    [[nodiscard]] auto stage_catalog() -> result<mod_operation>;

    /// Stage the removal of the active version of `id` without touching the
    /// store; the caller already removed the receipt.
    [[nodiscard]] auto stage_removal(std::string_view id,
                                     std::string_view version = {})
        -> result<mod_operation>;

    /// Publish the staged change at a tick boundary. With nothing staged this
    /// is a no-op that reports the committed epoch.
    [[nodiscard]] auto commit() -> result<mod_commit_result>;

    /// Drop the staged change without publishing it.
    void abort() noexcept;

    [[nodiscard]] auto pending() const noexcept -> bool {
        return owner_.staged_change().has_value();
    }
    [[nodiscard]] auto options() const noexcept
        -> const mod_lifecycle_options & {
        return options_;
    }
    [[nodiscard]] auto host() noexcept -> mod::host & { return owner_; }

  private:
    [[nodiscard]] auto stage_resolved() -> result<mod_operation>;
    [[nodiscard]] auto primary_store() -> result<std::string>;
    [[nodiscard]] auto resolve_composition() -> result<mod::composition>;

    mod::host &owner_;
    mod_lifecycle_options options_{};
};

/// Build the hot-lifecycle port the shared command service
/// (`app::mod_management`) calls. The command layer owns parsing, permission
/// checks, durable store transactions and audit; the returned port stages the
/// running-composition change and reports what will publish at the next tick
/// boundary. Install requests are applied to the catalog the command layer just
/// changed, uninstall requests stage the removal the command layer already
/// committed to the store, and reload requests rebuild the active generation.
[[nodiscard]] auto make_mod_lifecycle_port(mod_lifecycle &lifecycle)
    -> mod_lifecycle_port;

/// Register `controller` as the target of the tick-boundary commit. Only one
/// controller per host is expected; the tick loop commits every attached
/// controller.
void attach_mod_lifecycle(mod_lifecycle &controller);
void detach_mod_lifecycle(mod_lifecycle &controller) noexcept;

/// Commit every staged composition change. Called once per tick by the server
/// runtime before the tick body: the registry switch is atomic and no package
/// or store IO happens here. Returns the first failure (for the tick log); the
/// remaining controllers are still committed.
[[nodiscard]] auto commit_staged_mod_changes() -> status;

} // namespace refrain::app
