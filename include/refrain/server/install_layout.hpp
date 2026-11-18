// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>

#include <string>

namespace refrain::server {

// Installed-layout discovery for a zero-argument start.
//
// A release archive ships the binary under `bin/`, the default composition
// under `gamepack/` and the tracked game-derived tables under
// `content/vanilla/<version>/`. A checkout keeps the same three trees at its
// root, one directory above `build/Release/bin`, and additionally carries the
// tooling that regenerates the tracked data from a server archive. Rather than
// embedding a copy of the composition and the tables in the executable, the
// server searches the working directory, the executable's directory and a few
// parent directories for that layout. A checkout and an extracted release
// archive therefore both start without an argument, and an operator can inspect
// or replace the composition with an editor.
//
// The default composition is `gamepack/gamepack.toml`; the installed content
// root is `content/vanilla/<version>` and holds the tracked tables, reports and
// packet registry (never the datapack, which is generated locally). The data
// generation entry point is `tools/content/generate.sh`, present only in a
// source checkout.
//
// The documented layout is chosen over an embedded composition because the
// gamepack and the tracked tables are already repository files that ship in the
// release archive and carry their own notice: embedding them would duplicate
// several MiB in every binary and make a composition edit require a rebuild,
// while the search keeps the binary a thin loader and the files inspectable.

/// Resolved install layout.
struct install_layout {
    /// Directory the default composition was found under; empty when
    /// `--gamepack` named the file explicitly.
    std::string root{};
    /// Gamepack file selected for this run.
    std::string gamepack{};
    /// Installed `content/vanilla/<version>` directory holding the tracked
    /// game-derived tables and reports; empty when no install has one.
    std::string content_root{};
    /// Human-readable location the content root came from, for the startup
    /// summary: "working directory" or a location above the executable.
    std::string content_source{};
    /// Directory holding the running binaries; empty when the platform did not
    /// report the executable path. The generation fallback looks for
    /// `refrain-tools` here.
    std::string bin_directory{};
    /// `refrain-tools` next to the server binary; empty when the install has
    /// none.
    std::string tools_program{};
    /// Data generation entry point (`tools/content/generate.sh`) in a source
    /// checkout; empty when the install ships only a packaged content snapshot.
    std::string generation_script{};
};

struct install_layout_options {
    /// Working directory to search first; empty asks the process for its own.
    std::string working_directory{};
    /// Executable path to search from; empty asks the platform layer for the
    /// running image (tests pass a synthetic path).
    std::string executable{};
    /// Game version whose content directory is looked up (`26.3`).
    std::string game_version{"26.3"};
    /// Explicit `--gamepack` file; skips the default composition search.
    std::string gamepack{};
};

/// Discover the default composition and the installed content root.
///
/// A missing gamepack is a `not_found` error naming the paths that were
/// searched, because without a composition the server cannot start at all. A
/// missing content root is not an error: the first-run data acquisition can
/// still unpack the datapack, and the tracked tables then fall back to the
/// binary's built-in behaviour with a warning.
[[nodiscard]] auto
discover_install_layout(const install_layout_options &options)
    -> result<install_layout>;

} // namespace refrain::server
