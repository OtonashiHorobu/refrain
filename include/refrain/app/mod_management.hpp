// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/mod/install_policy.hpp>

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::app {

// Mod management application service.
//
// One implementation answers both the server console `mods ...` commands and
// the in-game `/mods ...` command, so the two frontends cannot drift in their
// results, errors or audit entries. The service parses the command line
// itself: a frontend only supplies the tokens after `mods`, the actor identity
// and a report sink.
//
// Scope: catalog inspection, package verification, installation into the
// content store, uninstallation, signer-pin (trust) administration and the
// request to change the running composition. Installing into the content
// store is durable state; changing the *running* composition is the hot
// lifecycle implementation's job and goes through `mod_lifecycle_port`, which
// this build leaves unavailable. Every operation is audited, including the
// ones that fail or are refused.

/// Permission node the in-game frontend checks. The console is always
/// allowed. A player needs this node; the built-in permission service decides
/// who holds it (an operator listed under `[ops].players` or an explicit
/// grant), and a composition without the service falls back to the declared
/// vanilla op level of the command tree.
inline constexpr std::string_view mod_admin_permission{"refrain.mods.admin"};

/// Who runs a management command. The console has no player uuid; a player
/// carries the canonical uuid from the session so the audit record identifies
/// them independently of the display name.
enum class mod_management_actor_kind : std::uint8_t {
    console = 0,
    player = 1,
};

struct mod_management_actor {
    mod_management_actor_kind kind{mod_management_actor_kind::console};
    std::string name{"@console"};
    std::string uuid{};
};

/// One mod of the running composition, snapshotted by the composition root
/// when the service is built. The snapshot is immutable for the session.
struct mod_management_mod {
    std::string id{};
    std::string version{};
    std::string loader{};
    /// True when the loader executed code for the mod (a data-only mod still
    /// participates in the composition but runs nothing).
    bool has_code{false};
    /// `builtin` for a mod compiled and embedded in the binary, `installed`
    /// for one read from the content store's install receipts.
    std::string source{};
    /// Service keys this mod provides, in registry order.
    std::vector<std::string> services{};
};

/// A change to the running composition. The hot lifecycle implementation
/// stages it, commits it at a tick boundary and retires the previous
/// generation; this layer only asks for it and reports the answer.
enum class mod_lifecycle_action : std::uint8_t {
    install = 0,
    uninstall = 1,
    reload = 2,
};

struct mod_lifecycle_request {
    mod_lifecycle_action action{mod_lifecycle_action::install};
    std::string id{};
    std::string version{};
    /// `.rfmod` path for `install`; empty for the other actions.
    std::string package{};
};

struct mod_lifecycle_outcome {
    /// Operator-facing lines describing what committed. One event per line.
    std::vector<std::string> lines{};
};

/// Replacement point for the hot lifecycle implementation.
///
/// The implementation owns staging, the tick-boundary epoch commit, the
/// in-flight drain and native retire; this service never touches the service
/// registry or a loader. `apply` empty means the running composition cannot be
/// changed at all; `unavailable_reason` then says why, and every operation that
/// would need it reports `unsupported` with that text instead of pretending the
/// change happened.
struct mod_lifecycle_port {
    std::function<result<mod_lifecycle_outcome>(const mod_lifecycle_request &)>
        apply{};
    std::string unavailable_reason{};
};

/// Result of a content reload request: the operator-facing lines that describe
/// which generation is live and when the rebuilt view publishes.
struct content_reload_outcome {
    std::vector<std::string> lines{};
};

/// Replacement point for the content view epoch.
///
/// The implementation owns the background rebuild, the validation and the
/// tick-boundary swap of the datapack view. `request` empty means the running
/// server has no content view to rebuild (the composition root never published
/// one); `unavailable_reason` then says why, and the command reports
/// `unsupported` instead of pretending the reload happened.
struct content_reload_port {
    std::function<result<content_reload_outcome>()> request{};
    std::string unavailable_reason{};
};

/// One structured audit record. Every operation appends one, whatever its
/// result; the default sink writes it through the `mod-audit` logger.
struct mod_audit_record {
    std::string actor_kind{};
    std::string actor_name{};
    std::string actor_uuid{};
    std::string action{};
    std::string id{};
    std::string version{};
    /// BLAKE3 hex of the package archive when the operation verified or
    /// installed one; empty otherwise.
    std::string digest{};
    /// Signing key id of the package; empty for an unsigned package.
    std::string key_id{};
    /// Install signature policy in force: `strict`, `trusted-any` or
    /// `unsigned`.
    std::string policy{};
    /// `ok`, `restart-required`, `dry-run`, `refused`, `unsupported` or
    /// `failed`.
    std::string result{};
    /// The error text when the result is not `ok`, empty otherwise.
    std::string detail{};
    std::uint64_t duration_ms{0};
};

/// Format one record as the single operator sentence the audit sink writes.
[[nodiscard]] auto format_mod_audit_record(const mod_audit_record &record)
    -> std::string;

/// Inputs the composition root assembles once. Everything the service needs is
/// a value here, so tests can build it without a platform runtime.
struct mod_management_context {
    /// Content store that install/uninstall/trust target. Empty means the
    /// server started without one; operations that need it report that.
    std::string store{};
    /// Effective install/trust policy (`config/mods.toml`) and the file it was
    /// read from, for diagnostics. Empty source means the built-in default.
    mod::install_policy_config policy{};
    std::string policy_source{};
    /// Mod administration file (`config/mods_admin.toml`) the hot gates were
    /// read from; empty means the built-in default. Diagnostics only.
    std::string admin_source{};
    /// Hot gates read from `config/mods_admin.toml`; all default off.
    bool hot_install{false};
    bool hot_reload{false};
    /// `[lifecycle] hot_reload_data`: rebuild the datapack content view at
    /// runtime.
    bool hot_reload_data{false};
    /// Running composition snapshot, in load order.
    std::vector<mod_management_mod> loaded{};
    mod_lifecycle_port lifecycle{};
    /// Content view epoch (datapack content, not mod code).
    content_reload_port content_reload{};
    /// Audit sink. Empty writes the formatted record through the module
    /// logger; tests pass a capturing sink.
    std::function<void(const mod_audit_record &)> audit{};
};

/// Result of one management command. Both frontends print `lines` in order,
/// then report `error` through their own failure path when `ok` is false.
struct mod_management_report {
    bool ok{true};
    std::vector<std::string> lines{};
    std::string error{};
    /// Machine-readable outcome recorded in the audit record.
    std::string outcome{"ok"};
};

/// The shared management service.
class mod_management {
  public:
    explicit mod_management(mod_management_context context);

    /// Execute one `mods` command. `arguments` are the tokens after the
    /// `mods` word; an empty span prints the usage. Never throws; a command
    /// that fails returns `ok == false` with the operator-facing reason.
    [[nodiscard]] auto execute(std::span<const std::string_view> arguments,
                               const mod_management_actor &actor)
        -> mod_management_report;

    [[nodiscard]] auto context() const noexcept
        -> const mod_management_context & {
        return context_;
    }

  private:
    mod_management_context context_;
};

} // namespace refrain::app
