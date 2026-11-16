// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>

#include <string>
#include <string_view>

namespace refrain::server {

// First-run EULA acceptance.
//
// Running a Minecraft server requires accepting the Minecraft End User License
// Agreement. Refrain records that acceptance in a small TOML file next to the
// working directory (`eula.toml` by default) instead of the vanilla
// `eula.txt`; the file is written with `eula = false` on the first run and the
// server refuses to create a world or listen until it is accepted. A vanilla
// `eula.txt` present when the TOML file does not exist is imported exactly
// once, so an existing installation keeps its recorded decision.
//
// The refusal notice names the file and the one command that accepts the
// agreement; it never reproduces the agreement text, which lives at the
// official URL below.

/// Official location of the Minecraft End User License Agreement.
inline constexpr std::string_view eula_url{
    "https://www.minecraft.net/en-us/eula"};

/// Where and how this run resolves the acceptance.
struct eula_options {
    /// Refrain's acceptance file. Defaults to `eula.toml` in the working
    /// directory, like the vanilla server's `eula.txt`.
    std::string path{"eula.toml"};
    /// Vanilla acceptance file imported when `path` does not exist yet. An
    /// empty value disables the import.
    std::string legacy_path{"eula.txt"};
    /// Record acceptance now and continue (`--accept-eula`).
    bool accept{false};
};

/// Result of one resolution.
struct eula_state {
    /// True when the agreement is accepted after this call.
    bool accepted{false};
    /// True when the acceptance file existed before this call.
    bool existed{false};
    /// True when the decision was imported from the vanilla `eula.txt`.
    bool imported_legacy{false};
    /// True when this call created or rewrote the acceptance file.
    bool written{false};
    /// Resolved acceptance file path.
    std::string path{};
    /// Vanilla file consulted for the import (empty when disabled).
    std::string legacy_path{};
};

/// Read the acceptance, importing a vanilla `eula.txt` once when needed and
/// writing the TOML file on the first run or on `--accept-eula`.
///
/// Fails with `invalid_argument` for an empty or non-regular path,
/// `parse_error` for an unreadable acceptance document or an `eula` entry that
/// is not a boolean, and `io_error` when the file cannot be written. A
/// malformed vanilla `eula.txt` is treated as "not accepted" rather than as a
/// fatal error, which matches the vanilla server.
[[nodiscard]] auto ensure_eula(const eula_options &options)
    -> result<eula_state>;

/// The operator-facing refusal notice: what is required, where the acceptance
/// file lives, the command that accepts it, and what did not happen.
[[nodiscard]] auto format_eula_notice(const eula_state &state) -> std::string;

} // namespace refrain::server
