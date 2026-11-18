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
//   players = ["<uuid>", "Notch"]   # identities that count as operators
//
//   [grants]
//   "player:<uuid>" = ["refrain.mods.admin", "!refrain.command.stop"]
//   "plugin:com.example.mymod" = ["@moderators"]
//
//   [groups]
//   moderators = ["refrain.command.*", "!refrain.permissions.admin"]
//
//   [contexts]
//   creative = { gamemode = "creative" }
//   nether = { world = "minecraft:the_nether" }
//
//   [context_grants]
//   creative = { "player:<uuid>" = ["!refrain.command.gamemode"] }
//
// A group is a node set; a subject joins a group with an `@name` entry in
// `[grants]`. Groups are single-level in v1: a group list may not reference
// another group. Unknown tables, unknown keys, invalid nodes, invalid subject
// keys, unknown groups and unknown context references are configuration errors
// rather than silently ignored text, so a typo cannot widen an access rule.
//
// `[ops].players` is the operator source the built-in resolver owns: a player
// named by canonical uuid or profile name is treated as an operator and holds
// `[op].nodes` even though no vanilla op list is persisted yet.
// `[op].min_level` stays the ladder rule for a host that supplies a real op
// level in the query.
//
// A profile name is accepted for compatibility with an existing policy, but a
// name identifies whoever holds it rather than the account: an online-mode
// player who renames can pass a `[ops].players` entry or a `player:<name>`
// grant to the account that claims the old name. Every name-keyed entry emits a
// load-time warning that names the entry and recommends the uuid form.
//
// Resolution order for one query:
//   1. console holds every node;
//   2. a matching deny anywhere denies (deny wins over every grant);
//   3. a matching grant allows;
//   4. an operator's `[op].nodes` grant allows, where an operator is a subject
//      listed in `[ops].players` or a player at or above `[op].min_level`;
//   5. a grant in `[defaults].nodes` allows;
//   6. `[defaults].default` decides: `deny`, or `op` for an operator.
//
// Context rules are additive to that order. `[contexts]` declares named
// predicates over the query context (world, game mode and extra
// provider-defined keys), and `[context_grants]` attaches rule sets to a
// subject under one context. Every declared context whose predicate matches the
// query applies, and its rules join the subject's rule sets in the same
// deny-wins lattice:
//   - a deny in any applicable set, global or contextual, denies;
//   - otherwise a grant in any applicable set allows;
//   - otherwise the operator, public-default and `[defaults].default` rules
//     decide as above.
// A context rule therefore narrows or extends one subject's access for a
// matching context but cannot override a deny. A context rule may join a global
// `[groups]` entry; groups stay single-level and their rules are global. When
// several contexts match they are scanned in name order, and `explain()`
// reports the first matching deny (or the first matching grant) in that order.
//
// A malformed query context (an empty or oversized key or value, more entries
// than the limit, a reserved key in the extra slice, or a duplicate extra key)
// is rejected by the resolver instead of being matched: a query the resolver
// cannot interpret fails closed.

#pragma once

#include <refrain/contracts/services/permissions.hpp>
#include <refrain/core/config.hpp>
#include <refrain/core/error.hpp>
#include <refrain/permissions/node.hpp>

#include <cstdint>
#include <map>
#include <span>
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

/// Longest accepted context key or value in bytes. The cap keeps a query
/// context cheap to compare and stops a malformed caller from growing a match
/// walk without bound.
inline constexpr std::size_t max_context_length = 255;

/// Largest number of extra context entries one query may carry.
inline constexpr std::size_t max_context_entries = 32;

/// One key/value pair of a query context supplied by the caller, for example
/// `dimension_type = "overworld"`.
struct context_value_view {
    std::string_view key{};
    std::string_view value{};
};

/// Context carried by one permission query: the subject's world and game mode
/// plus provider-defined keys. A query without a context is matched by no
/// context rule.
struct context_view {
    std::string_view world{};
    std::string_view gamemode{};
    std::span<const context_value_view> extra{};
};

/// True when the context is structurally usable: the world, game mode, keys and
/// values are within `max_context_length`, no extra key is empty, reserved or
/// duplicated, and the extra slice is within `max_context_entries`. A malformed
/// context is denied by the provider instead of being matched.
[[nodiscard]] auto valid_context(const context_view &context) noexcept -> bool;

/// One condition of a named context: the query context must carry `value` for
/// `key`. `world` and `gamemode` address the dedicated fields of the query
/// context; every other key addresses an extra entry.
struct context_condition {
    std::string key{};
    std::string value{};

    friend auto operator==(const context_condition &, const context_condition &)
        -> bool = default;
};

/// Named context predicate: every condition must match the query context for
/// the context to apply. Conditions are stored in configuration order.
struct context_definition {
    std::vector<context_condition> conditions{};
};

/// Rules attached to one configured subject under one context, keyed by the
/// configured subject key (`player:<uuid>`, ...).
using context_subject_rules = std::map<std::string, subject_rules, std::less<>>;

/// True when every condition of `definition` matches `context`.
[[nodiscard]] auto context_matches(const context_definition &definition,
                                   const context_view &context) noexcept
    -> bool;

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
    /// uuid and profile name. A profile name is a compatibility fallback and
    /// warns at load, because another account can hold the name after a rename.
    std::vector<std::string> operators{};
    /// Keyed by configured subject key, for example `player:<uuid>`.
    std::map<std::string, subject_rules, std::less<>> subjects{};
    /// Keyed by group name.
    std::map<std::string, node_set, std::less<>> groups{};
    /// Named context predicates (`[contexts]`), keyed by context name.
    std::map<std::string, context_definition, std::less<>> contexts{};
    /// Context-qualified rules (`[context_grants]`): context name, then
    /// configured subject key.
    std::map<std::string, context_subject_rules, std::less<>>
        context_subjects{};
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

/// True when `name` is a valid context name (`[a-z0-9][a-z0-9_-]*`, at most 64
/// bytes), the name a `[context_grants]` entry uses to reference `[contexts]`.
[[nodiscard]] auto valid_context_name(std::string_view name) noexcept -> bool;

/// True when `key` is a valid context condition key: `world`, `gamemode` or an
/// extra key (`[a-z0-9][a-z0-9_.-]*`, at most 64 bytes).
[[nodiscard]] auto valid_context_key(std::string_view key) noexcept -> bool;

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
/// subject and is ignored for other subject kinds. `context` is the caller's
/// query context; an empty context matches no `[contexts]` predicate, so a
/// policy without context rules answers the same way for every context.
[[nodiscard]] auto evaluate(const policy &policy, const subject_view &subject,
                            std::string_view node, std::uint32_t op_level,
                            const context_view &context = {})
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
    /// A grant in a matching context's rule set.
    context_grant,
    /// A deny in a matching context's rule set.
    context_deny,
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
/// decision came from the console rule or from the configured fallback,
/// `group` is set only for a decision taken through a group and `context` only
/// for a decision taken through a matching context.
struct decision_explanation {
    services::permissions::permission_result result{
        services::permissions::permission_result::denied};
    decision_source source{decision_source::fallback_deny};
    std::string rule{};
    std::string group{};
    std::string context{};
};

/// Explain one query against a policy: the same answer `evaluate()` returns,
/// together with the rule and the rule set that produced it.
[[nodiscard]] auto explain(const policy &policy, const subject_view &subject,
                           std::string_view node, std::uint32_t op_level,
                           const context_view &context = {})
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
