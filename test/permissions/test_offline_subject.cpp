// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Offline-subject resolution of the `/permissions` commands: a bare name that
// no live session owns is resolved through the composition's profile service
// when one is available, and the change targets the canonical account uuid
// instead of the mutable name. An online-mode composition without that service
// keeps the explicit `player:<uuid>` requirement.

#include <refrain/core/log.hpp>
#include <refrain/net/auth.hpp>
#include <refrain/permissions/admin.hpp>
#include <refrain/permissions/policy.hpp>
#include <refrain/server/permissions_admin.hpp>

#include <support/permission_policy_provider.hpp>

#include <boost/ut.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace permissions = refrain::permissions;
namespace server = refrain::server;
namespace test_support = refrain::test_support;

constexpr std::string_view offline_name{"Alice"};
constexpr std::string_view online_name{"Bob"};

[[nodiscard]] auto offline_uuid() -> std::string {
    const auto profile{refrain::net::offline_profile(offline_name)};
    expect(static_cast<bool>(profile));
    return profile ? profile->id.to_string() : std::string{};
}

[[nodiscard]] auto has_line(const std::vector<std::string> &lines,
                            std::string_view needle) -> bool {
    for (const auto &line : lines) {
        if (line.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

/// A command frontend wired to a real policy provider plus the subject lookup
/// callbacks the composition root installs. `profile_uuid` empty disables the
/// offline profile service.
struct subject_fixture {
    test_support::permission_policy_provider provider{};
    std::vector<server::permissions_audit_record> audit{};
    server::permissions_admin_state state{};
    std::vector<std::string> lines{};

    explicit subject_fixture(std::string_view policy_text,
                             bool profile_service = true) {
        expect(provider.reload(policy_text));
        state.admin = provider.admin();
        state.audit = [this](const server::permissions_audit_record &record) {
            audit.push_back(record);
        };
        state.find_player = [](std::string_view name)
            -> std::optional<server::permissions_online_player> {
            if (name != online_name) {
                return std::nullopt;
            }
            auto uuid{refrain::net::uuid::parse(
                "11111111-2222-3333-4444-555555555555")};
            if (!uuid) {
                return std::nullopt;
            }
            return server::permissions_online_player{
                .name = std::string{online_name},
                .uuid = uuid->to_string(),
                .world = "minecraft:overworld",
                .gamemode = "creative",
            };
        };
        if (profile_service) {
            state.find_profile = [](std::string_view name)
                -> std::optional<server::permissions_player_profile> {
                if (name != offline_name) {
                    return std::nullopt;
                }
                const auto uuid{offline_uuid()};
                if (uuid.empty()) {
                    return std::nullopt;
                }
                return server::permissions_player_profile{
                    .uuid = uuid, .name = std::string{offline_name}};
            };
        }
    }

    [[nodiscard]] auto
    run(std::span<const std::string_view> tokens,
        server::permissions_actor actor = server::permissions_actor{})
        -> server::permissions_command_report {
        lines.clear();
        auto report{server::execute_permissions_command(state, tokens, actor)};
        lines = report.lines;
        return report;
    }

    [[nodiscard]] auto subject_keys() -> std::vector<std::string> {
        std::vector<std::string> keys{};
        const auto snapshot{provider.admin()->snapshot()};
        for (const auto &[key, rules] : snapshot.value.subjects) {
            static_cast<void>(rules);
            keys.push_back(key);
        }
        return keys;
    }
};

constexpr std::string_view empty_policy{"[defaults]\ndefault = \"deny\"\n"};

/// A global grant the online player holds, narrowed by a creative-context deny.
constexpr std::string_view creative_deny_policy{R"(
[grants]
"player:11111111-2222-3333-4444-555555555555" = ["refrain.command.teleport"]

[contexts]
creative = { gamemode = "creative" }

[context_grants.creative]
"player:11111111-2222-3333-4444-555555555555" = ["!refrain.command.teleport"]
)"};

const suite offline_subject_tests = [] {
    "offline subject/a bare name resolves through the profile service"_test =
        [] {
            subject_fixture fixture{empty_policy};
            const std::string_view tokens[]{"grant", offline_name,
                                            "refrain.command.teleport"};
            const auto report{fixture.run(tokens)};
            expect(report.ok) << report.error;
            const auto expected_key{"player:" + offline_uuid()};
            expect(has_line(report.lines, expected_key)) << expected_key;
            expect(fixture.audit.size() == 1U);
            if (fixture.audit.size() == 1U) {
                expect(fixture.audit.front().subject == expected_key);
                expect(fixture.audit.front().result == "ok");
            }
            const auto keys{fixture.subject_keys()};
            expect(keys.size() == 1U);
            if (keys.size() == 1U) {
                expect(keys.front() == expected_key);
            }
        };

    "offline subject/an explicit player name canonicalises to its account"_test =
        [] {
            subject_fixture fixture{empty_policy};
            const std::string_view tokens[]{"grant", "player:Alice",
                                            "refrain.command.teleport"};
            const auto report{fixture.run(tokens)};
            expect(report.ok) << report.error;
            const auto expected_key{"player:" + offline_uuid()};
            expect(has_line(report.lines, expected_key)) << expected_key;
            expect(fixture.subject_keys() ==
                   std::vector<std::string>{expected_key});
        };

    "offline subject/an online session takes precedence over the profile"_test =
        [] {
            subject_fixture fixture{empty_policy};
            const std::string_view tokens[]{"grant", online_name,
                                            "refrain.command.teleport"};
            const auto report{fixture.run(tokens)};
            expect(report.ok) << report.error;
            expect(has_line(report.lines,
                            "player:11111111-2222-3333-4444-555555555555"));
            expect(fixture.subject_keys() ==
                   std::vector<std::string>{"player:11111111-2222-3333-"
                                            "4444-555555555555"});
        };

    "offline subject/without a profile service a bare name stays unknown"_test =
        [] {
            subject_fixture fixture{empty_policy, false};
            const std::string_view tokens[]{"grant", offline_name,
                                            "refrain.command.teleport"};
            const auto report{fixture.run(tokens)};
            expect(!report.ok);
            expect(report.error.find("no online player") != std::string::npos)
                << report.error;
            expect(report.error.find("player:<uuid>") != std::string::npos)
                << report.error;
            expect(fixture.audit.size() == 1U);
            if (fixture.audit.size() == 1U) {
                expect(fixture.audit.front().result == "invalid");
            }
        };

    "offline subject/a profile service that cannot resolve the name reports it"_test =
        [] {
            subject_fixture fixture{empty_policy};
            const std::string_view tokens[]{"grant", "Unknown",
                                            "refrain.command.teleport"};
            const auto report{fixture.run(tokens)};
            expect(!report.ok);
            expect(report.error.find("no player profile") != std::string::npos)
                << report.error;
        };

    "offline subject/check still evaluates an explicitly keyed offline subject"_test =
        [] {
            subject_fixture fixture{empty_policy};
            const auto key{"player:" + offline_uuid()};
            const std::string_view grant_tokens[]{"grant", key,
                                                  "refrain.command.teleport"};
            const auto grant = fixture.run(grant_tokens);
            expect(grant.ok) << grant.error;
            const std::string_view check_tokens[]{"check", key,
                                                  "refrain.command.teleport"};
            const auto check = fixture.run(check_tokens);
            expect(check.ok) << check.error;
            expect(has_line(check.lines, "allowed")) << check.error;
            expect(has_line(check.lines, "no player context"));
        };

    "offline subject/check reports a context answer a wrapper-shaped query decided"_test =
        [] {
            subject_fixture fixture{creative_deny_policy};
            // The administration surface of the fixture explains the global
            // policy; a provider that answers through context rules is the
            // chain-aware query the composition root installs.
            const auto policy{fixture.provider.admin()->snapshot().value};
            fixture.state.query =
                [policy](const permissions::subject_view &subject,
                         std::string_view node,
                         const permissions::context_view &context)
                -> refrain::services::permissions::permission_result {
                return permissions::evaluate(policy, subject, node, 0, context);
            };
            const std::string_view tokens[]{"check", online_name,
                                            "refrain.command.teleport"};
            const auto report{fixture.run(tokens)};
            expect(report.ok) << report.error;
            expect(has_line(report.lines, "denied")) << report.error;
            expect(has_line(report.lines, "wrapper provider answered"));
            expect(has_line(report.lines, "creative"));
        };
};

} // namespace
