// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/kernel/gamepack.hpp>
#include <refrain/kernel/mod_ref.hpp>
#include <refrain/mod/origin.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::mod {

// Mod auto-discovery + `mods.lock`, the disk half of the mod
// subsystem. This layer is pure manifest bookkeeping on top of
// `refrain/kernel` (gamepack/manifest primitives): it never loads code and
// never writes outside the lock file itself.
//
// Flow (per server/instance start):
//   1. the host builds one `discovery_root` per installed mod tree, tagging it
//      with a host-observed origin (never read from a manifest);
//   2. `scan()` walks `<root>/mods/<id>/manifest.toml`, parses every manifest,
//      hashes the raw bytes and reports the winning candidate per id plus the
//      shadowed (lower-priority) ones;
//   3. `resolve()` combines the gamepack's explicit `[mods].list` pins with the
//      auto-scanned candidates (only when `gamepack.auto_scan = true`),
//      enforces version/ABI/dependency/cycle rules and produces a `mod_lock`;
//   4. `write_mod_lock()`/`read_mod_lock()` persist and re-verify the lock,
//      `verify_mod_lock()` compares a stored lock against a fresh scan.
//
// Determinism and identity: origins are host-observed tiers
// `builtin < first_party < user`; winners are chosen by that fixed priority,
// results are sorted by id and the BLAKE3 `fingerprint()` over the canonical
// lock text (gamepack id@version + per-mod id/version/abi/origin/manifest hash)
// is the stable composition identity used by world metadata.
// Paths are host-local audit data and are deliberately not part of the
// fingerprint (a relocatable instance keeps the same world identity).
//
// Disk layout scanned by `scan()` (directory mods only):
//   <root>/mods/<id>/manifest.toml
// `.rfmod` archives (tar.zst + signed manifest) are installed by the `.rfmod`
// installer first and then appear as directory trees; scanning loose `.rfmod`
// files/other extensions/hidden-temp entries is intentionally out of scope
// here (unknown entries are skipped, or rejected with `strict_entries`).

/// Where a mod's files come from (the package source the host reads through).
/// Scanning produces `directory` candidates; the builtin image seeds
/// `builtin` candidates; `.rfmod` packages are installed as directory trees, so
/// `rfmod` names a reserved native source that no scanner produces yet.
enum class package_kind : std::uint8_t {
    builtin = 0,   ///< manifest/data live in the embedded RFSI image
    directory = 1, ///< `<root>/mods/<id>/` directory tree
    rfmod = 2,     ///< reserved: `.rfmod` archive member
};

[[nodiscard]] auto to_string(package_kind kind) noexcept -> std::string_view;

/// One installed mod tree to scan. `path` is the root that contains the
/// `mods/` directory (`<path>/mods/<id>/manifest.toml`), given as a host path;
/// '/' and '\\' separators are both accepted and the reported manifest paths
/// are normalized to '/'.
struct discovery_root {
    std::string path{};
    mod_origin origin{mod_origin::builtin};
};

struct discover_options {
    /// Reject (instead of skipping) entries under `mods/` that are not mod
    /// directories with a `manifest.toml` (files, empty dirs, `.rfmod` files).
    bool strict_entries{false};
    /// Reject a directory under `mods/` that has no `manifest.toml`, while
    /// still skipping top-level non-directory entries (README, `.gitkeep`).
    /// `refrain-tools mods check` uses this: a half-installed mod directory is
    /// a broken tree, but repository metadata files are not.
    bool require_manifest_in_directories{false};
    /// Reject a root without a `mods/` subdirectory instead of treating it as
    /// an empty tree (a root that itself cannot be listed is always an error).
    bool require_mods_dir{false};
    /// `resolve()`: a present optional dependency whose version does not match
    /// its declared range is a `version_mismatch`. When false,
    /// optional mismatches are ignored.
    bool check_optional_requirements{true};
};

/// A candidate that lost to the reported winner for the same id (same id
/// provided by a lower-priority origin, or a case-only variant of the id).
struct shadowed_mod {
    mod_origin origin{mod_origin::builtin};
    std::string version{};
    std::string path{};
    data::digest32 manifest_hash{};
};

/// One discovered mod. `version` is the manifest's SemVer text (validated by
/// the manifest parser); `path` is `<root>/mods/<id>/manifest.toml` with '/'
/// separators (a pseudo path for builtin-image candidates).
///
/// This is the single catalog record every composition consumer reads: `scan()`
/// fills it from disk, the builtin image seeds it from the loaded host, and
/// `resolve()` turns it into a `composition`. Fields mirror the manifest so a
/// caller (e.g. `refrain-tools mods check`) never needs a second parser.
struct candidate {
    std::string id{};
    std::string version{};
    std::uint32_t abi{0};
    mod_origin origin{mod_origin::builtin};
    kernel::manifest_side side{kernel::manifest_side::both};
    kernel::manifest_meta meta{};
    /// Manifest `[provides]` contract ids (the declared provider roles).
    std::vector<std::string> provides{};
    std::string path{};
    data::digest32 manifest_hash{};
    std::vector<kernel::mod_requirement> required{};
    std::vector<kernel::mod_requirement> optional{};
    /// Where the host reads the package from.
    package_kind kind{package_kind::directory};
    /// Present for external (loadable) mods, mirroring the manifest `[entry]`
    /// table. Internal/static mods have no entry.
    std::optional<kernel::manifest_entry_point> entry{};
    /// Lower-priority candidates for the same id that lost during `scan()`.
    /// `resolve()` decides whether that shadowing is a failure or a warning
    /// (gamepack `[policy].conflict`).
    std::vector<shadowed_mod> shadowed{};

    [[nodiscard]] auto is_external() const noexcept -> bool {
        return entry.has_value();
    }
};

/// Historical name of `candidate` (kept while callers migrate; new code uses
/// `candidate`).
using discovered_mod = candidate;

/// Scan every root and return one winning candidate per mod id, sorted by
/// (id, origin). Errors are values:
///  * `invalid_argument` for empty roots/options misuse;
///  * `io_error` for a missing/unreadable root or `mods/` directory
///    (`require_mods_dir`) and unreadable manifests;
///  * `parse_error` for malformed manifests or `<dir>` != manifest id;
///  * `already_exists` when the same id (case-insensitively) is installed
///    twice within one origin.
[[nodiscard]] auto scan(std::span<const discovery_root> roots,
                        const discover_options &options = {})
    -> result<std::vector<discovered_mod>>;

/// Scan one `<mods_dir>/<id>/manifest.toml` tree directly (the `--mods <dir>`
/// form used by `refrain-tools mods check`). Same single scanner as `scan()`:
/// only the root construction differs (`<mods_dir>` instead of
/// `<root>/mods`).
[[nodiscard]] auto scan_mods_tree(std::string_view mods_dir,
                                  mod_origin origin = mod_origin::builtin,
                                  const discover_options &options = {})
    -> result<std::vector<discovered_mod>>;

/// Stable per-mod audit text for one candidate. Byte-identical to
/// `kernel::format_mod_manifest()` for the same manifest, so `refrain-tools
/// mods check` (which goes through `scan()`) keeps its stable output
/// without re-reading the manifest.
[[nodiscard]] auto format_candidate(const candidate &mod) -> std::string;

// ---------------------------------------------------------------------------
// package file collection (single walker for builtin image / `.rfmod` pack)
// ---------------------------------------------------------------------------

/// One file of a mod package: '/'-normalized path relative to the mod root.
struct package_file {
    std::string path{};
    std::vector<std::byte> data{};
};

struct package_collect_options {
    /// Prefix used by diagnostics, e.g. "rfmod pack" or "builtin image".
    std::string_view error_prefix{"package"};
    /// When non-empty, only these top-level directories are walked (all other
    /// top-level entries are ignored except `extra_root_files`). Empty = walk
    /// the whole mod directory.
    std::span<const std::string_view> roots{};
    /// Top-level directories that are never shipped (e.g. `src/`: the mod's
    /// native code is linked into the host).
    std::span<const std::string_view> exclude_roots{};
    /// Extra top-level files to include when present (e.g. LICENSE/NOTICE).
    std::span<const std::string_view> extra_root_files{};
    std::size_t max_file_bytes{64ULL * 1024ULL * 1024ULL};
    std::size_t max_files{4096};
    /// Include `manifest.toml` in the result (the `.rfmod` writer rebuilds it,
    /// so packing passes `false`).
    bool include_manifest{true};
    /// Skip dotfiles/dot directories (VCS metadata, editor droppings).
    bool skip_dotfiles{true};
    /// Fail with `permission_denied` when a symlink is encountered instead of
    /// skipping it.
    bool reject_symlinks{true};
    /// Fail with `invalid_argument` for non-regular files (FIFO/device).
    bool reject_special_files{true};
};

/// Walk one mod directory and read every shippable file. This is the single
/// file collector used by `refrain-tools builtin image` and
/// `refrain-tools mods pack`; no other code walks a mod tree.
///
/// The result is sorted by path. Errors:
///  * `invalid_argument` for an empty directory, an unsafe relative path, a
///    file over `max_file_bytes`, too many files or a non-regular file;
///  * `permission_denied` for a symlink when `reject_symlinks` is set;
///  * `not_found` when `include_manifest` is set and `manifest.toml` is
///    missing;
///  * `io_error` for unreadable directories/files.
[[nodiscard]] auto
collect_package_files(std::string_view mod_directory,
                      const package_collect_options &options = {})
    -> result<std::vector<package_file>>;

/// Current `mods.lock` format version.
inline constexpr std::uint16_t mod_lock_format_version{1};

/// One resolved/active mod entry of `mods.lock` (canonical, sorted by id).
struct locked_mod {
    std::string id{};
    std::string version{}; ///< SemVer 2.0.0 text
    std::uint32_t abi{0};
    mod_origin origin{mod_origin::builtin};
    std::string path{}; ///< host-local manifest path (audit only)
    data::digest32 manifest_hash{};

    friend auto operator==(const locked_mod &, const locked_mod &)
        -> bool = default;
};

/// Deterministic resolution result written to `mods.lock`.
///
/// The fingerprint is BLAKE3 (lowercase hex) over `canonical_text()`; it is
/// the composition identity consumed by world metadata/handshake later. It
/// intentionally excludes host-local `path` values and warnings so that the
/// same mod composition resolved in a different instance directory yields the
/// same identity.
struct mod_lock {
    std::uint16_t format_version{mod_lock_format_version};
    std::string gamepack_id{};
    std::string gamepack_version{};
    /// Selected mods, sorted by id.
    std::vector<locked_mod> mods{};
    /// Resolution warnings (shadowed candidates, optional deps), audit only:
    /// emitted as comments in the TOML and not covered by the fingerprint.
    std::vector<std::string> warnings{};

    /// Canonical multi-line text hashed by `fingerprint()`:
    /// `refrain-mods-lock` header, format version, gamepack `id@version` and
    /// one `mod <id> <version> <abi> <origin> <manifest hash>` line per mod
    /// (id-sorted). Documented so external tools can recompute the identity.
    [[nodiscard]] auto canonical_text() const -> std::string;

    /// BLAKE3 over `canonical_text()`, lowercase hex (64 chars).
    [[nodiscard]] auto fingerprint() const -> std::string;

    /// Deterministic TOML serialization:
    ///   `[lock]` format_version / gamepack_id / gamepack_version / fingerprint
    ///   `[mods."<id>"]` version / abi / origin / path / manifest_hash
    /// Tables only (no array-of-tables); warnings become leading comments.
    /// `abi` is a TOML string (manifest convention) so ids containing dots can
    /// be read back through literal key paths.
    [[nodiscard]] auto to_toml() const -> std::string;

    friend auto operator==(const mod_lock &, const mod_lock &)
        -> bool = default;
};

/// One fully resolved composition: the single output of the mod pipeline
/// (`catalog → resolve → select → load → activate`).
///
/// It inherits `mod_lock` because the lock text is the deterministic identity
/// half of the composition (`canonical_text()`/`fingerprint()` are the stable
/// contract); the additional members are what the host needs to load
/// and activate providers.
struct composition : public mod_lock {
    /// Gamepack manifest the composition was resolved from.
    kernel::gamepack_spec spec{};
    /// Selected mods in deterministic topological order (dependencies first;
    /// gamepack `[mods].list` declaration order breaks ties for pinned mods,
    /// auto-scanned-only mods follow in id order).
    std::vector<candidate> load_order{};
    /// Provider selection (`kernel::select_providers`) for every `[providers]`
    /// key, sorted by service.
    std::vector<kernel::resolved_provider> providers{};
    /// True for a manifest-level preview (no package/disk verification): the
    /// gamepack's declared list only, used by `gamepack check|dump` and
    /// `--dump-gamepack`. Never fed to the host.
    bool preview{false};

    [[nodiscard]] auto provider(std::string_view service) const noexcept
        -> const kernel::resolved_provider *;
    [[nodiscard]] auto find(std::string_view id) const noexcept
        -> const candidate *;

    /// Stable human-readable plan, byte-identical to
    /// `kernel::startup_plan::format()`.
    [[nodiscard]] auto format() const -> std::string;
};

/// Resolve the gamepack composition against a scan result (the runtime path).
///
/// Explicit `[mods].list` pins are authoritative: every pinned id must be
/// discovered, its version must equal the pin exactly and its ABI must equal
/// `spec.abi`, otherwise `not_found`/`version_mismatch` is returned. Pins are
/// selected first; `spec.auto_scan = true` then adds every other discovered mod
/// (auto_scan = false keeps only the pins). Shadowed candidates are a hard
/// `already_exists` under `policy.conflict = "fail"`; under `"explicit"` the
/// higher origin wins and a warning is recorded when
/// `policy.allow_custom_override` is set, otherwise the override is refused.
/// The selected set must satisfy every manifest `[requires]` range (missing ->
/// `not_found`, out-of-range -> `version_mismatch`) and must be acyclic
/// (`mod_error`); `[optional_requires]` are version-checked when present and
/// tolerated when absent. Providers are selected with
/// `kernel::select_providers()`. The returned lock is id-sorted and ready for
/// `write_mod_lock()`; `load_order` is dependency-ordered for the host.
[[nodiscard]] auto
resolve(const kernel::gamepack_spec &spec,
        std::span<const candidate> discovered,
        const discover_options &options = {},
        std::span<const std::string_view> required_services = {})
    -> result<composition>;

/// Manifest-level preview: resolve the gamepack's own `[mods].list` and
/// `[providers]` without touching packages (the manifest-level
/// resolver). Used by `refrain-tools gamepack
/// check|dump` and `refrain-server --dump-gamepack`, which must work without a
/// built-in image. An empty `[mods].list` is rejected (`unsupported` when
/// auto_scan is on, `invalid_argument` otherwise).
[[nodiscard]] auto
preview_composition(const kernel::gamepack_spec &spec,
                    std::span<const std::string_view> required_services = {})
    -> result<composition>;

struct composition_load_options {
    /// Built-in RFSI image to seed origin=builtin candidates (server/tests have
    /// it embedded; tools may read it from a file). Empty = no builtin seed.
    std::span<const std::byte> builtin_image{};
    /// Host side checked by the gamepack parser ("server"/"client").
    std::string_view host_side{"server"};
    /// `[providers]` keys the host cannot start without.
    std::span<const std::string_view> required_services{};
    /// Discovery options for `scan()`.
    discover_options discovery{};
    /// Override `gamepack.auto_scan` (CLI `--auto-scan` / `--no-auto-scan`).
    /// Unset keeps the manifest value.
    std::optional<bool> auto_scan{};
};

/// Read `gamepack_path`, scan `roots`, seed the builtin candidates from
/// `options.builtin_image` and resolve everything into one composition. This is
/// the single composition entry point shared by `refrain-server` and
/// `refrain-tools` (no caller re-implements the pipeline).
[[nodiscard]] auto load_composition(
    std::string_view gamepack_path, std::span<const discovery_root> roots,
    const composition_load_options &options = {}) -> result<composition>;

/// `load_composition()` with an already-parsed gamepack spec (avoids a second
/// parse when the caller needed `auto_scan` before deciding whether to scan).
[[nodiscard]] auto resolve_composition(
    const kernel::gamepack_spec &spec, std::span<const discovery_root> roots,
    const composition_load_options &options = {}) -> result<composition>;

/// Enumerate the builtin image's mod manifests (`mods/<id>/manifest.toml`) as
/// origin=builtin candidates. The image is read-only and never copied.
[[nodiscard]] auto scan_builtin_image(std::span<const std::byte> image)
    -> result<std::vector<candidate>>;

/// Write `<path>` atomically (tmp + fsync + rename via core::io).
[[nodiscard]] auto write_mod_lock(std::string_view path, const mod_lock &lock)
    -> status;

/// Read + structurally validate a `mods.lock` and re-verify its fingerprint.
/// Errors: `io_error` for unreadable files, `version_mismatch` for an
/// unsupported `format_version`, `parse_error` for everything else (unknown
/// tables/keys, bad SemVer/origin/hash, tampered fingerprint).
/// Warnings are comments and are therefore empty after a round-trip.
[[nodiscard]] auto read_mod_lock(std::string_view path) -> result<mod_lock>;

/// Compare a stored lock against a fresh `scan()` result: ids, exact SemVer
/// versions, ABIs, origins and manifest hashes must match (paths are
/// host-local and may move). Drift is reported as `version_mismatch` with
/// detail; a duplicate id in `discovered` is `already_exists`.
[[nodiscard]] auto verify_mod_lock(const mod_lock &lock,
                                   std::span<const discovered_mod> discovered)
    -> status;

/// Stable multi-line audit dump (server startup log / `refrain-tools mods
/// list`): gamepack id@version, fingerprint and every mod with origin/hash.
[[nodiscard]] auto format_mod_lock(const mod_lock &lock) -> std::string;

} // namespace refrain::mod
