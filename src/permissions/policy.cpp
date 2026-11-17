// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/permissions/policy.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::permissions {

namespace {

namespace config = core::config;
using services::permissions::permission_result;
using services::permissions::subject_kind;

constexpr std::array<std::string_view, 4> root_tables{"defaults", "op",
                                                      "grants", "groups"};
constexpr std::array<std::string_view, 2> default_keys{"default"};
constexpr std::array<std::string_view, 2> op_keys{"min_level", "nodes"};
constexpr std::string_view deny_prefix{"!"};
constexpr std::string_view group_prefix{"@"};

[[nodiscard]] auto policy_error(const config::document &document,
                                std::string message) -> error {
    return make_error(error_code::parse_error,
                      fmt::format("{}: {}", document.origin(), message));
}

[[nodiscard]] auto type_error(const config::document &document,
                              std::string_view path, std::string_view expected)
    -> error {
    return policy_error(document,
                        fmt::format("'{}' must be {}", path, expected));
}

[[nodiscard]] auto contains_one_of(std::span<const std::string_view> known,
                                   std::string_view candidate) -> bool {
    return std::ranges::find(known, candidate) != known.end();
}

/// Reject keys the schema does not define so a typo cannot be read as "not
/// configured".
[[nodiscard]] auto validate_keys(const config::document &document,
                                 std::string_view table,
                                 std::span<const std::string_view> known,
                                 std::span<const std::string> present)
    -> status {
    for (const auto &key : present) {
        if (contains_one_of(known, key)) {
            continue;
        }
        return std::unexpected{policy_error(
            document, fmt::format("unknown key '{}' in [{}]", key, table))};
    }
    return status_ok();
}

[[nodiscard]] auto parse_node_set(const config::document &document,
                                  std::span<const std::string> values,
                                  std::string_view table) -> result<node_set> {
    node_set set{};
    for (std::size_t index{0}; index < values.size(); ++index) {
        auto rule{
            parse_rule(values[index], fmt::format("{}[{}]", table, index))};
        if (!rule) {
            return std::unexpected{
                policy_error(document, rule.error().message)};
        }
        set.add(std::move(*rule));
    }
    return set;
}

[[nodiscard]] auto parse_defaults(const config::document &document,
                                  policy &policy_value) -> status {
    const std::array<std::string_view, 1> path{"defaults"};
    if (!document.contains_at(path)) {
        return status_ok();
    }
    if (document.kind_at(path) != config::value_kind::table) {
        return std::unexpected{type_error(document, "defaults", "a table")};
    }
    if (auto checked{validate_keys(document, "defaults", default_keys,
                                   document.keys("defaults"))};
        !checked) {
        return checked;
    }
    const std::array<std::string_view, 2> value_path{"defaults", "default"};
    if (!document.contains_at(value_path)) {
        return status_ok();
    }
    const auto value{document.get_string_at(value_path)};
    if (!value) {
        return std::unexpected{
            type_error(document, "defaults.default", "a string")};
    }
    if (*value == "deny") {
        policy_value.fallback = default_answer::deny;
        return status_ok();
    }
    if (*value == "op") {
        policy_value.fallback = default_answer::op;
        return status_ok();
    }
    return std::unexpected{policy_error(
        document,
        fmt::format("defaults.default '{}' is not 'deny' or 'op'", *value))};
}

[[nodiscard]] auto parse_op(const config::document &document,
                            policy &policy_value) -> status {
    const std::array<std::string_view, 1> path{"op"};
    if (!document.contains_at(path)) {
        return status_ok();
    }
    if (document.kind_at(path) != config::value_kind::table) {
        return std::unexpected{type_error(document, "op", "a table")};
    }
    if (auto checked{
            validate_keys(document, "op", op_keys, document.keys("op"))};
        !checked) {
        return checked;
    }
    const std::array<std::string_view, 2> level_path{"op", "min_level"};
    if (document.contains_at(level_path)) {
        if (document.kind_at(level_path) != config::value_kind::integer) {
            return std::unexpected{
                type_error(document, "op.min_level", "an integer")};
        }
        const auto level{document.get_int64_at(level_path)};
        if (!level) {
            return std::unexpected{
                policy_error(document, "op.min_level is not an integer")};
        }
        if (*level < 0 || *level > 4) {
            return std::unexpected{policy_error(
                document,
                fmt::format(
                    "op.min_level {} is outside the vanilla 0..4 ladder",
                    *level))};
        }
        policy_value.op_min_level = static_cast<std::uint32_t>(*level);
    }
    const std::array<std::string_view, 2> nodes_path{"op", "nodes"};
    if (!document.contains_at(nodes_path)) {
        return status_ok();
    }
    auto values{document.get_strings_at(nodes_path)};
    if (!values) {
        return std::unexpected{
            type_error(document, "op.nodes", "an array of node strings")};
    }
    auto set{parse_node_set(document, *values, "[op].nodes")};
    if (!set) {
        return std::unexpected{std::move(set.error())};
    }
    policy_value.op_nodes = std::move(*set);
    return status_ok();
}

[[nodiscard]] auto parse_groups(const config::document &document,
                                policy &policy_value) -> status {
    const std::array<std::string_view, 1> path{"groups"};
    if (!document.contains_at(path)) {
        return status_ok();
    }
    if (document.kind_at(path) != config::value_kind::table) {
        return std::unexpected{type_error(document, "groups", "a table")};
    }
    for (const auto &name : document.keys("groups")) {
        if (!valid_group_name(name)) {
            return std::unexpected{policy_error(
                document,
                fmt::format("invalid group name '{}' (lowercase letters, "
                            "digits, '_' and '-', at most 64 bytes)",
                            name))};
        }
        const std::array<std::string_view, 2> value_path{"groups", name};
        auto values{document.get_strings_at(value_path)};
        if (!values) {
            return std::unexpected{policy_error(
                document,
                fmt::format("groups.{} must be an array of node strings",
                            name))};
        }
        for (std::size_t index{0}; index < values->size(); ++index) {
            const auto &text{(*values)[index]};
            if (text.starts_with(group_prefix)) {
                return std::unexpected{policy_error(
                    document,
                    fmt::format("groups.{}[{}] references '{}'; groups are "
                                "single-level and may not include another "
                                "group",
                                name, index, text))};
            }
        }
        auto set{parse_node_set(document, *values,
                                fmt::format("[groups].{}", name))};
        if (!set) {
            return std::unexpected{std::move(set.error())};
        }
        policy_value.groups.emplace(name, std::move(*set));
    }
    return status_ok();
}

[[nodiscard]] auto parse_grants(const config::document &document,
                                policy &policy_value) -> status {
    const std::array<std::string_view, 1> path{"grants"};
    if (!document.contains_at(path)) {
        return status_ok();
    }
    if (document.kind_at(path) != config::value_kind::table) {
        return std::unexpected{type_error(document, "grants", "a table")};
    }
    for (const auto &key : document.keys("grants")) {
        if (!valid_subject_key(key)) {
            return std::unexpected{policy_error(
                document,
                fmt::format("invalid subject key '{}' (expected 'console', "
                            "'player:<uuid or profile name>', "
                            "'entity:<uuid>' or 'plugin:<mod id>')",
                            key))};
        }
        const std::array<std::string_view, 2> value_path{"grants", key};
        auto values{document.get_strings_at(value_path)};
        if (!values) {
            return std::unexpected{policy_error(
                document,
                fmt::format("grants.{} must be an array of node strings",
                            key))};
        }
        subject_rules rules{};
        for (std::size_t index{0}; index < values->size(); ++index) {
            const auto &text{(*values)[index]};
            if (text.starts_with(group_prefix)) {
                const auto group{text.substr(group_prefix.size())};
                if (!valid_group_name(group)) {
                    return std::unexpected{policy_error(
                        document,
                        fmt::format("grants.{}[{}]: invalid group name '{}'",
                                    key, index, group))};
                }
                if (!policy_value.groups.contains(group)) {
                    return std::unexpected{policy_error(
                        document,
                        fmt::format("grants.{}[{}] references unknown group "
                                    "'{}'",
                                    key, index, group))};
                }
                rules.groups.emplace_back(group);
                continue;
            }
            auto rule{
                parse_rule(text, fmt::format("grants.{}[{}]", key, index))};
            if (!rule) {
                return std::unexpected{
                    policy_error(document, rule.error().message)};
            }
            rules.nodes.add(std::move(*rule));
        }
        policy_value.subjects.emplace(key, std::move(rules));
    }
    return status_ok();
}

[[nodiscard]] auto find_subject_rules(const policy &policy_value,
                                      std::string_view key) noexcept
    -> const subject_rules * {
    if (key.empty()) {
        return nullptr;
    }
    const auto found{policy_value.subjects.find(key)};
    return found == policy_value.subjects.end() ? nullptr : &found->second;
}

/// Match one node against every node set that applies to the subject. Returns
/// deny when any set denies, otherwise grant when any set grants, otherwise
/// none.
[[nodiscard]] auto
match_subject(const policy &policy_value, const subject_view &subject,
              const subject_rules *rules, std::string_view node,
              std::uint32_t op_level) noexcept -> rule_match {
    auto result{rule_match::none};
    const auto apply = [&node, &result](const node_set &set) {
        switch (set.match(node)) {
        case rule_match::deny:
            result = rule_match::deny;
            return true;
        case rule_match::grant:
            if (result == rule_match::none) {
                result = rule_match::grant;
            }
            return false;
        case rule_match::none:
            return false;
        }
        return false;
    };
    if (rules != nullptr) {
        for (const auto &group : rules->groups) {
            const auto found{policy_value.groups.find(group)};
            if (found != policy_value.groups.end() && apply(found->second)) {
                return rule_match::deny;
            }
        }
        if (apply(rules->nodes)) {
            return rule_match::deny;
        }
    }
    const bool is_operator{subject.kind == subject_kind::player &&
                           op_level >= policy_value.op_min_level};
    if (is_operator && apply(policy_value.op_nodes)) {
        return rule_match::deny;
    }
    return result;
}

} // namespace

auto node_set::match(std::string_view node) const noexcept -> rule_match {
    auto result{rule_match::none};
    for (const auto &rule : rules_) {
        if (!node_matches_rule(rule.node, node)) {
            continue;
        }
        if (rule.deny) {
            return rule_match::deny;
        }
        result = rule_match::grant;
    }
    return result;
}

auto is_uuid(std::string_view text) noexcept -> bool {
    if (text.size() != 36) {
        return false;
    }
    for (std::size_t index{0}; index < text.size(); ++index) {
        const auto character{text[index]};
        const bool dash_position{index == 8 || index == 13 || index == 18 ||
                                 index == 23};
        if (dash_position) {
            if (character != '-') {
                return false;
            }
            continue;
        }
        const bool hex{(character >= '0' && character <= '9') ||
                       (character >= 'a' && character <= 'f')};
        if (!hex) {
            return false;
        }
    }
    return true;
}

auto valid_group_name(std::string_view name) noexcept -> bool {
    if (name.empty() || name.size() > 64) {
        return false;
    }
    const auto is_start = [](char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9');
    };
    const auto is_body = [&is_start](char character) {
        return is_start(character) || character == '_' || character == '-';
    };
    if (!is_start(name.front())) {
        return false;
    }
    return std::ranges::all_of(name.substr(1), is_body);
}

namespace {

[[nodiscard]] auto valid_profile_name(std::string_view name) noexcept -> bool {
    if (name.size() < 1 || name.size() > 16) {
        return false;
    }
    return std::ranges::all_of(name, [](char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '_';
    });
}

[[nodiscard]] auto valid_plugin_id(std::string_view id) noexcept -> bool {
    if (id.empty() || id.size() > 128) {
        return false;
    }
    const auto is_start = [](char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9');
    };
    if (!is_start(id.front())) {
        return false;
    }
    return std::ranges::all_of(id.substr(1), [&is_start](char character) {
        return is_start(character) || character == '.' || character == '_' ||
               character == '-';
    });
}

} // namespace

auto valid_subject_key(std::string_view key) noexcept -> bool {
    if (key == "console") {
        return true;
    }
    const auto colon{key.find(':')};
    if (colon == std::string_view::npos) {
        return false;
    }
    const auto kind{key.substr(0, colon)};
    const auto id{key.substr(colon + 1)};
    if (kind == "player") {
        return is_uuid(id) || valid_profile_name(id);
    }
    if (kind == "entity") {
        return is_uuid(id);
    }
    if (kind == "plugin") {
        return valid_plugin_id(id);
    }
    return false;
}

auto make_subject_key(services::permissions::subject_kind kind,
                      std::string_view id) -> std::string {
    switch (kind) {
    case subject_kind::console:
        return id.empty() ? std::string{"console"} : std::string{};
    case subject_kind::player:
        return (is_uuid(id) || valid_profile_name(id))
                   ? fmt::format("player:{}", id)
                   : std::string{};
    case subject_kind::entity:
        return is_uuid(id) ? fmt::format("entity:{}", id) : std::string{};
    case subject_kind::plugin:
        return valid_plugin_id(id) ? fmt::format("plugin:{}", id)
                                   : std::string{};
    }
    return {};
}

auto parse_rule(std::string_view text, std::string_view context)
    -> result<node_rule> {
    if (text.empty()) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       fmt::format("{}: empty permission rule", context))};
    }
    node_rule rule{};
    auto node{text};
    if (node.starts_with(deny_prefix)) {
        rule.deny = true;
        node.remove_prefix(deny_prefix.size());
        if (node.empty()) {
            return std::unexpected{make_error(
                error_code::parse_error,
                fmt::format("{}: deny prefix '!' without a node", context))};
        }
    }
    if (!valid_rule_node(node)) {
        return std::unexpected{make_error(
            error_code::parse_error,
            fmt::format("{}: invalid permission node '{}'", context, text))};
    }
    rule.node = std::string{node};
    return rule;
}

auto parse_policy(const core::config::document &document) -> result<policy> {
    for (const auto &key : document.keys("")) {
        if (contains_one_of(root_tables, key)) {
            continue;
        }
        return std::unexpected{
            policy_error(document, fmt::format("unknown table '{}'", key))};
    }

    policy policy_value{};
    // Groups first: grants may reference them and a dangling reference is an
    // error, not a silently ignored rule.
    if (auto parsed{parse_groups(document, policy_value)}; !parsed) {
        return std::unexpected{std::move(parsed.error())};
    }
    if (auto parsed{parse_grants(document, policy_value)}; !parsed) {
        return std::unexpected{std::move(parsed.error())};
    }
    if (auto parsed{parse_defaults(document, policy_value)}; !parsed) {
        return std::unexpected{std::move(parsed.error())};
    }
    if (auto parsed{parse_op(document, policy_value)}; !parsed) {
        return std::unexpected{std::move(parsed.error())};
    }
    return policy_value;
}

auto evaluate(const policy &policy_value, const subject_view &subject,
              std::string_view node, std::uint32_t op_level)
    -> permission_result {
    if (!valid_node(node)) {
        return permission_result::denied;
    }
    if (subject.kind == subject_kind::console) {
        return permission_result::allowed;
    }

    // A player can be configured by uuid and, as a fallback, by profile name.
    // Both rule sets apply when both exist, so a name-based grant cannot
    // override a uuid-based deny.
    const auto primary_key{make_subject_key(subject.kind, subject.id)};
    const auto *primary{find_subject_rules(policy_value, primary_key)};
    const subject_rules *secondary{nullptr};
    if (subject.kind == subject_kind::player && !subject.profile_name.empty()) {
        const auto name_key{
            make_subject_key(subject.kind, subject.profile_name)};
        if (!name_key.empty() && name_key != primary_key) {
            secondary = find_subject_rules(policy_value, name_key);
        }
    }

    auto matched{match_subject(policy_value, subject, primary, node, op_level)};
    if (matched == rule_match::deny) {
        return permission_result::denied;
    }
    const auto secondary_match{
        match_subject(policy_value, subject, secondary, node, op_level)};
    if (secondary_match == rule_match::deny) {
        return permission_result::denied;
    }
    if (matched == rule_match::grant || secondary_match == rule_match::grant) {
        return permission_result::allowed;
    }
    if (policy_value.fallback == default_answer::op &&
        subject.kind == subject_kind::player &&
        op_level >= policy_value.op_min_level) {
        return permission_result::allowed;
    }
    return permission_result::denied;
}

} // namespace refrain::permissions
