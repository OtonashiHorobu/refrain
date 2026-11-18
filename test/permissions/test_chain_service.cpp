// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// End-to-end chain composition: the built-in permission resolver composes the
// wrapper the gamepack plan declares, a wrapper answers only the nodes it owns
// and forwards the rest to the base, the composed revision covers both links,
// and a reload reaches every link.

#include <refrain/contracts/abi.hpp>
#include <refrain/contracts/services/permissions.hpp>
#include <refrain/core/config.hpp>
#include <refrain/core/io.hpp>
#include <refrain/mod/discovery.hpp>
#include <refrain/mod/gamepack.hpp>
#include <refrain/mod/host.hpp>
#include <refrain/mod/package.hpp>
#include <refrain/mod/static_mod.hpp>
#include <refrain/permissions/admin.hpp>
#include <refrain/permissions/client.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using namespace boost::ut;

namespace config = refrain::core::config;
namespace io = refrain::core::io;
namespace mod = refrain::mod;
namespace permissions = refrain::permissions;

using refrain::services::permissions::permission_result;

constexpr std::string_view player_uuid{"069a79f4-44e9-4726-a5be-fca90e38aaf5"};
constexpr std::string_view player_name{"Notch"};

/// The wrapper the test composition loads. It owns the `com.example.*` nodes:
/// `com.example.owned.home` is granted, `com.example.blocked` is denied and
/// every other node is forwarded to the base resolver.
struct wrapper_state {
    std::atomic<std::uint64_t> revision{1};
    std::atomic<int> reloads{0};
    std::atomic<int> queries{0};
};

wrapper_state g_wrapper{};

auto wrapper_query(void *,
                   const refrain_services_permissions_permission_query *query)
    -> refrain_services_permissions_permission_result {
    g_wrapper.queries.fetch_add(1, std::memory_order_relaxed);
    if (query == nullptr) {
        return REFRAIN_SERVICES_PERMISSIONS_PERMISSION_RESULT_DENIED;
    }
    const auto node{refrain::contracts::as_string_view(query->node)};
    if (node == "com.example.owned.home") {
        return REFRAIN_SERVICES_PERMISSIONS_PERMISSION_RESULT_ALLOWED;
    }
    if (node == "com.example.blocked") {
        return REFRAIN_SERVICES_PERMISSIONS_PERMISSION_RESULT_DENIED;
    }
    return REFRAIN_SERVICES_PERMISSIONS_PERMISSION_RESULT_NOT_HANDLED;
}

auto wrapper_revision(void *) -> std::uint64_t {
    return g_wrapper.revision.load(std::memory_order_relaxed);
}

auto wrapper_reload(void *, std::uint64_t *out_value) -> refrain_status {
    g_wrapper.reloads.fetch_add(1, std::memory_order_relaxed);
    const auto next{g_wrapper.revision.fetch_add(1, std::memory_order_relaxed) +
                    1};
    if (out_value != nullptr) {
        *out_value = next;
    }
    return REFRAIN_STATUS_OK;
}

auto wrapper_has(void *self,
                 const refrain_services_permissions_permission_query *query)
    -> std::uint8_t {
    return wrapper_query(self, query) !=
                   REFRAIN_SERVICES_PERMISSIONS_PERMISSION_RESULT_DENIED
               ? 1U
               : 0U;
}

auto wrapper_vtable() -> const refrain_services_permissions_vtable & {
    static const refrain_services_permissions_vtable table{
        .struct_size = REFRAIN_SERVICES_PERMISSIONS_VTABLE_SIZE,
        .abi_major = REFRAIN_SERVICES_PERMISSIONS_ABI_MAJOR,
        .abi_minor = REFRAIN_SERVICES_PERMISSIONS_ABI_MINOR,
        .abi_patch = REFRAIN_SERVICES_PERMISSIONS_ABI_PATCH,
        .query = &wrapper_query,
        .default_result = &wrapper_query,
        .has = &wrapper_has,
        .revision = &wrapper_revision,
        .reload = &wrapper_reload,
    };
    return table;
}

auto make_wrapper(mod::host &host) -> refrain::status {
    const mod::provider_api api{.vtable = &wrapper_vtable(),
                                .self = &g_wrapper,
                                .abi_major =
                                    REFRAIN_SERVICES_PERMISSIONS_ABI_MAJOR};
    return host.register_provider("refrain:permissions", api, "1.0.0");
}

REFR_MOD_STATIC("refrain.test.permissions_wrapper", "1.0.0", 1, make_wrapper);

[[nodiscard]] auto scratch_root() -> std::filesystem::path {
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    const auto path{std::filesystem::temp_directory_path() /
                    ("refrain_permissions_chain_" + std::to_string(stamp))};
    std::error_code error{};
    std::filesystem::create_directories(path, error);
    return path;
}

[[nodiscard]] auto write_manifest(const std::filesystem::path &root,
                                  std::string_view id, std::string_view version,
                                  std::string_view provides) -> bool {
    std::error_code error{};
    std::filesystem::create_directories(root, error);
    if (error) {
        return false;
    }
    auto text{std::string{"[mod]\n"}};
    text += "id = \"" + std::string{id} + "\"\n";
    text += "version = \"" + std::string{version} + "\"\n";
    text += "abi = \"1\"\n";
    text += "side = \"server\"\n";
    if (!provides.empty()) {
        text +=
            "\n[provides]\nservices = [\"" + std::string{provides} + "\"]\n";
    }
    std::ofstream stream{root / "manifest.toml", std::ios::binary};
    if (!stream) {
        return false;
    }
    stream << text;
    return static_cast<bool>(stream);
}

[[nodiscard]] auto make_plan() -> refrain::result<mod::composition> {
    constexpr std::string_view text{R"([gamepack]
id = "refrain.test.permissions_chain"
version = "1.0.0"
side = "server"
abi = "1"
auto_scan = false

[mods]
list = ["refrain.test.permissions_wrapper@1.0.0", "refrain.permissions@1.0.0"]

[providers]
permissions = { base = "refrain.permissions@^1.0.0", wrappers = ["refrain.test.permissions_wrapper@^1.0.0"] }
)"};
    auto document{
        config::document::parse_text(std::string{text}, "<gamepack>")};
    if (!document) {
        return std::unexpected{document.error()};
    }
    auto spec{mod::parse_gamepack(*document, "server")};
    if (!spec) {
        return std::unexpected{spec.error()};
    }
    return mod::preview_composition(*spec);
}

/// One loaded composition with the built-in permission resolver and the test
/// wrapper active, plus a mounted policy file.
class chain_fixture {
  public:
    explicit chain_fixture(std::string_view policy_text) {
        root_ = scratch_root();
        permissions_root_ = root_ / "mods" / "permissions";
        wrapper_root_ = root_ / "mods" / "wrapper";
        expect(write_manifest(permissions_root_, "refrain.permissions", "1.0.0",
                              "refrain:permissions"));
        expect(write_manifest(wrapper_root_, "refrain.test.permissions_wrapper",
                              "1.0.0", "refrain:permissions"));
        const auto policy_path{(root_ / "permissions.toml").string()};
        expect(static_cast<bool>(io::write_text(policy_path, policy_text)));
        expect(static_cast<bool>(
            store_.mount({.name = "permissions", .path = policy_path})));
        expect(static_cast<bool>(store_.load_all()));
        policy_path_ = policy_path;
    }

    chain_fixture(const chain_fixture &) = delete;
    auto operator=(const chain_fixture &) -> chain_fixture & = delete;
    chain_fixture(chain_fixture &&) = delete;
    auto operator=(chain_fixture &&) -> chain_fixture & = delete;

    ~chain_fixture() {
        std::error_code error{};
        std::filesystem::remove_all(root_, error);
    }

    [[nodiscard]] auto load() -> refrain::status {
        auto plan{make_plan()};
        if (!plan) {
            return std::unexpected{plan.error()};
        }
        mod::static_mod_registry registry{};
        for (const auto &descriptor :
             mod::static_mod_registry::global().all()) {
            if (descriptor.id == "refrain.permissions" ||
                descriptor.id == "refrain.test.permissions_wrapper") {
                if (auto added{registry.add(descriptor)}; !added) {
                    return added;
                }
            }
        }
        host_.emplace(registry);
        host_->set_config(store_);
        const auto resolver = [this](const mod::candidate &candidate)
            -> refrain::result<std::unique_ptr<mod::package_source>> {
            return mod::make_directory_package_source(
                (candidate.id == "refrain.permissions" ? permissions_root_
                                                       : wrapper_root_)
                    .string());
        };
        return host_->load(*plan, resolver);
    }

    [[nodiscard]] auto host() noexcept -> mod::host & { return *host_; }

    void rewrite_policy(std::string_view text) {
        expect(static_cast<bool>(io::write_text(policy_path_, text)));
    }

  private:
    std::filesystem::path root_{};
    std::filesystem::path permissions_root_{};
    std::filesystem::path wrapper_root_{};
    std::string policy_path_{};
    config::store store_{};
    std::optional<mod::host> host_{};
};

[[nodiscard]] auto bind_client(mod::host &host)
    -> refrain::result<permissions::client> {
    const auto provider{host.services().active_provider("refrain:permissions")};
    if (!provider || provider->instance == nullptr) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::not_found,
            "the composition has no refrain:permissions provider")};
    }
    const auto *api{static_cast<const mod::provider_api *>(provider->instance)};
    if (api == nullptr || api->vtable == nullptr || api->self == nullptr) {
        return std::unexpected{
            refrain::make_error(refrain::error_code::invalid_argument,
                                "the provider table is incomplete")};
    }
    return permissions::client::bind(api->vtable, api->self);
}

[[nodiscard]] auto player() -> permissions::subject {
    return permissions::subject::player(player_uuid, player_name);
}

constexpr std::string_view base_policy{R"(
[defaults]
default = "deny"

[grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["refrain.command.teleport"]

[contexts]
creative = { gamemode = "creative" }

[context_grants.creative]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["!refrain.command.teleport"]
)"};

const suite chain_service_tests = [] {
    "chain service/the built-in resolver composes the planned wrapper"_test =
        [] {
            g_wrapper.revision.store(1);
            g_wrapper.reloads.store(0);
            g_wrapper.queries.store(0);
            chain_fixture fixture{base_policy};
            auto loaded{fixture.load()};
            expect(static_cast<bool>(loaded))
                << (loaded ? std::string{} : loaded.error().message);
            if (!loaded) {
                return;
            }
            expect(fixture.host()
                       .services()
                       .active_provider("refrain:permissions")
                       ->id == "refrain.permissions");
            auto bound{bind_client(fixture.host())};
            expect(static_cast<bool>(bound));
            if (!bound) {
                return;
            }
            const auto &client{*bound};
            // A node the wrapper owns is answered by the wrapper.
            expect(client.query(player(), "com.example.owned.home") ==
                   permission_result::allowed);
            expect(client.query(player(), "com.example.blocked") ==
                   permission_result::denied);
            // A node the wrapper forwards is answered by the base policy.
            expect(client.query(player(), "refrain.command.teleport") ==
                   permission_result::allowed);
            expect(client.query(player(), "refrain.command.stop") ==
                   permission_result::denied);
            // The base resolver alone ignores the wrapper grant.
            expect(client.default_result(player(), "com.example.owned.home") ==
                   permission_result::denied);
            // The composed revision is the base revision plus the wrapper
            // revision, so a consumer keyed by it sees both links.
            expect(client.revision() == 2U);
            expect(g_wrapper.queries.load() > 0);
        };

    "chain service/the composed revision covers both links and reloads them"_test =
        [] {
            g_wrapper.revision.store(1);
            g_wrapper.reloads.store(0);
            chain_fixture fixture{base_policy};
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
            const auto baseline{client.revision()};
            expect(baseline == 2U);

            // A wrapper reload advances the composed revision.
            const auto reloaded{client.reload()};
            expect(static_cast<bool>(reloaded))
                << (reloaded ? std::string{}
                             : std::to_string(static_cast<std::uint32_t>(
                                   reloaded.error())));
            if (reloaded) {
                expect(*reloaded == baseline + 1);
            }
            expect(client.revision() == baseline + 1);
            expect(g_wrapper.reloads.load() == 1);

            // An unreachable base configuration file is unchanged, so a second
            // contract reload only moves the wrapper revision.
            const auto again{client.reload()};
            expect(static_cast<bool>(again));
            if (again) {
                expect(*again == baseline + 2);
            }
            expect(g_wrapper.reloads.load() == 2);
        };

    "chain service/an administration change invalidates the composed revision"_test =
        [] {
            g_wrapper.revision.store(1);
            g_wrapper.reloads.store(0);
            chain_fixture fixture{base_policy};
            auto loaded{fixture.load()};
            expect(static_cast<bool>(loaded));
            if (!loaded) {
                return;
            }
            auto *admin{permissions::active_admin()};
            expect(admin != nullptr);
            if (admin == nullptr) {
                return;
            }
            auto bound{bind_client(fixture.host())};
            expect(static_cast<bool>(bound));
            if (!bound) {
                return;
            }
            const auto &client{*bound};
            const auto baseline{client.revision()};
            auto changed{
                admin->grant("player:069a79f4-44e9-4726-a5be-fca90e38aaf5",
                             "refrain.mods.admin")};
            expect(static_cast<bool>(changed))
                << (changed ? std::string{} : changed.error().message);
            if (changed) {
                expect(changed->changed);
            }
            // The base resolver bumped its own revision and the composed
            // revision moved with it; the command-tree cache keyed by it
            // rebuilds.
            expect(client.revision() == baseline + 1);
            expect(client.query(player(), "refrain.mods.admin") ==
                   permission_result::allowed);
            // The admin reload reloads the whole chain, wrapper included.
            const auto before{client.revision()};
            auto reloaded{admin->reload()};
            expect(static_cast<bool>(reloaded));
            if (reloaded) {
                expect(reloaded->revision > before);
            }
            expect(g_wrapper.reloads.load() == 1);
        };

    "chain service/context rules answer through the composed provider"_test =
        [] {
            g_wrapper.revision.store(1);
            g_wrapper.reloads.store(0);
            chain_fixture fixture{base_policy};
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
            const permissions::context creative{.gamemode = "creative"};
            const permissions::context survival{.gamemode = "survival"};
            expect(client.query(player(), "refrain.command.teleport",
                                survival) == permission_result::allowed);
            expect(client.query(player(), "refrain.command.teleport",
                                creative) == permission_result::denied);
            // The wrapper's answer is independent of the context rule the base
            // resolver applies.
            expect(client.query(player(), "com.example.owned.home", creative) ==
                   permission_result::allowed);

            // A duplicate extra key is a context the resolver cannot match, so
            // the query fails closed before any link is called: a wrapper that
            // owns the node cannot turn the malformed query into a grant.
            const permissions::context_entry duplicated[] = {
                {.key = "event", .value = "a"}, {.key = "event", .value = "b"}};
            const permissions::context broken{.world = "minecraft:overworld",
                                              .extra = duplicated};
            const auto wrapper_queries_before{g_wrapper.queries.load()};
            expect(client.query(player(), "com.example.owned.home", broken) ==
                   permission_result::denied);
            expect(g_wrapper.queries.load() == wrapper_queries_before);
        };

    "chain service/a wrapper reload failure keeps the base policy serving"_test =
        [] {
            g_wrapper.revision.store(1);
            g_wrapper.reloads.store(0);
            chain_fixture fixture{base_policy};
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
            expect(client.query(player(), "refrain.command.teleport") ==
                   permission_result::allowed);
            // A policy file the parser rejects fails the base reload before the
            // wrapper reload runs; the running policy and revision stay.
            fixture.rewrite_policy("[defaults]\nunknown = true\n");
            const auto revision_before{client.revision()};
            const auto reloaded{client.reload()};
            expect(!reloaded);
            expect(client.revision() == revision_before);
            expect(g_wrapper.reloads.load() == 0);
            expect(client.query(player(), "refrain.command.teleport") ==
                   permission_result::allowed);
        };
};

} // namespace
