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
// The datapack and the tracked artifacts (the table container, the protocol
// registry and the reports world generation reads) resolve separately, in this
// order:
//
//   1. the data directory itself (a staged, cached or downloaded copy);
//   2. the content snapshot shipped next to the binary
//      (`content/vanilla/<version>`), which is how a release archive starts
//      without the network;
//   3. the downloaded archive, when the download is allowed and a transport is
//      available;
//   4. generation from the operator's own server archive, when the install
//      carries the generation tooling and a jar is named or was just
//      downloaded. Generation is optional: it is attempted only when every
//      input exists and it never becomes a runtime dependency.
//
// A new world cannot be generated without the tracked block data and the
// climate reports. When all four sources are unavailable the resolver returns
// one error that names the missing artifact and the command that produces it,
// before the composition root creates a world.
//
// Nothing Mojang publishes is committed, redistributed or bundled: the archive
// is downloaded (or read from the operator's own copy) on the operator's
// machine, only the extracted data stays in the local data directory, and the
// marker never contains game content.

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

/// Inputs for the optional generation fallback.
///
/// The fallback runs the repository entry point that reuses the existing
/// tooling: the vanilla data generator (Java), the light and mining probes and
/// `refrain-tools vanilla import`. Every input is detected; an empty field
/// disables the corresponding path, so a release archive that ships the
/// tracked artifacts never depends on Java, Python or the source tooling.
struct game_data_generation_options {
    /// Generation entry point (`tools/content/generate.sh`); empty when the
    /// install does not carry the source tooling.
    std::string script{};
    /// The operator's own official server archive; empty disables generation
    /// unless a first-run download supplied one.
    std::string server_jar{};
    /// `refrain-tools` binary that imports the reports; empty disables the
    /// fallback because the entry point cannot import the tables.
    std::string tools{};
    /// Java executable for the data generator; empty uses `java` from PATH.
    std::string java{};
    /// Wall-clock bound for one generation run.
    std::chrono::milliseconds timeout{std::chrono::minutes{30}};
};

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
    /// Generation inputs; all fields empty in an install that ships the
    /// tracked data.
    game_data_generation_options generation{};
    /// Require the tracked tables, the climate reports and the protocol
    /// registry before returning.
    ///
    /// A world that generates chunks and serves clients cannot run without
    /// them: the resolver then fails with one actionable message instead of
    /// letting the failure reach the middle of world generation. The
    /// composition root sets this for every start; embedders that only resolve
    /// or inspect a data directory leave it clear and keep the degraded paths.
    bool require_tracked_artifacts{false};
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
    /// Reports directory holding the climate, default-state, light and mining
    /// reports; the data directory's when present, else the installed content
    /// root's copied next to the datapack, else empty.
    std::string reports_root{};
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

/// Resolve the game data, fetching, generating or installing it when the data
/// directory does not hold a usable copy yet.
///
/// `transport` may be null only when the resolved directory already holds
/// usable data or the generation fallback can produce it; a fetch without a
/// transport fails with `unsupported`.
///
/// A directory is usable when its marker names this game version and the
/// datapack is present, or when a pre-staged `data/` tree with worldgen
/// documents exists and no marker claims otherwise. A marker for another game
/// version is stale: it is replaced when downloads are allowed and is a
/// `version_mismatch` error otherwise.
///
/// With `require_tracked_artifacts` the resolver returns `not_found` naming the
/// missing table container or climate report and the generation command when
/// no source produced them; without it the paths are returned with empty
/// artifact fields, matching the degraded runtime.
[[nodiscard]] auto ensure_game_data(const game_data_options &options,
                                    net::download_transport *transport,
                                    const game_data_limits &limits = {})
    -> result<game_data_paths>;

} // namespace refrain::server
