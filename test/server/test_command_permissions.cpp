// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Permission gate at the command frontends: the server console holds every
// node through the injected provider, and a composition without a permission
// service keeps the declared op-level fallback for every node.

#include <refrain/command/command.hpp>
#include <refrain/contracts/services/permissions.hpp>
#include <refrain/net/server.hpp>
#include <refrain/server/command_bridge.hpp>

#include <support/permission_policy_provider.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace command = refrain::command;
namespace server = refrain::server;

template <typename TTree>
[[nodiscard]] auto tree_has_literal(const TTree &tree, std::string_view name)
    -> bool {
    return std::ranges::any_of(
        tree.nodes, [name](const auto &node) { return node.name == name; });
}

/// Policy that holds nothing except the resolver's own console rule.
constexpr std::string_view deny_everything_policy{R"(
[defaults]
default = "deny"
)"};

const suite command_permission_gate_tests = [] {
    "commands/the console holds every node through the provider"_test = [] {
        auto environment{std::make_shared<server::command_environment>()};
        refrain::test_support::permission_policy_provider provider{};
        expect(provider.reload(deny_everything_policy));
        server::bind_permissions(environment, provider.vtable(),
                                 provider.self());
        expect(static_cast<bool>(environment->permission_query));
        expect(static_cast<bool>(environment->permission_revision));
        expect(environment->permission_revision() == provider.revision());
        const auto bridge{server::make_command_bridge(environment)};
        expect(static_cast<bool>(bridge.tree));
        provider.clear_queries();

        // A source at the lowest vanilla level still passes every node,
        // because the provider answers the console subject as allowed.
        command::console_source console{command::level::everyone};
        const auto tree{environment->dispatcher.tree(console)};
        expect(tree != nullptr);
        if (tree == nullptr) {
            return;
        }
        for (const auto *name :
             {"help", "list", "say", "gamemode", "defaultgamemode",
              "difficulty", "time", "weather", "hardcore", "mods"}) {
            expect(tree_has_literal(*tree, name)) << name;
        }
        const auto executed{environment->dispatcher.execute("list", console)};
        expect(static_cast<bool>(executed)) << "the console failed /list";
        bool saw_console_query{false};
        for (const auto &query : provider.queries()) {
            if (query.node != "refrain.command.gamemode") {
                continue;
            }
            saw_console_query = true;
            expect(query.kind ==
                   refrain::services::permissions::subject_kind::console);
        }
        expect(saw_console_query);
    };

    "commands/without a provider the declared op level decides"_test = [] {
        auto environment{std::make_shared<server::command_environment>()};
        server::register_runtime_commands(environment);
        expect(!environment->permission_query);

        command::player_source owner{"Alice", 1U, command::level::admin};
        const auto owner_tree{environment->dispatcher.tree(owner)};
        expect(owner_tree != nullptr);
        if (owner_tree != nullptr) {
            for (const auto *name :
                 {"help", "list", "say", "gamemode", "defaultgamemode",
                  "difficulty", "time", "weather", "hardcore", "mods"}) {
                expect(tree_has_literal(*owner_tree, name)) << name;
            }
        }
        command::player_source guest{"Bob", 2U, command::level::everyone};
        const auto guest_tree{environment->dispatcher.tree(guest)};
        expect(guest_tree != nullptr);
        if (guest_tree != nullptr) {
            expect(tree_has_literal(*guest_tree, "help"));
            expect(tree_has_literal(*guest_tree, "list"));
            for (const auto *name :
                 {"say", "gamemode", "defaultgamemode", "difficulty", "time",
                  "weather", "hardcore", "mods"}) {
                expect(!tree_has_literal(*guest_tree, name)) << name;
            }
        }
        expect(!environment->dispatcher.execute("mods list", guest));
        expect(!environment->dispatcher.execute("gamemode creative", guest));
    };

    "commands/the default composition keeps the development fallback"_test =
        [] {
            auto environment{std::make_shared<server::command_environment>()};
            auto bridge{server::make_command_bridge(environment)};
            const auto id{refrain::net::uuid::parse(
                "dd12be42-52a9-4a91-a8a1-11c01849e498")};
            expect(id.has_value());
            const refrain::net::player_identity identity{
                .connection = 1,
                .id = id.value_or(refrain::net::uuid{}),
                .name = "Alice",
                .world = {},
            };
            const auto tree{bridge.tree(identity)};
            expect(tree != nullptr);
            if (tree != nullptr) {
                // The development fallback level is admin, the level the
                // runtime applied to every player before a provider exists.
                expect(tree_has_literal(*tree, "mods"));
                expect(tree_has_literal(*tree, "gamemode"));
                expect(tree_has_literal(*tree, "help"));
            }
        };
};

} // namespace
