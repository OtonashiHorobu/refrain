// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/mod/manifest.hpp>
#include <refrain/mod/mod_ref.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::core::config {
class document;
} // namespace refrain::core::config

namespace refrain::mod {

// Gamepack manifests: the composition root reads a `gamepack.toml`, validates
// it, resolves the mod dependency graph and selects providers per service.
// This module is pure manifest logic: it never loads a dynamic library or
// touches the filesystem beyond reading the document it is given.
//
// Terminology: the only distributable/loadable unit is a "mod"
// (`.rfmod`). A mod that implements a service contract can be selected as that
// service's "provider" (a role, not a package type). All declared versions are
// SemVer 2.0.0 values.
//
// The common manifest layer (id/version/side/abi + `[meta]`, shared with mod
// manifests) lives in `refrain/mod/manifest.hpp`; `mod_ref` and
// `mod_requirement` live in `refrain/mod/mod_ref.hpp`.

/// One entry of `[mods].list` plus its optional settings/dependency table
/// (`[mods."<id>"]`).
struct mod_entry {
    mod_ref ref{};
    /// Hard dependencies declared as `requires = [...]` in the settings table.
    std::vector<mod_requirement> dependencies{};
    /// Remaining settings table, normalized to TOML text (empty when the mod
    /// has no settings table). The loader hands this to the mod unchanged.
    std::string settings{};
};

/// Service composition mode. Declared by the contract; the gamepack
/// selects providers according to it.
enum class provider_mode : std::uint8_t {
    exclusive = 0, ///< exactly one active provider (default)
    instanced = 1, ///< several active providers addressed by instance name
    chain = 2,     ///< one base provider wrapped by ordered wrappers
};

[[nodiscard]] auto to_string(provider_mode mode) noexcept -> std::string_view;

/// `name = "id@range"` entry of an instanced provider binding.
struct provider_instance {
    std::string name{};
    mod_requirement requirement{};
};

/// Provider selection for one service.
///
/// * exclusive: ordered fallback candidates (first resolvable wins);
/// * instanced: named instances that are all activated simultaneously;
/// * chain: a base provider plus ordered wrappers (outermost last).
struct provider_binding {
    std::string service{};
    provider_mode mode{provider_mode::exclusive};
    std::vector<mod_requirement> candidates{};
    std::vector<provider_instance> instances{};
    /// instanced: name of the default instance (`default = "<name>"`); empty
    /// when the manifest does not declare one.
    std::string default_instance{};
    std::optional<mod_requirement> base{};
    std::vector<mod_requirement> wrappers{};
};

// Short-slot mapping: a gamepack names a service by its `[providers]` key
// ("data", "world", "protocol"), while a contract table or a factory passes the
// full contract id ("refrain:data.packs", "refrain:net.protocol"). These
// predicates are the single definition of that mapping: the host uses them to
// activate a planned provider, and the tools use them to check that a mod's
// `[provides]` list covers the slots its gamepack selects it for.

namespace detail {

/// Is `service` exactly `<prefix><slot>`?
[[nodiscard]] inline auto equals_slot(std::string_view service,
                                      std::string_view prefix,
                                      std::string_view slot) -> bool {
    return service.size() == prefix.size() + slot.size() &&
           service.starts_with(prefix) && service.substr(prefix.size()) == slot;
}

/// Is `service` a member of the `<prefix><slot>.` namespace?
[[nodiscard]] inline auto in_slot_namespace(std::string_view service,
                                            std::string_view prefix,
                                            std::string_view slot) -> bool {
    return service.size() > prefix.size() + slot.size() &&
           service.starts_with(prefix) &&
           service.substr(prefix.size(), slot.size()) == slot &&
           service[prefix.size() + slot.size()] == '.';
}

/// Compare `refrain:<slot with dots>` against an underscore slot, so
/// "refrain:session.status" activates the gamepack slot "session_status".
[[nodiscard]] inline auto equals_dotted_slot(std::string_view service,
                                             std::string_view slot) -> bool {
    constexpr std::string_view prefix{"refrain:"};
    if (!service.starts_with(prefix)) {
        return false;
    }
    const auto rest{service.substr(prefix.size())};
    if (rest.size() != slot.size()) {
        return false;
    }
    for (std::size_t index{0}; index < rest.size(); ++index) {
        const auto character{rest[index] == '.' ? '_' : rest[index]};
        if (character != slot[index]) {
            return false;
        }
    }
    return true;
}

} // namespace detail

/// Does the contract id `service` belong to the gamepack service slot `slot`?
///
/// A service matches a slot when it is the slot itself, `refrain:<slot>`,
/// `refrain:net.<slot>`, a member of the `refrain:<slot>.` namespace, or the
/// dotted spelling of `refrain:<slot>` whose dots are underscores in the slot
/// (`refrain:net.session` matches `net_session`). Anything else is a different
/// service.
[[nodiscard]] inline auto matches_slot(std::string_view service,
                                       std::string_view slot) -> bool {
    if (service.empty() || slot.empty()) {
        return false;
    }
    constexpr std::string_view namespace_prefix{"refrain:"};
    constexpr std::string_view net_prefix{"refrain:net."};
    return service == slot ||
           detail::equals_slot(service, namespace_prefix, slot) ||
           detail::equals_slot(service, net_prefix, slot) ||
           detail::in_slot_namespace(service, namespace_prefix, slot) ||
           detail::equals_dotted_slot(service, slot);
}

/// Does the manifest `[provides]` list cover `service`? Exact id match, or the
/// short-slot rule applied to a declared entry (a mod may declare "data"
/// instead of "refrain:data.packs").
[[nodiscard]] inline auto
declares_service(std::span<const std::string> provides,
                 std::string_view service) -> bool {
    return std::ranges::any_of(
        provides, [service](const std::string &declared) {
            return declared == service || matches_slot(service, declared);
        });
}

/// Display metadata for launcher/UI (never used by resolution). The shape is
/// shared with mod manifests.
using gamepack_meta = manifest_meta;

/// Auto-discovery policy. Origin tiers come from the host-observed
/// install location, never from a mod manifest.
struct discovery_policy {
    /// Tier order written from lowest to highest priority: the last entry wins,
    /// so the default is `user > first_party > builtin`. Origins are attached
    /// during discovery; resolution accepts only this canonical order, so the
    /// list documents the policy instead of re-ranking discovery roots.
    std::vector<std::string> origin_priority{"builtin", "first_party", "user"};
    bool allow_custom_override{true};
    std::string conflict{"fail"}; ///< "fail" | "explicit"
};

/// Validated gamepack manifest.
struct gamepack_spec {
    std::string id{};
    std::string version{};
    std::string side{}; ///< "server" | "client" | "both"
    std::uint32_t abi{0};
    gamepack_meta meta{};
    /// Scan mod directories to complete the composition. Discovery is
    /// implemented by `refrain::mod::scan`; this layer only stores the policy
    /// and never touches the filesystem.
    bool auto_scan{true};
    std::vector<std::string> mods_dir{"mods"};
    discovery_policy policy{};
    std::vector<mod_entry> mods{};             ///< declaration order
    std::vector<provider_binding> providers{}; ///< sorted by service key

    [[nodiscard]] auto find_mod(std::string_view id) const noexcept
        -> const mod_entry *;
};

struct resolved_instance {
    std::string name{};
    std::string provider{};
};

/// Selected provider(s) for one service after resolution.
struct resolved_provider {
    std::string service{};
    provider_mode mode{provider_mode::exclusive};
    /// exclusive or chain base: canonical `id@version`.
    std::string provider{};
    /// instanced: all active instances, sorted by instance name.
    std::vector<resolved_instance> instances{};
    /// instanced: the instance marked `default` in the manifest (empty when
    /// none was declared).
    std::string default_instance{};
    /// chain: wrappers, outermost last (same order as the manifest).
    std::vector<std::string> wrappers{};
    /// exclusive: earlier candidates that did not resolve, in declaration
    /// order.
    std::vector<std::string> skipped{};
};

/// Validate a parsed gamepack document. `host_side` ("server"/"client") is
/// checked against `gamepack.side`; pass an empty string to skip the check.
[[nodiscard]] auto parse_gamepack(const core::config::document &doc,
                                  std::string_view host_side = {})
    -> result<gamepack_spec>;

/// Read-only view of one already-resolved mod, used by the pure provider
/// selector. This layer never reads a package or the filesystem: the caller
/// (mod::resolve / mod::preview_composition) owns discovery and hands over the
/// resolved identities (`mod` does not build the dependency graph).
struct resolved_mod_view {
    std::string_view id{};
    std::string_view version{};
    std::uint32_t abi{0};
    std::span<const std::string> provides{};
};

/// Pure provider selection over an already-resolved mod set: the single
/// provider selector, with no dependency resolution inside.
///
/// Semantics per binding mode:
///  * exclusive: the first candidate that is resolved and satisfies its range
///    wins; earlier candidates are recorded in `skipped`;
///  * instanced: every named instance must resolve and satisfy its range;
///  * chain: base plus ordered wrappers, all strictly resolved.
/// `required_services` lists `[providers]` keys the host cannot start without.
///
/// Errors: `not_found` (no exclusive candidate resolves / an instanced or
/// chain requirement is missing / a required service has no provider),
/// `version_mismatch` (a strict requirement resolves to an out-of-range
/// version), `invalid_argument` (empty instanced list, chain without base).
[[nodiscard]] auto
select_providers(const gamepack_spec &spec,
                 std::span<const resolved_mod_view> resolved,
                 std::span<const std::string_view> required_services = {})
    -> result<std::vector<resolved_provider>>;

/// Read + validate a `gamepack.toml` without resolving it. Errors carry the
/// path. Callers that need a plan use `mod::preview_composition()` (manifest
/// level) or `mod::load_composition()` (catalog + lock, the runtime path).
[[nodiscard]] auto load_gamepack_spec(std::string_view path,
                                      std::string_view host_side = {})
    -> result<gamepack_spec>;

} // namespace refrain::mod
