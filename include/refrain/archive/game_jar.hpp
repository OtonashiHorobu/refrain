// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/archive/zip.hpp>
#include <refrain/core/error.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace refrain::archive {

// Read-only view of a Minecraft server archive.
//
// Modern server archives are bundler jars: the outer archive holds metadata,
// libraries and the real server jar at
// `META-INF/versions/<version>/server-<version>.jar`, and only the inner jar
// contains the `data/` tree. Older archives and synthetic test archives keep
// `data/` at the root. `game_jar` hides that difference: the data-bearing
// reader is either the outer archive or the inner jar, which is unpacked into a
// private temporary file for the lifetime of the object.
//
// The class never ships or rewrites game content; it only exposes the read side
// for local extraction into a data directory the operator owns.

class game_jar {
  public:
    game_jar(game_jar &&other) noexcept;
    auto operator=(game_jar &&other) noexcept -> game_jar &;
    ~game_jar();

    game_jar(const game_jar &) = delete;
    auto operator=(const game_jar &) -> game_jar & = delete;

    /// Open a server archive. Fails with `io_error` when the file is not a
    /// readable archive and with `parse_error` when it is an archive but
    /// carries neither a root `data/` tree nor a bundler server jar.
    [[nodiscard]] static auto open(std::string_view path) -> result<game_jar>;

    /// Archive that holds the game data tree.
    [[nodiscard]] auto reader() const noexcept -> const zip_reader &;

    /// True when the data tree came from the inner jar of a bundler archive.
    [[nodiscard]] auto bundled() const noexcept -> bool;

    /// Original archive path as passed to `open()`.
    [[nodiscard]] auto archive_path() const noexcept -> std::string_view;

    /// Game version id from the archive's `version.json` (`id` field); empty
    /// when no readable version document exists.
    [[nodiscard]] auto version_id() const noexcept -> std::string_view;

    /// Extract the archive's root `version.json` below `output_root`, so the
    /// pack format metadata sits next to the extracted `data/` tree. Returns
    /// false when the archive has no version document.
    [[nodiscard]] auto extract_version_json(std::string_view output_root)
        -> result<bool>;

  private:
    struct impl;

    explicit game_jar(std::unique_ptr<impl> state) noexcept;

    std::unique_ptr<impl> impl_;
};

} // namespace refrain::archive
