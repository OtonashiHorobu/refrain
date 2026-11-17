// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Default resolver policy: strict configuration parsing and the resolution
// order (console, deny, grant, operator nodes, fallback).

#include <refrain/permissions/policy.hpp>

#include <refrain/core/config.hpp>
#include <refrain/core/error.hpp>

#include <fmt/format.h>

#include <boost/ut.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace boost::ut;

namespace config = refrain::core::config;
namespace permissions = refrain::permissions;
using refrain::services::permissions::permission_result;
using refrain::services::permissions::subject_kind;

constexpr std::string_view player_uuid{"069a79f4-44e9-4726-a5be-fca90e38aaf5"};
constexpr std::string_view other_uuid{"11111111-2222-3333-4444-555555555555"};
constexpr std::string_view entity_uuid{"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"};

[[nodiscard]] auto parse_document(std::string_view text)
    -> refrain::result<permissions::policy> {
    auto document{config::document::parse_text(text, "permissions.toml")};
    if (!document) {
        return std::unexpected{document.error()};
    }
    return permissions::parse_policy(*document);
}

[[nodiscard]] auto parse_ok(std::string_view text) -> permissions::policy {
    auto parsed{parse_document(text)};
    expect(static_cast<bool>(parsed))
        << (parsed ? std::string{} : parsed.error().message);
    if (!parsed) {
        return {};
    }
    return std::move(*parsed);
}

/// Every rejection is expected to name the offending key or value.
[[nodiscard]] auto parse_error(std::string_view text) -> std::string {
    auto parsed{parse_document(text)};
    expect(!parsed) << "expected the configuration to be rejected";
    if (parsed) {
        return {};
    }
    expect(parsed.error().code == refrain::error_code::parse_error);
    return parsed.error().message;
}

[[nodiscard]] auto answer(const permissions::policy &policy, subject_kind kind,
                          std::string_view id, std::string_view node,
                          std::uint32_t op_level = 0,
                          std::string_view profile_name = {})
    -> permission_result {
    return permissions::evaluate(
        policy,
        permissions::subject_view{
            .kind = kind, .id = id, .profile_name = profile_name},
        node, op_level);
}

[[nodiscard]] auto answer(const permissions::policy &policy,
                          std::string_view node, std::uint32_t op_level = 0)
    -> permission_result {
    return answer(policy, subject_kind::player, player_uuid, node, op_level);
}

const suite policy_tests = [] {
    "policy/an empty document is deny-by-default with console access"_test =
        [] {
            const auto policy{parse_ok("")};
            expect(policy.fallback == permissions::default_answer::deny);
            expect(policy.op_min_level == 1U);
            expect(policy.op_nodes.empty());
            expect(policy.subjects.empty());
            expect(policy.groups.empty());
            expect(answer(policy, "refrain.command.teleport") ==
                   permission_result::denied);
            expect(answer(policy, subject_kind::console, {}, "a.b") ==
                   permission_result::allowed);
        };

    "policy/console holds every node even when rules deny them"_test = [] {
        const auto policy{parse_ok(R"(
[grants]
console = ["!refrain.command.teleport"]
[groups]
everyone = ["*"]
)")};
        expect(answer(policy, subject_kind::console, {},
                      "refrain.command.teleport") ==
               permission_result::allowed);
        expect(answer(policy, subject_kind::console, {}, "a.b") ==
               permission_result::allowed);
    };

    "policy/an invalid query node is denied even for the console"_test = [] {
        const auto policy{parse_ok("")};
        expect(answer(policy, subject_kind::console, {}, "") ==
               permission_result::denied);
        expect(answer(policy, subject_kind::console, {}, "Refrain.Command") ==
               permission_result::denied);
        expect(answer(policy, subject_kind::console, {}, "refrain.command.*") ==
               permission_result::denied);
        expect(answer(policy, subject_kind::console, {}, "!refrain.command") ==
               permission_result::denied);
    };

    "policy/an exact grant allows and other nodes stay denied"_test = [] {
        const auto policy{parse_ok(R"(
[grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["refrain.command.teleport"]
)")};
        expect(answer(policy, "refrain.command.teleport") ==
               permission_result::allowed);
        expect(answer(policy, "refrain.command.gamemode") ==
               permission_result::denied);
    };

    "policy/a wildcard grant covers descendants only"_test = [] {
        const auto policy{parse_ok(R"(
[grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["refrain.command.*"]
)")};
        expect(answer(policy, "refrain.command.teleport") ==
               permission_result::allowed);
        expect(answer(policy, "refrain.command.gamemode.creative") ==
               permission_result::allowed);
        expect(answer(policy, "refrain.mods.admin") ==
               permission_result::denied);
    };

    "policy/a deny wins over a broader grant"_test = [] {
        const auto policy{parse_ok(R"(
[grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["refrain.command.*", "!refrain.command.stop"]
)")};
        expect(answer(policy, "refrain.command.teleport") ==
               permission_result::allowed);
        expect(answer(policy, "refrain.command.stop") ==
               permission_result::denied);
    };

    "policy/an allow-all grant is narrowed by one deny"_test = [] {
        const auto policy{parse_ok(R"(
[grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["*", "!refrain.mods.admin"]
)")};
        expect(answer(policy, "refrain.command.teleport") ==
               permission_result::allowed);
        expect(answer(policy, "refrain.mods.admin") ==
               permission_result::denied);
    };

    "policy/groups grant their node set and deny wins over a direct grant"_test =
        [] {
            const auto policy{parse_ok(R"(
[grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["@moderators", "refrain.command.stop"]
[groups]
moderators = ["refrain.command.*", "!refrain.command.stop"]
)")};
            expect(answer(policy, "refrain.command.teleport") ==
                   permission_result::allowed);
            // The group deny beats the subject's own grant.
            expect(answer(policy, "refrain.command.stop") ==
                   permission_result::denied);
        };

    "policy/a player grant applies by uuid and by profile name"_test = [] {
        const auto policy{parse_ok(R"(
[grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["refrain.mods.admin"]
"player:Alex" = ["refrain.command.teleport"]
)")};
        expect(answer(policy, "refrain.mods.admin") ==
               permission_result::allowed);
        expect(answer(policy, subject_kind::player, other_uuid,
                      "refrain.command.teleport", 0,
                      "Alex") == permission_result::allowed);
        expect(answer(policy, subject_kind::player, other_uuid,
                      "refrain.command.teleport", 0,
                      "Steve") == permission_result::denied);
    };

    "policy/a uuid deny wins over a profile name grant"_test = [] {
        const auto policy{parse_ok(R"(
[grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["!refrain.command.stop"]
"player:Alex" = ["refrain.command.stop"]
)")};
        expect(answer(policy, subject_kind::player, player_uuid,
                      "refrain.command.stop", 0,
                      "Alex") == permission_result::denied);
    };

    "policy/entity and plugin subjects use their own grant keys"_test = [] {
        const auto policy{parse_ok(R"(
[grants]
"entity:aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee" = ["refrain.entity.*"]
"plugin:com.example.mymod" = ["refrain.world.edit"]
)")};
        expect(answer(policy, subject_kind::entity, entity_uuid,
                      "refrain.entity.spawn") == permission_result::allowed);
        expect(answer(policy, subject_kind::plugin, "com.example.mymod",
                      "refrain.world.edit") == permission_result::allowed);
        expect(answer(policy, subject_kind::plugin, "com.example.othermod",
                      "refrain.world.edit") == permission_result::denied);
    };

    "policy/operator nodes grant below the fallback"_test = [] {
        const auto policy{parse_ok(R"(
[defaults]
default = "deny"
[op]
min_level = 1
nodes = ["refrain.mods.admin"]
)")};
        expect(answer(policy, "refrain.mods.admin", 1) ==
               permission_result::allowed);
        expect(answer(policy, "refrain.mods.admin", 0) ==
               permission_result::denied);
        expect(answer(policy, "refrain.command.teleport", 4) ==
               permission_result::denied);
    };

    "policy/the op fallback follows the configured minimum level"_test = [] {
        const auto policy{parse_ok(R"(
[defaults]
default = "op"
[op]
min_level = 4
)")};
        expect(answer(policy, "refrain.mods.admin", 4) ==
               permission_result::allowed);
        expect(answer(policy, "refrain.mods.admin", 3) ==
               permission_result::denied);
        expect(answer(policy, "refrain.mods.admin", 0) ==
               permission_result::denied);
    };

    "policy/op nodes still apply under the deny fallback"_test = [] {
        const auto policy{parse_ok(R"(
[defaults]
default = "deny"
[op]
min_level = 2
nodes = ["!refrain.command.stop", "refrain.command.*"]
)")};
        expect(answer(policy, "refrain.command.teleport", 2) ==
               permission_result::allowed);
        expect(answer(policy, "refrain.command.stop", 2) ==
               permission_result::denied);
        expect(answer(policy, "refrain.command.teleport", 1) ==
               permission_result::denied);
    };

    "policy/op rules do not apply to non-player subjects"_test = [] {
        const auto policy{parse_ok(R"(
[op]
nodes = ["*"]
)")};
        expect(answer(policy, subject_kind::entity, entity_uuid,
                      "refrain.entity.spawn", 4) == permission_result::denied);
        expect(answer(policy, subject_kind::plugin, "com.example.mymod",
                      "refrain.world.edit", 4) == permission_result::denied);
    };

    "policy/a parsed document exposes its rule sets"_test = [] {
        const auto policy{parse_ok(R"(
[defaults]
default = "op"
[op]
min_level = 2
nodes = ["refrain.mods.*"]
[grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["@moderators", "refrain.permissions.admin"]
[groups]
moderators = ["refrain.command.*"]
)")};
        expect(policy.fallback == permissions::default_answer::op);
        expect(policy.op_min_level == 2U);
        expect(policy.op_nodes.rules().size() == 1U);
        expect(policy.groups.size() == 1U);
        expect(policy.subjects.size() == 1U);
        const auto subject{policy.subjects.find(
            "player:069a79f4-44e9-4726-a5be-fca90e38aaf5")};
        expect(subject != policy.subjects.end());
        if (subject != policy.subjects.end()) {
            expect(subject->second.groups.size() == 1U);
            expect(subject->second.groups.front() == "moderators");
            expect(subject->second.nodes.rules().size() == 1U);
        }
    };

    "policy/unknown tables are rejected"_test = [] {
        const auto message = parse_error(R"(
[default]
default = "deny"
)");
        expect(message.find("default") != std::string::npos);
    };

    "policy/unknown keys are rejected"_test = [] {
        expect(parse_error(R"(
[defaults]
fallback = "allow"
)")
                   .find("fallback") != std::string::npos);
        expect(parse_error(R"(
[op]
level = 2
)")
                   .find("level") != std::string::npos);
    };

    "policy/invalid rule nodes are rejected"_test = [] {
        expect(parse_error(R"(
[grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["Refrain.Command.Teleport"]
)")
                   .find("Refrain.Command.Teleport") != std::string::npos);
        expect(parse_error(R"(
[grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["refrain.command.*.stop"]
)")
                   .find("refrain.command.*.stop") != std::string::npos);
        expect(parse_error(R"(
[groups]
moderators = ["!"]
)")
                   .find("deny prefix") != std::string::npos);
    };

    "policy/invalid subject keys are rejected"_test = [] {
        expect(parse_error(R"(
[grants]
"player:" = ["refrain.command.*"]
)")
                   .find("player:") != std::string::npos);
        expect(parse_error(R"(
[grants]
"world:minecraft:overworld" = ["refrain.command.*"]
)")
                   .find("world:minecraft:overworld") != std::string::npos);
        expect(parse_error(R"(
[grants]
"entity:not-a-uuid" = ["refrain.command.*"]
)")
                   .find("entity:not-a-uuid") != std::string::npos);
    };

    "policy/an unknown group reference is rejected"_test = [] {
        const auto message = parse_error(R"(
[grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["@missing"]
)");
        expect(message.find("missing") != std::string::npos);
    };

    "policy/nested groups are rejected"_test = [] {
        const auto message = parse_error(R"(
[groups]
base = ["refrain.command.*"]
extended = ["@base"]
)");
        expect(message.find("single-level") != std::string::npos);
    };

    "policy/invalid fallback values are rejected"_test = [] {
        expect(parse_error(R"(
[defaults]
default = "allow"
)")
                   .find("allow") != std::string::npos);
        expect(parse_error(R"(
[defaults]
default = "deny"
[op]
min_level = 5
)")
                   .find("min_level") != std::string::npos);
    };

    "policy/wrong value types are rejected"_test = [] {
        expect(parse_error(R"(
[grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = "refrain.command.*"
)")
                   .find("array") != std::string::npos);
        expect(parse_error(R"(
[groups]
moderators = 7
)")
                   .find("array") != std::string::npos);
        expect(parse_error(R"(
[op]
min_level = "one"
)")
                   .find("integer") != std::string::npos);
        expect(parse_error(R"(
[defaults]
default = 1
)")
                   .find("string") != std::string::npos);
    };

    "policy/default nodes give every subject the public command surface"_test =
        [] {
            const auto policy{parse_ok(R"(
[defaults]
default = "deny"
nodes = ["refrain.command.help", "refrain.command.list"]
)")};
            expect(policy.default_nodes.rules().size() == 2U);
            expect(answer(policy, "refrain.command.help") ==
                   permission_result::allowed);
            expect(answer(policy, subject_kind::player, other_uuid,
                          "refrain.command.list", 0,
                          "Alex") == permission_result::allowed);
            // Everything the default list does not name stays denied.
            expect(answer(policy, "refrain.command.gamemode") ==
                   permission_result::denied);
            expect(answer(policy, subject_kind::plugin, "com.example.mymod",
                          "refrain.command.help") ==
                   permission_result::allowed);
        };

    "policy/a deny in the default nodes beats a subject grant"_test = [] {
        const auto policy{parse_ok(R"(
[defaults]
nodes = ["!refrain.command.stop"]
[grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["refrain.command.*"]
)")};
        expect(answer(policy, "refrain.command.teleport") ==
               permission_result::allowed);
        expect(answer(policy, "refrain.command.stop") ==
               permission_result::denied);
    };

    "policy/an operator from the ops list holds the op nodes at level zero"_test =
        [] {
            const auto policy{parse_ok(R"(
[defaults]
default = "deny"
[op]
min_level = 1
nodes = ["refrain.mods.admin"]
[ops]
players = ["069a79f4-44e9-4726-a5be-fca90e38aaf5", "Alex"]
)")};
            expect(policy.operators.size() == 2U);
            // The uuid entry counts even though the query op level is zero.
            expect(answer(policy, "refrain.mods.admin", 0) ==
                   permission_result::allowed);
            expect(answer(policy, subject_kind::player, other_uuid,
                          "refrain.mods.admin", 0,
                          "Alex") == permission_result::allowed);
            expect(answer(policy, subject_kind::player, other_uuid,
                          "refrain.mods.admin") == permission_result::denied);
            // The op node set only grants what it lists.
            expect(answer(policy, "refrain.command.gamemode", 0) ==
                   permission_result::denied);
        };

    "policy/an ops player still wins the op fallback"_test = [] {
        const auto policy{parse_ok(R"(
[defaults]
default = "op"
[ops]
players = ["Alex"]
)")};
        expect(answer(policy, subject_kind::player, other_uuid, "a.b", 0,
                      "Alex") == permission_result::allowed);
        expect(answer(policy, "a.b", 0) == permission_result::denied);
    };

    "policy/an ops entry that is not a player identity is rejected"_test = [] {
        for (const auto *entry : {"\"not a name\"", "\"player:Alex\"",
                                  "\"069A79F4-44E9-4726-A5BE-FCA90E38AAF5\""}) {
            const auto message{parse_error(fmt::format(R"(
[ops]
players = [{}]
)",
                                                       entry))};
            expect(message.find("ops.players") != std::string::npos) << message;
        }
        expect(parse_error(R"(
[ops]
unknown = []
)")
                   .find("unknown") != std::string::npos);
        expect(parse_error(R"(
[ops]
players = "Alex"
)")
                   .find("array") != std::string::npos);
    };

    "policy/key checks are order independent"_test = [] {
        // Groups may be declared after the grant that references them; the
        // parser resolves the reference against the whole document.
        const auto policy{parse_ok(R"(
[grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["@late"]
[groups]
late = ["refrain.command.*"]
)")};
        expect(answer(policy, "refrain.command.teleport") ==
               permission_result::allowed);
    };
};

const suite explanation_tests = [] {
    "explanation/names the rule set that decided the answer"_test = [] {
        const auto loaded{parse_ok(R"(
[defaults]
default = "deny"
nodes = ["refrain.command.help"]
[op]
min_level = 2
nodes = ["refrain.command.gamemode"]
[grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["refrain.command.*", "!refrain.command.stop", "@moderators"]
"entity:aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee" = ["entity.node"]
[groups]
moderators = ["refrain.mods.admin", "!refrain.mods.trust"]
)")};
        const auto view =
            permissions::subject_view{.kind = subject_kind::player,
                                      .id = player_uuid,
                                      .profile_name = {}};
        const auto explain = [&loaded, &view](std::string_view node,
                                              std::uint32_t op_level = 0) {
            return permissions::explain(loaded, view, node, op_level);
        };
        auto answer{explain("refrain.command.teleport")};
        expect(answer.result == permission_result::allowed);
        expect(answer.source == permissions::decision_source::subject_grant);
        expect(answer.rule == "refrain.command.*");
        answer = explain("refrain.command.stop");
        expect(answer.result == permission_result::denied);
        expect(answer.source == permissions::decision_source::subject_deny);
        expect(answer.rule == "!refrain.command.stop");
        answer = explain("refrain.mods.admin");
        expect(answer.result == permission_result::allowed);
        expect(answer.source == permissions::decision_source::group_grant);
        expect(answer.group == "moderators");
        expect(answer.rule == "refrain.mods.admin");
        answer = explain("refrain.mods.trust");
        expect(answer.result == permission_result::denied);
        expect(answer.source == permissions::decision_source::group_deny);
        expect(answer.rule == "!refrain.mods.trust");
        // A subject without a matching grant falls through to the public
        // default and to the configured fallback.
        const auto plain_view = permissions::subject_view{
            .kind = subject_kind::player, .id = other_uuid};
        answer =
            permissions::explain(loaded, plain_view, "refrain.command.help", 0);
        expect(answer.result == permission_result::allowed);
        expect(answer.source == permissions::decision_source::default_grant);
        expect(answer.rule == "refrain.command.help");
        answer =
            permissions::explain(loaded, plain_view, "refrain.command.fly", 0);
        expect(answer.result == permission_result::denied);
        expect(answer.source == permissions::decision_source::fallback_deny);
        expect(answer.rule.empty());
        // An operator reaches the operator node set.
        const auto operator_view = permissions::subject_view{
            .kind = subject_kind::player, .id = other_uuid, .profile_name = {}};
        const auto operator_answer = permissions::explain(
            loaded, operator_view, "refrain.command.gamemode", 2);
        expect(operator_answer.result == permission_result::allowed);
        expect(operator_answer.source ==
               permissions::decision_source::op_grant);
        expect(operator_answer.rule == "refrain.command.gamemode");
        const auto console_answer = permissions::explain(
            loaded, permissions::subject_view{.kind = subject_kind::console},
            "refrain.command.stop", 0);
        expect(console_answer.result == permission_result::allowed);
        expect(console_answer.source == permissions::decision_source::console);
        expect(console_answer.rule.empty());
        const auto invalid = explain("NotANode");
        expect(invalid.result == permission_result::denied);
        expect(invalid.source == permissions::decision_source::invalid_node);
    };

    "explanation/agrees with evaluate on every rule set"_test = [] {
        const auto loaded{parse_ok(R"(
[defaults]
default = "op"
nodes = ["refrain.command.help"]
[op]
min_level = 1
nodes = ["refrain.command.gamemode", "!refrain.command.gamemode.special"]
[ops]
players = ["Notch"]
[grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["refrain.command.*", "!refrain.command.stop"]
[groups]
moderators = ["refrain.mods.admin"]
)")};
        const auto view =
            permissions::subject_view{.kind = subject_kind::player,
                                      .id = player_uuid,
                                      .profile_name = "Notch"};
        for (const auto *node :
             {"refrain.command.teleport", "refrain.command.stop",
              "refrain.command.gamemode", "refrain.command.gamemode.special",
              "refrain.mods.admin", "refrain.mods.trust",
              "refrain.command.help"}) {
            for (const auto op_level : {0U, 1U}) {
                expect(
                    permissions::explain(loaded, view, node, op_level).result ==
                    permissions::evaluate(loaded, view, node, op_level))
                    << node;
            }
        }
    };

    "explanation/lists groups with nodes and members"_test = [] {
        const auto loaded{parse_ok(R"(
[grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["@moderators"]
"plugin:com.example.mymod" = ["@moderators"]
[groups]
moderators = ["refrain.command.*", "!refrain.command.stop"]
builders = []
)")};
        const auto groups{permissions::list_groups(loaded)};
        expect(groups.size() == 2U);
        if (groups.size() != 2U) {
            return;
        }
        expect(groups[0].name == "builders");
        expect(groups[0].nodes.empty());
        expect(groups[0].members.empty());
        expect(groups[1].name == "moderators");
        expect(groups[1].nodes.size() == 2U);
        if (groups[1].nodes.size() == 2U) {
            expect(groups[1].nodes[0] == "refrain.command.*");
            expect(groups[1].nodes[1] == "!refrain.command.stop");
        }
        expect(groups[1].members.size() == 2U);
        if (groups[1].members.size() == 2U) {
            expect(groups[1].members[0] ==
                   "player:069a79f4-44e9-4726-a5be-fca90e38aaf5");
            expect(groups[1].members[1] == "plugin:com.example.mymod");
        }
    };
};

} // namespace
