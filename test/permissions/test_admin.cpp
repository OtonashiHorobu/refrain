// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Management surface of the built-in permission resolver: the loaded provider
// publishes it, a grant or revoke changes the effective policy, writes the
// configuration file and bumps the revision, and a failed write leaves the
// running policy untouched.

#include <refrain/builtin/image.hpp>
#include <refrain/contracts/services/permissions.hpp>
#include <refrain/core/config.hpp>
#include <refrain/core/error.hpp>
#include <refrain/core/io.hpp>
#include <refrain/mod/gamepack.hpp>
#include <refrain/mod/host.hpp>
#include <refrain/permissions/admin.hpp>
#include <refrain/permissions/client.hpp>
#include <refrain/permissions/policy.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace config = refrain::core::config;
namespace io = refrain::core::io;
namespace mod = refrain::mod;
namespace permissions = refrain::permissions;

using permission_result = refrain::services::permissions::permission_result;
using subject_kind = refrain::services::permissions::subject_kind;

constexpr std::string_view player_uuid{"069a79f4-44e9-4726-a5be-fca90e38aaf5"};
constexpr std::string_view player_name{"Notch"};
constexpr std::string_view player_key{
    "player:069a79f4-44e9-4726-a5be-fca90e38aaf5"};

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

[[nodiscard]] auto make_temp_path() -> std::filesystem::path {
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    return std::filesystem::temp_directory_path() /
           ("refrain_permissions_admin_" + std::to_string(stamp) + ".toml");
}

[[nodiscard]] auto read_text(const std::filesystem::path &path) -> std::string {
    std::ifstream in{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{in},
                       std::istreambuf_iterator<char>{}};
}

/// One loaded host with a mounted `permissions` configuration namespace.
class admin_fixture {
  public:
    explicit admin_fixture(std::string_view config_text) {
        path_ = make_temp_path();
        expect(static_cast<bool>(io::write_text(path_.string(), config_text)));
        expect(static_cast<bool>(
            store_.mount({.name = "permissions", .path = path_.string()})));
        expect(static_cast<bool>(store_.load_all()));
    }

    admin_fixture(const admin_fixture &) = delete;
    auto operator=(const admin_fixture &) -> admin_fixture & = delete;
    admin_fixture(admin_fixture &&) = delete;
    auto operator=(admin_fixture &&) -> admin_fixture & = delete;
    ~admin_fixture() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] auto load() -> refrain::status {
        auto plan{load_default_plan()};
        if (!plan) {
            return std::unexpected{plan.error()};
        }
        host_.set_config(store_);
        return host_.load(*plan, builtin_bytes());
    }

    [[nodiscard]] auto host() noexcept -> refrain::mod::host & { return host_; }

    void rewrite(std::string_view text) const {
        expect(static_cast<bool>(io::write_text(path_.string(), text)));
    }

    [[nodiscard]] auto config_path() const -> const std::filesystem::path & {
        return path_;
    }

  private:
    refrain::core::config::store store_{};
    refrain::mod::host host_{};
    std::filesystem::path path_;
};

[[nodiscard]] auto bind_client(refrain::mod::host &host)
    -> std::optional<permissions::client> {
    const auto provider{host.services().active_provider("refrain:permissions")};
    expect(static_cast<bool>(provider));
    if (!provider) {
        return std::nullopt;
    }
    const auto *api{static_cast<const mod::provider_api *>(provider->instance)};
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

const suite admin_tests = [] {
    "admin/a loaded host publishes the management surface"_test = [] {
        admin_fixture fixture{basic_config};
        const auto loaded{fixture.load()};
        expect(static_cast<bool>(loaded))
            << (loaded ? std::string{} : loaded.error().message);
        if (!loaded) {
            return;
        }
        auto *admin{permissions::active_admin()};
        expect(admin != nullptr);
        if (admin == nullptr) {
            return;
        }
        expect(admin->persistent());
        expect(admin->revision() == 1U);
        const auto snapshot{admin->snapshot()};
        expect(snapshot.revision == 1U);
        expect(snapshot.persistent);
        expect(snapshot.value.subjects.contains(std::string{player_key}));
        expect(snapshot.value.groups.size() == 1U);
    };

    "admin/a grant persists, answers and bumps the revision"_test = [] {
        admin_fixture fixture{basic_config};
        const auto loaded{fixture.load()};
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        auto *admin{permissions::active_admin()};
        expect(admin != nullptr);
        if (admin == nullptr) {
            return;
        }
        constexpr std::string_view granted_uuid{
            "11111111-2222-3333-4444-555555555555"};
        constexpr std::string_view granted_key{
            "player:11111111-2222-3333-4444-555555555555"};
        auto changed{admin->grant(granted_key, "refrain.command.gamemode")};
        expect(static_cast<bool>(changed))
            << (changed ? std::string{} : changed.error().message);
        if (!changed) {
            return;
        }
        expect(changed->changed);
        expect(changed->revision == 2U);
        expect(changed->subject == granted_key);
        expect(changed->rule == "refrain.command.gamemode");
        expect(admin->revision() == 2U);
        const auto text{read_text(fixture.config_path())};
        expect(text.find("refrain.command.gamemode") != std::string::npos)
            << text;
        const auto answer{admin->explain(
            permissions::subject_view{.kind = subject_kind::player,
                                      .id = granted_uuid},
            "refrain.command.gamemode", 0)};
        expect(answer.result == permission_result::allowed);
        expect(answer.source == permissions::decision_source::subject_grant);
        expect(answer.rule == "refrain.command.gamemode");
        auto client{bind_client(fixture.host())};
        expect(client.has_value());
        if (client) {
            expect(client->revision() == 2U);
            expect(
                client->query(player_subject(), "refrain.command.gamemode") ==
                permission_result::allowed);
        }
        // A second identical grant is a no-op: no file write, no revision
        // bump.
        const auto again{admin->grant(granted_key, "refrain.command.gamemode")};
        expect(static_cast<bool>(again));
        if (again) {
            expect(!again->changed);
            expect(again->revision == 2U);
        }
        expect(admin->revision() == 2U);
        expect(read_text(fixture.config_path()) == text);
    };

    "admin/a revoke removes a rule and drops a deny"_test = [] {
        admin_fixture fixture{basic_config};
        const auto loaded{fixture.load()};
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        auto *admin{permissions::active_admin()};
        expect(admin != nullptr);
        if (admin == nullptr) {
            return;
        }
        const auto view{permissions::subject_view{.kind = subject_kind::player,
                                                  .id = player_uuid,
                                                  .profile_name = player_name}};
        auto denied{admin->explain(view, "refrain.command.stop", 0)};
        expect(denied.result == permission_result::denied);
        expect(denied.source == permissions::decision_source::subject_deny);
        expect(denied.rule == "!refrain.command.stop");
        auto changed{admin->revoke(player_key, "!refrain.command.stop")};
        expect(static_cast<bool>(changed))
            << (changed ? std::string{} : changed.error().message);
        if (!changed) {
            return;
        }
        expect(changed->changed);
        expect(changed->revision == 2U);
        const auto allowed{admin->explain(view, "refrain.command.stop", 0)};
        expect(allowed.result == permission_result::allowed);
        expect(allowed.source == permissions::decision_source::subject_grant);
        expect(read_text(fixture.config_path()).find("!refrain.command.stop") ==
               std::string::npos);
        // Revoking a rule the subject does not hold changes nothing.
        const auto absent{
            admin->revoke(player_key, "refrain.command.teleport")};
        expect(static_cast<bool>(absent));
        if (absent) {
            expect(!absent->changed);
            expect(absent->revision == 2U);
        }
    };

    "admin/a failed write keeps the policy and the revision"_test = [] {
        admin_fixture fixture{basic_config};
        const auto loaded{fixture.load()};
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        auto *admin{permissions::active_admin()};
        expect(admin != nullptr);
        if (admin == nullptr) {
            return;
        }
        const auto before{read_text(fixture.config_path())};
        // Replace the file with a directory: the atomic write cannot rename
        // over it, so persistence fails.
        std::error_code error;
        expect(std::filesystem::remove(fixture.config_path(), error));
        expect(std::filesystem::create_directory(fixture.config_path(), error));
        const auto failed{admin->grant(player_key, "refrain.command.teleport")};
        expect(!failed);
        if (!failed) {
            expect(failed.error().code == refrain::error_code::io_error);
        }
        expect(admin->revision() == 1U);
        const auto view{permissions::subject_view{.kind = subject_kind::player,
                                                  .id = player_uuid,
                                                  .profile_name = player_name}};
        expect(admin->explain(view, "refrain.mods.admin", 0).result ==
               permission_result::denied);
        expect(!std::filesystem::is_regular_file(fixture.config_path()));
        static_cast<void>(before);
    };

    "admin/an in-memory policy refuses changes and keeps serving"_test = [] {
        auto plan{load_default_plan()};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        // No `set_config()`: the provider serves the built-in policy.
        mod::host host{};
        const auto loaded{host.load(*plan, builtin_bytes())};
        expect(static_cast<bool>(loaded))
            << (loaded ? std::string{} : loaded.error().message);
        if (!loaded) {
            return;
        }
        auto *admin{permissions::active_admin()};
        expect(admin != nullptr);
        if (admin == nullptr) {
            return;
        }
        expect(!admin->persistent());
        expect(admin->revision() == 1U);
        const auto refused{admin->grant("console", "refrain.command.teleport")};
        expect(!refused);
        if (!refused) {
            expect(refused.error().code == refrain::error_code::unsupported);
        }
        expect(admin->revision() == 1U);
        const auto answer{admin->explain(
            permissions::subject_view{.kind = subject_kind::console},
            "refrain.command.teleport", 0)};
        expect(answer.result == permission_result::allowed);
        expect(answer.source == permissions::decision_source::console);
    };

    "admin/reload publishes an external edit and reports both revisions"_test =
        [] {
            admin_fixture fixture{R"([defaults]
default = "deny"
)"};
            const auto loaded{fixture.load()};
            expect(static_cast<bool>(loaded));
            if (!loaded) {
                return;
            }
            auto *admin{permissions::active_admin()};
            expect(admin != nullptr);
            if (admin == nullptr) {
                return;
            }
            const auto view{
                permissions::subject_view{.kind = subject_kind::player,
                                          .id = player_uuid,
                                          .profile_name = player_name}};
            expect(admin->explain(view, "refrain.command.teleport", 0).result ==
                   permission_result::denied);
            fixture.rewrite(R"([grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["refrain.command.teleport"]
)");
            auto reloaded{admin->reload()};
            expect(static_cast<bool>(reloaded))
                << (reloaded ? std::string{} : reloaded.error().message);
            if (!reloaded) {
                return;
            }
            expect(reloaded->changed);
            expect(reloaded->previous_revision == 1U);
            expect(reloaded->revision == 2U);
            expect(admin->revision() == 2U);
            expect(admin->explain(view, "refrain.command.teleport", 0).result ==
                   permission_result::allowed);
            // An unchanged file publishes nothing new.
            const auto again{admin->reload()};
            expect(static_cast<bool>(again));
            if (again) {
                expect(!again->changed);
                expect(again->previous_revision == 2U);
                expect(again->revision == 2U);
            }
        };

    "admin/a failed reload keeps the old policy and revision"_test = [] {
        admin_fixture fixture{basic_config};
        const auto loaded{fixture.load()};
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        auto *admin{permissions::active_admin()};
        expect(admin != nullptr);
        if (admin == nullptr) {
            return;
        }
        const auto view{permissions::subject_view{.kind = subject_kind::player,
                                                  .id = player_uuid,
                                                  .profile_name = player_name}};
        fixture.rewrite("[defaults]\nunknown = true\n");
        const auto failed{admin->reload()};
        expect(!failed);
        if (!failed) {
            expect(failed.error().code == refrain::error_code::parse_error);
        }
        expect(admin->revision() == 1U);
        expect(admin->explain(view, "refrain.command.teleport", 0).result ==
               permission_result::allowed);
        fixture.rewrite(R"([grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["refrain.command.teleport"]
)");
        const auto recovered{admin->reload()};
        expect(static_cast<bool>(recovered));
        if (recovered) {
            expect(recovered->changed);
            expect(recovered->revision == 2U);
        }
        expect(admin->explain(view, "refrain.mods.admin", 0).result ==
               permission_result::denied);
        expect(admin->explain(view, "refrain.command.teleport", 0).result ==
               permission_result::allowed);
    };

    "admin/groups list nodes and members deterministically"_test = [] {
        admin_fixture fixture{R"(
[grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["@moderators"]
"plugin:com.example.mymod" = ["@moderators"]

[groups]
moderators = ["refrain.command.*", "!refrain.command.stop"]
)"};
        const auto loaded{fixture.load()};
        expect(static_cast<bool>(loaded))
            << (loaded ? std::string{} : loaded.error().message);
        if (!loaded) {
            return;
        }
        auto *admin{permissions::active_admin()};
        expect(admin != nullptr);
        if (admin == nullptr) {
            return;
        }
        const auto groups{admin->groups()};
        expect(groups.size() == 1U);
        if (groups.size() != 1U) {
            return;
        }
        expect(groups[0].name == "moderators");
        expect(groups[0].nodes.size() == 2U);
        if (groups[0].nodes.size() == 2U) {
            expect(groups[0].nodes[0] == "refrain.command.*");
            expect(groups[0].nodes[1] == "!refrain.command.stop");
        }
        expect(groups[0].members.size() == 2U);
        if (groups[0].members.size() == 2U) {
            expect(groups[0].members[0] == "player:069a79f4-44e9-4726-a5be-"
                                           "fca90e38aaf5");
            expect(groups[0].members[1] == "plugin:com.example.mymod");
        }
    };
};

} // namespace
