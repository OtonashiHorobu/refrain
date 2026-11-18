// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Console and in-game mod management frontends: the console `mods ...`
// dispatch, the `/mods` command tree registration, the `refrain.mods.admin`
// permission gate, the explicit denial for a player without it, and the audit
// actor identity.

#include <refrain/app/mod_management.hpp>
#include <refrain/permissions/policy.hpp>
#include <refrain/server/command_bridge.hpp>
#include <refrain/server/console_commands.hpp>

#include <refrain/core/log.hpp>
#include <refrain/core/time.hpp>
#include <refrain/sim/world.hpp>
#include <refrain/world/store.hpp>

#include <support/permission_policy_provider.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace app = refrain::app;
namespace command = refrain::command;
namespace fs = std::filesystem;
namespace net = refrain::net;
namespace server = refrain::server;
namespace sim = refrain::sim;

[[nodiscard]] auto scratch_directory(std::string_view name) -> fs::path {
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    auto path{fs::temp_directory_path() /
              ("refrain-mod-command-" + std::to_string(stamp) + "-" +
               std::string{name})};
    std::error_code error{};
    fs::create_directories(path, error);
    return path;
}

[[nodiscard]] auto read_text(const fs::path &path) -> std::string {
    std::ifstream in{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{in},
                       std::istreambuf_iterator<char>{}};
}

[[nodiscard]] auto open_empty_world(const fs::path &directory)
    -> std::optional<sim::world> {
    auto store{refrain::world::world_store::create(directory.string())};
    if (!store) {
        return std::nullopt;
    }
    if (!store->commit()) {
        return std::nullopt;
    }
    auto opened{sim::world::open(directory.string())};
    if (!opened) {
        return std::nullopt;
    }
    return std::optional<sim::world>{std::move(*opened)};
}

[[nodiscard]] auto sample_uuid(std::string_view text) -> net::uuid {
    auto parsed{net::uuid::parse(text)};
    expect(static_cast<bool>(parsed));
    return parsed ? *parsed : net::uuid{};
}

/// A management context with one loaded mod and a capturing audit sink.
struct management_fixture {
    std::vector<app::mod_audit_record> audit{};
    app::mod_management_context context{};

    management_fixture() {
        context.loaded.push_back(app::mod_management_mod{
            .id = "refrain.test.manager",
            .version = "1.0.0",
            .loader = "native",
            .has_code = true,
            .source = "installed",
            .services = {"refrain:world"},
        });
        context.hot_reload = true;
        context.lifecycle.unavailable_reason =
            "the hot lifecycle registry is not available in this build";
        context.audit = [this](const app::mod_audit_record &record) {
            audit.push_back(record);
        };
    }

    [[nodiscard]] auto service() -> app::mod_management {
        return app::mod_management{context};
    }
};

/// Player source with a chosen identity and recorded replies.
class recording_player final : public command::source {
  public:
    recording_player(std::string name, std::uint64_t key,
                     std::vector<std::string> &replies,
                     command::permission_level ladder = command::level::admin)
        : name_{std::move(name)}, key_{key}, replies_{replies},
          ladder_{ladder} {}

    [[nodiscard]] auto identity() const noexcept
        -> command::source_identity override {
        return command::source_identity{name_, key_, true};
    }

    [[nodiscard]] auto permission() const noexcept
        -> command::permission_level override {
        return ladder_;
    }

    void reply(std::string_view message) override {
        replies_.emplace_back(message);
    }

  private:
    std::string name_;
    std::uint64_t key_;
    std::vector<std::string> &replies_;
    command::permission_level ladder_;
};

template <typename TTree>
[[nodiscard]] auto tree_has_literal(const TTree &tree, std::string_view name)
    -> bool {
    for (const auto &node : tree.nodes) {
        if (node.name == name) {
            return true;
        }
    }
    return false;
}

/// Names of the direct children of the first node named `parent`.
template <typename TTree>
[[nodiscard]] auto tree_child_names(const TTree &tree, std::string_view parent)
    -> std::vector<std::string> {
    std::vector<std::string> names{};
    for (const auto &node : tree.nodes) {
        if (node.name != parent) {
            continue;
        }
        for (const auto child : node.children) {
            names.push_back(tree.nodes[child].name);
        }
        break;
    }
    return names;
}

[[nodiscard]] auto has_element(const std::vector<std::string> &values,
                               std::string_view needle) -> bool {
    return std::any_of(
        values.begin(), values.end(),
        [needle](const std::string &value) { return value == needle; });
}

[[nodiscard]] auto replies_contain(const std::vector<std::string> &replies,
                                   std::string_view needle) -> bool {
    return std::any_of(replies.begin(), replies.end(),
                       [needle](const std::string &reply) {
                           return reply.find(needle) != std::string::npos;
                       });
}

} // namespace

const suite mod_console_tests = [] {
    "mod console/list reports the composition through the mods logger"_test =
        [] {
            const auto directory{scratch_directory("console-list")};
            auto world{open_empty_world(directory)};
            expect(world.has_value());
            if (!world) {
                std::error_code error;
                fs::remove_all(directory, error);
                return;
            }
            auto port{server::make_borrowed_world_port(*world)};
            management_fixture fixture{};
            auto service{fixture.service()};
            server::tick_metrics metrics{};
            server::console_commands commands{
                server::console_dependencies{.world = port.get(),
                                             .mods = &service},
                metrics};
            const refrain::core::tick_info tick{};

            const auto log_path = directory / "console.log";
            refrain::log::shutdown();
            refrain::log::config cfg{};
            cfg.console_level = refrain::log::level::off;
            cfg.file_path = log_path.string();
            cfg.file_level = refrain::log::level::info;
            expect(static_cast<bool>(refrain::log::init(cfg)));
            commands.execute("mods list", tick);
            commands.execute("mods reload refrain.test.manager", tick);
            commands.execute("mods help", tick);
            refrain::log::shutdown();
            const auto text{read_text(log_path)};
            expect(text.find("refrain.test.manager 1.0.0") != std::string::npos)
                << text;
            expect(text.find("the hot lifecycle registry is not available") !=
                   std::string::npos)
                << text;
            expect(text.find("the available mod commands are") !=
                   std::string::npos)
                << text;
            expect(fixture.audit.size() == 3U);
            expect(fixture.audit.front().actor_kind == "console");
            std::error_code error;
            fs::remove_all(directory, error);
        };

    "mod console/failure uses the error level and the same message"_test = [] {
        const auto directory{scratch_directory("console-failure")};
        auto world{open_empty_world(directory)};
        expect(world.has_value());
        if (!world) {
            std::error_code error;
            fs::remove_all(directory, error);
            return;
        }
        auto port{server::make_borrowed_world_port(*world)};
        management_fixture fixture{};
        auto service{fixture.service()};
        server::tick_metrics metrics{};
        server::console_commands commands{
            server::console_dependencies{.world = port.get(), .mods = &service},
            metrics};
        const refrain::core::tick_info tick{};

        const auto log_path = directory / "console.log";
        refrain::log::shutdown();
        refrain::log::config cfg{};
        cfg.console_level = refrain::log::level::off;
        cfg.file_path = log_path.string();
        cfg.file_level = refrain::log::level::info;
        expect(static_cast<bool>(refrain::log::init(cfg)));
        commands.execute("mods uninstall some.mod", tick);
        refrain::log::shutdown();
        const auto text{read_text(log_path)};
        expect(text.find("no content store is configured") != std::string::npos)
            << text;
        std::error_code error;
        fs::remove_all(directory, error);
    };

    "mod console/mod management is reported unavailable without a service"_test =
        [] {
            const auto directory{scratch_directory("console-unavailable")};
            auto world{open_empty_world(directory)};
            expect(world.has_value());
            if (!world) {
                std::error_code error;
                fs::remove_all(directory, error);
                return;
            }
            auto port{server::make_borrowed_world_port(*world)};
            server::tick_metrics metrics{};
            server::console_commands commands{
                server::console_dependencies{.world = port.get()}, metrics};
            const refrain::core::tick_info tick{};
            const auto log_path = directory / "console.log";
            refrain::log::shutdown();
            refrain::log::config cfg{};
            cfg.console_level = refrain::log::level::off;
            cfg.file_path = log_path.string();
            cfg.file_level = refrain::log::level::info;
            expect(static_cast<bool>(refrain::log::init(cfg)));
            commands.execute("mods list", tick);
            refrain::log::shutdown();
            const auto text{read_text(log_path)};
            expect(text.find("mod management is unavailable") !=
                   std::string::npos)
                << text;
            std::error_code error;
            fs::remove_all(directory, error);
        };
};

namespace {

constexpr std::string_view operator_uuid_text{
    "11111111-1111-1111-1111-111111111111"};
constexpr std::string_view guest_uuid_text{
    "22222222-2222-2222-2222-222222222222"};

/// Policy with the public command surface, one operator from `[ops].players`
/// and every node for an operator.
[[nodiscard]] auto operator_policy_text() -> std::string {
    return "[defaults]\n"
           "default = \"deny\"\n"
           "nodes = [\"refrain.command.help\", \"refrain.command.list\"]\n"
           "\n[op]\n"
           "min_level = 1\n"
           "nodes = [\"*\"]\n"
           "\n[ops]\n"
           "players = [\"" +
           std::string{operator_uuid_text} + "\"]\n";
}

/// Policy that grants one player the mod administration node directly, with
/// no operator list involved.
[[nodiscard]] auto granted_policy_text() -> std::string {
    return "[defaults]\n"
           "default = \"deny\"\n"
           "nodes = [\"refrain.command.help\", \"refrain.command.list\"]\n"
           "\n[grants]\n\"player:" +
           std::string{operator_uuid_text} + "\" = [\"refrain.mods.admin\"]\n";
}

/// Policy with only the public command surface: no operator and no grant.
[[nodiscard]] auto public_policy_text() -> std::string {
    return "[defaults]\n"
           "default = \"deny\"\n"
           "nodes = [\"refrain.command.help\", \"refrain.command.list\"]\n";
}

/// Bind one policy provider to the environment and return the provider so the
/// test can publish a new policy later.
[[nodiscard]] auto
bind_policy(const std::shared_ptr<server::command_environment> &environment,
            refrain::test_support::permission_policy_provider &provider)
    -> bool {
    if (!provider.reload(operator_policy_text())) {
        return false;
    }
    server::bind_permissions(environment, provider.vtable(), provider.self());
    return true;
}

[[nodiscard]] auto identity_of(std::string_view uuid_text,
                               std::string_view name, std::uint64_t connection)
    -> net::player_identity {
    // The identity borrows both strings; the literals at every call site
    // outlive the test.
    return net::player_identity{
        .connection = connection,
        .id = sample_uuid(uuid_text),
        .name = name,
        .world = "minecraft:overworld",
    };
}

} // namespace

const suite mod_command_tree_tests = [] {
    "mod command tree/registers /mods and its subcommands for an operator"_test =
        [] {
            management_fixture fixture{};
            auto service{fixture.service()};
            auto environment{std::make_shared<server::command_environment>()};
            environment->mods = &service;
            server::register_runtime_commands(environment);

            std::vector<std::string> replies{};
            // No permission provider: the declared op-level fallback of the
            // tree is what an operator at the admin level passes.
            recording_player op{"Alice", 7U, replies};
            auto tree{environment->dispatcher.tree(op)};
            expect(tree != nullptr);
            if (!tree) {
                return;
            }
            expect(tree_has_literal(*tree, "mods"));
            const auto children{tree_child_names(*tree, "mods")};
            for (const auto *name : {"list", "installed", "verify", "install",
                                     "uninstall", "reload", "trust"}) {
                expect(has_element(children, name)) << name;
            }
            // verify/install/uninstall/reload take an argument node.
            expect(has_element(tree_child_names(*tree, "verify"), "package"));
            expect(has_element(tree_child_names(*tree, "install"), "package"));
            expect(has_element(tree_child_names(*tree, "uninstall"), "id"));
            expect(has_element(tree_child_names(*tree, "reload"), "id"));
        };

    "mod command tree/hides /mods from a player below the fallback level"_test =
        [] {
            management_fixture fixture{};
            auto service{fixture.service()};
            auto environment{std::make_shared<server::command_environment>()};
            environment->mods = &service;
            server::register_runtime_commands(environment);

            std::vector<std::string> replies{};
            recording_player guest{"Bob", 7U, replies,
                                   command::level::gamemode};
            auto tree{environment->dispatcher.tree(guest)};
            expect(tree != nullptr);
            if (tree) {
                expect(!tree_has_literal(*tree, "mods"));
                // The public command surface is still there.
                expect(tree_has_literal(*tree, "help"));
                expect(tree_has_literal(*tree, "list"));
            }
        };

    "mod command tree/the declared node is the mod admin permission"_test = [] {
        management_fixture fixture{};
        auto service{fixture.service()};
        auto environment{std::make_shared<server::command_environment>()};
        environment->mods = &service;
        server::register_runtime_commands(environment);
        const auto declared{environment->dispatcher.permission_of("mods")};
        expect(declared.node == app::mod_admin_permission) << declared.node;
        expect(declared.fallback == command::level::admin);
    };

    "mod command tree/an operator runs /mods list through the shared service"_test =
        [] {
            management_fixture fixture{};
            auto service{fixture.service()};
            auto environment{std::make_shared<server::command_environment>()};
            environment->mods = &service;
            server::register_runtime_commands(environment);

            std::vector<std::string> replies{};
            recording_player op{"Alice", 7U, replies};
            auto executed{environment->dispatcher.execute("mods list", op)};
            expect(static_cast<bool>(executed)) << "dispatch failed";
            if (!executed) {
                return;
            }
            expect(executed->success);
            expect(!replies.empty());
            expect(replies_contain(replies, "refrain.test.manager 1.0.0"))
                << replies.front();
            expect(fixture.audit.size() == 1U);
            expect(fixture.audit.front().actor_kind == "player");
            expect(fixture.audit.front().actor_name == "Alice");
        };

    "mod command tree/a player below the fallback level cannot execute"_test =
        [] {
            management_fixture fixture{};
            auto service{fixture.service()};
            auto environment{std::make_shared<server::command_environment>()};
            environment->mods = &service;
            server::register_runtime_commands(environment);
            std::vector<std::string> replies{};
            recording_player guest{"Bob", 7U, replies,
                                   command::level::gamemode};
            const auto executed{
                environment->dispatcher.execute("mods list", guest)};
            expect(!executed);
            if (!executed) {
                expect(executed.error().code ==
                       refrain::error_code::parse_error);
            }
            expect(fixture.audit.empty());
        };
};

const suite mod_game_bridge_tests = [] {
    "mod game bridge/the tree is gated by the permission node, not a list"_test =
        [] {
            management_fixture fixture{};
            auto service{fixture.service()};
            auto environment{std::make_shared<server::command_environment>()};
            environment->mods = &service;
            refrain::test_support::permission_policy_provider provider{};
            expect(bind_policy(environment, provider));
            auto bridge{server::make_command_bridge(environment)};
            provider.clear_queries();

            // An operator from [ops].players holds the node without being in
            // the old operator allowlist: the command is announced and runs.
            const auto operator_identity{
                identity_of(operator_uuid_text, "Alice", 1)};
            const auto operator_tree{bridge.tree(operator_identity)};
            expect(operator_tree != nullptr);
            if (operator_tree != nullptr) {
                expect(tree_has_literal(*operator_tree, "mods"));
            }
            // Completion below the root is offered to the operator.
            const auto suggestions{
                bridge.suggest(operator_identity, "mods ", 5)};
            std::vector<std::string> offered{};
            offered.reserve(suggestions.entries.size());
            for (const auto &entry : suggestions.entries) {
                offered.push_back(entry.text);
            }
            expect(!offered.empty());
            expect(has_element(offered, "list"));
            // The query carries the player subject and its context: profile
            // uuid, profile name, world and game mode.
            bool saw_node{false};
            for (const auto &query : provider.queries()) {
                if (query.node != app::mod_admin_permission) {
                    continue;
                }
                saw_node = true;
                expect(query.kind ==
                       refrain::services::permissions::subject_kind::player);
                expect(query.id == std::string{operator_uuid_text});
                expect(query.profile_name == "Alice");
                expect(query.world == "minecraft:overworld");
                expect(query.gamemode == "creative");
                expect(query.op_level == 0U);
            }
            expect(saw_node) << "the tree query did not ask for the node";
            const auto feedback{bridge.execute(operator_identity, "mods list")};
            expect(feedback.handled);
            expect(feedback.success) << feedback.message;
            expect(fixture.audit.size() == 1U);
            expect(fixture.audit.front().actor_kind == "player");
            expect(fixture.audit.front().actor_uuid ==
                   std::string{operator_uuid_text});
        };

    "mod game bridge/a granted player holds the node without any operator list"_test =
        [] {
            management_fixture fixture{};
            auto service{fixture.service()};
            auto environment{std::make_shared<server::command_environment>()};
            environment->mods = &service;
            refrain::test_support::permission_policy_provider provider{};
            expect(provider.reload(granted_policy_text()));
            server::bind_permissions(environment, provider.vtable(),
                                     provider.self());
            auto bridge{server::make_command_bridge(environment)};

            const auto identity{identity_of(operator_uuid_text, "Alice", 1)};
            const auto tree{bridge.tree(identity)};
            expect(tree != nullptr);
            if (tree != nullptr) {
                expect(tree_has_literal(*tree, "mods"));
            }
            expect(bridge.execute(identity, "mods list").success);
            expect(fixture.audit.size() == 1U);
        };

    "mod game bridge/a player without the node is hidden and denied"_test = [] {
        management_fixture fixture{};
        auto service{fixture.service()};
        auto environment{std::make_shared<server::command_environment>()};
        environment->mods = &service;
        refrain::test_support::permission_policy_provider provider{};
        expect(provider.reload(public_policy_text()));
        server::bind_permissions(environment, provider.vtable(),
                                 provider.self());
        auto bridge{server::make_command_bridge(environment)};
        const auto guest{identity_of(guest_uuid_text, "Bob", 2)};

        const auto tree{bridge.tree(guest)};
        expect(tree != nullptr);
        if (tree != nullptr) {
            expect(!tree_has_literal(*tree, "mods"));
            // The public command surface is still announced.
            expect(tree_has_literal(*tree, "help"));
            expect(tree_has_literal(*tree, "list"));
        }

        const auto feedback{bridge.execute(guest, "mods list")};
        expect(feedback.handled);
        expect(!feedback.success);
        expect(feedback.message.find(app::mod_admin_permission) !=
               std::string::npos)
            << feedback.message;
        expect(fixture.audit.empty());

        // Completion offers nothing below the hidden root.
        const auto suggestions{bridge.suggest(guest, "mods ", 5)};
        expect(suggestions.entries.empty());
    };

    "mod game bridge/the tree cache does not leak the operator tree"_test = [] {
        management_fixture fixture{};
        auto service{fixture.service()};
        auto environment{std::make_shared<server::command_environment>()};
        environment->mods = &service;
        refrain::test_support::permission_policy_provider provider{};
        expect(bind_policy(environment, provider));
        auto bridge{server::make_command_bridge(environment)};

        const auto operator_identity{
            identity_of(operator_uuid_text, "Alice", 1)};
        const auto operator_tree{bridge.tree(operator_identity)};
        expect(operator_tree != nullptr);
        if (operator_tree != nullptr) {
            expect(tree_has_literal(*operator_tree, "mods"));
        }
        const auto guest_identity{identity_of(guest_uuid_text, "Bob", 2)};
        const auto guest_tree{bridge.tree(guest_identity)};
        expect(guest_tree != nullptr);
        if (guest_tree != nullptr) {
            expect(!tree_has_literal(*guest_tree, "mods"));
            expect(guest_tree != operator_tree);
        }
    };

    "mod game bridge/a policy revision rebuilds the cached tree"_test = [] {
        management_fixture fixture{};
        auto service{fixture.service()};
        auto environment{std::make_shared<server::command_environment>()};
        environment->mods = &service;
        refrain::test_support::permission_policy_provider provider{};
        expect(bind_policy(environment, provider));
        auto bridge{server::make_command_bridge(environment)};

        const auto guest_identity{identity_of(guest_uuid_text, "Bob", 2)};
        const auto before{bridge.tree(guest_identity)};
        expect(before != nullptr);
        if (before != nullptr) {
            expect(!tree_has_literal(*before, "mods"));
        }
        const auto revision_before{provider.revision()};
        const auto bridge_revision_before{bridge.revision()};

        // Grant the node to the guest: the policy revision advances and the
        // next tree request must not reuse the snapshot built before it.
        expect(provider.reload("[defaults]\n"
                               "default = \"deny\"\n"
                               "nodes = [\"refrain.command.help\", "
                               "\"refrain.command.list\"]\n"
                               "\n[grants]\n\"player:" +
                               std::string{guest_uuid_text} +
                               "\" = [\"refrain.mods.admin\"]\n"));
        expect(provider.revision() > revision_before);
        expect(bridge.revision() != bridge_revision_before);
        const auto after{bridge.tree(guest_identity)};
        expect(after != nullptr);
        expect(after != before);
        if (after != nullptr) {
            expect(tree_has_literal(*after, "mods"));
        }
        expect(bridge.execute(guest_identity, "mods list").success);
    };

    "mod game bridge/an operator executes /mods and the audit carries the "
    "uuid"_test = [] {
        management_fixture fixture{};
        auto service{fixture.service()};
        auto environment{std::make_shared<server::command_environment>()};
        environment->mods = &service;
        refrain::test_support::permission_policy_provider provider{};
        expect(bind_policy(environment, provider));
        auto bridge{server::make_command_bridge(environment)};
        const auto operator_identity{
            identity_of(operator_uuid_text, "Alice", 1)};
        const auto feedback{bridge.execute(operator_identity, "mods list")};
        expect(feedback.handled);
        expect(feedback.success) << feedback.message;
        expect(fixture.audit.size() == 1U);
        expect(fixture.audit.front().actor_kind == "player");
        expect(fixture.audit.front().actor_name == "Alice");
        expect(fixture.audit.front().actor_uuid ==
               std::string{operator_uuid_text});
        expect(fixture.audit.front().action == "list");
    };

    "mod game bridge/an operator reload reaches the lifecycle port"_test = [] {
        management_fixture fixture{};
        std::vector<app::mod_lifecycle_request> requests{};
        fixture.context.lifecycle.apply =
            [&requests](const app::mod_lifecycle_request &request)
            -> refrain::result<app::mod_lifecycle_outcome> {
            requests.push_back(request);
            return app::mod_lifecycle_outcome{
                .lines = {"staged the reload for the next tick"}};
        };
        auto service{fixture.service()};
        auto environment{std::make_shared<server::command_environment>()};
        environment->mods = &service;
        refrain::test_support::permission_policy_provider provider{};
        expect(bind_policy(environment, provider));
        auto bridge{server::make_command_bridge(environment)};
        const auto operator_identity{
            identity_of(operator_uuid_text, "Alice", 1)};
        const auto feedback{bridge.execute(operator_identity,
                                           "mods reload refrain.test.manager")};
        expect(feedback.handled);
        expect(feedback.success) << feedback.message;
        expect(requests.size() == 1U);
        if (!requests.empty()) {
            expect(requests.front().action ==
                   app::mod_lifecycle_action::reload);
            expect(requests.front().id == "refrain.test.manager");
            expect(requests.front().version == "1.0.0");
        }
        expect(fixture.audit.size() == 1U);
        expect(fixture.audit.front().actor_kind == "player");
        expect(fixture.audit.front().actor_uuid ==
               std::string{operator_uuid_text});
        expect(fixture.audit.front().action == "reload");
        expect(fixture.audit.front().result == "ok");
    };
};
