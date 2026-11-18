// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Context-sensitive permission rules: the context map carried by a query, the
// `[contexts]` predicates, `[context_grants]` rule sets, and the precedence
// between global rules, context rules and the fallback.

#include <refrain/core/config.hpp>
#include <refrain/permissions/policy.hpp>

#include <boost/ut.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace {

using namespace boost::ut;

namespace config = refrain::core::config;
namespace permissions = refrain::permissions;
using refrain::services::permissions::permission_result;
using refrain::services::permissions::subject_kind;

constexpr std::string_view player_uuid{"069a79f4-44e9-4726-a5be-fca90e38aaf5"};
constexpr std::string_view player_key{
    "player:069a79f4-44e9-4726-a5be-fca90e38aaf5"};

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

[[nodiscard]] auto parse_error(std::string_view text) -> std::string {
    auto parsed{parse_document(text)};
    expect(!parsed) << "expected the configuration to be rejected";
    if (parsed) {
        return {};
    }
    expect(parsed.error().code == refrain::error_code::parse_error);
    return parsed.error().message;
}

[[nodiscard]] auto answer(const permissions::policy &policy,
                          std::string_view node,
                          const permissions::context_view &context,
                          std::uint32_t op_level = 0) -> permission_result {
    return permissions::evaluate(
        policy,
        permissions::subject_view{.kind = subject_kind::player,
                                  .id = player_uuid,
                                  .profile_name = "Notch"},
        node, op_level, context);
}

/// A query context with one world and one game mode.
struct test_context {
    std::string world{};
    std::string gamemode{};
    std::vector<permissions::context_value_view> extra{};

    [[nodiscard]] auto view() const noexcept -> permissions::context_view {
        return permissions::context_view{
            .world = world, .gamemode = gamemode, .extra = extra};
    }
};

const suite context_rule_tests = [] {
    "context/a context grant applies only while its predicate matches"_test =
        [] {
            const auto policy{parse_ok(R"(
[defaults]
default = "deny"

[contexts]
creative = { gamemode = "creative" }

[context_grants.creative]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["refrain.command.gamemode"]
)")};
            const test_context creative{.gamemode = "creative"};
            const test_context survival{.gamemode = "survival"};
            expect(answer(policy, "refrain.command.gamemode",
                          creative.view()) == permission_result::allowed);
            expect(answer(policy, "refrain.command.gamemode",
                          survival.view()) == permission_result::denied);
            // The predicate only gates its own rule set; an unrelated node
            // stays denied under the fallback.
            expect(answer(policy, "refrain.command.teleport",
                          creative.view()) == permission_result::denied);
        };

    "context/an expanded context table and an inline one resolve the same"_test =
        [] {
            const auto expanded{parse_ok(R"(
[contexts.nether]
world = "minecraft:the_nether"
[context_grants.nether]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["refrain.command.teleport"]
)")};
            const auto inline_table{parse_ok(R"(
[contexts]
nether = { world = "minecraft:the_nether" }
[context_grants]
nether = { "player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["refrain.command.teleport"] }
)")};
            const test_context nether{.world = "minecraft:the_nether"};
            expect(answer(expanded, "refrain.command.teleport",
                          nether.view()) == permission_result::allowed);
            expect(answer(inline_table, "refrain.command.teleport",
                          nether.view()) == permission_result::allowed);
            expect(expanded.contexts.size() == inline_table.contexts.size());
            expect(expanded.context_subjects.size() ==
                   inline_table.context_subjects.size());
        };

    "context/a deny in a matching context beats a global grant"_test = [] {
        const auto policy{parse_ok(R"(
[grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["refrain.command.*"]

[contexts]
creative = { gamemode = "creative" }

[context_grants.creative]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["!refrain.command.gamemode"]
)")};
        const test_context creative{.gamemode = "creative"};
        const test_context survival{.gamemode = "survival"};
        expect(answer(policy, "refrain.command.gamemode", creative.view()) ==
               permission_result::denied);
        expect(answer(policy, "refrain.command.gamemode", survival.view()) ==
               permission_result::allowed);
        expect(answer(policy, "refrain.command.teleport", creative.view()) ==
               permission_result::allowed);
    };

    "context/a context grant cannot override a global deny"_test = [] {
        const auto policy{parse_ok(R"(
[grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["!refrain.command.stop"]

[contexts]
creative = { gamemode = "creative" }

[context_grants.creative]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["refrain.command.stop"]
)")};
        const test_context creative{.gamemode = "creative"};
        expect(answer(policy, "refrain.command.stop", creative.view()) ==
               permission_result::denied);
    };

    "context/a deny in one of several matching contexts wins"_test = [] {
        const auto policy{parse_ok(R"(
[contexts]
overworld = { world = "minecraft:overworld" }
creative = { gamemode = "creative" }

[context_grants.overworld]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["refrain.command.teleport"]

[context_grants.creative]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["!refrain.command.teleport"]
)")};
        const test_context both{.world = "minecraft:overworld",
                                .gamemode = "creative"};
        expect(answer(policy, "refrain.command.teleport", both.view()) ==
               permission_result::denied);
        const test_context overworld{.world = "minecraft:overworld",
                                     .gamemode = "survival"};
        expect(answer(policy, "refrain.command.teleport", overworld.view()) ==
               permission_result::allowed);
    };

    "context/a context rule joins a global single-level group"_test = [] {
        const auto policy{parse_ok(R"(
[groups]
moderators = ["refrain.mods.admin", "!refrain.command.stop"]

[contexts]
survival = { gamemode = "survival" }

[context_grants.survival]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["@moderators"]
)")};
        const test_context survival{.gamemode = "survival"};
        const test_context creative{.gamemode = "creative"};
        expect(answer(policy, "refrain.mods.admin", survival.view()) ==
               permission_result::allowed);
        expect(answer(policy, "refrain.command.stop", survival.view()) ==
               permission_result::denied);
        expect(answer(policy, "refrain.mods.admin", creative.view()) ==
               permission_result::denied);
    };

    "context/a wildcard rule inside a context matches descendants only"_test =
        [] {
            const auto policy{parse_ok(R"(
[contexts]
adventure = { gamemode = "adventure" }

[context_grants.adventure]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["refrain.command.*"]
)")};
            const test_context adventure{.gamemode = "adventure"};
            expect(answer(policy, "refrain.command.teleport",
                          adventure.view()) == permission_result::allowed);
            expect(answer(policy, "refrain.command", adventure.view()) ==
                   permission_result::denied);
        };

    "context/an extra key matches its exact value"_test = [] {
        const auto policy{parse_ok(R"(
[contexts]
event = { event = "halloween" }

[context_grants.event]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["refrain.command.me"]
)")};
        std::vector<permissions::context_value_view> extra{
            {.key = "event", .value = "halloween"}};
        test_context matching{.extra = extra};
        expect(answer(policy, "refrain.command.me", matching.view()) ==
               permission_result::allowed);
        extra[0].value = "christmas";
        test_context other{.extra = extra};
        expect(answer(policy, "refrain.command.me", other.view()) ==
               permission_result::denied);
        const test_context empty{};
        expect(answer(policy, "refrain.command.me", empty.view()) ==
               permission_result::denied);
    };

    "context/a name-keyed context rule applies through the profile fallback"_test =
        [] {
            const auto policy{parse_ok(R"(
[contexts]
creative = { gamemode = "creative" }

[context_grants.creative]
"player:Notch" = ["refrain.command.gamemode"]
)")};
            const auto answer_name =
                [&policy](const permissions::context_view &view) {
                    return permissions::evaluate(
                        policy,
                        permissions::subject_view{.kind = subject_kind::player,
                                                  .id = player_uuid,
                                                  .profile_name = "Notch"},
                        "refrain.command.gamemode", 0, view);
                };
            const test_context creative{.gamemode = "creative"};
            expect(answer_name(creative.view()) == permission_result::allowed);
        };

    "context/explain names the matching context and its rule"_test = [] {
        const auto policy{parse_ok(R"(
[grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["refrain.command.*"]

[contexts]
creative = { gamemode = "creative" }

[context_grants.creative]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["!refrain.command.gamemode"]
)")};
        const test_context creative{.gamemode = "creative"};
        const auto explanation{permissions::explain(
            policy,
            permissions::subject_view{.kind = subject_kind::player,
                                      .id = player_uuid,
                                      .profile_name = "Notch"},
            "refrain.command.gamemode", 0, creative.view())};
        expect(explanation.result == permission_result::denied);
        expect(explanation.source ==
               permissions::decision_source::context_deny);
        expect(explanation.context == "creative");
        expect(explanation.rule == "!refrain.command.gamemode");

        const auto granted{permissions::explain(
            policy,
            permissions::subject_view{.kind = subject_kind::player,
                                      .id = player_uuid,
                                      .profile_name = "Notch"},
            "refrain.command.teleport", 0, creative.view())};
        expect(granted.result == permission_result::allowed);
        expect(granted.source == permissions::decision_source::subject_grant);
        expect(granted.context.empty());
    };

    "context/the console keeps every node in every context"_test = [] {
        const auto policy{parse_ok(R"(
[contexts]
creative = { gamemode = "creative" }
[context_grants.creative]
console = ["!refrain.command.teleport"]
)")};
        const test_context creative{.gamemode = "creative"};
        expect(permissions::evaluate(
                   policy,
                   permissions::subject_view{.kind = subject_kind::console},
                   "refrain.command.teleport", 0,
                   creative.view()) == permission_result::allowed);
    };

    "context/the operator fallback still applies without context rules"_test =
        [] {
            const auto policy{parse_ok(R"(
[defaults]
default = "op"
[op]
min_level = 2
)")};
            const test_context creative{.gamemode = "creative"};
            expect(answer(policy, "refrain.command.teleport", creative.view(),
                          2) == permission_result::allowed);
            expect(answer(policy, "refrain.command.teleport", creative.view(),
                          0) == permission_result::denied);
        };

    "context/a malformed query context is rejected"_test = [] {
        permissions::context_value_view duplicate_a{.key = "event",
                                                    .value = "a"};
        permissions::context_value_view duplicate_b{.key = "event",
                                                    .value = "b"};
        const permissions::context_value_view entries[]{duplicate_a,
                                                        duplicate_b};
        const permissions::context_view duplicated{.extra = entries};
        expect(!permissions::valid_context(duplicated));

        const permissions::context_value_view reserved[]{
            {.key = "world", .value = "minecraft:overworld"}};
        expect(!permissions::valid_context(
            permissions::context_view{.extra = reserved}));

        const std::string oversized_value(300, 'x');
        const permissions::context_value_view oversized[]{
            {.key = "event", .value = oversized_value}};
        expect(!permissions::valid_context(
            permissions::context_view{.extra = oversized}));

        std::vector<permissions::context_value_view> too_many{};
        for (std::size_t index{0}; index <= permissions::max_context_entries;
             ++index) {
            too_many.push_back(
                {.key = "k" + std::to_string(index), .value = "v"});
        }
        expect(!permissions::valid_context(
            permissions::context_view{.extra = too_many}));

        const permissions::context_view world{.world = "minecraft:overworld",
                                              .gamemode = "creative"};
        expect(permissions::valid_context(world));
    };

    "context/a context predicate matches every condition at once"_test = [] {
        permissions::context_definition definition{};
        definition.conditions.push_back(permissions::context_condition{
            .key = "gamemode", .value = "creative"});
        definition.conditions.push_back(permissions::context_condition{
            .key = "event", .value = "halloween"});
        const permissions::context_value_view extra[]{
            {.key = "event", .value = "halloween"}};
        const permissions::context_view matching{.gamemode = "creative",
                                                 .extra = extra};
        expect(permissions::context_matches(definition, matching));
        const permissions::context_view wrong_mode{.gamemode = "survival",
                                                   .extra = extra};
        expect(!permissions::context_matches(definition, wrong_mode));
        const permissions::context_view missing_extra{.gamemode = "creative"};
        expect(!permissions::context_matches(definition, missing_extra));
    };
};

const suite context_configuration_tests = [] {
    "context/context rules reject a reference to an undeclared context"_test =
        [] {
            const auto message{parse_error(R"(
[context_grants.creative]
"player:Notch" = ["refrain.command.teleport"]
)")};
            expect(message.find("unknown context 'creative'") !=
                   std::string::npos)
                << message;
        };

    "context/a context without conditions is rejected"_test = [] {
        const auto message{parse_error(R"(
[contexts]
everything = {}
)")};
        expect(message.find("defines no condition") != std::string::npos)
            << message;
    };

    "context/an invalid condition key and an empty value are rejected"_test =
        [] {
            auto message{parse_error(R"(
[contexts]
creative = { Gamemode = "creative" }
)")};
            expect(message.find("invalid context key") != std::string::npos)
                << message;
            message = parse_error(R"(
[contexts]
creative = { gamemode = "" }
)");
            expect(message.find("non-empty string") != std::string::npos)
                << message;
        };

    "context/an invalid context name and subject key are rejected"_test = [] {
        auto message{parse_error(R"(
[contexts]
"Creative Mode" = { gamemode = "creative" }
)")};
        expect(message.find("invalid context name") != std::string::npos)
            << message;
        message = parse_error(R"(
[contexts]
creative = { gamemode = "creative" }
[context_grants.creative]
"not a subject" = ["refrain.command.teleport"]
)");
        expect(message.find("invalid subject key") != std::string::npos)
            << message;
    };

    "context/a context grant rejects a dangling group"_test = [] {
        const auto message{parse_error(R"(
[contexts]
creative = { gamemode = "creative" }
[context_grants.creative]
"player:Notch" = ["@missing"]
)")};
        expect(message.find("unknown group 'missing'") != std::string::npos)
            << message;
    };

    "context/a context grant rejects a malformed rule"_test = [] {
        const auto message{parse_error(R"(
[contexts]
creative = { gamemode = "creative" }
[context_grants.creative]
"player:Notch" = ["Not A Node"]
)")};
        expect(message.find("invalid permission node") != std::string::npos)
            << message;
    };
};

} // namespace
