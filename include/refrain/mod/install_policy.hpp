// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/mod/rfmod.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::mod {

// Mod install/trust policy read from server configuration.
//
// The policy decides *which* installs are acceptable and how updates are
// constrained; the operator edits it as `config/mods.toml`, the startup
// bootstrap and `refrain-tools mods install` read it, and the command-line
// flags only override individual values for one invocation. Command-line-only
// trust configuration is not a supported deployment shape: a restart would
// silently drop it.
//
// File schema (all tables optional):
//
//   [signature]
//   require = true          # reject unsigned packages / unknown signers
//   policy = "strict"       # "strict" or "trusted-any" (self-hosted only)
//
//   [trust]                 # trust anchors, key id -> Ed25519 public key hex
//   "refrain.first-party" = "<64 lowercase hex characters>"
//
//   [install]
//   allow_downgrade = false        # --allow-downgrade overrides one run
//   replace_same_version = false   # --replace-same-version overrides one run
//   keep_signer_pin = false        # keep the pin after the last uninstall
//
//   [rotation."example.mod"]       # explicit signer transition allowlist
//   old_key_id = "example-2025"
//   new_key_id = "example-2026"
//
// Unknown tables/keys are rejected (`parse_error`): a typo in a trust anchor
// silently widening or narrowing the policy is worse than a failed startup.

/// Signature continuity policy plus install switches, as configured.
struct install_policy_config {
    /// Trust anchors matched by manifest `key_id`.
    std::vector<rfmod_trusted_key> trusted_keys{};
    rfmod_signature_policy signature_policy{rfmod_signature_policy::strict};
    /// Reject unsigned packages and signatures without a configured anchor.
    /// Production default is `true`; a development store may set it false.
    bool require_signature{true};
    /// Accept a version lower than the installed highest (one-run CLI
    /// override).
    bool allow_downgrade{false};
    /// Accept the same `(id, version)` with a different archive digest.
    bool replace_same_version{false};
    /// Keep the signer pin after uninstalling the last version.
    bool keep_signer_pin{false};
    /// Explicit signer transitions (`[rotation."<id>"]`).
    std::vector<rfmod_key_rotation> rotations{};
    /// Source file the policy was read from (diagnostics only; empty for the
    /// built-in default).
    std::string source{};
};

/// One-run command-line overrides applied on top of a loaded policy.
struct install_policy_overrides {
    std::optional<bool> require_signature{};
    std::optional<bool> allow_downgrade{};
    std::optional<bool> replace_same_version{};
    std::optional<bool> keep_signer_pin{};
    std::optional<rfmod_signature_policy> signature_policy{};
    /// Added to (never replacing) the configured anchors.
    std::vector<rfmod_trusted_key> trusted_keys{};
    /// Added to the configured rotation allowlist.
    std::vector<rfmod_key_rotation> rotations{};
};

/// Parse one policy file. A missing file is `not_found`; a malformed file is
/// `parse_error` naming the file and the offending key.
[[nodiscard]] auto load_install_policy(std::string_view path)
    -> result<install_policy_config>;

/// Apply command-line overrides to `policy` (in place).
void apply_install_policy_overrides(install_policy_config &policy,
                                    const install_policy_overrides &overrides);

/// Copy the install-time subset of `policy` into `options`:
/// `require_signature`, `trusted_keys`, `signature_policy`, `rotations`,
/// `allow_downgrade` and `replace_same_version`.
void apply_install_policy(const install_policy_config &policy,
                          rfmod_install_options &options);

/// Format the configured trust anchors as `key_id=hex` lines (audit output;
/// sorted by key id).
[[nodiscard]] auto format_install_policy(const install_policy_config &policy)
    -> std::string;

} // namespace refrain::mod
