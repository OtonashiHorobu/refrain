// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/permissions/policy.hpp>

#include <refrain/core/log.hpp>

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

constexpr refrain::log::logger log_permissions{"permissions"};

/// Warnings collected while a document parses. They are emitted only once the
/// whole policy is accepted, so a rejected document does not leave the operator
/// with advice about a configuration that is not in effect.
using load_warnings = std::vector<std::string>;

constexpr std::string_view player_key_prefix{"player:"};

constexpr std::array<std::string_view, 7> root_tables{
    "defaults", "op", "ops", "grants", "groups", "contexts", "context_grants"};
constexpr std::array<std::string_view, 2> default_keys{"default", "nodes"};
constexpr std::array<std::string_view, 2> op_keys{"min_level", "nodes"};
constexpr std::array<std::string_view, 1> ops_keys{"players"};
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

/// The profile name of a `player:<name>` subject key, or empty for a uuid key
/// and for the other subject kinds. Only an accepted configuration reaches the
/// point that calls this, so a non-empty result is a valid profile name.
[[nodiscard]] auto profile_name_key(std::string_view key) noexcept
    -> std::string_view {
    if (!key.starts_with(player_key_prefix)) {
        return {};
    }
    const auto id{key.substr(player_key_prefix.size())};
    return is_uuid(id) ? std::string_view{} : id;
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
    const std::array<std::string_view, 2> nodes_path{"defaults", "nodes"};
    if (document.contains_at(nodes_path)) {
        auto values{document.get_strings_at(nodes_path)};
        if (!values) {
            return std::unexpected{type_error(document, "defaults.nodes",
                                              "an array of node strings")};
        }
        auto set{parse_node_set(document, *values, "[defaults].nodes")};
        if (!set) {
            return std::unexpected{std::move(set.error())};
        }
        policy_value.default_nodes = std::move(*set);
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

/// Parse `[ops].players`: the operator source the built-in resolver owns. Each
/// entry is a canonical lowercase player uuid or a profile name; anything else
/// is a configuration error, because a malformed entry would otherwise be a
/// silently ineffective grant of operator rights. A profile name is accepted
/// for compatibility and warned about, because the name belongs to the account
/// that holds it, not to the operator who configured it.
[[nodiscard]] auto parse_ops(const config::document &document,
                             policy &policy_value, load_warnings &warnings)
    -> status {
    const std::array<std::string_view, 1> path{"ops"};
    if (!document.contains_at(path)) {
        return status_ok();
    }
    if (document.kind_at(path) != config::value_kind::table) {
        return std::unexpected{type_error(document, "ops", "a table")};
    }
    if (auto checked{
            validate_keys(document, "ops", ops_keys, document.keys("ops"))};
        !checked) {
        return checked;
    }
    const std::array<std::string_view, 2> players_path{"ops", "players"};
    if (!document.contains_at(players_path)) {
        return status_ok();
    }
    auto values{document.get_strings_at(players_path)};
    if (!values) {
        return std::unexpected{
            type_error(document, "ops.players",
                       "an array of player uuids or profile names")};
    }
    policy_value.operators.reserve(values->size());
    for (std::size_t index{0}; index < values->size(); ++index) {
        const auto &entry{(*values)[index]};
        if (!valid_subject_key(fmt::format("player:{}", entry))) {
            return std::unexpected{policy_error(
                document,
                fmt::format("ops.players[{}] '{}' is not a lowercase player "
                            "uuid or a profile name",
                            index, entry))};
        }
        if (!is_uuid(entry)) {
            warnings.push_back(fmt::format(
                "operator entry '{}' in [ops].players of '{}' matches by "
                "profile name, which another account can claim after the "
                "holder renames; list the player uuid instead",
                entry, document.origin()));
        }
        policy_value.operators.push_back(entry);
    }
    return status_ok();
}

/// Parse one array of subject rules: node rules plus `@group` joins. `location`
/// names the configuration entry in error messages; the array index is
/// appended. Groups must already be parsed so a dangling reference is an error.
[[nodiscard]] auto parse_subject_rules(const config::document &document,
                                       const policy &policy_value,
                                       std::span<const std::string> values,
                                       std::string_view location)
    -> result<subject_rules> {
    subject_rules rules{};
    for (std::size_t index{0}; index < values.size(); ++index) {
        const auto &text{values[index]};
        if (text.starts_with(group_prefix)) {
            const auto group{text.substr(group_prefix.size())};
            if (!valid_group_name(group)) {
                return std::unexpected{policy_error(
                    document, fmt::format("{}[{}]: invalid group name '{}'",
                                          location, index, group))};
            }
            if (!policy_value.groups.contains(group)) {
                return std::unexpected{policy_error(
                    document,
                    fmt::format("{}[{}] references unknown group '{}'",
                                location, index, group))};
            }
            rules.groups.emplace_back(group);
            continue;
        }
        auto rule{parse_rule(text, fmt::format("{}[{}]", location, index))};
        if (!rule) {
            return std::unexpected{
                policy_error(document, rule.error().message)};
        }
        rules.nodes.add(std::move(*rule));
    }
    return rules;
}

/// Parse `[contexts]`: named predicates over the query context. Each entry is a
/// table of `key = "value"` conditions and applies to a query only when every
/// condition matches. `world` and `gamemode` address the dedicated query
/// fields, every other key an extra entry. An empty predicate is rejected
/// because it would match every query while reading like a constraint.
[[nodiscard]] auto parse_contexts(const config::document &document,
                                  policy &policy_value) -> status {
    const std::array<std::string_view, 1> path{"contexts"};
    if (!document.contains_at(path)) {
        return status_ok();
    }
    if (document.kind_at(path) != config::value_kind::table) {
        return std::unexpected{type_error(document, "contexts", "a table")};
    }
    for (const auto &name : document.keys("contexts")) {
        if (!valid_context_name(name)) {
            return std::unexpected{policy_error(
                document,
                fmt::format("invalid context name '{}' (lowercase letters, "
                            "digits, '_' and '-', at most 64 bytes)",
                            name))};
        }
        const std::array<std::string_view, 2> value_path{"contexts", name};
        if (document.kind_at(value_path) != config::value_kind::table) {
            return std::unexpected{policy_error(
                document, fmt::format("contexts.{} must be a table of context "
                                      "conditions",
                                      name))};
        }
        auto keys{document.keys_at(value_path)};
        if (!keys) {
            return std::unexpected{
                policy_error(document, fmt::format("contexts.{}: {}", name,
                                                   keys.error().message))};
        }
        if (keys->empty()) {
            return std::unexpected{policy_error(
                document,
                fmt::format("contexts.{} defines no condition; a context must "
                            "match at least one context key",
                            name))};
        }
        if (keys->size() > max_context_entries) {
            return std::unexpected{policy_error(
                document,
                fmt::format("contexts.{} has {} conditions; at most {} are "
                            "accepted",
                            name, keys->size(), max_context_entries))};
        }
        context_definition definition{};
        definition.conditions.reserve(keys->size());
        for (const auto &key : *keys) {
            if (!valid_context_key(key)) {
                return std::unexpected{policy_error(
                    document,
                    fmt::format("contexts.{}: invalid context key '{}' "
                                "(expected 'world', 'gamemode' or a lowercase "
                                "provider key)",
                                name, key))};
            }
            const std::array<std::string_view, 3> condition_path{"contexts",
                                                                 name, key};
            auto value{document.get_string_at(condition_path)};
            if (!value) {
                return std::unexpected{policy_error(
                    document,
                    fmt::format("contexts.{}.{} must be a string", name, key))};
            }
            if (value->empty() || value->size() > max_context_length) {
                return std::unexpected{policy_error(
                    document,
                    fmt::format("contexts.{}.{} must be a non-empty string of "
                                "at most {} bytes",
                                name, key, max_context_length))};
            }
            definition.conditions.push_back(
                context_condition{.key = key, .value = std::move(*value)});
        }
        policy_value.contexts.emplace(name, std::move(definition));
    }
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
                                policy &policy_value, load_warnings &warnings)
    -> status {
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
        if (!profile_name_key(key).empty()) {
            warnings.push_back(fmt::format(
                "grant '{}' in '{}' matches by profile name, which another "
                "account can claim after the holder renames; key it by the "
                "player uuid instead",
                key, document.origin()));
        }
        const std::array<std::string_view, 2> value_path{"grants", key};
        auto values{document.get_strings_at(value_path)};
        if (!values) {
            return std::unexpected{policy_error(
                document,
                fmt::format("grants.{} must be an array of node strings",
                            key))};
        }
        auto rules{parse_subject_rules(document, policy_value, *values,
                                       fmt::format("grants.{}", key))};
        if (!rules) {
            return std::unexpected{std::move(rules.error())};
        }
        policy_value.subjects.emplace(key, std::move(*rules));
    }
    return status_ok();
}

/// Parse `[context_grants]`: rule sets attached to a subject under one declared
/// context. The table is keyed by context name and then by configured subject
/// key; a set joins global groups with `@name` exactly like `[grants]`, because
/// groups and their rules stay global and single-level. A context name the
/// `[contexts]` table does not declare is a configuration error, as is a
/// malformed subject key or rule.
[[nodiscard]] auto parse_context_grants(const config::document &document,
                                        policy &policy_value,
                                        load_warnings &warnings) -> status {
    const std::array<std::string_view, 1> path{"context_grants"};
    if (!document.contains_at(path)) {
        return status_ok();
    }
    if (document.kind_at(path) != config::value_kind::table) {
        return std::unexpected{
            type_error(document, "context_grants", "a table")};
    }
    for (const auto &context_name : document.keys("context_grants")) {
        if (!valid_context_name(context_name)) {
            return std::unexpected{policy_error(
                document,
                fmt::format("invalid context name '{}' in [context_grants]",
                            context_name))};
        }
        if (!policy_value.contexts.contains(context_name)) {
            return std::unexpected{policy_error(
                document,
                fmt::format("context_grants references unknown context '{}'; "
                            "declare it in [contexts]",
                            context_name))};
        }
        const std::array<std::string_view, 2> subjects_path{"context_grants",
                                                            context_name};
        if (document.kind_at(subjects_path) != config::value_kind::table) {
            return std::unexpected{policy_error(
                document,
                fmt::format("context_grants.{} must be a table of subject "
                            "keys",
                            context_name))};
        }
        auto subject_keys{document.keys_at(subjects_path)};
        if (!subject_keys) {
            return std::unexpected{policy_error(
                document, fmt::format("context_grants.{}: {}", context_name,
                                      subject_keys.error().message))};
        }
        context_subject_rules rules{};
        for (const auto &key : *subject_keys) {
            if (!valid_subject_key(key)) {
                return std::unexpected{policy_error(
                    document,
                    fmt::format("context_grants.{}: invalid subject key '{}'",
                                context_name, key))};
            }
            if (!profile_name_key(key).empty()) {
                warnings.push_back(fmt::format(
                    "context grant '{}' in '{}' matches by profile name, which "
                    "another account can claim after the holder renames; key "
                    "it by the player uuid instead",
                    key, document.origin()));
            }
            const std::array<std::string_view, 3> rules_path{"context_grants",
                                                             context_name, key};
            auto values{document.get_strings_at(rules_path)};
            if (!values) {
                return std::unexpected{policy_error(
                    document,
                    fmt::format("context_grants.{}.{} must be an array of node "
                                "strings",
                                context_name, key))};
            }
            auto parsed{parse_subject_rules(
                document, policy_value, *values,
                fmt::format("context_grants.{}.{}", context_name, key))};
            if (!parsed) {
                return std::unexpected{std::move(parsed.error())};
            }
            rules.emplace(key, std::move(*parsed));
        }
        policy_value.context_subjects.emplace(context_name, std::move(rules));
    }
    return status_ok();
}

/// The rule sets of one configured subject table, in the order `evaluate()`
/// applies them: the identity the query names first, then the profile-name
/// fallback.
struct subject_rule_set_pair {
    const subject_rules *primary{nullptr};
    const subject_rules *secondary{nullptr};
};

/// Look up the subject's rule sets in one table keyed by configured subject key
/// (`policy.subjects` or one context's rules).
[[nodiscard]] auto subject_rule_sets_in(
    const std::map<std::string, subject_rules, std::less<>> &table,
    const subject_view &subject) -> subject_rule_set_pair {
    // A player can be configured by uuid and, as a fallback, by profile name.
    // Both rule sets apply when both exist, so a name-based grant cannot
    // override a uuid-based deny.
    const auto primary_key{make_subject_key(subject.kind, subject.id)};
    subject_rule_set_pair pair{};
    if (const auto found{table.find(primary_key)}; found != table.end()) {
        pair.primary = &found->second;
    }
    if (subject.kind == subject_kind::player && !subject.profile_name.empty()) {
        const auto name_key{
            make_subject_key(subject.kind, subject.profile_name)};
        if (!name_key.empty() && name_key != primary_key) {
            if (const auto found{table.find(name_key)}; found != table.end()) {
                pair.secondary = &found->second;
            }
        }
    }
    return pair;
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

auto node_set::remove(const node_rule &rule) -> bool {
    const auto found{std::ranges::find(rules_, rule)};
    if (found == rules_.end()) {
        return false;
    }
    rules_.erase(found);
    return true;
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

auto valid_context_name(std::string_view name) noexcept -> bool {
    return valid_group_name(name);
}

auto valid_context_key(std::string_view key) noexcept -> bool {
    if (key == "world" || key == "gamemode") {
        return true;
    }
    if (key.empty() || key.size() > 64) {
        return false;
    }
    const auto is_start = [](char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9');
    };
    const auto is_body = [&is_start](char character) {
        return is_start(character) || character == '_' || character == '-' ||
               character == '.';
    };
    if (!is_start(key.front())) {
        return false;
    }
    return std::ranges::all_of(key.substr(1), is_body);
}

auto valid_context(const context_view &context) noexcept -> bool {
    if (context.world.size() > max_context_length ||
        context.gamemode.size() > max_context_length) {
        return false;
    }
    if (context.extra.size() > max_context_entries) {
        return false;
    }
    for (std::size_t index{0}; index < context.extra.size(); ++index) {
        const auto &entry{context.extra[index]};
        if (entry.key.empty() || entry.key.size() > max_context_length ||
            entry.value.size() > max_context_length) {
            return false;
        }
        if (entry.key == "world" || entry.key == "gamemode") {
            return false;
        }
        for (std::size_t earlier{0}; earlier < index; ++earlier) {
            if (context.extra[earlier].key == entry.key) {
                return false;
            }
        }
    }
    return true;
}

auto context_matches(const context_definition &definition,
                     const context_view &context) noexcept -> bool {
    for (const auto &condition : definition.conditions) {
        std::string_view actual{};
        if (condition.key == "world") {
            actual = context.world;
        } else if (condition.key == "gamemode") {
            actual = context.gamemode;
        } else {
            const auto found{std::ranges::find_if(
                context.extra, [&condition](const context_value_view &entry) {
                    return entry.key == condition.key;
                })};
            if (found == context.extra.end()) {
                return false;
            }
            actual = found->value;
        }
        if (actual != condition.value) {
            return false;
        }
    }
    return true;
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
    load_warnings warnings{};
    // Contexts and groups first: grants may join a group and contextual grants
    // may reference a context, so a dangling reference is an error, not a
    // silently ignored rule.
    if (auto parsed{parse_contexts(document, policy_value)}; !parsed) {
        return std::unexpected{std::move(parsed.error())};
    }
    if (auto parsed{parse_groups(document, policy_value)}; !parsed) {
        return std::unexpected{std::move(parsed.error())};
    }
    if (auto parsed{parse_grants(document, policy_value, warnings)}; !parsed) {
        return std::unexpected{std::move(parsed.error())};
    }
    if (auto parsed{parse_context_grants(document, policy_value, warnings)};
        !parsed) {
        return std::unexpected{std::move(parsed.error())};
    }
    if (auto parsed{parse_defaults(document, policy_value)}; !parsed) {
        return std::unexpected{std::move(parsed.error())};
    }
    if (auto parsed{parse_op(document, policy_value)}; !parsed) {
        return std::unexpected{std::move(parsed.error())};
    }
    if (auto parsed{parse_ops(document, policy_value, warnings)}; !parsed) {
        return std::unexpected{std::move(parsed.error())};
    }
    // A name-keyed entry is a compatibility fallback, so it stays in effect.
    // The warning tells the operator that the rule follows a mutable profile
    // name, which a different account can hold after a rename.
    for (const auto &warning : warnings) {
        log_permissions.warn("{}", warning);
    }
    return policy_value;
}

namespace {

/// One rule of one set that matched the query node.
struct set_match {
    rule_match match{rule_match::none};
    const node_rule *rule{nullptr};
};

/// Match one concrete node against one rule set: a deny anywhere in the set
/// wins over the set's grants, so the first matching deny is reported and a
/// matching grant is only remembered until the set is exhausted.
[[nodiscard]] auto match_set(const node_set &set, std::string_view node)
    -> set_match {
    set_match outcome{};
    for (const auto &rule : set.rules()) {
        if (!node_matches_rule(rule.node, node)) {
            continue;
        }
        if (rule.deny) {
            return set_match{.match = rule_match::deny, .rule = &rule};
        }
        if (outcome.rule == nullptr) {
            outcome = set_match{.match = rule_match::grant, .rule = &rule};
        }
    }
    return outcome;
}

/// Fold one node set into a running match. A deny reports true because the walk
/// must stop: deny wins over every grant. A grant is remembered only while no
/// earlier set granted.
[[nodiscard]] auto apply_set(const node_set &set, std::string_view node,
                             rule_match &result) noexcept -> bool {
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
}

/// True when the subject is an operator: a player listed in `[ops].players`
/// (by canonical uuid or profile name) or a player at or above
/// `[op].min_level` on the vanilla ladder supplied in the query.
[[nodiscard]] auto is_operator_subject(const policy &policy_value,
                                       const subject_view &subject,
                                       std::uint32_t op_level) noexcept
    -> bool {
    if (subject.kind != subject_kind::player) {
        return false;
    }
    if (op_level >= policy_value.op_min_level) {
        return true;
    }
    const auto named = [&policy_value](std::string_view identity) {
        return !identity.empty() &&
               std::ranges::find(policy_value.operators, identity) !=
                   policy_value.operators.end();
    };
    return named(subject.id) || named(subject.profile_name);
}

/// Apply one subject's rule sets, groups first and then its own node set.
/// Returns true on a matching deny, which ends the walk.
[[nodiscard]] auto match_rules(const policy &policy_value,
                               const subject_rules *rules,
                               std::string_view node,
                               rule_match &result) noexcept -> bool {
    if (rules == nullptr) {
        return false;
    }
    for (const auto &group : rules->groups) {
        const auto found{policy_value.groups.find(group)};
        if (found != policy_value.groups.end() &&
            apply_set(found->second, node, result)) {
            return true;
        }
    }
    return apply_set(rules->nodes, node, result);
}

/// Match one node against the subject's global rule sets: its own nodes, the
/// groups it joined, the operator node set and the public default set.
[[nodiscard]] auto
match_subject(const policy &policy_value, const subject_view &subject,
              const subject_rules *rules, std::string_view node,
              std::uint32_t op_level) noexcept -> rule_match {
    rule_match result{rule_match::none};
    if (match_rules(policy_value, rules, node, result)) {
        return rule_match::deny;
    }
    const bool operator_subject{
        is_operator_subject(policy_value, subject, op_level)};
    if (operator_subject && apply_set(policy_value.op_nodes, node, result)) {
        return rule_match::deny;
    }
    // A public grant applies to every subject and a deny in it still wins over
    // any grant above it.
    if (apply_set(policy_value.default_nodes, node, result)) {
        return rule_match::deny;
    }
    return result;
}

/// Match one node against every context whose predicate matches the query. All
/// matching contexts apply; a deny in any of them wins over the grants.
[[nodiscard]] auto
match_context(const policy &policy_value, const subject_view &subject,
              std::string_view node, const context_view &context) noexcept
    -> rule_match {
    rule_match result{rule_match::none};
    for (const auto &[name, definition] : policy_value.contexts) {
        if (!context_matches(definition, context)) {
            continue;
        }
        const auto found{policy_value.context_subjects.find(name)};
        if (found == policy_value.context_subjects.end()) {
            continue;
        }
        const auto rule_sets{subject_rule_sets_in(found->second, subject)};
        if (match_rules(policy_value, rule_sets.primary, node, result) ||
            match_rules(policy_value, rule_sets.secondary, node, result)) {
            return rule_match::deny;
        }
    }
    return result;
}

/// One rule set of a query in the order an explanation scans it.
struct scan_entry {
    const node_set *set{nullptr};
    decision_source grant{decision_source::subject_grant};
    decision_source deny{decision_source::subject_deny};
    std::string group{};
    std::string context{};
};

void append_subject_sets(const policy &policy_value, const subject_rules *rules,
                         std::vector<scan_entry> &entries) {
    if (rules == nullptr) {
        return;
    }
    for (const auto &group : rules->groups) {
        const auto found{policy_value.groups.find(group)};
        if (found == policy_value.groups.end()) {
            continue;
        }
        entries.push_back(scan_entry{.set = &found->second,
                                     .grant = decision_source::group_grant,
                                     .deny = decision_source::group_deny,
                                     .group = group});
    }
    entries.push_back(scan_entry{.set = &rules->nodes,
                                 .grant = decision_source::subject_grant,
                                 .deny = decision_source::subject_deny});
}

/// Append the rule sets of every matching context for this subject, in context
/// name order. A group rule reached through a context keeps its group name and
/// names the context that made it apply.
void append_context_sets(const policy &policy_value,
                         const subject_view &subject,
                         const context_view &context,
                         std::vector<scan_entry> &entries) {
    const auto append_rules = [&policy_value,
                               &entries](const subject_rules *rules,
                                         std::string_view context_name) {
        if (rules == nullptr) {
            return;
        }
        for (const auto &group : rules->groups) {
            const auto found{policy_value.groups.find(group)};
            if (found == policy_value.groups.end()) {
                continue;
            }
            entries.push_back(
                scan_entry{.set = &found->second,
                           .grant = decision_source::context_grant,
                           .deny = decision_source::context_deny,
                           .group = group,
                           .context = std::string{context_name}});
        }
        entries.push_back(scan_entry{.set = &rules->nodes,
                                     .grant = decision_source::context_grant,
                                     .deny = decision_source::context_deny,
                                     .context = std::string{context_name}});
    };
    for (const auto &[name, definition] : policy_value.contexts) {
        if (!context_matches(definition, context)) {
            continue;
        }
        const auto found{policy_value.context_subjects.find(name)};
        if (found == policy_value.context_subjects.end()) {
            continue;
        }
        const auto rule_sets{subject_rule_sets_in(found->second, subject)};
        append_rules(rule_sets.primary, name);
        append_rules(rule_sets.secondary, name);
    }
}

} // namespace

auto evaluate(const policy &policy_value, const subject_view &subject,
              std::string_view node, std::uint32_t op_level,
              const context_view &context) -> permission_result {
    if (!valid_node(node)) {
        return permission_result::denied;
    }
    if (subject.kind == subject_kind::console) {
        return permission_result::allowed;
    }

    const auto rule_sets{subject_rule_sets_in(policy_value.subjects, subject)};
    const auto matched{match_subject(policy_value, subject, rule_sets.primary,
                                     node, op_level)};
    if (matched == rule_match::deny) {
        return permission_result::denied;
    }
    const auto contextual{match_context(policy_value, subject, node, context)};
    if (contextual == rule_match::deny) {
        return permission_result::denied;
    }
    const auto secondary_match{match_subject(
        policy_value, subject, rule_sets.secondary, node, op_level)};
    if (secondary_match == rule_match::deny) {
        return permission_result::denied;
    }
    if (matched == rule_match::grant || contextual == rule_match::grant ||
        secondary_match == rule_match::grant) {
        return permission_result::allowed;
    }
    if (policy_value.fallback == default_answer::op &&
        is_operator_subject(policy_value, subject, op_level)) {
        return permission_result::allowed;
    }
    return permission_result::denied;
}

auto rule_text(const node_rule &rule) -> std::string {
    return rule.deny ? fmt::format("!{}", rule.node) : rule.node;
}

auto explain(const policy &policy_value, const subject_view &subject,
             std::string_view node, std::uint32_t op_level,
             const context_view &context) -> decision_explanation {
    decision_explanation explanation{};
    explanation.result =
        evaluate(policy_value, subject, node, op_level, context);
    if (!valid_node(node)) {
        explanation.source = decision_source::invalid_node;
        return explanation;
    }
    if (subject.kind == subject_kind::console) {
        explanation.source = decision_source::console;
        return explanation;
    }

    // Scan the sets in the order the evaluator applies them. The scan only
    // names the deciding rule; `evaluate()` above is what answers the query.
    const auto rule_sets{subject_rule_sets_in(policy_value.subjects, subject)};
    std::vector<scan_entry> entries{};
    append_subject_sets(policy_value, rule_sets.primary, entries);
    append_context_sets(policy_value, subject, context, entries);
    const auto apply_global_sets = [&entries, &policy_value, op_level,
                                    &subject] {
        if (is_operator_subject(policy_value, subject, op_level)) {
            entries.push_back(scan_entry{.set = &policy_value.op_nodes,
                                         .grant = decision_source::op_grant,
                                         .deny = decision_source::op_deny});
        }
        entries.push_back(scan_entry{.set = &policy_value.default_nodes,
                                     .grant = decision_source::default_grant,
                                     .deny = decision_source::default_deny});
    };
    apply_global_sets();
    append_subject_sets(policy_value, rule_sets.secondary, entries);

    if (explanation.result == permission_result::denied) {
        for (const auto &entry : entries) {
            const auto matched{match_set(*entry.set, node)};
            if (matched.match != rule_match::deny) {
                continue;
            }
            explanation.source = entry.deny;
            explanation.rule = rule_text(*matched.rule);
            explanation.group = entry.group;
            explanation.context = entry.context;
            return explanation;
        }
        explanation.source = decision_source::fallback_deny;
        return explanation;
    }

    for (const auto &entry : entries) {
        const auto matched{match_set(*entry.set, node)};
        if (matched.match == rule_match::none) {
            continue;
        }
        explanation.source =
            matched.match == rule_match::deny ? entry.deny : entry.grant;
        explanation.rule = rule_text(*matched.rule);
        explanation.group = entry.group;
        explanation.context = entry.context;
        return explanation;
    }
    explanation.source = decision_source::fallback_grant;
    return explanation;
}

auto list_groups(const policy &policy_value) -> std::vector<group_listing> {
    std::vector<group_listing> groups{};
    groups.reserve(policy_value.groups.size());
    for (const auto &[name, set] : policy_value.groups) {
        group_listing listing{};
        listing.name = name;
        listing.nodes.reserve(set.rules().size());
        for (const auto &rule : set.rules()) {
            listing.nodes.push_back(rule_text(rule));
        }
        groups.push_back(std::move(listing));
    }
    const auto append_members = [&groups](std::string_view subject_key,
                                          const subject_rules &rules,
                                          std::string_view context) {
        for (const auto &group : rules.groups) {
            const auto found{std::ranges::find_if(
                groups, [&group](const group_listing &listing) {
                    return listing.name == group;
                })};
            if (found == groups.end()) {
                continue;
            }
            if (context.empty()) {
                found->members.push_back(std::string{subject_key});
            } else {
                found->members.push_back(
                    fmt::format("{} (context '{}')", subject_key, context));
            }
        }
    };
    for (const auto &[subject_key, rules] : policy_value.subjects) {
        append_members(subject_key, rules, {});
    }
    for (const auto &[context_name, subjects] : policy_value.context_subjects) {
        for (const auto &[subject_key, rules] : subjects) {
            append_members(subject_key, rules, context_name);
        }
    }
    return groups;
}

} // namespace refrain::permissions
