// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// End-to-end checks for the built-in `refrain.permissions` mod: the default
// gamepack selects it as the chain base, the host activates its provider, and
// the queries, revision and reload behaviour cross the generated contract.

#include <refrain/builtin/image.hpp>
#include <refrain/contracts/services/permissions.hpp>
#include <refrain/core/config.hpp>
#include <refrain/core/error.hpp>
#include <refrain/core/io.hpp>
#include <refrain/mod/gamepack.hpp>
#include <refrain/mod/host.hpp>
#include <refrain/permissions/client.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace {

using namespace boost::ut;

namespace config = refrain::core::config;
namespace io = refrain::core::io;
namespace mod = refrain::mod;
namespace permissions = refrain::permissions;

using permissions::permission_result;
using permissions::subject;

constexpr std::string_view player_uuid{"069a79f4-44e9-4726-a5be-fca90e38aaf5"};
constexpr std::string_view player_name{"Notch"};

[[nodiscard]] auto builtin_bytes() -> std::span<const std::byte> {
    return std::span<const std::byte>{refrain::builtin::image_data(),
                                      refrain::builtin::image_size()};
}

[[nodiscard]] auto load_default_plan()
    -> refrain::result<refrain::mod::composition> {
    auto spec{mod::load_gamepack_spec(
        std::string{REFRAIN_SOURCE_DIR} + "/gamepack/gamepack.toml", "server")};
    if (!spec) {
        return std::unexpected{spec.error()};
    }
    return refrain::mod::preview_composition(*spec);
}

/// Unique configuration file under the system temp directory.
[[nodiscard]] auto make_temp_path() -> std::filesystem::path {
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    return std::filesystem::temp_directory_path() /
           ("refrain_permissions_test_" + std::to_string(stamp) + ".toml");
}

void remove_quietly(const std::filesystem::path &path) noexcept {
    auto error{std::error_code{}};
    std::filesystem::remove(path, error);
}

/// One loaded host with a mounted `permissions` configuration namespace.
///
/// The configuration store is declared before the host so it outlives the
/// provider the host constructs from it.
class service_fixture {
  public:
    explicit service_fixture(std::string_view config_text) {
        path_ = make_temp_path();
        expect(static_cast<bool>(io::write_text(path_.string(), config_text)));
        expect(static_cast<bool>(
            store_.mount({.name = "permissions", .path = path_.string()})));
        expect(static_cast<bool>(store_.load_all()));
    }

    service_fixture(const service_fixture &) = delete;
    auto operator=(const service_fixture &) -> service_fixture & = delete;
    service_fixture(service_fixture &&) = delete;
    auto operator=(service_fixture &&) -> service_fixture & = delete;
    ~service_fixture() { remove_quietly(path_); }

    [[nodiscard]] auto load() -> refrain::status {
        auto plan{load_default_plan()};
        if (!plan) {
            return std::unexpected{plan.error()};
        }
        host_.set_config(store_);
        return host_.load(*plan, builtin_bytes());
    }

    [[nodiscard]] auto host() noexcept -> refrain::mod::host & { return host_; }

    /// Replace the configuration file. The provider reads the file on reload;
    /// the store is not reloaded.
    void rewrite(std::string_view text) const {
        expect(static_cast<bool>(io::write_text(path_.string(), text)));
    }

    [[nodiscard]] auto config_path() const -> std::string {
        return path_.string();
    }

  private:
    refrain::core::config::store store_{};
    refrain::mod::host host_{};
    std::filesystem::path path_;
};

[[nodiscard]] auto bind_client(refrain::mod::host &host)
    -> refrain::result<permissions::client> {
    const auto provider{host.services().active_provider("refrain:permissions")};
    if (!provider) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::not_found,
            "test host has no active refrain:permissions provider")};
    }
    const auto *api{
        static_cast<const refrain::mod::provider_api *>(provider->instance)};
    if (api == nullptr || api->vtable == nullptr || api->self == nullptr) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::invalid_argument,
            "test host published an incomplete provider table")};
    }
    return permissions::client::bind(api->vtable, api->self);
}

[[nodiscard]] auto player_subject(std::uint32_t op_level = 0)
    -> permissions::subject {
    return permissions::subject::player(player_uuid, player_name, op_level);
}

constexpr std::string_view basic_config{R"(
[defaults]
default = "deny"

[op]
min_level = 1
nodes = ["refrain.mods.admin"]

[grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["refrain.command.*", "!refrain.command.stop"]

[groups]
moderators = ["refrain.permissions.admin"]
)"};

const suite service_tests = [] {
    "service/the default gamepack activates the chain base"_test = [] {
        auto spec{mod::load_gamepack_spec(std::string{REFRAIN_SOURCE_DIR} +
                                              "/gamepack/gamepack.toml",
                                          "server")};
        expect(static_cast<bool>(spec));
        if (!spec) {
            return;
        }
        const auto binding{
            std::find_if(spec->providers.begin(), spec->providers.end(),
                         [](const mod::provider_binding &provider) {
                             return provider.service == "permissions";
                         })};
        expect(binding != spec->providers.end());
        if (binding == spec->providers.end()) {
            return;
        }
        expect(binding->mode == mod::provider_mode::chain);
        expect(binding->base.has_value());
        if (binding->base) {
            expect(binding->base->id == "refrain.permissions");
        }
        expect(binding->wrappers.empty());
    };

    "service/the built-in mod loads and answers through the contract"_test =
        [] {
            service_fixture fixture{basic_config};
            auto loaded{fixture.load()};
            expect(static_cast<bool>(loaded))
                << (loaded ? std::string{} : loaded.error().message);
            if (!loaded) {
                return;
            }
            const auto provider{fixture.host().services().active_provider(
                "refrain:permissions")};
            expect(provider.has_value());
            if (!provider) {
                return;
            }
            expect(provider->id == "refrain.permissions");
            expect(provider->version == "1.0.0");
            const auto *api{
                static_cast<const mod::provider_api *>(provider->instance)};
            expect(api != nullptr);
            if (api == nullptr) {
                return;
            }
            expect(api->abi_major == REFRAIN_SERVICES_PERMISSIONS_ABI_MAJOR);

            auto bound{bind_client(fixture.host())};
            expect(static_cast<bool>(bound));
            if (!bound) {
                return;
            }
            const auto &client{*bound};
            expect(client.revision() == 1U);
            expect(client.query(permissions::subject::console(),
                                "refrain.mods.admin") ==
                   permission_result::allowed);
            expect(client.query(permissions::subject::console(), "a.b") ==
                   permission_result::allowed);
            expect(client.query(player_subject(), "refrain.command.teleport") ==
                   permission_result::allowed);
            expect(client.query(player_subject(), "refrain.command.stop") ==
                   permission_result::denied);
            expect(client.query(player_subject(1), "refrain.mods.admin") ==
                   permission_result::allowed);
            expect(client.query(permissions::subject::player(
                                    "11111111-2222-3333-4444-555555555555"),
                                "refrain.command.teleport") ==
                   permission_result::denied);
            expect(client.has(player_subject(), "refrain.command.teleport"));
            expect(!client.has(player_subject(), "refrain.command.stop"));
            // The base resolver answers its own fallback the same way the chain
            // query does: there are no wrapper grants in this composition.
            expect(client.default_result(player_subject(),
                                         "refrain.command.teleport") ==
                   client.query(player_subject(), "refrain.command.teleport"));
        };

    "service/groups and operator nodes resolve through the built-in mod"_test =
        [] {
            service_fixture fixture{R"(
[defaults]
default = "op"
[op]
min_level = 2
nodes = ["refrain.command.gamemode"]

[grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["@moderators"]
[groups]
moderators = ["refrain.permissions.admin"]
)"};
            auto loaded{fixture.load()};
            expect(static_cast<bool>(loaded))
                << (loaded ? std::string{} : loaded.error().message);
            if (!loaded) {
                return;
            }
            auto bound{bind_client(fixture.host())};
            expect(static_cast<bool>(bound));
            if (!bound) {
                return;
            }
            const auto &client{*bound};
            // Group grant.
            expect(
                client.query(player_subject(), "refrain.permissions.admin") ==
                permission_result::allowed);
            // Operator node at and above the configured level.
            expect(
                client.query(player_subject(2), "refrain.command.gamemode") ==
                permission_result::allowed);
            expect(
                client.query(player_subject(1), "refrain.command.gamemode") ==
                permission_result::denied);
            // The "op" fallback: an operator may use an unmatched node, a
            // non-operator may not.
            expect(
                client.query(player_subject(2), "refrain.command.teleport") ==
                permission_result::allowed);
            expect(
                client.query(player_subject(1), "refrain.command.teleport") ==
                permission_result::denied);
        };

    "service/a context is carried but v1 resolves globally"_test = [] {
        service_fixture fixture{basic_config};
        auto loaded{fixture.load()};
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        auto bound{bind_client(fixture.host())};
        expect(static_cast<bool>(bound));
        if (!bound) {
            return;
        }
        const auto &client{*bound};
        const permissions::context_entry extra[] = {
            {.key = "dimension_type", .value = "overworld"}};
        const permissions::context creative{
            .world = "minecraft:overworld",
            .gamemode = "creative",
            .extra = std::span<const permissions::context_entry>{extra}};
        expect(client.query(player_subject(), "refrain.command.teleport",
                            creative) ==
               client.query(player_subject(), "refrain.command.teleport"));
        expect(client.query(player_subject(), "refrain.mods.admin", creative) ==
               client.query(player_subject(), "refrain.mods.admin"));
    };

    "service/a malformed query is denied instead of evaluated"_test = [] {
        service_fixture fixture{basic_config};
        auto loaded{fixture.load()};
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        auto bound{bind_client(fixture.host())};
        expect(static_cast<bool>(bound));
        if (!bound) {
            return;
        }
        const auto &client{*bound};
        const auto &table{client.table()};

        refrain_services_permissions_permission_query zero{};
        expect(table.query(client.self(), &zero) ==
               REFRAIN_SERVICES_PERMISSIONS_PERMISSION_RESULT_DENIED);
        expect(table.query(client.self(), nullptr) ==
               REFRAIN_SERVICES_PERMISSIONS_PERMISSION_RESULT_DENIED);

        auto query{refrain::services::permissions::make_permission_query()};
        query.subject = refrain::services::permissions::make_subject_ref();
        query.subject.struct_size = 0;
        query.node = refrain::contracts::to_borrowed("a.b");
        expect(table.query(client.self(), &query) ==
               REFRAIN_SERVICES_PERMISSIONS_PERMISSION_RESULT_DENIED);

        // Unknown subject kind: a well-formed struct with an enum value the
        // resolver does not understand still fails closed.
        query = refrain::services::permissions::make_permission_query();
        query.subject = refrain::services::permissions::make_subject_ref();
        query.context =
            refrain::services::permissions::make_permission_context();
        query.subject.kind = 99;
        query.node = refrain::contracts::to_borrowed("a.b");
        expect(table.query(client.self(), &query) ==
               REFRAIN_SERVICES_PERMISSIONS_PERMISSION_RESULT_DENIED);
    };

    "service/without a mounted configuration console keeps every node"_test =
        [] {
            auto plan{load_default_plan()};
            expect(static_cast<bool>(plan));
            if (!plan) {
                return;
            }
            // No `set_config()`: the provider serves the built-in policy.
            mod::host host{};
            auto loaded{host.load(*plan, builtin_bytes())};
            expect(static_cast<bool>(loaded))
                << (loaded ? std::string{} : loaded.error().message);
            if (!loaded) {
                return;
            }
            auto bound{bind_client(host)};
            expect(static_cast<bool>(bound));
            if (!bound) {
                return;
            }
            const auto &client{*bound};
            expect(client.revision() == 1U);
            expect(client.query(permissions::subject::console(), "a.b") ==
                   permission_result::allowed);
            expect(
                client.query(player_subject(4), "refrain.command.teleport") ==
                permission_result::denied);
            expect(client.query(permissions::subject::player(player_uuid),
                                "refrain.mods.admin") ==
                   permission_result::denied);
            // Nothing to read: a reload is a successful no-op.
            const auto reloaded{client.reload()};
            expect(static_cast<bool>(reloaded));
            if (reloaded) {
                expect(*reloaded == 1U);
            }
        };

    "service/reload applies an edited configuration and bumps the revision"_test =
        [] {
            service_fixture fixture{R"(
[defaults]
default = "deny"
)"};
            auto loaded{fixture.load()};
            expect(static_cast<bool>(loaded))
                << (loaded ? std::string{} : loaded.error().message);
            if (!loaded) {
                return;
            }
            auto bound{bind_client(fixture.host())};
            expect(static_cast<bool>(bound));
            if (!bound) {
                return;
            }
            const auto &client{*bound};
            expect(client.revision() == 1U);
            expect(client.query(player_subject(), "refrain.command.teleport") ==
                   permission_result::denied);

            fixture.rewrite(R"(
[grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["refrain.command.teleport"]
)");
            const auto reloaded{client.reload()};
            expect(static_cast<bool>(reloaded))
                << (reloaded ? std::string{}
                             : "reload failed with code " +
                                   std::to_string(static_cast<std::uint32_t>(
                                       reloaded.error())));
            if (reloaded) {
                expect(*reloaded == 2U);
            }
            expect(client.revision() == 2U);
            expect(client.query(player_subject(), "refrain.command.teleport") ==
                   permission_result::allowed);
            expect(client.query(player_subject(), "refrain.command.gamemode") ==
                   permission_result::denied);

            // An unchanged file publishes nothing new.
            const auto again{client.reload()};
            expect(static_cast<bool>(again));
            if (again) {
                expect(*again == 2U);
            }
            expect(client.revision() == 2U);
        };

    "service/a failed reload keeps the previous policy and revision"_test = [] {
        service_fixture fixture{basic_config};
        auto loaded{fixture.load()};
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        auto bound{bind_client(fixture.host())};
        expect(static_cast<bool>(bound));
        if (!bound) {
            return;
        }
        const auto &client{*bound};
        expect(client.revision() == 1U);

        fixture.rewrite("[defaults]\nunknown = true\n");
        const auto failed{client.reload()};
        expect(!failed);
        if (!failed) {
            expect(
                failed.error() ==
                refrain::services::permissions::permission_error::parse_error);
        }
        expect(client.revision() == 1U);
        expect(client.query(player_subject(), "refrain.command.teleport") ==
               permission_result::allowed);

        // The file is broken; a query must not touch it. A later valid edit
        // still reloads normally.
        fixture.rewrite(R"(
[grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["refrain.mods.admin"]
)");
        const auto recovered{client.reload()};
        expect(static_cast<bool>(recovered));
        if (recovered) {
            expect(*recovered == 2U);
        }
        expect(client.query(player_subject(), "refrain.command.teleport") ==
               permission_result::denied);
        expect(client.query(player_subject(1), "refrain.mods.admin") ==
               permission_result::allowed);
    };

    "service/an invalid configuration aborts startup and names the entry"_test =
        [] {
            service_fixture fixture{R"(
[grants]
"player:not a valid key" = ["refrain.command.teleport"]
)"};
            const auto loaded{fixture.load()};
            expect(!loaded);
            if (!loaded) {
                expect(loaded.error().message.find("player:not a valid key") !=
                       std::string::npos);
                expect(loaded.error().message.find("permissions") !=
                       std::string::npos);
            }
        };
};

} // namespace
