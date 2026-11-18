// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The composed permission chain follows the committed composition epoch. The
// host cannot currently publish a change to a chain link: a staged wrapper
// reload or uninstall retires the default provider slot the base resolver
// occupies and is refused, which the tests pin. The provider's epoch refresh is
// defensive; a test seam forces fresh chain instances so the concurrent
// snapshot lifetime, the re-resolution path and the monotonic revision offset
// can still be exercised.

#include <refrain/contracts/abi.hpp>
#include <refrain/contracts/services/mod_loader_abi.h>
#include <refrain/contracts/services/permissions.hpp>
#include <refrain/core/config.hpp>
#include <refrain/core/io.hpp>
#include <refrain/data/builtin_image.hpp>
#include <refrain/mod/discovery.hpp>
#include <refrain/mod/gamepack.hpp>
#include <refrain/mod/host.hpp>
#include <refrain/mod/package.hpp>
#include <refrain/mod/static_mod.hpp>
#include <refrain/permissions/admin.hpp>
#include <refrain/permissions/client.hpp>

#include <boost/ut.hpp>

#include <fmt/format.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace refrain::permissions::test_seam {
extern std::atomic<std::uint64_t> forced_chain_rebuilds;
extern std::function<void()> query_probe;
} // namespace refrain::permissions::test_seam

namespace {

using namespace boost::ut;

namespace config = refrain::core::config;
namespace data = refrain::data;
namespace io = refrain::core::io;
namespace mod = refrain::mod;
namespace permissions = refrain::permissions;

using refrain::services::permissions::permission_result;

constexpr std::string_view player_uuid{"069a79f4-44e9-4726-a5be-fca90e38aaf5"};
constexpr std::string_view player_name{"Notch"};
constexpr std::string_view wrapper_id{"refrain.test.permissions_wrapper"};
constexpr std::string_view clean_loader_id{"com.example.clean"};
constexpr std::string_view companion_id{"refrain.test.permissions_companion"};

/// One wrapper generation. The loader keeps every generation alive so the test
/// observes a stale call as an answer instead of a dangling pointer.
struct wrapper_generation {
    int generation{1};
    std::atomic<int> queries{0};
    std::uint64_t revision{5};
    bool reload_succeeds{true};
};

auto wrapper_query(void *self,
                   const refrain_services_permissions_permission_query *query)
    -> refrain_services_permissions_permission_result {
    auto &generation{*static_cast<wrapper_generation *>(self)};
    generation.queries.fetch_add(1, std::memory_order_relaxed);
    if (query == nullptr) {
        return REFRAIN_SERVICES_PERMISSIONS_PERMISSION_RESULT_DENIED;
    }
    const auto node{refrain::contracts::as_string_view(query->node)};
    if (node == "com.example.owned.home") {
        return REFRAIN_SERVICES_PERMISSIONS_PERMISSION_RESULT_ALLOWED;
    }
    return REFRAIN_SERVICES_PERMISSIONS_PERMISSION_RESULT_NOT_HANDLED;
}

auto wrapper_has(void *self,
                 const refrain_services_permissions_permission_query *query)
    -> std::uint8_t {
    return wrapper_query(self, query) !=
                   REFRAIN_SERVICES_PERMISSIONS_PERMISSION_RESULT_DENIED
               ? 1U
               : 0U;
}

auto wrapper_revision(void *self) -> std::uint64_t {
    return static_cast<const wrapper_generation *>(self)->revision;
}

auto wrapper_reload(void *self, std::uint64_t *out_value) -> refrain_status {
    auto &generation{*static_cast<wrapper_generation *>(self)};
    if (!generation.reload_succeeds) {
        return REFRAIN_SERVICES_PERMISSIONS_PERMISSION_ERROR_PARSE_ERROR;
    }
    ++generation.revision;
    if (out_value != nullptr) {
        *out_value = generation.revision;
    }
    return REFRAIN_STATUS_OK;
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

/// The fake clean loader of the lifecycle tests: it registers one provider for
/// the wrapper it loads and reports every load, reload and unload call.
struct clean_loader_state {
    mod::host *owner{nullptr};
    std::string loader_id{clean_loader_id};
    int load_calls{0};
    int reload_calls{0};
    int unload_calls{0};
    std::vector<std::unique_ptr<wrapper_generation>> generations{};

    [[nodiscard]] auto current() const noexcept -> wrapper_generation * {
        return generations.empty() ? nullptr : generations.back().get();
    }

    [[nodiscard]] auto register_guest(bool provides_permissions) -> bool {
        if (owner == nullptr || !provides_permissions) {
            return true;
        }
        auto generation{std::make_unique<wrapper_generation>()};
        generation->generation = load_calls + reload_calls + 1;
        generation->revision =
            static_cast<std::uint64_t>(generation->generation) * 5U;
        const mod::provider_api api{
            .vtable = &wrapper_vtable(),
            .self = generation.get(),
            .abi_major = REFRAIN_SERVICES_PERMISSIONS_ABI_MAJOR,
        };
        generations.push_back(std::move(generation));
        return owner->register_provider("refrain:permissions", api, "1.0.0")
            .has_value();
    }
};

void clean_capabilities(
    void *self, refrain_services_mod_loader_mod_loader_capabilities *out) {
    const auto &state{*static_cast<clean_loader_state *>(self)};
    out->struct_size = REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_CAPABILITIES_SIZE;
    out->loader_id = refrain::contracts::to_borrowed(state.loader_id);
    out->abi_major = REFRAIN_SERVICES_MOD_LOADER_ABI_MAJOR;
    out->unload = REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_UNLOAD_CLEAN;
    out->sandboxed = 0;
    out->supports_hot_install = 1;
}

auto clean_validate(void *,
                    const refrain_services_mod_loader_mod_entry_manifest *)
    -> refrain_status {
    return REFRAIN_STATUS_OK;
}

auto clean_load(void *self,
                const refrain_services_mod_loader_mod_load_request *request,
                refrain_services_mod_loader_handle_mod_instance *out)
    -> refrain_status {
    auto &state{*static_cast<clean_loader_state *>(self)};
    ++state.load_calls;
    *out = REFRAIN_SERVICES_MOD_LOADER_HANDLE_MOD_INSTANCE_INVALID;
    if (request == nullptr) {
        return static_cast<refrain_status>(
            REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_ERROR_INTERNAL);
    }
    const auto mod_id{
        refrain::contracts::as_string_view(request->manifest.mod_id)};
    if (!state.register_guest(mod_id == wrapper_id)) {
        return static_cast<refrain_status>(
            REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_ERROR_INTERNAL);
    }
    *out = static_cast<refrain_services_mod_loader_handle_mod_instance>(
        state.load_calls + state.reload_calls);
    return REFRAIN_STATUS_OK;
}

auto clean_reload(void *self,
                  refrain_services_mod_loader_handle_mod_instance instance,
                  const refrain_services_mod_loader_mod_load_request *request,
                  refrain_services_mod_loader_handle_mod_instance *out)
    -> refrain_status {
    auto &state{*static_cast<clean_loader_state *>(self)};
    ++state.reload_calls;
    if (request == nullptr ||
        instance == REFRAIN_SERVICES_MOD_LOADER_HANDLE_MOD_INSTANCE_INVALID) {
        return static_cast<refrain_status>(
            REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_ERROR_INTERNAL);
    }
    const auto mod_id{
        refrain::contracts::as_string_view(request->manifest.mod_id)};
    if (!state.register_guest(mod_id == wrapper_id)) {
        return static_cast<refrain_status>(
            REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_ERROR_INTERNAL);
    }
    *out = static_cast<refrain_services_mod_loader_handle_mod_instance>(
        state.load_calls + state.reload_calls);
    return REFRAIN_STATUS_OK;
}

auto clean_unload(void *self,
                  refrain_services_mod_loader_handle_mod_instance instance,
                  refrain_services_mod_loader_mod_unload_reason)
    -> refrain_status {
    auto &state{*static_cast<clean_loader_state *>(self)};
    static_cast<void>(instance);
    ++state.unload_calls;
    return REFRAIN_STATUS_OK;
}

auto clean_loader_vtable() -> const refrain_services_mod_loader_vtable & {
    static const refrain_services_mod_loader_vtable table{
        .struct_size = REFRAIN_SERVICES_MOD_LOADER_VTABLE_SIZE,
        .abi_major = REFRAIN_SERVICES_MOD_LOADER_ABI_MAJOR,
        .abi_minor = REFRAIN_SERVICES_MOD_LOADER_ABI_MINOR,
        .abi_patch = REFRAIN_SERVICES_MOD_LOADER_ABI_PATCH,
        .capabilities = &clean_capabilities,
        .validate = &clean_validate,
        .load = &clean_load,
        .unload = &clean_unload,
        .reload = &clean_reload,
    };
    return table;
}

/// Build a synthetic builtin image with one manifest per mod. The image is the
/// package source the host loads from, so the resolved composition carries real
/// manifest hashes and a staged uninstall diffs the way the production path
/// does.
struct image_builder {
    std::vector<data::builtin_image_input> inputs{};

    void add(std::string_view id, std::string manifest) {
        const auto bytes{std::as_bytes(
            std::span<const char>{manifest.data(), manifest.size()})};
        std::vector<std::byte> owned{bytes.begin(), bytes.end()};
        inputs.push_back(data::builtin_image_input{
            .path = "mods/" + std::string{id} + "/manifest.toml",
            .data = std::move(owned),
        });
    }

    [[nodiscard]] auto build() -> std::vector<std::byte> {
        auto bytes{data::build_builtin_image(inputs)};
        expect(static_cast<bool>(bytes)) << "synthetic image must build";
        return bytes ? *bytes : std::vector<std::byte>{};
    }
};

[[nodiscard]] auto wrapper_manifest() -> std::string {
    return fmt::format(
        "[mod]\nid = \"{}\"\nversion = \"1.0.0\"\nabi = \"1\"\nside = "
        "\"server\"\n\n[provides]\nservices = [\"refrain:permissions\"]"
        "\n\n[entry]\nloader = \"{}\"\n",
        wrapper_id, clean_loader_id);
}

[[nodiscard]] auto companion_manifest() -> std::string {
    return fmt::format(
        "[mod]\nid = \"{}\"\nversion = \"1.0.0\"\nabi = \"1\"\nside = "
        "\"server\"\n\n[entry]\nloader = \"{}\"\n",
        companion_id, clean_loader_id);
}

[[nodiscard]] auto base_manifest() -> std::string {
    return "[mod]\nid = \"refrain.permissions\"\nversion = \"1.0.0\"\nabi = "
           "\"1\"\nside = \"server\"\n\n[provides]\nservices = "
           "[\"refrain:permissions\"]\n";
}

[[nodiscard]] auto make_spec() -> refrain::result<mod::gamepack_spec> {
    constexpr std::string_view text{R"([gamepack]
id = "refrain.test.permissions_lifecycle"
version = "1.0.0"
side = "server"
abi = "1"
auto_scan = false

[mods]
list = ["refrain.test.permissions_wrapper@1.0.0", "refrain.test.permissions_companion@1.0.0", "refrain.permissions@1.0.0"]

[providers]
permissions = { base = "refrain.permissions@^1.0.0", wrappers = ["refrain.test.permissions_wrapper@^1.0.0"] }
)"};
    auto document{
        config::document::parse_text(std::string{text}, "<gamepack>")};
    if (!document) {
        return std::unexpected{document.error()};
    }
    return mod::parse_gamepack(*document, "server");
}

/// One loaded composition: the built-in resolver as the chain base and the
/// wrapper loaded by the fake clean loader, plus a mounted policy file. The
/// package source is a synthetic builtin image so the resolved plan carries the
/// same manifest hashes the host records, which a staged change needs.
class lifecycle_fixture {
  public:
    explicit lifecycle_fixture(std::string_view policy_text) {
        const auto stamp{
            std::chrono::steady_clock::now().time_since_epoch().count()};
        root_ = std::filesystem::temp_directory_path() /
                ("refrain-permissions-lifecycle-" + std::to_string(stamp));
        std::error_code error{};
        std::filesystem::create_directories(root_, error);
        const auto policy_path{(root_ / "permissions.toml").string()};
        expect(static_cast<bool>(io::write_text(policy_path, policy_text)));
        expect(static_cast<bool>(
            store_.mount({.name = "permissions", .path = policy_path})));
        expect(static_cast<bool>(store_.load_all()));
    }

    lifecycle_fixture(const lifecycle_fixture &) = delete;
    auto operator=(const lifecycle_fixture &) -> lifecycle_fixture & = delete;
    lifecycle_fixture(lifecycle_fixture &&) = delete;
    auto operator=(lifecycle_fixture &&) -> lifecycle_fixture & = delete;

    ~lifecycle_fixture() {
        std::error_code error{};
        std::filesystem::remove_all(root_, error);
    }

    [[nodiscard]] auto load() -> refrain::status {
        image_builder builder{};
        builder.add(wrapper_id, wrapper_manifest());
        builder.add(companion_id, companion_manifest());
        builder.add("refrain.permissions", base_manifest());
        image_ = builder.build();
        auto spec{make_spec()};
        if (!spec) {
            return std::unexpected{spec.error()};
        }
        auto candidates{mod::scan_builtin_image(image_)};
        if (!candidates) {
            return std::unexpected{candidates.error()};
        }
        auto plan{mod::resolve(*spec, *candidates)};
        if (!plan) {
            return std::unexpected{plan.error()};
        }
        plan_ = std::move(*plan);
        mod::static_mod_registry registry{};
        for (const auto &descriptor :
             mod::static_mod_registry::global().all()) {
            if (descriptor.id == "refrain.permissions") {
                if (auto added{registry.add(descriptor)}; !added) {
                    return added;
                }
            }
        }
        host_.emplace(registry);
        const mod::provider_api loader_api{
            .vtable = &clean_loader_vtable(),
            .self = &loader_,
            .abi_major = REFRAIN_SERVICES_MOD_LOADER_ABI_MAJOR,
        };
        if (auto registered{
                host_->register_mod_loader(clean_loader_id, loader_api)};
            !registered) {
            return registered;
        }
        loader_.owner = &*host_;
        host_->set_config(store_);
        return host_->load(plan_, image_);
    }

    [[nodiscard]] auto host() noexcept -> mod::host & { return *host_; }
    [[nodiscard]] auto plan() noexcept -> mod::composition & { return plan_; }
    [[nodiscard]] auto loader() noexcept -> clean_loader_state & {
        return loader_;
    }

    [[nodiscard]] auto resolver() -> mod::package_resolver {
        auto resolver{mod::make_package_resolver(image_)};
        expect(static_cast<bool>(resolver)) << "package resolver must build";
        return resolver ? *resolver : mod::package_resolver{};
    }

  private:
    std::filesystem::path root_{};
    config::store store_{};
    std::vector<std::byte> image_{};
    mod::composition plan_{};
    clean_loader_state loader_{};
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
)"};

const suite chain_lifecycle_tests = [] {
    "chain lifecycle/a retired wrapper cannot be published, the chain keeps "
    "serving"_test = [] {
        lifecycle_fixture fixture{base_policy};
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
        expect(client.query(player(), "com.example.owned.home") ==
               permission_result::allowed);
        auto *wrapper{fixture.loader().current()};
        expect(wrapper != nullptr);
        if (wrapper == nullptr) {
            return;
        }
        const auto queries_before{wrapper->queries.load()};
        const auto revision_before{client.revision()};

        // A chain wrapper's provider slot is the default instance, which the
        // base resolver occupies once it loads; staging the wrapper's
        // retirement stages the base slot away, so the host refuses the
        // composition change before it is published. The committed chain and
        // its revision therefore never observe a retired wrapper.
        auto staged{fixture.host().stage_uninstall(wrapper_id)};
        expect(!staged);
        if (staged) {
            return;
        }
        expect(staged.error().message.find("no active provider") !=
               std::string::npos)
            << staged.error().message;
        expect(fixture.host().epoch() == 1U);
        expect(fixture.loader().unload_calls == 0);
        expect(client.revision() == revision_before);
        expect(client.query(player(), "com.example.owned.home") ==
               permission_result::allowed);
        expect(wrapper->queries.load() == queries_before + 1);
    };

    "chain lifecycle/a forced link-instance change keeps the revision "
    "monotonic"_test = [] {
        lifecycle_fixture fixture{base_policy};
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
        expect(client.query(player(), "com.example.owned.home") ==
               permission_result::allowed);
        const auto revision_before{client.revision()};

        // The test seam publishes a fresh chain instance in place of a
        // composition epoch change, so the changed-link-set path runs without a
        // host that can publish a link change. The raw revision sum is
        // unchanged, so the monotonic offset has to move the composed revision
        // forward.
        permissions::test_seam::forced_chain_rebuilds.fetch_add(
            1U, std::memory_order_relaxed);
        const auto revision_after{client.revision()};
        expect(revision_after == revision_before + 1U)
            << (std::to_string(revision_before) + " -> " +
                std::to_string(revision_after));
        expect(permissions::test_seam::forced_chain_rebuilds.load() == 0U);
        expect(client.query(player(), "com.example.owned.home") ==
               permission_result::allowed);
        expect(client.revision() == revision_after);
    };

    "chain lifecycle/a query keeps its chain snapshot across a forced "
    "rebuild"_test = [] {
        lifecycle_fixture fixture{base_policy};
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
        expect(client.query(player(), "com.example.owned.home") ==
               permission_result::allowed);

        // The probe runs after the query acquired its published chain snapshot
        // and before it walks it; triggering a rebuild there makes the walk a
        // use-after-free unless the query keeps the snapshot alive.
        std::atomic<int> probe_calls{0};
        permissions::test_seam::query_probe = [&client, &probe_calls] {
            probe_calls.fetch_add(1, std::memory_order_relaxed);
            permissions::test_seam::forced_chain_rebuilds.fetch_add(
                1U, std::memory_order_relaxed);
            static_cast<void>(client.revision());
        };
        const auto answer{client.query(player(), "com.example.owned.home")};
        permissions::test_seam::query_probe = {};
        expect(answer == permission_result::allowed);
        expect(probe_calls.load() > 0);
        expect(permissions::test_seam::forced_chain_rebuilds.load() == 0U);
        expect(client.query(player(), "com.example.owned.home") ==
               permission_result::allowed);
    };

    "chain lifecycle/a concurrent refresh keeps a queried chain alive"_test =
        [] {
            lifecycle_fixture fixture{base_policy};
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
            // Warm up the link set on this thread before the worker starts.
            expect(client.query(player(), "com.example.owned.home") ==
                   permission_result::allowed);

            std::atomic<bool> stop{false};
            std::atomic<int> wrong_answers{0};
            std::thread worker{[&client, &stop, &wrong_answers] {
                while (!stop.load(std::memory_order_acquire)) {
                    if (client.query(player(), "com.example.owned.home") !=
                        permission_result::allowed) {
                        wrong_answers.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }};
            for (int iteration{0}; iteration < 200; ++iteration) {
                // Every forced rebuild publishes a fresh chain instance, so a
                // query that released its snapshot too early would walk a freed
                // chain; a revision poll performs the rebuild.
                permissions::test_seam::forced_chain_rebuilds.fetch_add(
                    1U, std::memory_order_relaxed);
                static_cast<void>(client.revision());
            }
            stop.store(true, std::memory_order_release);
            worker.join();
            expect(wrong_answers.load() == 0);
        };
};

} // namespace
