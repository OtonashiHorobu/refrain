// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The command layer over a composed permission chain: the wrapper decides its
// own nodes, the base resolves the nodes it forwards, the composed revision
// invalidates the cached command tree and a composition without any provider
// keeps the declared op-level fallback.

#include <refrain/command/command.hpp>
#include <refrain/contracts/services/permissions.hpp>
#include <refrain/net/server.hpp>
#include <refrain/permissions/chain.hpp>
#include <refrain/permissions/client.hpp>
#include <refrain/server/command_bridge.hpp>
#include <refrain/server/permissions_admin.hpp>

#include <support/permission_policy_provider.hpp>

#include <boost/ut.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace command = refrain::command;
namespace net = refrain::net;
namespace permissions = refrain::permissions;
namespace server = refrain::server;
namespace test_support = refrain::test_support;

using permission_result = refrain::services::permissions::permission_result;

constexpr std::string_view alice_uuid{"069a79f4-44e9-4726-a5be-fca90e38aaf5"};
constexpr std::string_view alice_key{
    "player:069a79f4-44e9-4726-a5be-fca90e38aaf5"};
constexpr std::uint64_t alice_connection{11};

constexpr std::string_view base_policy{R"(
[defaults]
default = "deny"

[grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["refrain.command.teleport"]
)"};

/// A wrapper that owns `com.example.home` and forwards every other node.
class wrapper_provider {
  public:
    [[nodiscard]] auto vtable() const
        -> const refrain_services_permissions_vtable & {
        static const refrain_services_permissions_vtable table{
            .struct_size = REFRAIN_SERVICES_PERMISSIONS_VTABLE_SIZE,
            .abi_major = REFRAIN_SERVICES_PERMISSIONS_ABI_MAJOR,
            .abi_minor = REFRAIN_SERVICES_PERMISSIONS_ABI_MINOR,
            .abi_patch = REFRAIN_SERVICES_PERMISSIONS_ABI_PATCH,
            .query = &wrapper_provider::query,
            .default_result = &wrapper_provider::query,
            .has = &wrapper_provider::has,
            .revision = &wrapper_provider::revision,
            .reload = &wrapper_provider::reload,
        };
        return table;
    }

    [[nodiscard]] auto self() noexcept -> void * { return &revision_; }

  private:
    static auto
    query(void *self,
          const refrain_services_permissions_permission_query *value)
        -> refrain_services_permissions_permission_result {
        static_cast<void>(self);
        if (value == nullptr) {
            return REFRAIN_SERVICES_PERMISSIONS_PERMISSION_RESULT_DENIED;
        }
        if (refrain::contracts::as_string_view(value->node) ==
            "com.example.home") {
            return REFRAIN_SERVICES_PERMISSIONS_PERMISSION_RESULT_ALLOWED;
        }
        return REFRAIN_SERVICES_PERMISSIONS_PERMISSION_RESULT_NOT_HANDLED;
    }

    static auto has(void *self,
                    const refrain_services_permissions_permission_query *value)
        -> std::uint8_t {
        return query(self, value) !=
                       REFRAIN_SERVICES_PERMISSIONS_PERMISSION_RESULT_DENIED
                   ? 1U
                   : 0U;
    }

    static auto revision(void *self) -> std::uint64_t {
        const auto *value{static_cast<const std::uint64_t *>(self)};
        return value == nullptr ? 0U : *value;
    }

    static auto reload(void *self, std::uint64_t *out_value) -> refrain_status {
        auto *value{static_cast<std::uint64_t *>(self)};
        if (value == nullptr || out_value == nullptr) {
            return REFRAIN_SERVICES_PERMISSIONS_PERMISSION_ERROR_INVALID_ARGUMENT;
        }
        *out_value = *value;
        return REFRAIN_STATUS_OK;
    }

    std::uint64_t revision_{1};
};

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
    for (const auto &line : lines) {
        if (line.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

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

/// The command environment with a chain bound over one wrapper and the base
/// policy provider, plus the joined player and the context/admin lookups the
/// composition root installs.
struct chain_environment {
    std::vector<std::string> replies{};
    std::vector<server::permissions_audit_record> audit{};
    std::shared_ptr<server::command_environment> environment{
        std::make_shared<server::command_environment>()};
    test_support::permission_policy_provider base{};
    wrapper_provider wrapper{};
    std::optional<permissions::provider_chain> chain{};
    net::player_identity alice{};
    net::command_bridge bridge{};

    chain_environment() {
        const auto parsed{net::uuid::parse(alice_uuid)};
        expect(parsed.has_value());
        alice = net::player_identity{
            .connection = alice_connection,
            .id = parsed.value_or(net::uuid{}),
            .name = "Alice",
            .world = "minecraft:overworld",
        };
        expect(base.reload(base_policy));
        auto built{build_chain()};
        expect(static_cast<bool>(built))
            << (built ? std::string{} : built.error().message);
        if (!built) {
            return;
        }
        chain.emplace(std::move(*built));
        environment->remember_key(server::player_identity_key(alice.id),
                                  alice_connection, std::string{alice_uuid},
                                  "Alice", "minecraft:overworld");
        environment->modes[alice_connection] = net::game_mode::creative;
        environment->permissions.audit =
            [this](const server::permissions_audit_record &record) {
                audit.push_back(record);
            };
        server::bind_permissions(environment, chain->vtable(), chain->self());
        environment->permissions.admin = base.admin();
        auto *const raw{environment.get()};
        environment->permissions.find_player = [raw](std::string_view name) {
            return raw->find_online_player(name);
        };
        const permissions::client chain_client{chain->vtable(), chain->self()};
        environment->permissions.query =
            [chain_client](
                const permissions::subject_view &subject, std::string_view node,
                const permissions::context_view &context) -> permission_result {
            const std::string id{subject.id};
            const std::string profile_name{subject.profile_name};
            std::vector<permissions::context_entry> extra{};
            extra.reserve(context.extra.size());
            for (const auto &entry : context.extra) {
                extra.push_back(permissions::context_entry{
                    .key = entry.key, .value = entry.value});
            }
            return chain_client.query(
                permissions::subject{.kind = subject.kind,
                                     .id = id,
                                     .profile_name = profile_name,
                                     .op_level = 0},
                node,
                permissions::context{.world = context.world,
                                     .gamemode = context.gamemode,
                                     .extra = extra});
        };
        server::register_permissions_commands(environment);
        bridge = server::make_command_bridge(environment);
    }

    [[nodiscard]] auto build_chain()
        -> refrain::result<permissions::provider_chain> {
        return permissions::provider_chain::create(
            {permissions::make_provider_link("refrain.test.wrapper",
                                             wrapper.vtable(), wrapper.self())},
            permissions::make_provider_link("refrain.test.base", base.vtable(),
                                            base.self()));
    }

    [[nodiscard]] auto console() -> recording_source {
        return recording_source{"@console", 0, false, command::level::owner,
                                replies};
    }

    [[nodiscard]] auto player() -> recording_source {
        return recording_source{"Alice", server::player_identity_key(alice.id),
                                true, command::level::everyone, replies};
    }
};

const suite chain_wiring_tests = [] {
    "chain wiring/a wrapper node is allowed while the base denies it"_test =
        [] {
            chain_environment fixture{};
            auto source{fixture.player()};
            expect(fixture.environment->query_permission(source,
                                                         "com.example.home") ==
                   command::permission_decision::allowed);
            expect(fixture.environment->query_permission(
                       source, "refrain.command.teleport") ==
                   command::permission_decision::allowed);
            expect(fixture.environment->query_permission(
                       source, "refrain.command.stop") ==
                   command::permission_decision::denied);
            // The composed revision covers the base and the wrapper.
            expect(fixture.environment->permission_revision() ==
                   fixture.base.revision() + 1U);
        };

    "chain wiring/an administration change invalidates the cached tree"_test =
        [] {
            chain_environment fixture{};
            const auto revision_before{
                fixture.environment->permission_revision()};
            expect(static_cast<bool>(fixture.bridge.tree));
            const auto before{fixture.bridge.tree(fixture.alice)};
            expect(before != nullptr);
            if (before != nullptr) {
                expect(!tree_has_literal(*before, "permissions"));
            }
            auto source{fixture.console()};
            const auto granted{fixture.environment->dispatcher.execute(
                "permissions grant Alice refrain.permissions.admin", source)};
            expect(static_cast<bool>(granted))
                << (granted ? std::string{} : granted.error().message);
            expect(fixture.environment->permission_revision() ==
                   revision_before + 1);
            // The tree cache is keyed by the composed revision, so the grant
            // built a new tree that contains the administration command.
            const auto after{fixture.bridge.tree(fixture.alice)};
            expect(after != nullptr);
            if (after != nullptr) {
                expect(tree_has_literal(*after, "permissions"));
            }
        };

    "chain wiring/check reports the wrapper answer and the base fallback"_test =
        [] {
            chain_environment fixture{};
            auto source{fixture.console()};
            fixture.replies.clear();
            const auto checked{fixture.environment->dispatcher.execute(
                "permissions check Alice com.example.home", source)};
            expect(static_cast<bool>(checked));
            const auto first_line = fixture.replies.empty()
                                        ? std::string{"no reply"}
                                        : fixture.replies.front();
            expect(has_line(fixture.replies, "allowed")) << first_line;
            expect(has_line(fixture.replies, "wrapper provider answered"))
                << first_line;
            fixture.replies.clear();
            expect(static_cast<bool>(fixture.environment->dispatcher.execute(
                "permissions check Alice refrain.command.teleport", source)));
            const auto base_line = fixture.replies.empty()
                                       ? std::string{"no reply"}
                                       : fixture.replies.front();
            expect(has_line(fixture.replies, "allowed")) << base_line;
            expect(!has_line(fixture.replies, "wrapper provider answered"))
                << base_line;
        };
};

/// A dispatcher with one command whose declared fallback is the admin level,
/// used to pin the provider-less composition behaviour.
const suite provider_less_tests = [] {
    "provider-less/a command without a permission provider keeps its op fallback"_test =
        [] {
            auto environment{std::make_shared<server::command_environment>()};
            environment->dispatcher.literal("probe")
                .require_node("refrain.command.probe", command::level::admin)
                .executes([](command::context &) { return 1; });
            expect(!environment->permission_query);
            expect(!environment->permission_revision);
            const auto bridge{server::make_command_bridge(environment)};
            static_cast<void>(bridge);
            const auto parsed{net::uuid::parse(alice_uuid)};
            expect(parsed.has_value());
            if (!parsed) {
                return;
            }
            std::vector<std::string> replies{};
            recording_source guest_source{"Alice", 1, true,
                                          command::level::everyone, replies};
            recording_source owner_source{"@console", 0, false,
                                          command::level::owner, replies};
            expect(!environment->dispatcher.execute("probe", guest_source));
            expect(static_cast<bool>(
                environment->dispatcher.execute("probe", owner_source)));
        };
};

} // namespace
