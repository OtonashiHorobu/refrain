// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// `/permissions` administration: the console and player frontends, the
// permission gate on the tree, the audit record of a change, the negative
// cases and the command-tree invalidation when the policy revision moves.

#include <refrain/command/command.hpp>
#include <refrain/contracts/services/permissions.hpp>
#include <refrain/core/config.hpp>
#include <refrain/core/log.hpp>
#include <refrain/core/time.hpp>
#include <refrain/net/server.hpp>
#include <refrain/server/command_bridge.hpp>
#include <refrain/server/console_commands.hpp>
#include <refrain/server/permissions_admin.hpp>

#include <support/permission_policy_provider.hpp>

#include <boost/ut.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace command = refrain::command;
namespace net = refrain::net;
namespace server = refrain::server;
namespace test_support = refrain::test_support;

constexpr std::string_view alice_uuid{"069a79f4-44e9-4726-a5be-fca90e38aaf5"};
constexpr std::string_view alice_key{
    "player:069a79f4-44e9-4726-a5be-fca90e38aaf5"};
constexpr std::uint64_t alice_connection{7};

constexpr std::string_view basic_policy{R"(
[defaults]
default = "deny"

[grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["refrain.permissions.admin"]
)"};

constexpr std::string_view deny_precedence_policy{R"(
[defaults]
default = "deny"

[grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["refrain.permissions.admin", "refrain.command.*", "!refrain.command.stop"]

[groups]
moderators = ["refrain.mods.admin"]
)"};

/// A command source that records the feedback lines an executor sends.
class recording_source final : public command::source {
  public:
    recording_source(std::string name, std::uint64_t key, bool player,
                     command::permission_level ladder,
                     std::vector<std::string> &replies)
        : name_{std::move(name)}, key_{key}, player_{player}, ladder_{ladder},
          replies_{replies} {}

    [[nodiscard]] auto identity() const noexcept
        -> command::source_identity override {
        return command::source_identity{name_, key_, player_};
    }

    [[nodiscard]] auto permission() const noexcept
        -> command::permission_level override {
        return ladder_;
    }

    void reply(std::string_view message) override {
        replies_.emplace_back(message);
    }

    [[nodiscard]] auto online_players() const
        -> std::vector<std::string> override {
        return {"Alice"};
    }

  private:
    std::string name_;
    std::uint64_t key_;
    bool player_;
    command::permission_level ladder_;
    std::vector<std::string> &replies_;
};

[[nodiscard]] auto has_line(const std::vector<std::string> &lines,
                            std::string_view needle) -> bool {
    return std::any_of(lines.begin(), lines.end(),
                       [needle](const std::string &line) {
                           return line.find(needle) != std::string::npos;
                       });
}

template <typename TTree>
[[nodiscard]] auto tree_has_literal(const TTree &tree, std::string_view name)
    -> bool {
    return std::ranges::any_of(
        tree.nodes, [name](const auto &node) { return node.name == name; });
}

[[nodiscard]] auto read_text(const std::filesystem::path &path) -> std::string {
    std::ifstream in{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{in},
                       std::istreambuf_iterator<char>{}};
}

/// Command environment with a loaded in-process permission provider, the
/// management binding and one joined player ("Alice").
struct command_fixture {
    std::vector<server::permissions_audit_record> audit{};
    std::vector<std::string> replies{};
    std::shared_ptr<server::command_environment> environment{
        std::make_shared<server::command_environment>()};
    test_support::permission_policy_provider provider{};
    net::player_identity alice{};
    net::command_bridge bridge{};

    explicit command_fixture(std::string_view policy_text,
                             bool bind_provider = true) {
        const auto parsed{net::uuid::parse(alice_uuid)};
        expect(parsed.has_value());
        alice = net::player_identity{
            .connection = alice_connection,
            .id = parsed.value_or(net::uuid{}),
            .name = "Alice",
            .world = "minecraft:overworld",
        };
        environment->listener = nullptr;
        environment->remember_key(server::player_identity_key(alice.id),
                                  alice_connection, std::string{alice_uuid},
                                  "Alice", "minecraft:overworld");
        environment->modes[alice_connection] = net::game_mode::creative;
        expect(provider.reload(policy_text));
        environment->permissions.audit =
            [this](const server::permissions_audit_record &record) {
                audit.push_back(record);
            };
        if (bind_provider) {
            server::bind_permissions(environment, provider.vtable(),
                                     provider.self());
            environment->permissions.admin = provider.admin();
            auto *const raw{environment.get()};
            environment->permissions.find_player =
                [raw](std::string_view name) {
                    return raw->find_online_player(name);
                };
        }
        bridge = server::make_command_bridge(environment);
    }

    [[nodiscard]] auto
    console(command::permission_level ladder = command::level::owner)
        -> recording_source {
        return recording_source{"@console", 0, false, ladder, replies};
    }

    [[nodiscard]] auto
    player(command::permission_level ladder = command::level::everyone)
        -> recording_source {
        return recording_source{"Alice", server::player_identity_key(alice.id),
                                true, ladder, replies};
    }

    void clear() {
        replies.clear();
        audit.clear();
    }
};

const suite permissions_command_tests = [] {
    "permissions/console check reports the console rule"_test = [] {
        command_fixture fixture{basic_policy};
        auto source{fixture.console()};
        const auto executed{fixture.environment->dispatcher.execute(
            "permissions check console refrain.command.teleport", source)};
        expect(static_cast<bool>(executed));
        expect(has_line(fixture.replies, "allowed"));
        expect(has_line(fixture.replies, "console holds every node"));
        expect(fixture.audit.empty());
    };

    "permissions/check reports a grant, a deny, the fallback and the context"_test =
        [] {
            command_fixture fixture{deny_precedence_policy};
            auto source{fixture.console()};
            const auto run = [&fixture, &source](std::string_view line) {
                fixture.replies.clear();
                const auto executed{
                    fixture.environment->dispatcher.execute(line, source)};
                expect(static_cast<bool>(executed))
                    << (executed ? std::string{} : executed.error().message);
                return fixture.replies;
            };
            auto lines{run("permissions check Alice refrain.command.teleport")};
            expect(has_line(lines, "allowed"));
            expect(has_line(lines, "grant"));
            expect(has_line(lines, "world minecraft:overworld"));
            expect(has_line(lines, "creative"));
            lines = run("permissions check Alice refrain.command.stop");
            expect(has_line(lines, "denied"));
            expect(has_line(lines, "deny"));
            expect(has_line(lines, "!refrain.command.stop"));
            lines = run("permissions check Alice refrain.mods.admin");
            expect(has_line(lines, "denied"));
            expect(has_line(lines, "default policy denies"));
            fixture.replies.clear();
            const auto invalid{fixture.environment->dispatcher.execute(
                "permissions check Alice NotANode", source)};
            expect(!invalid.has_value() || !invalid->success);
            expect(has_line(fixture.replies, "not a concrete permission node"));
        };

    "permissions/grant and revoke change the policy and write one audit record"_test =
        [] {
            command_fixture fixture{basic_policy};
            auto source{fixture.console()};
            const auto baseline{fixture.provider.revision()};
            const auto executed{fixture.environment->dispatcher.execute(
                "permissions grant Alice refrain.mods.admin", source)};
            expect(static_cast<bool>(executed));
            expect(has_line(fixture.replies, "granted"));
            expect(has_line(fixture.replies,
                            fmt::format("revision {}", baseline + 1)));
            expect(fixture.provider.revision() == baseline + 1);
            auto alice{fixture.player()};
            expect(static_cast<bool>(fixture.environment->dispatcher.execute(
                "permissions groups", alice)));
            expect(fixture.audit.size() == 1U);
            if (fixture.audit.size() == 1U) {
                const auto &record{fixture.audit.front()};
                expect(record.action == "grant");
                expect(record.actor_kind == "console");
                expect(record.subject == alice_key);
                expect(record.node == "refrain.mods.admin");
                expect(record.result == "ok");
                expect(record.revision == baseline + 1);
            }
            auto granted{fixture.environment->query_permission(
                alice, "refrain.mods.admin")};
            expect(granted == command::permission_decision::allowed);
            // The same grant again is reported unchanged and does not bump the
            // revision or write a second audit result.
            fixture.replies.clear();
            expect(static_cast<bool>(fixture.environment->dispatcher.execute(
                "permissions grant Alice refrain.mods.admin", source)));
            expect(has_line(fixture.replies, "already granted"));
            expect(fixture.provider.revision() == baseline + 1);
            expect(fixture.audit.size() == 2U);
            if (fixture.audit.size() == 2U) {
                expect(fixture.audit.back().result == "unchanged");
                expect(fixture.audit.back().revision == baseline + 1);
            }
            // Revoking drops it again.
            fixture.replies.clear();
            expect(static_cast<bool>(fixture.environment->dispatcher.execute(
                "permissions revoke Alice refrain.mods.admin", source)));
            expect(has_line(fixture.replies, "revoked"));
            expect(fixture.provider.revision() == baseline + 2);
            granted = fixture.environment->query_permission(
                alice, "refrain.mods.admin");
            expect(granted == command::permission_decision::denied);
            expect(fixture.audit.size() == 3U);
            if (fixture.audit.size() == 3U) {
                expect(fixture.audit.back().action == "revoke");
                expect(fixture.audit.back().result == "ok");
                expect(fixture.audit.back().revision == baseline + 2);
            }
        };

    "permissions/groups lists the configured groups and members"_test = [] {
        command_fixture fixture{R"(
[defaults]
default = "deny"
[grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["@moderators"]
[groups]
moderators = ["refrain.mods.admin", "!refrain.command.stop"]
)"};
        auto source{fixture.console()};
        expect(static_cast<bool>(fixture.environment->dispatcher.execute(
            "permissions groups", source)));
        expect(has_line(fixture.replies, "permission groups (1)"));
        expect(has_line(fixture.replies, "group 'moderators'"));
        expect(has_line(fixture.replies, "refrain.mods.admin"));
        expect(has_line(fixture.replies, alice_key));
        expect(fixture.audit.empty());
    };

    "permissions/reload publishes the staged policy and reports revisions"_test =
        [] {
            command_fixture fixture{basic_policy};
            expect(fixture.provider.stage_reload(R"(
[defaults]
default = "deny"
[grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["refrain.permissions.admin", "refrain.mods.admin"]
)"));
            auto source{fixture.console()};
            const auto baseline{fixture.provider.revision()};
            expect(static_cast<bool>(fixture.environment->dispatcher.execute(
                "permissions reload", source)));
            expect(
                has_line(fixture.replies, fmt::format("revision {} -> {}",
                                                      baseline, baseline + 1)));
            expect(fixture.provider.revision() == baseline + 1);
            expect(fixture.audit.size() == 1U);
            if (fixture.audit.size() == 1U) {
                expect(fixture.audit.front().action == "reload");
                expect(fixture.audit.front().result == "ok");
                expect(fixture.audit.front().revision == baseline + 1);
                expect(fixture.audit.front().subject.empty());
            }
            auto alice{fixture.player()};
            expect(fixture.environment->query_permission(
                       alice, "refrain.mods.admin") ==
                   command::permission_decision::allowed);
        };

    "permissions/invalid arguments fail without changing the policy"_test = [] {
        command_fixture fixture{basic_policy};
        auto source{fixture.console()};
        const auto run = [&fixture, &source](std::string_view line) {
            fixture.replies.clear();
            const auto executed{
                fixture.environment->dispatcher.execute(line, source)};
            return executed.has_value() && executed->success;
        };
        expect(!run("permissions grant Bob refrain.mods.admin"));
        expect(has_line(fixture.replies, "no online player"));
        expect(!run("permissions grant Alice NotANode"));
        expect(has_line(fixture.replies, "invalid permission node"));
        // Incomplete or unknown commands never reach an executor: the command
        // parser rejects them before any policy work.
        expect(!run("permissions grant Alice"));
        expect(!run("permissions check Alice"));
        expect(!run("permissions explode"));
        const auto baseline{fixture.provider.revision()};
        expect(fixture.provider.revision() == baseline);
        expect(fixture.audit.size() == 2U);
        for (const auto &record : fixture.audit) {
            expect(record.result == "invalid");
            expect(record.revision == baseline);
        }
    };

    "permissions/a player without the admin node cannot see or run the tree"_test =
        [] {
            command_fixture fixture{R"(
[defaults]
default = "deny"
[grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["refrain.command.help"]
)"};
            expect(static_cast<bool>(fixture.bridge.tree));
            const auto tree{fixture.bridge.tree(fixture.alice)};
            expect(tree != nullptr);
            if (tree != nullptr) {
                expect(!tree_has_literal(*tree, "permissions"));
            }
            auto alice{fixture.player()};
            expect(!fixture.environment->dispatcher.execute(
                "permissions groups", alice));
            // Granting the admin node from the console makes the tree contain
            // the command on the next snapshot, keyed by the new revision.
            auto console{fixture.console()};
            expect(static_cast<bool>(fixture.environment->dispatcher.execute(
                "permissions grant Alice refrain.permissions.admin", console)));
            const auto after{fixture.bridge.tree(fixture.alice)};
            expect(after != nullptr);
            if (after != nullptr) {
                expect(tree_has_literal(*after, "permissions"));
            }
            expect(static_cast<bool>(fixture.environment->dispatcher.execute(
                "permissions groups", alice)));
            expect(fixture.audit.size() == 1U);
            if (fixture.audit.size() == 1U) {
                expect(fixture.audit.front().actor_kind == "console");
            }
            // A player-run change records the player actor and its uuid.
            expect(static_cast<bool>(fixture.environment->dispatcher.execute(
                "permissions grant Alice refrain.mods.admin", alice)));
            expect(fixture.audit.size() == 2U);
            if (fixture.audit.size() == 2U) {
                expect(fixture.audit.back().actor_kind == "player");
                expect(fixture.audit.back().actor_uuid == alice_uuid);
                expect(fixture.audit.back().actor_name == "Alice");
            }
        };

    "permissions/without a provider the commands report not-handled"_test = [] {
        command_fixture fixture{basic_policy, false};
        auto source{fixture.console()};
        expect(static_cast<bool>(fixture.environment->dispatcher.execute(
            "permissions check console refrain.command.help", source)));
        expect(has_line(fixture.replies, "not-handled"));
        expect(has_line(fixture.replies, "no permission service"));
        fixture.replies.clear();
        const auto refused{fixture.environment->dispatcher.execute(
            "permissions grant console refrain.command.help", source)};
        expect(!refused.has_value() || !refused->success);
        expect(has_line(fixture.replies, "no permission service"));
        expect(fixture.audit.size() == 1U);
        if (fixture.audit.size() == 1U) {
            expect(fixture.audit.front().result == "failed");
        }
    };

    "permissions/audit formatting names actor, subject, node and revision"_test =
        [] {
            server::permissions_audit_record record{};
            record.actor_kind = "console";
            record.actor_name = "@console";
            record.action = "grant";
            record.subject = alice_key;
            record.node = "refrain.mods.admin";
            record.result = "ok";
            record.revision = 4;
            record.duration_ms = 2;
            const auto line{server::format_permissions_audit_record(record)};
            expect(line.find("console @console ran grant") != std::string::npos)
                << line;
            expect(line.find(alice_key) != std::string::npos) << line;
            expect(line.find("refrain.mods.admin") != std::string::npos)
                << line;
            expect(line.find("revision 4") != std::string::npos) << line;
            expect(line.find("the result was ok") != std::string::npos) << line;
            expect(line.find("after 2ms") != std::string::npos) << line;
        };
};

/// Console frontend: the `permissions` line shares the same bodies and audit.
const suite permissions_console_tests = [] {
    "permissions console/a change writes the audit record"_test = [] {
        command_fixture fixture{basic_policy};
        const auto log_path{std::filesystem::temp_directory_path() /
                            "refrain-permissions-console.log"};
        refrain::log::shutdown();
        refrain::log::config cfg{};
        cfg.console_level = refrain::log::level::off;
        cfg.file_path = log_path.string();
        cfg.file_level = refrain::log::level::info;
        cfg.enable_colors = false;
        expect(static_cast<bool>(refrain::log::init(cfg)));
        server::tick_metrics metrics{};
        server::console_commands commands{
            server::console_dependencies{.permissions =
                                             &fixture.environment->permissions},
            metrics};
        const refrain::core::tick_info tick{};
        commands.execute("permissions grant Alice refrain.mods.admin", tick);
        commands.execute("permissions groups", tick);
        commands.execute("permissions reload", tick);
        refrain::log::shutdown();
        const auto text{read_text(log_path)};
        expect(text.find("granted 'refrain.mods.admin'") != std::string::npos)
            << text;
        expect(text.find("permission groups") != std::string::npos) << text;
        expect(text.find("unchanged at revision") != std::string::npos) << text;
        expect(fixture.audit.size() == 2U);
        expect(fixture.audit.front().action == "grant");
        expect(fixture.audit.back().action == "reload");
        std::error_code error;
        std::filesystem::remove(log_path, error);
    };
};

} // namespace
