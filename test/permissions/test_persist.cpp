// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Persistence of runtime permission changes: the grant editor changes the
// smallest possible range of the configuration file, keeps comments and
// unrelated tables, and returns text the policy parser accepts.

#include <refrain/core/config.hpp>
#include <refrain/permissions/persist.hpp>
#include <refrain/permissions/policy.hpp>

#include <boost/ut.hpp>

#include <cstddef>
#include <string>
#include <string_view>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace config = refrain::core::config;
namespace permissions = refrain::permissions;

constexpr std::string_view player_key{
    "player:069a79f4-44e9-4726-a5be-fca90e38aaf5"};

[[nodiscard]] auto parse(const std::string &text)
    -> refrain::result<permissions::policy> {
    auto document{config::document::parse_text(text, "permissions.toml")};
    if (!document) {
        return std::unexpected{std::move(document.error())};
    }
    return permissions::parse_policy(*document);
}

[[nodiscard]] auto holds(const permissions::policy &policy_value,
                         std::string_view subject, std::string_view rule)
    -> bool {
    const auto found{policy_value.subjects.find(subject)};
    if (found == policy_value.subjects.end()) {
        return false;
    }
    for (const auto &entry : found->second.nodes.rules()) {
        if (permissions::rule_text(entry) == rule) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] auto contains(const std::string &text, std::string_view needle)
    -> bool {
    return text.find(needle) != std::string::npos;
}

const suite persist_tests = [] {
    "persist/a grant keeps comments and unrelated tables"_test = [] {
        const std::string text{R"(# Refrain permission policy.
# Keep this header.

[defaults]
# Public surface.
nodes = ["refrain.command.help"]

[grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = [
    "refrain.command.help", # keep this comment
    "refrain.command.list",
]

[groups]
moderators = ["refrain.mods.admin"]
)"};
        const auto edited{permissions::edit_grants(
            text, player_key, "refrain.command.gamemode", true)};
        expect(static_cast<bool>(edited))
            << (edited ? std::string{} : edited.error().message);
        if (!edited) {
            return;
        }
        expect(contains(*edited, "# Refrain permission policy."));
        expect(contains(*edited, "# Public surface."));
        expect(contains(*edited, "# keep this comment"));
        expect(contains(*edited, "[groups]"));
        expect(contains(*edited, "moderators = [\"refrain.mods.admin\"]"));
        auto policy{parse(*edited)};
        expect(static_cast<bool>(policy));
        if (policy) {
            expect(holds(*policy, player_key, "refrain.command.help"));
            expect(holds(*policy, player_key, "refrain.command.list"));
            expect(holds(*policy, player_key, "refrain.command.gamemode"));
        }
    };

    "persist/a grant adds the first entry of an empty table"_test = [] {
        const std::string text{R"(# A policy with an empty grants table.

[defaults]
default = "deny"

[grants]

[groups]
)"};
        const auto edited{permissions::edit_grants(
            text, player_key, "refrain.command.teleport", true)};
        expect(static_cast<bool>(edited));
        if (!edited) {
            return;
        }
        auto policy{parse(*edited)};
        expect(static_cast<bool>(policy));
        if (policy) {
            expect(holds(*policy, player_key, "refrain.command.teleport"));
        }
        expect(contains(*edited, "# A policy with an empty grants table."));
    };

    "persist/a grant appends the table when it is absent"_test = [] {
        const std::string text{"[defaults]\ndefault = \"deny\"\n"};
        const auto edited{permissions::edit_grants(
            text, "console", "refrain.permissions.admin", true)};
        expect(static_cast<bool>(edited));
        if (!edited) {
            return;
        }
        expect(contains(*edited, "[grants]"));
        expect(
            contains(*edited, "\"console\" = [\"refrain.permissions.admin\"]"));
        auto policy{parse(*edited)};
        expect(static_cast<bool>(policy));
        if (policy) {
            expect(holds(*policy, "console", "refrain.permissions.admin"));
        }
    };

    "persist/an adding grant in an empty document writes a grants table"_test =
        [] {
            const auto edited{
                permissions::edit_grants("", "plugin:com.example.mymod",
                                         "com.example.mymod.home", true)};
            expect(static_cast<bool>(edited));
            if (!edited) {
                return;
            }
            auto policy{parse(*edited)};
            expect(static_cast<bool>(policy));
            if (policy) {
                expect(holds(*policy, "plugin:com.example.mymod",
                             "com.example.mymod.home"));
            }
        };

    "persist/a revoke removes the element and one separator"_test = [] {
        const std::string text{R"([grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["refrain.command.help", "refrain.command.list", "refrain.command.me"]
)"};
        const auto edited{permissions::edit_grants(
            text, player_key, "refrain.command.list", false)};
        expect(static_cast<bool>(edited));
        if (!edited) {
            return;
        }
        auto policy{parse(*edited)};
        expect(static_cast<bool>(policy));
        if (policy) {
            expect(holds(*policy, player_key, "refrain.command.help"));
            expect(!holds(*policy, player_key, "refrain.command.list"));
            expect(holds(*policy, player_key, "refrain.command.me"));
        }
        expect(contains(*edited, "refrain.command.help"));
        expect(contains(*edited, "refrain.command.me"));
    };

    "persist/a revoke removes a trailing element with its trailing comma"_test =
        [] {
            const std::string text{R"([grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = [
    "refrain.command.help",
    "refrain.command.list",
]
)"};
            const auto edited{permissions::edit_grants(
                text, player_key, "refrain.command.list", false)};
            expect(static_cast<bool>(edited))
                << (edited ? std::string{} : edited.error().message);
            if (!edited) {
                return;
            }
            auto policy{parse(*edited)};
            expect(static_cast<bool>(policy));
            if (policy) {
                expect(holds(*policy, player_key, "refrain.command.help"));
                expect(!holds(*policy, player_key, "refrain.command.list"));
            }
        };

    "persist/a revoke removes a deny rule by its text"_test = [] {
        const std::string text{R"([grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["refrain.command.*", "!refrain.command.stop"]
)"};
        const auto edited{permissions::edit_grants(
            text, player_key, "!refrain.command.stop", false)};
        expect(static_cast<bool>(edited));
        if (!edited) {
            return;
        }
        auto policy{parse(*edited)};
        expect(static_cast<bool>(policy));
        if (policy) {
            expect(!holds(*policy, player_key, "!refrain.command.stop"));
            expect(holds(*policy, player_key, "refrain.command.*"));
        }
    };

    "persist/an absent revoke and a duplicate grant change nothing"_test = [] {
        const std::string text{R"([grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["refrain.command.help"]
)"};
        const auto duplicate{permissions::edit_grants(
            text, player_key, "refrain.command.help", true)};
        expect(static_cast<bool>(duplicate));
        if (duplicate) {
            expect(*duplicate == text);
        }
        const auto absent{permissions::edit_grants(
            text, player_key, "refrain.command.list", false)};
        expect(static_cast<bool>(absent));
        if (absent) {
            expect(*absent == text);
        }
    };

    "persist/an invalid rule and an invalid subject are rejected"_test = [] {
        const std::string text{"[grants]\n"};
        expect(!permissions::edit_grants(text, player_key, "Not A Node", true));
        expect(!permissions::edit_grants(text, player_key, "", true));
        expect(!permissions::edit_grants(text, "not a subject", "a.b", true));
    };

    "persist/dotted-key grants are refused instead of rewritten"_test = [] {
        const std::string text{
            "grants.\"player:069a79f4-44e9-4726-a5be-fca90e38aaf5\" = "
            "[\"refrain.command.help\"]\n"};
        const auto edited{permissions::edit_grants(
            text, player_key, "refrain.command.list", true)};
        expect(!edited);
    };
};

} // namespace
