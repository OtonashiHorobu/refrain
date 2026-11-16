// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Console and in-game mod management frontends: the console `mods ...`
// dispatch, the `/mods` command tree registration, the operator permission,
// the explicit denial for a non-operator and the audit actor identity.

#include <refrain/app/mod_management.hpp>
#include <refrain/server/command_bridge.hpp>
#include <refrain/server/console_commands.hpp>

#include <refrain/core/log.hpp>
#include <refrain/core/time.hpp>
#include <refrain/sim/world.hpp>
#include <refrain/world/store.hpp>

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
                     std::vector<std::string> &replies)
        : name_{std::move(name)}, key_{key}, replies_{replies} {}

    [[nodiscard]] auto identity() const noexcept
        -> command::source_identity override {
        return command::source_identity{name_, key_, true};
    }

    [[nodiscard]] auto permission() const noexcept
        -> command::permission_level override {
        return command::level::admin;
    }

    void reply(std::string_view message) override {
        replies_.emplace_back(message);
    }

  private:
    std::string name_;
    std::uint64_t key_;
    std::vector<std::string> &replies_;
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
            cfg.enable_colors = false;
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
        cfg.enable_colors = false;
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
            cfg.enable_colors = false;
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

const suite mod_command_tree_tests = [] {
    "mod command tree/registers /mods and its subcommands for an operator"_test =
        [] {
            management_fixture fixture{};
            auto service{fixture.service()};
            const auto operator_uuid{
                sample_uuid("11111111-1111-1111-1111-111111111111")};
            const auto operator_key{server::player_identity_key(operator_uuid)};
            auto environment{std::make_shared<server::command_environment>()};
            environment->mods = &service;
            environment->mod_admin =
                [operator_key](const command::source &source) {
                    return !source.identity().player ||
                           source.identity().key == operator_key;
                };
            server::register_runtime_commands(environment);

            std::vector<std::string> replies{};
            recording_player op{"Alice", operator_key, replies};
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

    "mod command tree/hides /mods from a non-operator"_test = [] {
        management_fixture fixture{};
        auto service{fixture.service()};
        const auto operator_key{server::player_identity_key(
            sample_uuid("11111111-1111-1111-1111-111111111111"))};
        auto environment{std::make_shared<server::command_environment>()};
        environment->mods = &service;
        environment->mod_admin = [operator_key](const command::source &source) {
            return !source.identity().player ||
                   source.identity().key == operator_key;
        };
        server::register_runtime_commands(environment);

        std::vector<std::string> replies{};
        recording_player guest{"Bob", 7U, replies};
        auto tree{environment->dispatcher.tree(guest)};
        expect(tree != nullptr);
        if (tree) {
            expect(!tree_has_literal(*tree, "mods"));
        }
    };

    "mod command tree/an operator runs /mods list through the shared service"_test =
        [] {
            management_fixture fixture{};
            auto service{fixture.service()};
            const auto operator_key{server::player_identity_key(
                sample_uuid("11111111-1111-1111-1111-111111111111"))};
            auto environment{std::make_shared<server::command_environment>()};
            environment->mods = &service;
            environment->mod_admin =
                [operator_key](const command::source &source) {
                    return !source.identity().player ||
                           source.identity().key == operator_key;
                };
            server::register_runtime_commands(environment);

            std::vector<std::string> replies{};
            recording_player op{"Alice", operator_key, replies};
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

    "mod command tree/a non-operator is rejected by the tree"_test = [] {
        management_fixture fixture{};
        auto service{fixture.service()};
        const auto operator_key{server::player_identity_key(
            sample_uuid("11111111-1111-1111-1111-111111111111"))};
        auto environment{std::make_shared<server::command_environment>()};
        environment->mods = &service;
        environment->mod_admin = [operator_key](const command::source &source) {
            return !source.identity().player ||
                   source.identity().key == operator_key;
        };
        server::register_runtime_commands(environment);
        std::vector<std::string> replies{};
        recording_player guest{"Bob", 7U, replies};
        const auto executed{
            environment->dispatcher.execute("mods list", guest)};
        expect(!executed);
        if (!executed) {
            expect(executed.error().code == refrain::error_code::parse_error);
        }
    };
};

const suite mod_game_bridge_tests = [] {
    "mod game bridge/answers an explicit denial to a non-operator"_test = [] {
        management_fixture fixture{};
        auto service{fixture.service()};
        const auto operator_uuid{
            sample_uuid("11111111-1111-1111-1111-111111111111")};
        const auto operator_key{server::player_identity_key(operator_uuid)};
        auto environment{std::make_shared<server::command_environment>()};
        environment->mods = &service;
        environment->mod_admin = [operator_key](const command::source &source) {
            return !source.identity().player ||
                   source.identity().key == operator_key;
        };
        auto bridge{server::make_command_bridge(environment)};
        const net::player_identity guest{
            .connection = 2,
            .id = sample_uuid("22222222-2222-2222-2222-222222222222"),
            .name = "Bob",
            .world = {},
        };
        const auto feedback{bridge.execute(guest, "mods list")};
        expect(feedback.handled);
        expect(!feedback.success);
        expect(feedback.message.find("refrain.mods.admin") != std::string::npos)
            << feedback.message;
        expect(fixture.audit.empty());
    };

    "mod game bridge/the tree cache does not leak the operator tree"_test = [] {
        management_fixture fixture{};
        auto service{fixture.service()};
        const net::player_identity operator_identity{
            .connection = 1,
            .id = sample_uuid("11111111-1111-1111-1111-111111111111"),
            .name = "Alice",
            .world = {},
        };
        const auto operator_key{
            server::player_identity_key(operator_identity.id)};
        auto environment{std::make_shared<server::command_environment>()};
        environment->mods = &service;
        environment->mod_admin = [operator_key](const command::source &source) {
            return !source.identity().player ||
                   source.identity().key == operator_key;
        };
        auto bridge{server::make_command_bridge(environment)};
        const auto operator_tree{bridge.tree(operator_identity)};
        expect(operator_tree != nullptr);
        if (!operator_tree) {
            return;
        }
        expect(tree_has_literal(*operator_tree, "mods"));
        const auto guest_tree{bridge.tree(net::player_identity{
            .connection = 2,
            .id = sample_uuid("22222222-2222-2222-2222-222222222222"),
            .name = "Bob",
            .world = {},
        })};
        expect(guest_tree != nullptr);
        if (guest_tree) {
            expect(!tree_has_literal(*guest_tree, "mods"));
        }
    };

    "mod game bridge/an operator executes /mods and the audit carries the "
    "uuid"_test = [] {
        management_fixture fixture{};
        auto service{fixture.service()};
        const net::player_identity operator_identity{
            .connection = 1,
            .id = sample_uuid("11111111-1111-1111-1111-111111111111"),
            .name = "Alice",
            .world = {},
        };
        const auto operator_key{
            server::player_identity_key(operator_identity.id)};
        auto environment{std::make_shared<server::command_environment>()};
        environment->mods = &service;
        environment->mod_admin = [operator_key](const command::source &source) {
            return !source.identity().player ||
                   source.identity().key == operator_key;
        };
        auto bridge{server::make_command_bridge(environment)};
        const auto feedback{bridge.execute(operator_identity, "mods list")};
        expect(feedback.handled);
        expect(feedback.success) << feedback.message;
        expect(fixture.audit.size() == 1U);
        expect(fixture.audit.front().actor_kind == "player");
        expect(fixture.audit.front().actor_name == "Alice");
        expect(fixture.audit.front().actor_uuid ==
               "11111111-1111-1111-1111-111111111111");
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
        const net::player_identity operator_identity{
            .connection = 1,
            .id = sample_uuid("11111111-1111-1111-1111-111111111111"),
            .name = "Alice",
            .world = {},
        };
        const auto operator_key{
            server::player_identity_key(operator_identity.id)};
        auto environment{std::make_shared<server::command_environment>()};
        environment->mods = &service;
        environment->mod_admin = [operator_key](const command::source &source) {
            return !source.identity().player ||
                   source.identity().key == operator_key;
        };
        auto bridge{server::make_command_bridge(environment)};
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
        // The same operation is audited with the player identity, so the
        // console and the in-game frontends leave the same trace.
        expect(fixture.audit.size() == 1U);
        expect(fixture.audit.front().actor_kind == "player");
        expect(fixture.audit.front().actor_uuid ==
               "11111111-1111-1111-1111-111111111111");
        expect(fixture.audit.front().action == "reload");
        expect(fixture.audit.front().result == "ok");
    };
};
