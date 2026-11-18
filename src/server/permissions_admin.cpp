// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/permissions_admin.hpp>

#include <refrain/core/log.hpp>
#include <refrain/permissions/admin.hpp>
#include <refrain/permissions/node.hpp>
#include <refrain/permissions/policy.hpp>

#include <fmt/format.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::server {

namespace {

namespace permissions = refrain::permissions;

using permissions::decision_source;
using refrain::services::permissions::permission_result;
using refrain::services::permissions::subject_kind;

constexpr refrain::log::logger log_audit{"permissions-audit"};

constexpr std::string_view usage_text{
    "usage: permissions check <subject> <node> | grant <subject> <node> | "
    "revoke <subject> <node> | groups | reload"};

[[nodiscard]] auto
elapsed_ms(const std::chrono::steady_clock::time_point &started)
    -> std::uint64_t {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started)
            .count());
}

[[nodiscard]] auto result_word(permission_result result) -> std::string_view {
    switch (result) {
    case permission_result::allowed:
        return "allowed";
    case permission_result::denied:
        return "denied";
    case permission_result::not_handled:
        return "not-handled";
    }
    return "denied";
}

/// The configured identity a command names, with the values its view points
/// at.
struct resolved_subject {
    subject_kind kind{subject_kind::player};
    std::string id{};
    std::string profile_name{};
    /// Canonical configured subject key, for example `player:<uuid>`.
    std::string key{};
    /// Operator-facing identity: the canonical key.
    std::string display{};
    std::optional<permissions_online_player> online{};
    /// True when the key is a mutable `player:<profile name>` a grant or revoke
    /// names literally, because no session or profile service resolved it to an
    /// account. A grant must not create such a key; a revoke may remove one.
    bool name_keyed{false};

    [[nodiscard]] auto view() const noexcept -> permissions::subject_view {
        return permissions::subject_view{
            .kind = kind, .id = id, .profile_name = profile_name};
    }
};

/// Resolve an online player by name or uuid through the live session table.
[[nodiscard]] auto online_player(const permissions_admin_state &state,
                                 std::string_view identity)
    -> std::optional<permissions_online_player> {
    if (!state.find_player) {
        return std::nullopt;
    }
    return state.find_player(identity);
}

/// Resolve a player who is not connected through the composition's profile
/// service, when it has one.
[[nodiscard]] auto offline_profile(const permissions_admin_state &state,
                                   std::string_view name)
    -> std::optional<permissions_player_profile> {
    if (!state.find_profile) {
        return std::nullopt;
    }
    return state.find_profile(name);
}

/// The context a query for this subject carries. Only a live session knows the
/// world and game mode; a resolved offline profile has no context.
[[nodiscard]] auto query_context(const resolved_subject &subject) noexcept
    -> permissions::context_view {
    if (!subject.online.has_value()) {
        return {};
    }
    return permissions::context_view{.world = subject.online->world,
                                     .gamemode = subject.online->gamemode};
}

/// Resolve a subject word into the identity the policy evaluates and the key a
/// change targets.
[[nodiscard]] auto resolve_subject(const permissions_admin_state &state,
                                   std::string_view text)
    -> result<resolved_subject> {
    if (text.empty()) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "the permission subject is empty")};
    }
    if (text == "console") {
        resolved_subject resolved{};
        resolved.kind = subject_kind::console;
        resolved.key = "console";
        resolved.display = "console";
        return resolved;
    }
    if (const auto colon{text.find(':')}; colon != std::string_view::npos) {
        if (!permissions::valid_subject_key(text)) {
            return std::unexpected{make_error(
                error_code::invalid_argument,
                fmt::format("'{}' is not a configurable permission subject",
                            text))};
        }
        resolved_subject resolved{};
        resolved.key = std::string{text};
        resolved.display = resolved.key;
        const auto kind_name{text.substr(0, colon)};
        const auto identity{text.substr(colon + 1)};
        resolved.id = std::string{identity};
        if (kind_name == "player") {
            resolved.kind = subject_kind::player;
            resolved.online = online_player(state, identity);
            if (resolved.online.has_value() && !resolved.online->name.empty()) {
                resolved.profile_name = resolved.online->name;
            }
            if (permissions::is_uuid(identity)) {
                // A uuid identifies the account directly; the online session
                // only adds the profile name and the query context.
                if (resolved.online.has_value() &&
                    !resolved.online->uuid.empty()) {
                    resolved.id = resolved.online->uuid;
                }
            } else {
                // A profile name is mutable. Canonicalise it to the account
                // uuid when the composition can, so a grant follows the
                // account instead of whoever holds the name next.
                resolved.profile_name = std::string{identity};
                if (resolved.online.has_value() &&
                    !resolved.online->uuid.empty()) {
                    resolved.id = resolved.online->uuid;
                } else if (auto profile{offline_profile(state, identity)};
                           profile.has_value() && !profile->uuid.empty()) {
                    resolved.id = profile->uuid;
                    if (!profile->name.empty()) {
                        resolved.profile_name = profile->name;
                    }
                } else {
                    // No session or profile service resolved the name. Keep it
                    // so a revoke can remove a legacy name-keyed rule, but
                    // remember that a grant must not create one.
                    resolved.id = std::string{identity};
                    resolved.name_keyed = true;
                }
                resolved.key = permissions::make_subject_key(
                    subject_kind::player, resolved.id);
                if (!resolved.key.empty()) {
                    resolved.display = resolved.key;
                }
            }
        } else if (kind_name == "entity") {
            resolved.kind = subject_kind::entity;
        } else {
            resolved.kind = subject_kind::plugin;
        }
        return resolved;
    }
    if (auto online{online_player(state, text)}; online.has_value()) {
        resolved_subject resolved{};
        resolved.kind = subject_kind::player;
        resolved.online = std::move(*online);
        if (!resolved.online->uuid.empty()) {
            resolved.id = resolved.online->uuid;
        } else {
            resolved.id = resolved.online->name;
        }
        resolved.profile_name = resolved.online->name;
        resolved.key =
            permissions::make_subject_key(subject_kind::player, resolved.id);
        if (resolved.key.empty()) {
            return std::unexpected{make_error(
                error_code::invalid_argument,
                fmt::format("'{}' has no configurable permission identity",
                            text))};
        }
        resolved.display = resolved.key;
        return resolved;
    }
    if (auto profile{offline_profile(state, text)}; profile.has_value()) {
        if (profile->uuid.empty()) {
            return std::unexpected{make_error(
                error_code::invalid_argument,
                fmt::format("the profile of '{}' has no canonical uuid",
                            text))};
        }
        resolved_subject resolved{};
        resolved.kind = subject_kind::player;
        resolved.id = profile->uuid;
        resolved.profile_name =
            profile->name.empty() ? std::string{text} : profile->name;
        resolved.key =
            permissions::make_subject_key(subject_kind::player, resolved.id);
        if (resolved.key.empty()) {
            return std::unexpected{make_error(
                error_code::invalid_argument,
                fmt::format("'{}' has no configurable permission identity",
                            text))};
        }
        resolved.display = resolved.key;
        return resolved;
    }
    if (state.find_profile) {
        return std::unexpected{make_error(
            error_code::not_found,
            fmt::format("no player profile is named '{}'; use console or "
                        "player:<uuid> for a subject this server cannot "
                        "resolve",
                        text))};
    }
    return std::unexpected{
        make_error(error_code::not_found,
                   fmt::format("no online player is named '{}'; use console or "
                               "player:<uuid> for a subject that is not online",
                               text))};
}

[[nodiscard]] auto context_text(const resolved_subject &subject)
    -> std::string {
    if (subject.kind == subject_kind::console) {
        return "no context";
    }
    if (!subject.online.has_value()) {
        return "no player context";
    }
    return fmt::format("world {}, game mode {}",
                       subject.online->world.empty() ? std::string{"unknown"}
                                                     : subject.online->world,
                       subject.online->gamemode.empty()
                           ? std::string{"unknown"}
                           : subject.online->gamemode);
}

[[nodiscard]] auto source_text(const permissions::decision_explanation &answer)
    -> std::string {
    std::string text{};
    switch (answer.source) {
    case decision_source::console:
        text = "the console holds every node";
        break;
    case decision_source::subject_grant:
        text = "grant";
        break;
    case decision_source::subject_deny:
        text = "deny";
        break;
    case decision_source::group_grant:
        text = fmt::format("grant from group '{}'", answer.group);
        break;
    case decision_source::group_deny:
        text = fmt::format("deny from group '{}'", answer.group);
        break;
    case decision_source::context_grant:
        text = answer.group.empty()
                   ? fmt::format("grant in context '{}'", answer.context)
                   : fmt::format("grant from group '{}' in context '{}'",
                                 answer.group, answer.context);
        break;
    case decision_source::context_deny:
        text = answer.group.empty()
                   ? fmt::format("deny in context '{}'", answer.context)
                   : fmt::format("deny from group '{}' in context '{}'",
                                 answer.group, answer.context);
        break;
    case decision_source::op_grant:
        text = "operator node";
        break;
    case decision_source::op_deny:
        text = "operator deny";
        break;
    case decision_source::default_grant:
        text = "public default";
        break;
    case decision_source::default_deny:
        text = "public default deny";
        break;
    case decision_source::fallback_grant:
        text = "default policy allows operators";
        break;
    case decision_source::fallback_deny:
        text = "default policy denies";
        break;
    case decision_source::invalid_node:
        text = "invalid node";
        break;
    }
    if (!answer.rule.empty()) {
        text += fmt::format(", rule '{}'", answer.rule);
    }
    return text;
}

void write_audit(const permissions_admin_state &state,
                 permissions_audit_record record) {
    if (state.audit) {
        state.audit(record);
        return;
    }
    log_audit.info("{}", format_permissions_audit_record(record));
}

[[nodiscard]] auto make_record(const permissions_actor &actor,
                               std::string action) -> permissions_audit_record {
    permissions_audit_record record{};
    record.actor_kind =
        actor.kind == permissions_actor_kind::console ? "console" : "player";
    record.actor_name = actor.name;
    record.actor_uuid = actor.uuid;
    record.action = std::move(action);
    return record;
}

[[nodiscard]] auto current_revision(const permissions_admin_state &state)
    -> std::uint64_t {
    return state.admin == nullptr ? 0U : state.admin->revision();
}

/// `permissions check <subject> <node>`.
[[nodiscard]] auto run_check(const permissions_admin_state &state,
                             std::span<const std::string_view> tokens)
    -> permissions_command_report {
    permissions_command_report report{};
    if (tokens.size() != 3) {
        report.ok = false;
        report.error = "usage: permissions check <subject> <node>";
        return report;
    }
    auto subject{resolve_subject(state, tokens[1])};
    if (!subject) {
        report.ok = false;
        report.error = subject.error().message;
        return report;
    }
    const auto node{tokens[2]};
    if (!permissions::valid_node(node)) {
        report.ok = false;
        report.error =
            fmt::format("'{}' is not a concrete permission node", node);
        return report;
    }
    if (state.admin == nullptr && !state.query) {
        report.lines.push_back(fmt::format(
            "not-handled '{}' for {}: no permission service with an "
            "administration surface is active",
            node, subject->display));
        return report;
    }

    // The chain-aware query is authoritative when the composition provides it;
    // the administration surface of the base resolver names the deciding rule
    // for the answers the base took. A wrapper answer therefore reports that a
    // wrapper decided and what the base would have answered.
    const auto context{query_context(*subject)};
    auto answer{permission_result::not_handled};
    auto have_answer{false};
    if (state.query) {
        answer = state.query(subject->view(), node, context);
        have_answer = true;
    }
    permissions::decision_explanation explanation{};
    if (state.admin != nullptr) {
        explanation = state.admin->explain(subject->view(), node, 0, context);
        if (!have_answer) {
            answer = explanation.result;
            have_answer = true;
        }
    }
    if (!have_answer) {
        report.lines.push_back(fmt::format(
            "not-handled '{}' for {}: no permission provider answered", node,
            subject->display));
        return report;
    }

    std::string detail{};
    if (state.admin == nullptr) {
        detail = "the active permission provider answered; this composition "
                 "has no administration surface";
    } else if (explanation.result == answer) {
        detail = source_text(explanation);
    } else {
        detail = fmt::format(
            "a wrapper provider answered; the base resolver alone would "
            "report {} ({})",
            result_word(explanation.result), source_text(explanation));
    }
    report.lines.push_back(
        fmt::format("{} '{}' for {} ({}; context {})", result_word(answer),
                    node, subject->display, detail, context_text(*subject)));
    return report;
}

/// `permissions grant|revoke <subject> <node>`.
[[nodiscard]] auto run_change(const permissions_admin_state &state,
                              std::span<const std::string_view> tokens,
                              const permissions_actor &actor, bool add)
    -> permissions_command_report {
    const auto started{std::chrono::steady_clock::now()};
    permissions_command_report report{};
    auto audit{make_record(actor, add ? "grant" : "revoke")};
    const auto finish = [&state, &report, &audit,
                         started](std::string result, std::string detail,
                                  std::uint64_t revision) {
        audit.result = std::move(result);
        audit.detail = std::move(detail);
        audit.revision = revision;
        audit.duration_ms = elapsed_ms(started);
        write_audit(state, audit);
        return report;
    };
    if (tokens.size() != 3) {
        report.ok = false;
        report.error = add ? "usage: permissions grant <subject> <node>"
                           : "usage: permissions revoke <subject> <node>";
        return finish("invalid", report.error, current_revision(state));
    }
    auto subject{resolve_subject(state, tokens[1])};
    if (!subject) {
        report.ok = false;
        report.error = subject.error().message;
        return finish("invalid", report.error, current_revision(state));
    }
    if (add && subject->name_keyed) {
        // A grant must target an account, not a mutable profile name: another
        // account can claim the name after a rename. A revoke still accepts the
        // literal key so a legacy name-keyed rule can be removed.
        report.ok = false;
        report.error = fmt::format(
            "the profile name in '{}' does not resolve to an account on this "
            "server; grant player:<uuid> instead",
            tokens[1]);
        return finish("invalid", report.error, current_revision(state));
    }
    const auto rule{tokens[2]};
    auto parsed_rule{permissions::parse_rule(rule, "permission rule")};
    if (!parsed_rule) {
        report.ok = false;
        report.error = parsed_rule.error().message;
        return finish("invalid", report.error, current_revision(state));
    }
    audit.subject = subject->key;
    audit.node = permissions::rule_text(*parsed_rule);
    if (state.admin == nullptr) {
        report.ok = false;
        report.error =
            "no permission service with an administration surface is active; "
            "falling back to the declared op levels means there is no policy "
            "to change";
        return finish("failed", "no permission administration surface",
                      current_revision(state));
    }
    auto change{add ? state.admin->grant(subject->key, audit.node)
                    : state.admin->revoke(subject->key, audit.node)};
    if (!change) {
        report.ok = false;
        report.error = change.error().message;
        const auto result{change.error().code == error_code::unsupported
                              ? "not-persistent"
                              : "failed"};
        return finish(result, report.error, current_revision(state));
    }
    if (!change->changed) {
        report.lines.push_back(
            fmt::format("the rule '{}' is already {} {} at revision {}",
                        change->rule, add ? "granted to" : "absent from",
                        subject->display, change->revision));
        return finish("unchanged", "", change->revision);
    }
    report.lines.push_back(fmt::format(
        "{} '{}' {} {} at revision {}", add ? "granted" : "revoked",
        change->rule, add ? "to" : "from", subject->display, change->revision));
    return finish("ok", "", change->revision);
}

/// `permissions groups`.
[[nodiscard]] auto run_groups(const permissions_admin_state &state)
    -> permissions_command_report {
    permissions_command_report report{};
    if (state.admin == nullptr) {
        report.ok = false;
        report.error =
            "no permission service with an administration surface is active";
        return report;
    }
    const auto groups{state.admin->groups()};
    if (groups.empty()) {
        report.lines.push_back("no permission groups are configured");
        return report;
    }
    report.lines.push_back(
        fmt::format("permission groups ({}):", groups.size()));
    for (const auto &group : groups) {
        std::string nodes{};
        for (const auto &node : group.nodes) {
            if (!nodes.empty()) {
                nodes += ", ";
            }
            nodes += node;
        }
        std::string members{};
        for (const auto &member : group.members) {
            if (!members.empty()) {
                members += ", ";
            }
            members += member;
        }
        report.lines.push_back(
            fmt::format("group '{}': nodes [{}]; members [{}]", group.name,
                        nodes.empty() ? std::string{"none"} : nodes,
                        members.empty() ? std::string{"none"} : members));
    }
    return report;
}

/// `permissions reload`.
[[nodiscard]] auto run_reload(const permissions_admin_state &state,
                              const permissions_actor &actor)
    -> permissions_command_report {
    const auto started{std::chrono::steady_clock::now()};
    permissions_command_report report{};
    auto audit{make_record(actor, "reload")};
    const auto finish = [&state, &report, &audit,
                         started](std::string result, std::string detail,
                                  std::uint64_t revision) {
        audit.result = std::move(result);
        audit.detail = std::move(detail);
        audit.revision = revision;
        audit.duration_ms = elapsed_ms(started);
        write_audit(state, audit);
        return report;
    };
    if (state.admin == nullptr) {
        report.ok = false;
        report.error =
            "no permission service with an administration surface is active";
        return finish("failed", "no permission administration surface", 0);
    }
    auto reloaded{state.admin->reload()};
    if (!reloaded) {
        report.ok = false;
        report.error = fmt::format("failed to reload the permission policy: {}",
                                   reloaded.error().message);
        return finish("failed", "the configuration could not be reloaded",
                      current_revision(state));
    }
    if (!reloaded->changed) {
        report.lines.push_back(
            fmt::format("the permission policy is unchanged at revision {}",
                        reloaded->revision));
        return finish("unchanged", "", reloaded->revision);
    }
    report.lines.push_back(
        fmt::format("reloaded the permission policy: revision {} -> {}",
                    reloaded->previous_revision, reloaded->revision));
    return finish("ok", "", reloaded->revision);
}

} // namespace

auto install_offline_profile_lookup(
    permissions_admin_state &state, bool online_mode,
    session_profile_resolver resolve_offline_profile) -> bool {
    state.find_profile = {};
    if (online_mode || !resolve_offline_profile) {
        return false;
    }
    state.find_profile = std::move(resolve_offline_profile);
    return true;
}

auto format_permissions_audit_record(const permissions_audit_record &record)
    -> std::string {
    std::string out{record.actor_kind.empty() ? "unknown" : record.actor_kind};
    if (!record.actor_uuid.empty()) {
        out += fmt::format(" {} ({})", record.actor_uuid,
                           record.actor_name.empty() ? "unnamed"
                                                     : record.actor_name);
    } else if (!record.actor_name.empty()) {
        out += fmt::format(" {}", record.actor_name);
    }
    out += fmt::format(" ran {}", record.action);
    if (!record.subject.empty()) {
        out += fmt::format(" on {}", record.subject);
    }
    if (!record.node.empty()) {
        out += fmt::format(" for node '{}'", record.node);
    }
    out += fmt::format(" at revision {}, the result was {}", record.revision,
                       record.result);
    if (!record.detail.empty()) {
        out += fmt::format(": {}", record.detail);
    }
    out += fmt::format(" after {}ms", record.duration_ms);
    return out;
}

auto execute_permissions_command(const permissions_admin_state &state,
                                 std::span<const std::string_view> tokens,
                                 const permissions_actor &actor)
    -> permissions_command_report {
    permissions_command_report report{};
    if (tokens.empty()) {
        report.lines.emplace_back(usage_text);
        return report;
    }
    const auto command{tokens[0]};
    if (command == "help") {
        report.lines.emplace_back(usage_text);
        return report;
    }
    if (command == "check") {
        return run_check(state, tokens);
    }
    if (command == "grant") {
        return run_change(state, tokens, actor, true);
    }
    if (command == "revoke") {
        return run_change(state, tokens, actor, false);
    }
    if (command == "groups") {
        if (tokens.size() != 1) {
            report.ok = false;
            report.error = "usage: permissions groups";
            return report;
        }
        return run_groups(state);
    }
    if (command == "reload") {
        if (tokens.size() != 1) {
            report.ok = false;
            report.error = "usage: permissions reload";
            return report;
        }
        return run_reload(state, actor);
    }
    report.ok = false;
    report.error = fmt::format(
        "unknown permissions command '{}'; expected check, grant, revoke, "
        "groups or reload",
        command);
    return report;
}

} // namespace refrain::server
