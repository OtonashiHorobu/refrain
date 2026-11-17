// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Default resolver policy: strict configuration parsing and the resolution
// order (console, deny, grant, operator nodes, fallback).

#include <refrain/permissions/policy.hpp>

#include <refrain/core/config.hpp>
#include <refrain/core/error.hpp>

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

} // namespace
