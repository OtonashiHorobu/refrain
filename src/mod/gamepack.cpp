// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/mod/gamepack.hpp>

#include <refrain/contracts/abi.h>
#include <refrain/core/config.hpp>
#include <refrain/core/semver.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>

namespace refrain::mod {

namespace {

namespace config = core::config;

// ---------------------------------------------------------------------------
// Validation helpers
// ---------------------------------------------------------------------------

constexpr std::size_t k_max_service_key_length{64};
constexpr std::size_t k_max_instance_name_length{64};
constexpr std::size_t k_max_mod_count{4096};

/// Provider service keys: manifest keys such as `world`, `worldgen`,
/// `net_protocol`. Lowercase, 1..64 chars.
[[nodiscard]] auto valid_service_key(std::string_view key) noexcept -> bool {
    if (key.empty() || key.size() > k_max_service_key_length) {
        return false;
    }
    const auto first = key.front();
    if (first < 'a' || first > 'z') {
        return false;
    }
    return std::all_of(key.begin() + 1, key.end(), [](char value) {
        return (value >= 'a' && value <= 'z') ||
               (value >= '0' && value <= '9') || value == '_';
    });
}

/// Instance names used by instanced provider bindings: lowercase
/// letters, digits and underscores, 1..64 chars.
[[nodiscard]] auto valid_instance_name(std::string_view name) noexcept -> bool {
    if (name.empty() || name.size() > k_max_instance_name_length) {
        return false;
    }
    const auto first = name.front();
    if (first < 'a' || first > 'z') {
        return false;
    }
    return std::all_of(name.begin() + 1, name.end(), [](char value) {
        return (value >= 'a' && value <= 'z') ||
               (value >= '0' && value <= '9') || value == '_';
    });
}

[[nodiscard]] auto fail(error_code code, std::string message) -> error {
    return make_error(code, std::move(message));
}

// ---------------------------------------------------------------------------
// Document parsing helpers
// ---------------------------------------------------------------------------

[[nodiscard]] auto get_required_string(const config::document &doc,
                                       std::span<const std::string_view> path,
                                       std::string_view what)
    -> result<std::string> {
    auto value{doc.get_string_at(path)};
    if (!value) {
        if (value.error().code == error_code::not_found) {
            return std::unexpected{fail(
                error_code::parse_error,
                fmt::format("gamepack: missing required field '{}'", what))};
        }
        return std::unexpected{value.error()};
    }
    return value;
}

[[nodiscard]] auto parse_string_array(const config::document &doc,
                                      std::span<const std::string_view> path,
                                      std::string_view what)
    -> result<std::vector<std::string>> {
    auto values{doc.get_strings_at(path)};
    if (!values) {
        if (values.error().code == error_code::not_found) {
            return std::unexpected{fail(
                error_code::parse_error,
                fmt::format("gamepack: missing required array '{}'", what))};
        }
        return std::unexpected{values.error()};
    }
    return values;
}

[[nodiscard]] auto parse_optional_bool(const config::document &doc,
                                       std::span<const std::string_view> path,
                                       std::string_view what, bool fallback)
    -> result<bool> {
    const auto kind{doc.kind_at(path)};
    if (kind == config::value_kind::missing) {
        return fallback;
    }
    if (kind != config::value_kind::boolean) {
        return std::unexpected{
            fail(error_code::parse_error,
                 fmt::format("gamepack: '{}' must be a boolean", what))};
    }
    return doc.get_bool(std::string{what}, fallback);
}

[[nodiscard]] auto parse_gamepack_section(const config::document &doc,
                                          std::string_view host_side)
    -> result<gamepack_spec> {
    // Identity/version/side/abi + [meta] are the shared manifest
    // layer. The gamepack gates the runtime ABI and requires an explicit side
    // (the loader compares declared mod ABIs at load time).
    constexpr std::array<std::string_view, 6> gamepack_keys{
        "id", "version", "side", "abi", "auto_scan", "mods_dir"};
    for (const auto &key : doc.keys("gamepack")) {
        const auto known = std::any_of(
            gamepack_keys.begin(), gamepack_keys.end(),
            [&key](std::string_view candidate) { return candidate == key; });
        if (!known) {
            return std::unexpected{fail(
                error_code::invalid_argument,
                fmt::format("gamepack: unknown [gamepack] key '{}'", key))};
        }
    }

    auto common{parse_manifest_common(
        doc, "gamepack",
        manifest_parse_options{.expected_abi = REFRAIN_ABI_MAJOR,
                               .require_side = true})};
    if (!common) {
        return std::unexpected{common.error()};
    }
    if (!manifest_side_matches(common->side, host_side)) {
        return std::unexpected{
            fail(error_code::version_mismatch,
                 fmt::format("gamepack: side '{}' cannot run on a '{}' host",
                             to_string(common->side), host_side))};
    }

    gamepack_spec spec{};
    spec.id = std::move(common->id);
    spec.version = std::move(common->version);
    spec.side = std::string{to_string(common->side)};
    spec.abi = common->abi;
    spec.meta = std::move(common->meta);

    // Auto-discovery policy (discovery itself lives in mod::scan).
    auto auto_scan{parse_optional_bool(
        doc, std::array<std::string_view, 2>{"gamepack", "auto_scan"},
        "gamepack.auto_scan", true)};
    if (!auto_scan) {
        return std::unexpected{auto_scan.error()};
    }
    spec.auto_scan = *auto_scan;

    // Mod search directories are host-relative; a single string is accepted as
    // a one-element list for convenience.
    const std::array<std::string_view, 2> mods_dir_path{"gamepack", "mods_dir"};
    switch (doc.kind_at(mods_dir_path)) {
    case config::value_kind::missing:
        break;
    case config::value_kind::string:
        spec.mods_dir = {
            doc.get_string_at(mods_dir_path).value_or(std::string{})};
        break;
    case config::value_kind::array: {
        auto dirs{doc.get_strings_at(mods_dir_path)};
        if (!dirs) {
            return std::unexpected{dirs.error()};
        }
        spec.mods_dir = std::move(*dirs);
        break;
    }
    default:
        return std::unexpected{
            fail(error_code::parse_error,
                 "gamepack: gamepack.mods_dir must be a string or an array of "
                 "strings")};
    }
    if (std::any_of(spec.mods_dir.begin(), spec.mods_dir.end(),
                    [](const std::string &dir) { return dir.empty(); })) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "gamepack: gamepack.mods_dir entries must not be empty")};
    }
    return spec;
}

[[nodiscard]] auto parse_policy(const config::document &doc)
    -> result<discovery_policy> {
    discovery_policy policy{};
    constexpr std::array<std::string_view, 1> policy_path{"policy"};
    if (doc.kind_at(policy_path) == config::value_kind::missing) {
        return policy;
    }
    if (doc.kind_at(policy_path) != config::value_kind::table) {
        return std::unexpected{fail(error_code::parse_error,
                                    "gamepack: [policy] must be a table")};
    }

    constexpr std::array<std::string_view, 3> policy_keys{
        "origin_priority", "allow_custom_override", "conflict"};
    for (const auto &key : doc.keys("policy")) {
        const auto known = std::any_of(
            policy_keys.begin(), policy_keys.end(),
            [&key](std::string_view candidate) { return candidate == key; });
        if (!known) {
            return std::unexpected{
                fail(error_code::invalid_argument,
                     fmt::format("gamepack: unknown [policy] key '{}'", key))};
        }
    }

    constexpr std::array<std::string_view, 2> origin_path{"policy",
                                                          "origin_priority"};
    if (doc.kind_at(origin_path) != config::value_kind::missing) {
        auto origins{doc.get_strings_at(origin_path)};
        if (!origins) {
            return std::unexpected{origins.error()};
        }
        if (origins->empty()) {
            return std::unexpected{
                fail(error_code::invalid_argument,
                     "gamepack: policy.origin_priority must not be empty")};
        }
        std::vector<std::string> seen{};
        for (const auto &origin : *origins) {
            if (origin.empty()) {
                return std::unexpected{
                    fail(error_code::invalid_argument,
                         "gamepack: policy.origin_priority entries must not be "
                         "empty")};
            }
            if (std::find(seen.begin(), seen.end(), origin) != seen.end()) {
                return std::unexpected{
                    fail(error_code::invalid_argument,
                         fmt::format("gamepack: duplicate policy origin '{}'",
                                     origin))};
            }
            seen.push_back(origin);
        }
        policy.origin_priority = std::move(*origins);
    }

    auto allow_custom_override{parse_optional_bool(
        doc, std::array<std::string_view, 2>{"policy", "allow_custom_override"},
        "policy.allow_custom_override", true)};
    if (!allow_custom_override) {
        return std::unexpected{allow_custom_override.error()};
    }
    policy.allow_custom_override = *allow_custom_override;

    if (doc.contains_at(
            std::array<std::string_view, 2>{"policy", "conflict"})) {
        auto conflict{get_required_string(
            doc, std::array<std::string_view, 2>{"policy", "conflict"},
            "policy.conflict")};
        if (!conflict) {
            return std::unexpected{conflict.error()};
        }
        if (*conflict != "fail" && *conflict != "explicit") {
            return std::unexpected{
                fail(error_code::invalid_argument,
                     fmt::format("gamepack: invalid policy.conflict '{}' "
                                 "(expected fail or explicit)",
                                 *conflict))};
        }
        policy.conflict = std::move(*conflict);
    }
    return policy;
}

[[nodiscard]] auto parse_ref_list(std::string_view what,
                                  std::span<const std::string> texts)
    -> result<std::vector<mod_ref>> {
    auto refs{std::vector<mod_ref>{}};
    refs.reserve(texts.size());
    for (const auto &text : texts) {
        auto ref{parse_mod_ref(text)};
        if (!ref) {
            return std::unexpected{fail(
                error_code::invalid_argument,
                fmt::format("gamepack: {}: {}", what, ref.error().message))};
        }
        const auto duplicate =
            std::find_if(refs.begin(), refs.end(),
                         [&ref](const mod_ref &x) { return x.id == ref->id; });
        if (duplicate != refs.end()) {
            return std::unexpected{
                fail(error_code::already_exists,
                     fmt::format("gamepack: {} lists mod '{}' more than once",
                                 what, ref->id))};
        }
        refs.push_back(std::move(*ref));
    }
    return refs;
}

[[nodiscard]] auto parse_requirement_list(std::string_view what,
                                          std::span<const std::string> texts)
    -> result<std::vector<mod_requirement>> {
    auto requirements{std::vector<mod_requirement>{}};
    requirements.reserve(texts.size());
    for (const auto &text : texts) {
        auto requirement{parse_mod_requirement(text)};
        if (!requirement) {
            return std::unexpected{
                fail(error_code::invalid_argument,
                     fmt::format("gamepack: {}: {}", what,
                                 requirement.error().message))};
        }
        const auto duplicate =
            std::find_if(requirements.begin(), requirements.end(),
                         [&requirement](const mod_requirement &x) {
                             return x.id == requirement->id;
                         });
        if (duplicate != requirements.end()) {
            return std::unexpected{
                fail(error_code::already_exists,
                     fmt::format("gamepack: {} lists '{}' more than once", what,
                                 requirement->id))};
        }
        requirements.push_back(std::move(*requirement));
    }
    return requirements;
}

[[nodiscard]] auto validate_root_keys(const config::document &doc) -> status {
    constexpr std::array<std::string_view, 5> allowed{
        "gamepack", "meta", "mods", "policy", "providers"};
    for (const auto &key : doc.keys("")) {
        const auto known = std::any_of(
            allowed.begin(), allowed.end(),
            [&key](std::string_view candidate) { return candidate == key; });
        if (!known) {
            return std::unexpected{fail(
                error_code::invalid_argument,
                fmt::format("gamepack: unknown top-level table '{}'", key))};
        }
    }
    return status_ok();
}

[[nodiscard]] auto parse_mod_section(const config::document &doc,
                                     gamepack_spec &spec) -> status {
    constexpr std::array<std::string_view, 2> list_path{"mods", "list"};
    std::vector<std::string> listed{};
    if (doc.contains_at(list_path)) {
        auto list{parse_string_array(doc, list_path, "mods.list")};
        if (!list) {
            return std::unexpected{list.error()};
        }
        listed = std::move(*list);
        if (listed.size() > k_max_mod_count) {
            return std::unexpected{
                fail(error_code::invalid_argument,
                     fmt::format("gamepack: mods.list has {} entries (max {})",
                                 listed.size(), k_max_mod_count))};
        }
    }
    auto refs{parse_ref_list("mods.list", listed)};
    if (!refs) {
        return std::unexpected{refs.error()};
    }
    spec.mods.clear();
    spec.mods.reserve(refs->size());
    for (auto &ref : *refs) {
        spec.mods.push_back(mod_entry{.ref = std::move(ref)});
    }

    // `[mods."<id>"]` settings/dependency tables (literal keys: mod ids
    // contain dots and must not be treated as toml paths).
    for (const auto &key : doc.keys("mods")) {
        if (key == "list") {
            continue;
        }
        const auto mod = std::find_if(
            spec.mods.begin(), spec.mods.end(), [&key](const mod_entry &entry) {
                return std::string_view{entry.ref.id} == key;
            });
        if (mod == spec.mods.end()) {
            return std::unexpected{
                fail(error_code::invalid_argument,
                     fmt::format("gamepack: [mods.\"{}\"] is not listed in "
                                 "mods.list",
                                 key))};
        }

        const std::array<std::string_view, 2> base_path{"mods", key};
        if (doc.kind_at(base_path) != config::value_kind::table) {
            return std::unexpected{fail(
                error_code::parse_error,
                fmt::format("gamepack: [mods.\"{}\"] must be a table", key))};
        }
        mod->settings = doc.dump_table_at(
            base_path, std::array<std::string_view, 1>{"requires"});

        const std::array<std::string_view, 3> requires_path{"mods", key,
                                                            "requires"};
        if (!doc.contains_at(requires_path)) {
            continue;
        }
        auto requires_texts{doc.get_strings_at(requires_path)};
        if (!requires_texts) {
            return std::unexpected{requires_texts.error()};
        }
        auto required{parse_requirement_list(
            fmt::format("mods.\"{}\".requires", key), *requires_texts)};
        if (!required) {
            return std::unexpected{required.error()};
        }
        mod->dependencies = std::move(*required);
    }
    return status_ok();
}

[[nodiscard]] auto parse_provider_value(const config::document &doc,
                                        std::string_view service,
                                        provider_binding &binding) -> status {
    const std::array<std::string_view, 2> path{"providers", service};
    const auto kind{doc.kind_at(path)};
    switch (kind) {
    case config::value_kind::string: {
        auto text{doc.get_string_at(path)};
        if (!text) {
            return std::unexpected{text.error()};
        }
        if (text->starts_with("@")) {
            return std::unexpected{
                fail(error_code::unsupported,
                     fmt::format("gamepack: providers.{} uses '{}'; automatic "
                                 "provider discovery lands with the mod "
                                 "loader",
                                 service, *text))};
        }
        auto requirement{parse_mod_requirement(*text)};
        if (!requirement) {
            return std::unexpected{
                fail(error_code::invalid_argument,
                     fmt::format("gamepack: providers.{}: {}", service,
                                 requirement.error().message))};
        }
        binding.mode = provider_mode::exclusive;
        binding.candidates.push_back(std::move(*requirement));
        return status_ok();
    }
    case config::value_kind::array: {
        auto values{doc.get_strings_at(path)};
        if (!values) {
            return std::unexpected{values.error()};
        }
        if (values->empty()) {
            return std::unexpected{
                fail(error_code::invalid_argument,
                     fmt::format("gamepack: providers.{} has no candidates",
                                 service))};
        }
        // Ordered fallback candidates may repeat the same mod id with
        // different ranges (e.g. prefer ^2.0.0, fall back to ^1.0.0), so
        // only exact id@range duplicates are rejected here.
        binding.mode = provider_mode::exclusive;
        for (const auto &candidate_text : *values) {
            auto requirement{parse_mod_requirement(candidate_text)};
            if (!requirement) {
                return std::unexpected{
                    fail(error_code::invalid_argument,
                         fmt::format("gamepack: providers.{}: {}", service,
                                     requirement.error().message))};
            }
            const auto duplicate = std::find_if(
                binding.candidates.begin(), binding.candidates.end(),
                [&requirement](const mod_requirement &existing) {
                    return existing.id == requirement->id &&
                           existing.range == requirement->range;
                });
            if (duplicate != binding.candidates.end()) {
                return std::unexpected{
                    fail(error_code::already_exists,
                         fmt::format("gamepack: providers.{} lists '{}' twice",
                                     service, requirement->text()))};
            }
            binding.candidates.push_back(std::move(*requirement));
        }
        return status_ok();
    }
    case config::value_kind::table:
        break;
    default:
        return std::unexpected{fail(
            error_code::parse_error,
            fmt::format("gamepack: providers.{} must be a string, an array "
                        "of strings or a provider table",
                        service))};
    }

    constexpr std::array<std::string_view, 4> provider_keys{
        "instances", "default", "base", "wrappers"};
    const auto provider_table{fmt::format("providers.{}", service)};
    for (const auto &key : doc.keys(provider_table)) {
        const auto known = std::any_of(
            provider_keys.begin(), provider_keys.end(),
            [&key](std::string_view candidate) { return candidate == key; });
        if (!known) {
            return std::unexpected{
                fail(error_code::invalid_argument,
                     fmt::format("gamepack: unknown {} key '{}'",
                                 provider_table, key))};
        }
    }

    const std::array<std::string_view, 3> instances_path{"providers", service,
                                                         "instances"};
    const std::array<std::string_view, 3> base_path{"providers", service,
                                                    "base"};
    if (doc.contains_at(instances_path) && doc.contains_at(base_path)) {
        return std::unexpected{fail(
            error_code::invalid_argument,
            fmt::format("gamepack: {} declares both 'instances' and 'base' "
                        "(ambiguous composition mode)",
                        provider_table))};
    }
    if (doc.contains_at(instances_path)) {
        if (doc.kind_at(instances_path) != config::value_kind::table) {
            return std::unexpected{fail(
                error_code::parse_error,
                fmt::format("gamepack: providers.{}.instances must be a table",
                            service))};
        }
        binding.mode = provider_mode::instanced;
        const auto instance_table{
            fmt::format("providers.{}.instances", service)};
        auto names{doc.keys(instance_table)};
        if (names.empty()) {
            return std::unexpected{
                fail(error_code::invalid_argument,
                     fmt::format("gamepack: providers.{}.instances must not be "
                                 "empty",
                                 service))};
        }
        for (const auto &name : names) {
            if (!valid_instance_name(name)) {
                return std::unexpected{fail(
                    error_code::invalid_argument,
                    fmt::format("gamepack: invalid provider instance name '{}' "
                                "(lowercase letters, digits, '_', 1..64)",
                                name))};
            }
            // Instance values are literal keys inside the instances table (no
            // dotted-path splitting).
            const std::array<std::string_view, 4> value_path{
                "providers", service, "instances", name};
            auto text{doc.get_string_at(value_path)};
            if (!text) {
                return std::unexpected{text.error()};
            }
            auto requirement{parse_mod_requirement(*text)};
            if (!requirement) {
                return std::unexpected{fail(
                    error_code::invalid_argument,
                    fmt::format("gamepack: providers.{}.instances.{}: {}",
                                service, name, requirement.error().message))};
            }
            binding.instances.push_back(provider_instance{
                .name = name, .requirement = std::move(*requirement)});
        }

        const std::array<std::string_view, 3> default_path{"providers", service,
                                                           "default"};
        if (doc.contains_at(default_path)) {
            auto default_name{get_required_string(
                doc, default_path,
                fmt::format("providers.{}.default", service))};
            if (!default_name) {
                return std::unexpected{default_name.error()};
            }
            const auto found =
                std::any_of(binding.instances.begin(), binding.instances.end(),
                            [&default_name](const provider_instance &instance) {
                                return instance.name == *default_name;
                            });
            if (!found) {
                return std::unexpected{fail(
                    error_code::invalid_argument,
                    fmt::format("gamepack: providers.{}.default '{}' is not "
                                "one of the declared instances",
                                service, *default_name))};
            }
            binding.default_instance = std::move(*default_name);
        }
        return status_ok();
    }

    if (doc.contains_at(base_path)) {
        binding.mode = provider_mode::chain;
        auto base{get_required_string(
            doc, base_path, fmt::format("providers.{}.base", service))};
        if (!base) {
            return std::unexpected{base.error()};
        }
        auto base_requirement{parse_mod_requirement(*base)};
        if (!base_requirement) {
            return std::unexpected{
                fail(error_code::invalid_argument,
                     fmt::format("gamepack: providers.{}.base: {}", service,
                                 base_requirement.error().message))};
        }
        binding.base = std::move(*base_requirement);

        const std::array<std::string_view, 3> wrappers_path{
            "providers", service, "wrappers"};
        if (doc.contains_at(wrappers_path)) {
            auto wrappers{doc.get_strings_at(wrappers_path)};
            if (!wrappers) {
                return std::unexpected{wrappers.error()};
            }
            auto wrapped{parse_requirement_list(
                fmt::format("providers.{}.wrappers", service),
                std::span<const std::string>{*wrappers})};
            if (!wrapped) {
                return std::unexpected{wrapped.error()};
            }
            binding.wrappers = std::move(*wrapped);
        }
        return status_ok();
    }

    return std::unexpected{fail(
        error_code::parse_error,
        fmt::format("gamepack: providers.{} table must define 'instances' or "
                    "'base'",
                    service))};
}

[[nodiscard]] auto parse_provider_section(const config::document &doc,
                                          gamepack_spec &spec) -> status {
    const auto kind{doc.kind_at(std::array<std::string_view, 1>{"providers"})};
    if (kind == config::value_kind::missing) {
        return status_ok(); // allowed for minimal/experimental packs
    }
    if (kind != config::value_kind::table) {
        return std::unexpected{fail(error_code::parse_error,
                                    "gamepack: [providers] must be a table")};
    }

    spec.providers.clear();
    for (const auto &service : doc.keys("providers")) {
        if (!valid_service_key(service)) {
            return std::unexpected{
                fail(error_code::invalid_argument,
                     fmt::format("gamepack: invalid provider service key '{}' "
                                 "(lowercase letters, digits and '_')",
                                 service))};
        }
        provider_binding binding{};
        binding.service = service;
        if (auto parsed{parse_provider_value(doc, service, binding)}; !parsed) {
            return std::unexpected{parsed.error()};
        }
        spec.providers.push_back(std::move(binding));
    }
    std::sort(spec.providers.begin(), spec.providers.end(),
              [](const provider_binding &lhs, const provider_binding &rhs) {
                  return lhs.service < rhs.service;
              });
    return status_ok();
}

[[nodiscard]] auto annotate(error original, std::string_view path) -> error {
    if (original.message.find(path) != std::string::npos) {
        return original;
    }
    original.message = std::string{path} + ": " + original.message;
    return original;
}

} // namespace

// ---------------------------------------------------------------------------
// mod_ref / mod_requirement
// ---------------------------------------------------------------------------

auto to_string(provider_mode mode) noexcept -> std::string_view {
    switch (mode) {
    case provider_mode::exclusive:
        return "exclusive";
    case provider_mode::instanced:
        return "instanced";
    case provider_mode::chain:
        return "chain";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// gamepack_spec
// ---------------------------------------------------------------------------

auto gamepack_spec::find_mod(std::string_view mod_id) const noexcept
    -> const mod_entry * {
    const auto match = std::find_if(
        mods.begin(), mods.end(), [mod_id](const mod_entry &entry) {
            return std::string_view{entry.ref.id} == mod_id;
        });
    return match == mods.end() ? nullptr : &*match;
}

// ---------------------------------------------------------------------------
// parse / resolve
// ---------------------------------------------------------------------------

auto parse_gamepack(const core::config::document &doc,
                    std::string_view host_side) -> result<gamepack_spec> {
    const auto gamepack_kind{
        doc.kind_at(std::array<std::string_view, 1>{"gamepack"})};
    if (gamepack_kind != config::value_kind::table) {
        return std::unexpected{
            fail(error_code::parse_error,
                 "gamepack: missing required [gamepack] table")};
    }
    const auto mods_kind{doc.kind_at(std::array<std::string_view, 1>{"mods"})};
    if (mods_kind != config::value_kind::table) {
        return std::unexpected{fail(error_code::parse_error,
                                    "gamepack: missing required [mods] table")};
    }

    if (auto root_check{validate_root_keys(doc)}; !root_check) {
        return std::unexpected{root_check.error()};
    }

    auto spec{parse_gamepack_section(doc, host_side)};
    if (!spec) {
        return std::unexpected{spec.error()};
    }
    auto policy{parse_policy(doc)};
    if (!policy) {
        return std::unexpected{policy.error()};
    }
    spec->policy = std::move(*policy);
    if (auto mods{parse_mod_section(doc, *spec)}; !mods) {
        return std::unexpected{mods.error()};
    }
    if (auto providers{parse_provider_section(doc, *spec)}; !providers) {
        return std::unexpected{providers.error()};
    }
    return spec;
}

namespace {

[[nodiscard]] auto version_satisfies(std::string_view version,
                                     const mod_requirement &requirement)
    -> bool {
    const auto matched{core::satisfies(version, requirement.range)};
    return matched && *matched;
}

[[nodiscard]] auto skip_reason(std::string_view version,
                               const mod_requirement &requirement)
    -> std::string {
    return fmt::format("{} (version {} does not satisfy {})",
                       requirement.text(), version, requirement.range);
}

/// Strictly resolve one requirement against the resolved mod set (instanced
/// instances, chain base/wrappers). No fallback: the mod must be present and
/// its version must satisfy the declared range.
[[nodiscard]] auto resolve_requirement_strict(
    const std::unordered_map<std::string_view, const resolved_mod_view *>
        &index,
    const mod_requirement &requirement, std::string_view context)
    -> result<const resolved_mod_view *> {
    const auto found{index.find(requirement.id)};
    if (found == index.end()) {
        return std::unexpected{
            fail(error_code::not_found,
                 fmt::format("gamepack: {} requires '{}', which is not in the "
                             "resolved mod set",
                             context, requirement.text()))};
    }
    const auto *mod{found->second};
    if (!version_satisfies(mod->version, requirement)) {
        return std::unexpected{fail(
            error_code::version_mismatch,
            fmt::format("gamepack: {} requires '{}' but '{}' resolves to "
                        "version '{}'",
                        context, requirement.text(), mod->id, mod->version))};
    }
    return mod;
}

[[nodiscard]] auto canonical_ref(const resolved_mod_view &mod) -> std::string {
    return fmt::format("{}@{}", mod.id, mod.version);
}

} // namespace

auto select_providers(const gamepack_spec &spec,
                      std::span<const resolved_mod_view> resolved,
                      std::span<const std::string_view> required_services)
    -> result<std::vector<resolved_provider>> {
    std::unordered_map<std::string_view, const resolved_mod_view *> index{};
    index.reserve(resolved.size());
    for (const auto &mod : resolved) {
        index.emplace(mod.id, &mod);
    }

    std::vector<resolved_provider> providers{};
    providers.reserve(spec.providers.size());
    for (const auto &binding : spec.providers) {
        resolved_provider selected{};
        selected.service = binding.service;
        selected.mode = binding.mode;

        switch (binding.mode) {
        case provider_mode::exclusive: {
            bool found_provider{false};
            for (const auto &candidate : binding.candidates) {
                const auto listed{index.find(candidate.id)};
                if (listed == index.end()) {
                    selected.skipped.push_back(candidate.text() +
                                               " (not listed)");
                    continue;
                }
                if (!version_satisfies(listed->second->version, candidate)) {
                    selected.skipped.push_back(
                        skip_reason(listed->second->version, candidate));
                    continue;
                }
                selected.provider = canonical_ref(*listed->second);
                found_provider = true;
                break;
            }
            if (!found_provider) {
                auto tried{std::string{}};
                for (const auto &candidate : binding.candidates) {
                    if (!tried.empty()) {
                        tried += ", ";
                    }
                    tried += candidate.text();
                }
                return std::unexpected{
                    fail(error_code::not_found,
                         fmt::format("gamepack: no provider for service '{}': "
                                     "tried {} against the resolved mod set",
                                     binding.service, tried))};
            }
            break;
        }
        case provider_mode::instanced: {
            selected.default_instance = binding.default_instance;
            for (const auto &instance : binding.instances) {
                auto provider{resolve_requirement_strict(
                    index, instance.requirement,
                    fmt::format("providers.{}.instances.{}", binding.service,
                                instance.name))};
                if (!provider) {
                    return std::unexpected{provider.error()};
                }
                selected.instances.push_back(
                    resolved_instance{.name = instance.name,
                                      .provider = canonical_ref(**provider)});
            }
            if (selected.instances.empty()) {
                return std::unexpected{
                    fail(error_code::invalid_argument,
                         fmt::format("gamepack: providers.{} has no instances",
                                     binding.service))};
            }
            std::sort(
                selected.instances.begin(), selected.instances.end(),
                [](const resolved_instance &lhs, const resolved_instance &rhs) {
                    return lhs.name < rhs.name;
                });
            break;
        }
        case provider_mode::chain: {
            if (!binding.base) {
                return std::unexpected{
                    fail(error_code::invalid_argument,
                         fmt::format("gamepack: providers.{} chain has no base",
                                     binding.service))};
            }
            auto base{resolve_requirement_strict(
                index, *binding.base,
                fmt::format("providers.{}.base", binding.service))};
            if (!base) {
                return std::unexpected{base.error()};
            }
            selected.provider = canonical_ref(**base);
            for (const auto &wrapper : binding.wrappers) {
                auto wrapped{resolve_requirement_strict(
                    index, wrapper,
                    fmt::format("providers.{}.wrappers", binding.service))};
                if (!wrapped) {
                    return std::unexpected{wrapped.error()};
                }
                selected.wrappers.push_back(canonical_ref(**wrapped));
            }
            break;
        }
        }
        providers.push_back(std::move(selected));
    }
    std::sort(providers.begin(), providers.end(),
              [](const resolved_provider &lhs, const resolved_provider &rhs) {
                  return lhs.service < rhs.service;
              });

    for (const auto required : required_services) {
        const auto found = std::find_if(
            providers.begin(), providers.end(),
            [required](const resolved_provider &entry) {
                if (entry.service != required) {
                    return false;
                }
                return !entry.provider.empty() || !entry.instances.empty();
            });
        if (found == providers.end()) {
            return std::unexpected{
                fail(error_code::not_found,
                     fmt::format("gamepack '{}': required service '{}' has no "
                                 "resolved provider",
                                 spec.id, required))};
        }
    }
    return providers;
}

auto load_gamepack_spec(std::string_view path, std::string_view host_side)
    -> result<gamepack_spec> {
    auto doc{config::document::parse_file(path)};
    if (!doc) {
        return std::unexpected{doc.error()};
    }
    auto spec{parse_gamepack(*doc, host_side)};
    if (!spec) {
        return std::unexpected{annotate(std::move(spec.error()), path)};
    }
    return spec;
}

} // namespace refrain::mod
