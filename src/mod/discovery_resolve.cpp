// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Composition resolution / dependency topology (split from discovery.cpp).

#include <refrain/mod/discovery.hpp>

#include "discovery_internal.hpp"

#include <refrain/core/semver.hpp>
#include <refrain/data/checksum.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
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
    return fmt::format("{}@{}(origin={}, path='{}')", id, version,
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
[[nodiscard]] auto topological_order(
    const mod::gamepack_spec &spec, std::span<const discovered_mod *> selected,
    const std::map<std::string, const discovered_mod *> &selected_by_id)
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
    for (const auto *mod : selected) {
        indegree.try_emplace(mod->id, 0);
        for (const auto &requirement : mod->required) {
            if (selected_by_id.find(requirement.id) == selected_by_id.end()) {
                continue; // a missing dependency is reported before ordering
            }
            dependents[requirement.id].push_back(mod->id);
            ++indegree[mod->id];
        }
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
        return std::unexpected{
            fail(error_code::mod_error,
                 fmt::format("resolve: dependency cycle detected among: {}",
                             cycle))};
    }
    return order;
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
    auto ordered_by_dependency{
        topological_order(spec, ordered, selected_by_id)};
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
    std::sort(lock.warnings.begin(), lock.warnings.end());
    const auto unique_end{
        std::unique(lock.warnings.begin(), lock.warnings.end())};
    lock.warnings.erase(unique_end, lock.warnings.end());

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
