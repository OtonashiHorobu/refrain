// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/net/download.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace refrain::server {

// First-run game data acquisition.
//
// World generation reads the datapack of the game version the server targets,
// which the repository does not ship. On the first start after the EULA is
// accepted, the server fetches the official server archive named by Mojang's
// version manifest, verifies the archive against the published SHA-1, and
// unpacks only the datapack documents (worldgen, tags and structure templates)
// into a data directory. A marker records the game version, the data version
// and the archive identity; later starts reuse the directory without touching
// the network.
//
// Nothing Mojang publishes is committed, redistributed or bundled: the archive
// is downloaded on the operator's machine, only the extracted data stays in the
// local data directory, and the marker never contains game content.

/// Game version this build targets. The datapack, the protocol registry and the
/// tables are versioned together, so the marker and the fetched archive name
/// the same version.
inline constexpr std::string_view game_data_version{"26.3"};

/// DataVersion (save-format generation) of the target game version.
inline constexpr std::int32_t game_data_data_version{5023};

/// Marker file that records which game data the directory holds.
inline constexpr std::string_view game_data_marker_name{"MANIFEST.json"};

/// Marker schema written by this build. A marker with a different schema is
/// treated as foreign and never trusted silently.
inline constexpr std::string_view game_data_marker_schema{
    "refrain.game-data/1"};

/// Default endpoint of Mojang's version manifest.
inline constexpr std::string_view default_version_manifest_url{
    "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json"};

/// Where a run's game data came from.
enum class game_data_source : std::uint8_t {
    /// Fetched and verified from the official archive during this run.
    downloaded = 0,
    /// A marker-validated data directory from an earlier run.
    cache = 1,
    /// A data directory staged by other means, without a marker.
    pre_staged = 2,
    /// The tracked content snapshot shipped next to the binary.
    installed = 3,
};

[[nodiscard]] auto to_string(game_data_source source) noexcept
    -> std::string_view;

struct game_data_options {
    /// Explicit `--data-dir`. Empty uses the default resolution: a valid
    /// `./data`, else the installed content snapshot when it already holds the
    /// datapack, else `./data` for the first-run fetch.
    std::string data_dir{};
    /// Installed `content/vanilla/<version>` directory holding the tracked
    /// tables and reports; empty when the install has none.
    std::string installed_content_root{};
    /// Allow the network fetch (`--no-data-download` clears it).
    bool download_allowed{true};
    /// Version manifest endpoint; mirrors and tests override it.
    std::string manifest_url{std::string{default_version_manifest_url}};
    /// Working directory for the default `./data`; empty asks the process.
    std::string working_directory{};
};

/// Timeouts for the two fetch stages.
struct game_data_limits {
    std::chrono::milliseconds metadata_timeout{30'000};
    std::chrono::milliseconds archive_timeout{600'000};
};

/// Resolved game data for one run.
struct game_data_paths {
    game_data_source source{game_data_source::cache};
    /// Data directory root: holds `data/`, the marker and the extracted
    /// templates. Tracked tables may also be found here for staged installs.
    std::string root{};
    /// Datapack `data` directory handed to world generation.
    std::string datapack_root{};
    /// Extracted structure templates (`<root>/templates`), empty when absent.
    std::string templates_root{};
    /// Tracked table container: the data directory's copy when present, else
    /// the installed content root's, else empty (the runtime degrades).
    std::string tables_path{};
    /// Protocol registry, resolved like `tables_path`.
    std::string protocol_registry_path{};
    /// Game version recorded in the cache marker (build target otherwise).
    std::string game_version{};
    /// Data version recorded in the cache marker (build target otherwise).
    std::int32_t data_version{0};
    /// One-line origin description for the startup summary.
    std::string detail{};
};

/// Publish the resolved data paths for the in-process runtime provider.
///
/// The frozen `refrain:runtime` contract carries no game-data fields. The
/// composition root resolves the data before the world is created and the
/// first-party runtime provider runs in the same process, so it reads this
/// handoff when it assembles its options. Exactly one server runs per process;
/// the startup path publishes once, before the runtime starts.
void publish_game_data_paths(const game_data_paths &paths);

/// The paths published by `publish_game_data_paths`; empty before startup.
[[nodiscard]] auto published_game_data_paths() noexcept
    -> const std::optional<game_data_paths> &;

/// Forget the published paths (test hygiene: the handoff is process-wide).
void clear_game_data_paths() noexcept;

/// Resolve the game data, fetching and installing it when the data directory
/// does not hold a usable copy yet.
///
/// `transport` may be null only when the resolved directory already holds
/// usable data; a fetch without a transport fails with `unsupported`.
///
/// A directory is usable when its marker names this game version and the
/// datapack is present, or when a pre-staged `data/` tree with worldgen
/// documents exists and no marker claims otherwise. A marker for another game
/// version is stale: it is replaced when downloads are allowed and is a
/// `version_mismatch` error otherwise.
[[nodiscard]] auto ensure_game_data(const game_data_options &options,
                                    net::download_transport *transport,
                                    const game_data_limits &limits = {})
    -> result<game_data_paths>;

} // namespace refrain::server
