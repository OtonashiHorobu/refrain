// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/mod/mod_ref.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::core::config {
class document;
} // namespace refrain::core::config

namespace refrain::mod {

// Shared manifest model: mod manifests, gamepack manifests and pack
// manifests all carry the same common section (id / version / side / abi) and
// the same `[meta]` display metadata. The gamepack-specific `[mods]`,
// `[providers]` and `[policy]` sections are parsed by
// `refrain/mod/gamepack.hpp` on top of this common layer.
//
// Section names per document kind:
//   * mod manifest:      [mod]      (file: mods/<id>/manifest.toml)
//   * gamepack manifest: [gamepack] (file: gamepack.toml)
//   * pack manifest:     [pack]     (reserved for pack.toml)
//
// Every declared version is SemVer 2.0.0; `abi` is the integer ABI major the
// code was built against. Internal/static mods omit an
// `[entry]` section: their code is linked into the host and paired with the
// manifest through the static mod registry (`mod/static_mod.hpp`). External
// mods declare exactly one of `[entry].native` / `[entry].wasm`.

/// Built-in / on-disk mod tree layout shared by every package source.
inline constexpr std::string_view mod_manifest_filename = "manifest.toml";
inline constexpr std::string_view builtin_image_filename = "builtin.image";
inline constexpr std::string_view builtin_index_filename = "mods.index.toml";

enum class manifest_side : std::uint8_t {
    server = 0,
    client = 1,
    both = 2,
};

[[nodiscard]] auto to_string(manifest_side side) noexcept -> std::string_view;

/// Parse `server` | `client` | `both`.
[[nodiscard]] auto parse_manifest_side(std::string_view text) noexcept
    -> std::optional<manifest_side>;

/// Host-side compatibility check: a `both` manifest runs anywhere; an empty,
/// `any` or `both` host side accepts everything.
[[nodiscard]] auto manifest_side_matches(manifest_side declared,
                                         std::string_view host_side) noexcept
    -> bool;

/// Optional display metadata shared by every manifest kind. It never affects
/// composition or resolution; launchers/UI read it.
struct manifest_meta {
    std::string display_name{};
    std::string description{};
    std::string icon{};
    std::string license{};  ///< SPDX expression (optional)
    std::string homepage{}; ///< optional URL
    std::vector<std::string> authors{};
};

/// The common section of a manifest (id/version/side/abi + meta).
struct manifest_common {
    std::string id{};
    std::string version{}; ///< SemVer 2.0.0 text, validated
    manifest_side side{manifest_side::both};
    std::uint32_t abi{0};
    manifest_meta meta{};
};

struct manifest_parse_options {
    /// When non-zero, the declared `abi` must equal this value (host runtime
    /// ABI gate). Zero accepts any positive ABI (tools/offline inspection).
    std::uint32_t expected_abi{0};
    /// Require an explicit `side` key (gamepack manifests do; mod manifests
    /// default to `both`).
    bool require_side{false};
    /// Require a non-empty `[meta].display_name` (shipped content does).
    bool require_display_name{false};
    /// Accept the `.rfmod` package tables a packed manifest carries:
    /// `[files]`, `[packaging]` and `[signature]`. Their *contents*
    /// are validated by the container layer (`refrain/mod/rfmod.hpp`) — the
    /// manifest parser only knows the TOML shape, because this layer may not
    /// depend on `data` (BLAKE3/Ed25519 live there). On-disk mod directories
    /// keep the default (`false`), so an accidentally committed packed manifest
    /// is rejected instead of silently treated as source.
    bool allow_package_tables{false};
};

/// Parse the common section of `doc` (`section` = "mod" | "gamepack" |
/// "pack"). Validates: section table, id, SemVer version,
/// integer/numeric-string ABI, side and `[meta]`.
[[nodiscard]] auto parse_manifest_common(
    const core::config::document &doc, std::string_view section,
    const manifest_parse_options &options = {}) -> result<manifest_common>;

/// Human-readable one-line identity, e.g. `refrain.data.vanilla@1.0.0
/// (side=server, abi=1)`.
[[nodiscard]] auto format_manifest_common(const manifest_common &common)
    -> std::string;

/// External package entry point. Internal/static mods have no entry section.
struct manifest_entry_point {
    /// Native shared library path relative to the package root (external
    /// native mods). Empty when unused.
    std::string native{};
    /// WASM module path relative to the package root (external script mods).
    /// Empty when unused.
    std::string wasm{};
    /// Optional exported symbol name for native entry points (reserved).
    std::string symbol{};
};

/// A parsed `mods/<id>/manifest.toml`.
struct mod_manifest {
    manifest_common common{};
    /// Hard dependencies (SemVer ranges). Named `required` because `requires`
    /// is a C++26 keyword; the manifest table stays `[requires]`.
    std::vector<mod_requirement> required{};
    /// Optional dependencies: absence is tolerated, presence is version-checked
    /// (manifest table `[optional_requires]`).
    std::vector<mod_requirement> optional{};
    /// Service contract ids this mod can provide, e.g. `refrain:data.packs`
    /// (a role declaration, not a mod type).
    std::vector<std::string> provides{};
    /// Optional host runtime API requirement (full SemVer, for example
    /// "1.0.0"). The loader rejects a major mismatch and a newer minor.
    std::string host_api{};
    /// Optional host runtime capabilities required by the mod (for example
    /// "log" or "config"). The loader rejects capabilities the host does not
    /// provide.
    std::vector<std::string> host_capabilities{};
    /// Present only for external (loadable) mods; absent for internal/static
    /// mods whose code is linked into the host.
    std::optional<manifest_entry_point> entry{};

    [[nodiscard]] auto is_external() const noexcept -> bool {
        return entry.has_value();
    }
};

/// Parse a mod manifest document. Root tables: `[mod] [meta] [requires]
/// [optional_requires] [provides] [entry]`; unknown root keys are rejected
/// unless `options.allow_package_tables` also admits the package
/// tables `[files] [packaging] [signature]` (contents are validated by the
/// container layer).
///
/// `options.expected_abi`, `require_side` and `require_display_name` are
/// forwarded to `parse_manifest_common`; `allow_package_tables` only widens the
/// root-key whitelist above.
///
/// `[mod]` may carry an optional integer `format_version` (`.rfmod` container
/// version). It is shape-checked (must be an integer) but not stored in
/// `mod_manifest`: the container layer owns its semantics.
[[nodiscard]] auto
parse_mod_manifest(const core::config::document &doc,
                   const manifest_parse_options &options = {})
    -> result<mod_manifest>;

/// Read + parse `mods/<id>/manifest.toml`.
[[nodiscard]] auto load_mod_manifest(std::string_view path)
    -> result<mod_manifest>;

/// Stable multi-line description used by `refrain-tools mods list` /
/// `manifest check` output.
[[nodiscard]] auto format_mod_manifest(const mod_manifest &manifest)
    -> std::string;

} // namespace refrain::mod
