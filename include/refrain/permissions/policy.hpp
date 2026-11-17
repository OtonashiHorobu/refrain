// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Default permission resolver: the configuration-driven policy the built-in
// `refrain.permissions` mod serves as the base of the `refrain:permissions`
// chain.
//
// The policy is a value: `parse_policy()` validates one TOML document and
// `evaluate()` answers a query from it. The service owns a published snapshot
// of this value, so queries never touch the configuration file.
//
// Configuration shape (`config/permissions.toml`):
//
//   [defaults]
//   default = "deny"          # "deny" or "op"
//   nodes = ["refrain.command.help", "refrain.command.list"]
//                             # nodes every subject holds
//
//   [op]
//   min_level = 1             # vanilla operator ladder level that counts as op
//   nodes = ["*"]             # nodes every operator holds
//
//   [ops]
//   players = ["<uuid>", "Notch"]   # player identities that count as operators
//
//   [grants]
//   "player:<uuid>" = ["refrain.mods.admin", "!refrain.command.stop"]
//   "plugin:com.example.mymod" = ["@moderators"]
//
//   [groups]
//   moderators = ["refrain.command.*", "!refrain.permissions.admin"]
//
// A group is a node set; a subject joins a group with an `@name` entry in
// `[grants]`. Groups are single-level in v1: a group list may not reference
// another group. Unknown tables, unknown keys, invalid nodes, invalid subject
// keys and unknown groups are configuration errors rather than silently ignored
// text, so a typo cannot widen an access rule.
//
// `[ops].players` is the operator source the built-in resolver owns: a player
// named by canonical uuid or profile name is treated as an operator and holds
// `[op].nodes` even though no vanilla op list is persisted yet.
// `[op].min_level` stays the ladder rule for a host that supplies a real op
// level in the query.
//
// Resolution order for one query:
//   1. console holds every node;
//   2. a matching deny anywhere denies (deny wins over every grant);
//   3. a matching grant allows;
//   4. an operator's `[op].nodes` grant allows, where an operator is a subject
//      listed in `[ops].players` or a player at or above `[op].min_level`;
//   5. a grant in `[defaults].nodes` allows;
//   6. `[defaults].default` decides: `deny`, or `op` for an operator.
// The constructed context (world, game mode, extra keys) is carried into the
// query and validated, but v1 matches every rule globally; context rules are a
// later, additive rule set.

#pragma once

#include <refrain/contracts/services/permissions.hpp>
#include <refrain/core/config.hpp>
#include <refrain/core/error.hpp>
#include <refrain/permissions/node.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::permissions {

/// Outcome of matching one node against a rule set: no rule matched, a grant
/// matched, or a deny matched. Deny precedence is applied by `node_set::match`,
/// so a set holding both a broad grant and a specific deny reports deny.
enum class rule_match : std::uint8_t { none, grant, deny };

/// One entry of a configured node set: a rule node text without the deny prefix
/// and whether the entry denies.
struct node_rule {
    std::string node{};
    bool deny{false};

    friend auto operator==(const node_rule &, const node_rule &)
        -> bool = default;
};

/// Ordered set of grant and deny rules. Order is not significant: a matching
/// deny always wins.
class node_set {
  public:
    [[nodiscard]] auto rules() const noexcept
        -> const std::vector<node_rule> & {
        return rules_;
    }

    [[nodiscard]] auto empty() const noexcept -> bool { return rules_.empty(); }

    void add(node_rule rule) { rules_.push_back(std::move(rule)); }

    /// Remove one identical rule; true when it was present.
    auto remove(const node_rule &rule) -> bool;

    /// Match one concrete node. Any matching deny reports `deny`; otherwise a
    /// matching grant reports `grant`; no match reports `none`.
    [[nodiscard]] auto match(std::string_view node) const noexcept
        -> rule_match;

  private:
    std::vector<node_rule> rules_{};
};

/// Rules attached to one subject: its own node set plus the groups it joined.
struct subject_rules {
    node_set nodes{};
    std::vector<std::string> groups{};
};

/// Fallback answer for a node no rule matched.
enum class default_answer : std::uint8_t { deny, op };

/// One immutable policy value.
struct policy {
    default_answer fallback{default_answer::deny};
    std::uint32_t op_min_level{1};
    node_set op_nodes{};
    /// Nodes every subject holds unless a matching deny exists
    /// (`[defaults].nodes`).
    node_set default_nodes{};
    /// Player identities that count as operators (`[ops].players`): canonical
    /// lowercase uuids and profile names, matched against a player subject's
    /// uuid and profile name.
    std::vector<std::string> operators{};
    /// Keyed by configured subject key, for example `player:<uuid>`.
    std::map<std::string, subject_rules, std::less<>> subjects{};
    /// Keyed by group name.
    std::map<std::string, node_set, std::less<>> groups{};
};

/// A query subject without the ABI layout: the service converts the contract
/// struct into this view before evaluating.
struct subject_view {
    services::permissions::subject_kind kind{
        services::permissions::subject_kind::player};
    std::string_view id{};
    std::string_view profile_name{};
};

/// True when `text` is a canonical lowercase player/entity uuid
/// (`8-4-4-4-12`).
[[nodiscard]] auto is_uuid(std::string_view text) noexcept -> bool;

/// True when `name` is a valid group name (`[a-z0-9][a-z0-9_-]*`, at most 64
/// bytes).
[[nodiscard]] auto valid_group_name(std::string_view name) noexcept -> bool;

/// True when `key` is a configured subject key: `console`, `player:<uuid or
/// profile name>`, `entity:<uuid>` or `plugin:<mod id>`.
[[nodiscard]] auto valid_subject_key(std::string_view key) noexcept -> bool;

/// Canonical subject key for a concrete identity. An empty string means the
/// identity is malformed and cannot be configured.
[[nodiscard]] auto make_subject_key(services::permissions::subject_kind kind,
                                    std::string_view id) -> std::string;

/// Parse one rule text (`node`, `node.*`, `*`, or any of those prefixed with
/// `!`). The context names the configuration location in the error message.
[[nodiscard]] auto parse_rule(std::string_view text, std::string_view context)
    -> result<node_rule>;

/// Validate a policy document and build its policy value. Strict: every
/// unknown table or key, invalid rule, invalid subject key and dangling group
/// reference is a `parse_error` that names the file and the offending entry.
[[nodiscard]] auto parse_policy(const core::config::document &document)
    -> result<policy>;

/// Answer one query against a policy.
///
/// `node` must be a concrete node; a malformed node is denied (fail closed)
/// even for the console. `op_level` is the vanilla operator ladder of a player
/// subject and is ignored for other subject kinds.
[[nodiscard]] auto evaluate(const policy &policy, const subject_view &subject,
                            std::string_view node, std::uint32_t op_level)
    -> services::permissions::permission_result;

/// Rule text of one configured rule, with the `!` prefix of a deny.
[[nodiscard]] auto rule_text(const node_rule &rule) -> std::string;

/// Which part of a policy decided an answer. It is diagnostic only: the
/// command frontend names the source to the operator, and `evaluate()` does not
/// track it on the query path.
enum class decision_source : std::uint8_t {
    /// The console subject holds every node before any rule is read.
    console = 0,
    /// An explicit grant in the subject's own node set.
    subject_grant,
    /// An explicit deny in the subject's own node set.
    subject_deny,
    /// A grant in a group the subject joined.
    group_grant,
    /// A deny in a group the subject joined.
    group_deny,
    /// A grant in `[op].nodes` for an operator subject.
    op_grant,
    /// A deny in `[op].nodes` for an operator subject.
    op_deny,
    /// A grant in `[defaults].nodes`.
    default_grant,
    /// A deny in `[defaults].nodes`.
    default_deny,
    /// `[defaults].default = "op"` allowed an operator.
    fallback_grant,
    /// `[defaults].default` denied a node no rule matched.
    fallback_deny,
    /// The query named a node the grammar rejects; the resolver fails closed.
    invalid_node,
};

/// One query answer plus the rule that decided it. `rule` is empty when the
/// decision came from the console rule or from the configured fallback, and
/// `group` is set only for a decision taken through a group.
struct decision_explanation {
    services::permissions::permission_result result{
        services::permissions::permission_result::denied};
    decision_source source{decision_source::fallback_deny};
    std::string rule{};
    std::string group{};
};

/// Explain one query against a policy: the same answer `evaluate()` returns,
/// together with the rule and the rule set that produced it.
[[nodiscard]] auto explain(const policy &policy, const subject_view &subject,
                           std::string_view node, std::uint32_t op_level)
    -> decision_explanation;

/// One group of the configured policy. `nodes` are the group's rule texts in
/// configuration order; `members` are the configured subject keys that join the
/// group, sorted.
struct group_listing {
    std::string name{};
    std::vector<std::string> nodes{};
    std::vector<std::string> members{};
};

/// Every `[groups]` entry of a policy, sorted by name.
[[nodiscard]] auto list_groups(const policy &policy)
    -> std::vector<group_listing>;

} // namespace refrain::permissions
