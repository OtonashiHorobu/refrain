// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Permission administration frontends.
//
// The in-game `/permissions` tree and the console `permissions` line share one
// implementation: both hand their tokens to `execute_permissions_command()`,
// which parses them, resolves the subject, performs the change through the
// built-in resolver's management API and writes one audit record per changing
// command. The server console and a granted player therefore see the same
// lines, the same errors and the same audit entries.
//
// The subject a command names is one of:
//   - `console`, the server console;
//   - an online player name, resolved to the player's canonical uuid;
//   - `player:<uuid>`, `player:<profile name>`, `entity:<uuid>` or
//     `plugin:<mod id>`, the configured subject keys, so an offline player or a
//     plugin can be granted a node without being online.

#pragma once

#include <refrain/core/error.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::permissions {
class admin;
} // namespace refrain::permissions

namespace refrain::server {

/// One online player as the command layer knows it: the stable identity a
/// grant targets and the context a query should carry.
struct permissions_online_player {
    std::string name{};
    std::string uuid{};
    std::string world{};
    std::string gamemode{};
};

/// Who runs a permission command. The console has no uuid; a player carries
/// the canonical uuid from the session so the audit record identifies them
/// independently of the display name.
enum class permissions_actor_kind : std::uint8_t { console = 0, player = 1 };

struct permissions_actor {
    permissions_actor_kind kind{permissions_actor_kind::console};
    std::string name{"@console"};
    std::string uuid{};
};

/// One structured audit record of a changing permission command. Every grant,
/// revoke and reload appends one, whatever its result; the default sink writes
/// the formatted record through the `permissions-audit` logger.
struct permissions_audit_record {
    std::string actor_kind{};
    std::string actor_name{};
    std::string actor_uuid{};
    /// `grant`, `revoke` or `reload`.
    std::string action{};
    /// Canonical subject key for grant/revoke; empty for reload.
    std::string subject{};
    /// Rule text for grant/revoke; empty for reload.
    std::string node{};
    /// `ok`, `unchanged`, `not-persistent`, `invalid`, `failed` or `denied`.
    std::string result{};
    /// Short operator-facing reason when the result is not `ok`; never carries
    /// a filesystem path.
    std::string detail{};
    /// Policy revision after the command (the unchanged revision on failure).
    std::uint64_t revision{0};
    std::uint64_t duration_ms{0};
};

/// Format one record as the single operator sentence the audit sink writes.
[[nodiscard]] auto
format_permissions_audit_record(const permissions_audit_record &record)
    -> std::string;

/// Inputs the command bodies read: the management API of the active base
/// resolver, the online player lookup and the audit sink.
struct permissions_admin_state {
    /// Management surface of the active `refrain:permissions` base provider.
    /// Null when the composition has no permission service; the commands then
    /// report `not-handled` and refuse changes.
    refrain::permissions::admin *admin{nullptr};
    /// Resolve an online player name (case-insensitive) to its identity.
    std::function<std::optional<permissions_online_player>(std::string_view)>
        find_player{};
    /// Audit sink. Empty writes the formatted record through the
    /// `permissions-audit` logger.
    std::function<void(const permissions_audit_record &)> audit{};
};

/// Result of one `permissions` command: the lines the frontend prints in order,
/// then the failure text through its own error path when `ok` is false.
struct permissions_command_report {
    bool ok{true};
    std::vector<std::string> lines{};
    std::string error{};
};

/// Execute one `permissions` command. `tokens` are the words after the
/// `permissions` word; an empty span prints the usage. Never throws.
[[nodiscard]] auto
execute_permissions_command(const permissions_admin_state &state,
                            std::span<const std::string_view> tokens,
                            const permissions_actor &actor)
    -> permissions_command_report;

} // namespace refrain::server
