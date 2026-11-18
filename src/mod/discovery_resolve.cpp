// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Composition resolution / dependency topology (split from discovery.cpp).

#include <refrain/mod/discovery.hpp>

#include "discovery_internal.hpp"

#include <refrain/contracts/services/mod_loader.hpp>
#include <refrain/core/semver.hpp>
#include <refrain/data/checksum.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::mod {

namespace data = refrain::data;

namespace {

using detail::annotate;
using detail::fail;
using detail::lower_id;

// ---------------------------------------------------------------------------
// resolve()
// ---------------------------------------------------------------------------

/// Stable one-line description of a mod candidate for conflict diagnostics.
[[nodiscard]] auto describe_mod(std::string_view id, mod_origin origin,
                                std::string_view version, std::string_view path)
    -> std::string {
    return fmt::format("{}@{} (origin={}, path='{}')", id, version,
                       to_string(origin), path);
}

[[nodiscard]] auto shadowed_summary(const discovered_mod &mod) -> std::string {
    std::string detail;
    for (const auto &loser : mod.shadowed) {
        if (!detail.empty()) {
            detail += ", ";
        }
        detail += describe_mod(mod.id, loser.origin, loser.version, loser.path);
    }
    return detail;
}

/// Validate every requirement range + id of a discovered manifest up front.
[[nodiscard]] auto validate_requirements(const discovered_mod &mod) -> status {
    for (const auto *list : {&mod.required, &mod.optional}) {
        for (const auto &requirement : *list) {
            if (!mod::is_valid_mod_id(requirement.id)) {
                return std::unexpected{
                    fail(error_code::invalid_argument,
                         fmt::format("resolve: mod '{}' declares an invalid "
                                     "dependency id '{}'",
                                     mod.id, requirement.id))};
            }
            auto range{core::parse_range(requirement.range)};
            if (!range) {
                return std::unexpected{fail(
                    error_code::parse_error,
                    fmt::format("resolve: mod '{}' declares an invalid range "
                                "'{}' for '{}': {}",
                                mod.id, requirement.range, requirement.id,
                                range.error().message))};
            }
        }
    }
    return status_ok();
}

[[nodiscard]] auto check_requirement(const discovered_mod &owner,
                                     const mod::mod_requirement &requirement,
                                     const discovered_mod &target,
                                     bool optional) -> status {
    auto satisfied{core::satisfies(target.version, requirement.range)};
    if (!satisfied) {
        return std::unexpected{
            fail(error_code::parse_error,
                 fmt::format("resolve: {} dependency '{}' of '{}': {}",
                             optional ? "optional" : "required", requirement.id,
                             owner.id, satisfied.error().message))};
    }
    if (!*satisfied) {
        return std::unexpected{fail(
            error_code::version_mismatch,
            fmt::format("resolve: mod '{}' {} '{}' range '{}' but it resolves "
                        "to version '{}'",
                        owner.id, optional ? "optionally requires" : "requires",
                        requirement.id, requirement.range, target.version))};
    }
    return status_ok();
}

/// Deterministic topological order over the selected mods: dependencies first;
/// ties follow the gamepack `[mods].list` declaration order for pinned mods and
/// id order for auto-scanned-only mods. This is the single dependency-graph
/// implementation (`detect_cycle` was folded into it).
///
/// `extra_edges` are additional prerequisite -> dependent pairs from the
/// composition rules outside the manifest `[requires]` graph (a package that
/// names a mod-provided loader depends on the loader's provider mod).
[[nodiscard]] auto topological_order(
    const mod::gamepack_spec &spec, std::span<const discovered_mod *> selected,
    const std::map<std::string, const discovered_mod *> &selected_by_id,
    std::span<const std::pair<std::string, std::string>> extra_edges = {})
    -> result<std::vector<const discovered_mod *>> {
    std::map<std::string, std::size_t> rank;
    for (std::size_t index{0}; index < spec.mods.size(); ++index) {
        rank.emplace(spec.mods[index].ref.id, index);
    }
    const auto auto_scan_base{spec.mods.size()};
    std::vector<const discovered_mod *> by_id(selected.begin(), selected.end());
    std::sort(by_id.begin(), by_id.end(),
              [](const discovered_mod *lhs, const discovered_mod *rhs) {
                  return lhs->id < rhs->id;
              });
    for (std::size_t index{0}; index < by_id.size(); ++index) {
        rank.try_emplace(by_id[index]->id, auto_scan_base + index);
    }

    std::map<std::string, std::size_t> indegree;
    std::map<std::string, std::vector<std::string>> dependents;
    std::set<std::pair<std::string, std::string>> present_edges;
    for (const auto *mod : selected) {
        indegree.try_emplace(mod->id, 0);
        for (const auto &requirement : mod->required) {
            if (selected_by_id.find(requirement.id) == selected_by_id.end()) {
                continue; // a missing dependency is reported before ordering
            }
            dependents[requirement.id].push_back(mod->id);
            present_edges.emplace(requirement.id, mod->id);
            ++indegree[mod->id];
        }
    }
    for (const auto &[prerequisite, dependent] : extra_edges) {
        if (selected_by_id.find(prerequisite) == selected_by_id.end() ||
            selected_by_id.find(dependent) == selected_by_id.end()) {
            continue;
        }
        if (!present_edges.emplace(prerequisite, dependent).second) {
            continue;
        }
        dependents[prerequisite].push_back(dependent);
        ++indegree[dependent];
    }

    std::set<std::pair<std::size_t, std::string>> ready;
    for (const auto *mod : selected) {
        if (indegree[mod->id] == 0) {
            ready.emplace(rank[mod->id], mod->id);
        }
    }
    std::vector<const discovered_mod *> order;
    order.reserve(selected.size());
    while (!ready.empty()) {
        const auto [position, id] = *ready.begin();
        static_cast<void>(position);
        ready.erase(ready.begin());
        order.push_back(selected_by_id.at(id));
        for (const auto &dependent : dependents[id]) {
            if (--indegree[dependent] == 0) {
                ready.emplace(rank[dependent], dependent);
            }
        }
    }
    if (order.size() != selected.size()) {
        std::string cycle;
        for (const auto *mod : by_id) {
            if (indegree[mod->id] == 0) {
                continue;
            }
            if (!cycle.empty()) {
                cycle += ", ";
            }
            cycle += mod->id;
        }
        auto message{
            fmt::format("resolve: dependency cycle detected among: {}", cycle)};
        if (!extra_edges.empty()) {
            message +=
                "; a package that names a mod-provided loader depends on that "
                "loader's provider mod, so a provider whose dependency graph "
                "leads back to a package naming its loader cannot be ordered";
        }
        return std::unexpected{fail(error_code::mod_error, std::move(message))};
    }
    return order;
}

/// Resolution of the gamepack `[loaders]` declarations: the loader ids and
/// provider mods the plan publishes, plus the ordering edges they add to the
/// dependency graph.
struct loader_ordering {
    std::vector<resolved_loader> loaders{};
    /// prerequisite provider mod id -> dependent mod id
    std::vector<std::pair<std::string, std::string>> edges{};
};

/// `a, b` for diagnostics; `none` for an empty list.
[[nodiscard]] auto join_ids(const std::vector<std::string> &ids)
    -> std::string {
    std::string out;
    for (const auto &id : ids) {
        if (!out.empty()) {
            out += ", ";
        }
        out += id;
    }
    return out.empty() ? std::string{"none"} : out;
}

/// Resolve every `[loaders]` declaration against the selected mod set and
/// derive the loader ordering edges. A loader-providing mod must be selected,
/// satisfy the declared range, declare `refrain:mod.loader` in its
/// `[provides]` list and run through the host-owned `builtin` or `native`
/// loader; anything else fails closed before the plan is published.
///
/// A manifest-level preview has no manifest data, so the `[provides]` and
/// entry checks run only on a resolved catalog.
[[nodiscard]] auto resolve_loaders(
    const mod::gamepack_spec &spec,
    const std::map<std::string, const discovered_mod *> &selected_by_id,
    bool preview) -> result<loader_ordering> {
    loader_ordering resolution{};
    std::map<std::string, const discovered_mod *> providers{};
    std::vector<std::string> declared_ids{};
    for (const auto &declaration : spec.loaders) {
        const auto found{selected_by_id.find(declaration.provider.id)};
        if (found == selected_by_id.end()) {
            return std::unexpected{
                fail(error_code::not_found,
                     fmt::format(
                         "resolve: gamepack declares loader '{}' provided by "
                         "'{}' but that mod is not in the resolved set "
                         "(gamepack.auto_scan = {})",
                         declaration.id, declaration.provider.text(),
                         spec.auto_scan ? "true" : "false"))};
        }
        const auto *provider{found->second};
        const auto satisfied{
            core::satisfies(provider->version, declaration.provider.range)};
        if (!satisfied || !*satisfied) {
            return std::unexpected{fail(
                error_code::version_mismatch,
                fmt::format("resolve: gamepack declares loader '{}' provided "
                            "by '{}' but mod '{}' resolves to version '{}'",
                            declaration.id, declaration.provider.text(),
                            provider->id, provider->version))};
        }
        if (!preview) {
            if (!declares_service(provider->provides,
                                  refrain::services::mod_loader::service_id)) {
                return std::unexpected{fail(
                    error_code::permission_denied,
                    fmt::format("resolve: loader '{}' is declared with "
                                "provider mod '{}' but its manifest [provides] "
                                "does not declare '{}'",
                                declaration.id, provider->id,
                                refrain::services::mod_loader::service_id))};
            }
            const auto effective{provider->entry
                                     ? std::string_view{provider->entry->loader}
                                     : builtin_loader_id};
            if (!is_host_loader_id(effective)) {
                return std::unexpected{fail(
                    error_code::unsupported,
                    fmt::format(
                        "resolve: loader '{}' is provided by mod '{}', which "
                        "is loaded through loader '{}'; a loader-providing mod "
                        "must run through the host-owned builtin or native "
                        "loader, because a sandboxed guest can never become a "
                        "loader",
                        declaration.id, provider->id, effective))};
            }
        }
        providers.emplace(declaration.id, provider);
        declared_ids.push_back(declaration.id);
        resolution.loaders.push_back(resolved_loader{
            .id = declaration.id,
            .provider = fmt::format("{}@{}", provider->id, provider->version)});
    }
    std::sort(resolution.loaders.begin(), resolution.loaders.end(),
              [](const resolved_loader &lhs, const resolved_loader &rhs) {
                  return lhs.id < rhs.id;
              });

    // A package that names a declared package-provided loader loads only after
    // that loader's provider mod. A gamepack that declares loaders must
    // declare every package-provided loader a selected package names, so an
    // undeclared namespaced id fails here instead of at the first dispatch.
    for (const auto &[id, mod] : selected_by_id) {
        const auto effective{mod->entry ? std::string_view{mod->entry->loader}
                                        : builtin_loader_id};
        if (is_first_party_loader_id(effective)) {
            continue;
        }
        const auto provider{providers.find(std::string{effective})};
        if (provider == providers.end()) {
            if (spec.loaders.empty()) {
                continue; // embedder-registered loaders predate the plan table
            }
            return std::unexpected{fail(
                error_code::not_found,
                fmt::format("resolve: mod '{}' names loader '{}' but the "
                            "gamepack declares no provider for it; declared "
                            "loaders: {}",
                            id, effective, join_ids(declared_ids)))};
        }
        if (provider->second->id == mod->id) {
            return std::unexpected{fail(
                error_code::mod_error,
                fmt::format("resolve: mod '{}' provides loader '{}' and also "
                            "names it as its own loader; a loader-providing "
                            "mod runs through the host-owned builtin or "
                            "native loader",
                            mod->id, effective))};
        }
        resolution.edges.emplace_back(provider->second->id, mod->id);
    }
    std::sort(resolution.edges.begin(), resolution.edges.end());
    resolution.edges.erase(
        std::unique(resolution.edges.begin(), resolution.edges.end()),
        resolution.edges.end());
    return resolution;
}

/// Does the gamepack name `id` explicitly? A pin in `[mods].list`, a provider
/// candidate/instance/wrapper or a loader provider is the composition root's
/// decision; a mod that only auto-discovery selected is not.
[[nodiscard]] auto explicitly_declared(const mod::gamepack_spec &spec,
                                       std::string_view id) -> bool {
    if (spec.find_mod(id) != nullptr) {
        return true;
    }
    for (const auto &binding : spec.providers) {
        if (std::ranges::any_of(binding.candidates,
                                [id](const mod::mod_requirement &candidate) {
                                    return candidate.id == id;
                                })) {
            return true;
        }
        if (std::ranges::any_of(binding.instances,
                                [id](const mod::provider_instance &instance) {
                                    return instance.requirement.id == id;
                                })) {
            return true;
        }
        if (binding.base && binding.base->id == id) {
            return true;
        }
        if (std::ranges::any_of(binding.wrappers,
                                [id](const mod::mod_requirement &wrapper) {
                                    return wrapper.id == id;
                                })) {
            return true;
        }
    }
    return std::ranges::any_of(spec.loaders,
                               [id](const mod::loader_provider &loader) {
                                   return loader.provider.id == id;
                               });
}

/// Canonical identity of a `[provides]` entry relative to one gamepack service
/// slot: the slot itself for the contract that owns the slot (its short,
/// namespaced and dotted spellings), the full contract id for a member of the
/// slot's namespace. Two mods compete for the same contract only when these
/// identities intersect, so a mod that provides `refrain:world.migration`
/// does not collide with the `world` binding.
[[nodiscard]] auto canonical_slot_service(std::string_view provided,
                                          std::string_view slot)
    -> std::optional<std::string> {
    if (provided == slot || detail::equals_slot(provided, "refrain:", slot) ||
        detail::equals_slot(provided, "refrain:net.", slot) ||
        detail::equals_dotted_slot(provided, slot)) {
        return std::string{slot};
    }
    if (detail::in_slot_namespace(provided, "refrain:", slot)) {
        return std::string{provided};
    }
    return std::nullopt;
}

/// Contract identities a manifest claims within one gamepack slot.
[[nodiscard]] auto provided_slot_services(const discovered_mod &mod,
                                          std::string_view slot)
    -> std::set<std::string> {
    std::set<std::string> services{};
    for (const auto &entry : mod.provides) {
        if (auto canonical{canonical_slot_service(entry, slot)};
            canonical.has_value()) {
            services.insert(std::move(*canonical));
        }
    }
    return services;
}

/// Provider ids of one resolved binding (base/active plus chain wrappers).
[[nodiscard]] auto binding_provider_ids(const resolved_provider &binding)
    -> std::set<std::string> {
    std::set<std::string> ids{};
    const auto add{[&ids](std::string_view reference) {
        const auto at{reference.rfind('@')};
        const auto id{reference.substr(0, at)};
        if (!id.empty()) {
            ids.emplace(id);
        }
    }};
    add(binding.provider);
    for (const auto &wrapper : binding.wrappers) {
        add(wrapper);
    }
    return ids;
}

/// Fail closed when a discovery-only mod claims a provider slot the gamepack
/// already binds.
///
/// Auto-discovery must not silently change which provider answers a service: a
/// mod that was only selected by scanning is not allowed to compete with the
/// gamepack's binding. The composition root can still choose it by naming it
/// (a pin or a provider candidate), which turns the claim into a recorded
/// warning instead of a failure.
[[nodiscard]] auto check_provider_conflicts(
    const mod::gamepack_spec &spec,
    std::span<const discovered_mod *const> selected,
    const std::map<std::string, const discovered_mod *> &selected_by_id,
    std::span<const resolved_provider> providers,
    std::vector<std::string> &warnings) -> status {
    for (const auto &binding : providers) {
        // Instanced services activate several providers by design; there is
        // no single bound slot to compete for.
        if (binding.mode == provider_mode::instanced) {
            continue;
        }
        const auto active{binding_provider_ids(binding)};
        std::set<std::string> bound_services{};
        for (const auto &id : active) {
            const auto found{selected_by_id.find(id)};
            if (found == selected_by_id.end()) {
                continue;
            }
            for (const auto &service :
                 provided_slot_services(*found->second, binding.service)) {
                bound_services.insert(service);
            }
        }
        if (bound_services.empty()) {
            continue; // data-only binding: no contract provider is active
        }
        for (const auto *mod : selected) {
            if (active.contains(mod->id)) {
                continue;
            }
            const auto claimed{provided_slot_services(*mod, binding.service)};
            const auto competes{std::ranges::any_of(
                claimed, [&bound_services](const std::string &service) {
                    return bound_services.contains(service);
                })};
            if (!competes) {
                continue;
            }
            if (explicitly_declared(spec, mod->id)) {
                warnings.push_back(fmt::format(
                    "mod '{}' also provides service '{}', which the gamepack "
                    "binds to '{}'; the explicit gamepack declaration wins",
                    mod->id, binding.service, binding.provider));
                continue;
            }
            const auto base{
                binding.provider.substr(0, binding.provider.rfind('@'))};
            const auto bound{selected_by_id.find(base)};
            const auto candidate{bound == selected_by_id.end()
                                     ? describe_mod(mod->id, mod->origin,
                                                    mod->version, mod->path)
                                     : describe_mod(bound->second->id,
                                                    bound->second->origin,
                                                    bound->second->version,
                                                    bound->second->path)};
            return std::unexpected{fail(
                error_code::already_exists,
                fmt::format(
                    "resolve: discovered mod {} provides service '{}', "
                    "which the gamepack already binds to {}; pin it in "
                    "[mods].list and bind it under [providers].{} to "
                    "choose it, or uninstall it",
                    describe_mod(mod->id, mod->origin, mod->version, mod->path),
                    binding.service, candidate, binding.service))};
        }
    }
    return status_ok();
}

[[nodiscard]] auto
resolve_impl(const mod::gamepack_spec &spec,
             std::span<const discovered_mod> discovered,
             const discover_options &options,
             std::span<const std::string_view> required_services, bool preview)
    -> result<composition> {
    // --- gamepack spec gates ------------------------------------------------
    if (spec.id.empty() || !mod::is_valid_mod_id(spec.id)) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 fmt::format("resolve: invalid gamepack id '{}'", spec.id))};
    }
    if (!core::semver::parse(spec.version).has_value()) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 fmt::format("resolve: invalid gamepack version '{}'",
                             spec.version))};
    }
    if (spec.abi == 0U) {
        return std::unexpected{fail(error_code::invalid_argument,
                                    "resolve: gamepack abi must be non-zero")};
    }
    if (spec.policy.conflict != "fail" && spec.policy.conflict != "explicit") {
        return std::unexpected{fail(
            error_code::invalid_argument,
            fmt::format("resolve: invalid policy.conflict '{}' (expected fail "
                        "or explicit)",
                        spec.policy.conflict))};
    }
    // Origins are attached by the host while building the discovery roots, so
    // resolution can only enforce the canonical order. A custom order is
    // rejected instead of being silently ignored.
    if (!spec.policy.origin_priority.empty()) {
        std::vector<mod_origin> order;
        order.reserve(spec.policy.origin_priority.size());
        for (const auto &name : spec.policy.origin_priority) {
            const auto origin{parse_mod_origin(name)};
            if (!origin) {
                return std::unexpected{
                    fail(error_code::invalid_argument,
                         fmt::format("resolve: unknown origin '{}' in "
                                     "policy.origin_priority",
                                     name))};
            }
            if (std::find(order.begin(), order.end(), *origin) != order.end()) {
                return std::unexpected{
                    fail(error_code::invalid_argument,
                         fmt::format("resolve: duplicate origin '{}' in "
                                     "policy.origin_priority",
                                     name))};
            }
            order.push_back(*origin);
        }
        const std::array<mod_origin, 3> canonical{
            mod_origin::builtin, mod_origin::first_party, mod_origin::user};
        if (!std::equal(order.begin(), order.end(), canonical.begin(),
                        canonical.end())) {
            return std::unexpected{fail(
                error_code::unsupported,
                "resolve: custom policy.origin_priority is not supported; "
                "origins are assigned during discovery, and resolution "
                "enforces the canonical builtin < first_party < user tiers "
                "(user wins)")};
        }
    }

    // --- discovered candidates: validate + index --------------------------
    std::vector<const discovered_mod *> sorted;
    sorted.reserve(discovered.size());
    for (const auto &mod : discovered) {
        sorted.push_back(&mod);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const discovered_mod *lhs, const discovered_mod *rhs) {
                  return lhs->id < rhs->id;
              });

    std::map<std::string, const discovered_mod *> by_id;
    std::map<std::string, std::string> by_lower_id;
    for (const auto *mod : sorted) {
        if (mod->id.empty() || !mod::is_valid_mod_id(mod->id)) {
            return std::unexpected{fail(
                error_code::invalid_argument,
                fmt::format("resolve: discovered mod has an invalid id '{}'",
                            mod->id))};
        }
        if (mod->path.empty()) {
            return std::unexpected{fail(
                error_code::invalid_argument,
                fmt::format("resolve: discovered mod '{}' has an empty path",
                            mod->id))};
        }
        if (mod->abi == 0U) {
            return std::unexpected{
                fail(error_code::invalid_argument,
                     fmt::format("resolve: discovered mod '{}' has abi 0",
                                 mod->id))};
        }
        if (!core::semver::parse(mod->version).has_value()) {
            return std::unexpected{
                fail(error_code::invalid_argument,
                     fmt::format("resolve: discovered mod '{}' has an invalid "
                                 "version '{}'",
                                 mod->id, mod->version))};
        }
        if (auto valid{validate_requirements(*mod)}; !valid) {
            return std::unexpected{std::move(valid.error())};
        }
        if (by_id.find(mod->id) != by_id.end()) {
            return std::unexpected{fail(
                error_code::already_exists,
                fmt::format("resolve: discovered candidate '{}' appears more "
                            "than once (paths '{}' and '{}')",
                            mod->id, by_id[mod->id]->path, mod->path))};
        }
        const auto lower{lower_id(mod->id)};
        const auto clash{by_lower_id.find(lower)};
        if (clash != by_lower_id.end()) {
            return std::unexpected{fail(
                error_code::already_exists,
                fmt::format("resolve: ids '{}' and '{}' only differ by case "
                            "(portable filesystem conflict)",
                            clash->second, mod->id))};
        }
        by_id.emplace(mod->id, mod);
        by_lower_id.emplace(lower, mod->id);
    }

    // --- explicit pins + auto_scan selection ------------------------------
    std::vector<const discovered_mod *> selected;
    std::map<std::string, const discovered_mod *> selected_by_id;
    // The ABI gate applies to *every* selected candidate, whether it was
    // pinned explicitly or picked up by auto_scan; an auto_scan mod built for
    // another gamepack ABI must not slip into the lock.
    const auto check_abi = [&spec](const discovered_mod *mod) -> status {
        if (mod->abi != spec.abi) {
            return std::unexpected{
                fail(error_code::version_mismatch,
                     fmt::format("resolve: mod '{}' declares abi {} but the "
                                 "gamepack requires abi {}",
                                 mod->id, mod->abi, spec.abi))};
        }
        return status_ok();
    };
    for (const auto &entry : spec.mods) {
        const auto &pin{entry.ref};
        if (pin.id.empty() || !mod::is_valid_mod_id(pin.id)) {
            return std::unexpected{fail(
                error_code::invalid_argument,
                fmt::format("resolve: gamepack pins an invalid mod id '{}'",
                            pin.id))};
        }
        if (selected_by_id.find(pin.id) != selected_by_id.end()) {
            return std::unexpected{
                fail(error_code::already_exists,
                     fmt::format("resolve: mod '{}' is pinned more than once",
                                 pin.id))};
        }
        const auto pin_version{core::semver::parse(pin.version)};
        if (!pin_version) {
            return std::unexpected{
                fail(error_code::invalid_argument,
                     fmt::format("resolve: pin '{}@{}' is not a valid SemVer "
                                 "reference",
                                 pin.id, pin.version))};
        }
        const auto found{by_id.find(pin.id)};
        if (found == by_id.end()) {
            return std::unexpected{fail(
                error_code::not_found,
                fmt::format("resolve: pinned mod '{}@{}' was not discovered "
                            "under the supplied roots",
                            pin.id, pin.version))};
        }
        const auto *mod{found->second};
        const auto found_version{core::semver::parse(mod->version)};
        if (!found_version) {
            return std::unexpected{
                fail(error_code::parse_error,
                     fmt::format("resolve: discovered mod '{}' has an invalid "
                                 "version '{}'",
                                 mod->id, mod->version))};
        }
        if (!(*pin_version == *found_version)) {
            return std::unexpected{fail(
                error_code::version_mismatch,
                fmt::format("resolve: gamepack pins '{}@{}' but the installed "
                            "manifest at '{}' declares '{}'",
                            pin.id, pin.version, mod->path, mod->version))};
        }
        if (auto compatible{check_abi(mod)}; !compatible) {
            return std::unexpected{std::move(compatible.error())};
        }
        selected.push_back(mod);
        selected_by_id.emplace(mod->id, mod);
    }
    if (spec.auto_scan) {
        for (const auto *mod : sorted) {
            if (selected_by_id.find(mod->id) == selected_by_id.end()) {
                if (auto compatible{check_abi(mod)}; !compatible) {
                    return std::unexpected{std::move(compatible.error())};
                }
                selected.push_back(mod);
                selected_by_id.emplace(mod->id, mod);
            }
        }
    }
    std::sort(selected.begin(), selected.end(),
              [](const discovered_mod *lhs, const discovered_mod *rhs) {
                  return lhs->id < rhs->id;
              });

    // --- shadowing under the conflict policy ------------------------------
    composition lock{};
    lock.preview = preview;
    lock.spec = spec;
    lock.format_version = mod_lock_format_version;
    lock.gamepack_id = spec.id;
    lock.gamepack_version = spec.version;
    for (const auto *mod : selected) {
        if (mod->shadowed.empty()) {
            continue;
        }
        const auto detail{fmt::format(
            "mod '{}' is provided by origin '{}' ('{}') and shadowed by: {}",
            mod->id, to_string(mod->origin), mod->path,
            shadowed_summary(*mod))};
        if (spec.policy.conflict == "fail") {
            return std::unexpected{
                fail(error_code::already_exists,
                     "resolve: conflicting mod origins and policy.conflict = "
                     "fail: " +
                         detail)};
        }
        if (!spec.policy.allow_custom_override) {
            return std::unexpected{
                fail(error_code::already_exists,
                     "resolve: policy.allow_custom_override = false forbids "
                     "higher-priority mods from overriding: " +
                         detail)};
        }
        lock.warnings.push_back("selected mod from higher origin shadows "
                                "lower origin: " +
                                detail);
    }

    // --- dependency graph + cycle check -----------------------------------
    std::vector<const discovered_mod *> ordered{selected};
    for (const auto *mod : ordered) {
        for (const auto &requirement : mod->required) {
            const auto found{selected_by_id.find(requirement.id)};
            if (found == selected_by_id.end()) {
                return std::unexpected{
                    fail(error_code::not_found,
                         fmt::format("resolve: mod '{}' requires '{}' (range "
                                     "'{}') but it is not in the resolved set "
                                     "(gamepack.auto_scan = {})",
                                     mod->id, requirement.id, requirement.range,
                                     spec.auto_scan))};
            }
            if (auto checked{check_requirement(*mod, requirement,
                                               *found->second, false)};
                !checked) {
                return std::unexpected{std::move(checked.error())};
            }
        }
        for (const auto &requirement : mod->optional) {
            const auto found{selected_by_id.find(requirement.id)};
            if (found == selected_by_id.end()) {
                lock.warnings.push_back(fmt::format(
                    "optional dependency '{}' of '{}' is not installed",
                    requirement.id, mod->id));
                continue;
            }
            if (!options.check_optional_requirements) {
                continue;
            }
            if (auto checked{
                    check_requirement(*mod, requirement, *found->second, true)};
                !checked) {
                return std::unexpected{std::move(checked.error())};
            }
        }
    }
    // --- package-provided loaders -----------------------------------------
    // The gamepack `[loaders]` declarations are the trust selection for
    // package-provided loader instances. They add ordering edges to the
    // dependency graph and turn an unregistered namespaced loader reference
    // into a resolution failure.
    auto loaders{resolve_loaders(spec, selected_by_id, preview)};
    if (!loaders) {
        return std::unexpected{std::move(loaders.error())};
    }
    auto ordered_by_dependency{
        topological_order(spec, ordered, selected_by_id, loaders->edges)};
    if (!ordered_by_dependency) {
        return std::unexpected{std::move(ordered_by_dependency.error())};
    }

    // --- lock output -------------------------------------------------------
    lock.mods.reserve(ordered.size());
    for (const auto *mod : ordered) {
        lock.mods.push_back(locked_mod{
            .id = mod->id,
            .version = mod->version,
            .abi = mod->abi,
            .origin = mod->origin,
            .path = mod->path,
            .manifest_hash = mod->manifest_hash,
        });
    }
    std::sort(lock.mods.begin(), lock.mods.end(),
              [](const locked_mod &lhs, const locked_mod &rhs) {
                  return lhs.id < rhs.id;
              });

    // Topological load order for the host (the lock stays id-sorted).
    lock.load_order.reserve(ordered_by_dependency->size());
    for (const auto *mod : *ordered_by_dependency) {
        lock.load_order.push_back(*mod);
    }

    // Provider selection: the single pure selector in gamepack. It sees only
    // resolved identities, never the filesystem.
    std::vector<mod::resolved_mod_view> views;
    views.reserve(selected.size());
    for (const auto *mod : selected) {
        views.push_back(mod::resolved_mod_view{.id = mod->id,
                                               .version = mod->version,
                                               .abi = mod->abi,
                                               .provides = mod->provides});
    }
    auto providers{mod::select_providers(spec, views, required_services)};
    if (!providers) {
        return std::unexpected{std::move(providers.error())};
    }
    lock.providers = std::move(*providers);
    lock.loaders = std::move(loaders->loaders);

    // A discovered mod may declare a contract the gamepack already binds to a
    // provider. That is a composition decision, not a load detail: fail closed
    // naming both candidates unless the gamepack names the mod explicitly.
    if (auto conflicts{check_provider_conflicts(spec, selected, selected_by_id,
                                                lock.providers, lock.warnings)};
        !conflicts) {
        return std::unexpected{std::move(conflicts.error())};
    }

    // Warnings are audit text emitted as lock comments; sort them last so a
    // warning appended by the provider check lands in canonical order.
    std::sort(lock.warnings.begin(), lock.warnings.end());
    const auto unique_end{
        std::unique(lock.warnings.begin(), lock.warnings.end())};
    lock.warnings.erase(unique_end, lock.warnings.end());
    return lock;
}

// ---------------------------------------------------------------------------
// read_mod_lock()
// ---------------------------------------------------------------------------

} // namespace

namespace detail {

auto resolve_catalog(const mod::gamepack_spec &spec,
                     std::span<const discovered_mod> discovered,
                     const discover_options &options,
                     std::span<const std::string_view> required_services,
                     bool preview) -> result<composition> {
    return resolve_impl(spec, discovered, options, required_services, preview);
}

} // namespace detail

} // namespace refrain::mod
