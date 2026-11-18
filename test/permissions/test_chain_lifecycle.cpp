// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The composed permission chain follows the committed composition epoch. A
// wrapper occupies its own `chain:<position>` registry slot, so a staged reload
// or uninstall of the wrapper never touches the base resolver's default slot:
// the composed chain applies at the commit and the previous chain answers until
// then. A query holds a registry lease for every wrapper link for the duration
// of one walk, so a commit that retires a wrapper drains the walk first instead
// of freeing the table underneath it.

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
using refrain::error_code;

using refrain::services::permissions::permission_result;

constexpr std::string_view player_uuid{"069a79f4-44e9-4726-a5be-fca90e38aaf5"};
constexpr std::string_view player_name{"Notch"};
constexpr std::string_view wrapper_id{"refrain.test.permissions_wrapper"};
constexpr std::string_view clean_loader_id{"com.example.clean"};
constexpr std::string_view companion_id{"refrain.test.permissions_companion"};
constexpr std::string_view owned_home{"com.example.owned.home"};
constexpr std::string_view owned_second{"com.example.owned.second"};

/// Signal used by the concurrent-commit test to hold one wrapper query inside
/// the table while another thread commits the composition epoch.
std::atomic<bool> g_block_wrapper_query{false};
std::atomic<bool> g_wrapper_query_entered{false};
std::atomic<bool> g_release_wrapper_query{false};

/// One wrapper generation. The loader keeps every generation alive so the test
/// observes a stale call as an answer instead of a dangling pointer.
struct wrapper_generation {
    int generation{1};
    std::atomic<int> queries{0};
    std::uint64_t revision{5};
    /// The node this generation answers `allowed` for; everything else is
    /// forwarded to the base resolver.
    std::string owned_node{owned_home};
};

auto wrapper_query(void *self,
                   const refrain_services_permissions_permission_query *query)
    -> refrain_services_permissions_permission_result {
    auto &generation{*static_cast<wrapper_generation *>(self)};
    generation.queries.fetch_add(1, std::memory_order_relaxed);
    if (g_block_wrapper_query.load(std::memory_order_acquire)) {
        g_wrapper_query_entered.store(true, std::memory_order_release);
        while (!g_release_wrapper_query.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }
    if (query == nullptr) {
        return REFRAIN_SERVICES_PERMISSIONS_PERMISSION_RESULT_DENIED;
    }
    const auto node{refrain::contracts::as_string_view(query->node)};
    if (node == generation.owned_node) {
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
    /// Node granted by the generation the next load/reload creates.
    std::string next_owned_node{owned_home};
    /// A reload whose validate slot fails before any generation is built.
    bool validate_fails{false};
    /// A load that succeeds without registering the provider: the host must
    /// reject the composition because the planned chain wrapper never
    /// activated.
    bool skip_registration{false};
    /// Set by the unload slot when it ran while a wrapper query was still
    /// blocked, which would mean the retire path skipped the drain.
    bool unloaded_while_query_blocked{false};
    std::vector<std::unique_ptr<wrapper_generation>> generations{};

    [[nodiscard]] auto current() const noexcept -> wrapper_generation * {
        return generations.empty() ? nullptr : generations.back().get();
    }

    [[nodiscard]] auto register_guest(bool provides_permissions) -> bool {
        if (owner == nullptr || !provides_permissions || skip_registration) {
            return true;
        }
        auto generation{std::make_unique<wrapper_generation>()};
        generation->generation = load_calls + reload_calls;
        generation->revision =
            static_cast<std::uint64_t>(generation->generation) * 5U;
        generation->owned_node = next_owned_node;
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

auto clean_validate(void *self,
                    const refrain_services_mod_loader_mod_entry_manifest *)
    -> refrain_status {
    const auto &state{*static_cast<clean_loader_state *>(self)};
    if (state.validate_fails) {
        return static_cast<refrain_status>(
            REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_ERROR_VERSION_MISMATCH);
    }
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
    if (g_block_wrapper_query.load(std::memory_order_acquire) &&
        !g_release_wrapper_query.load(std::memory_order_acquire)) {
        state.unloaded_while_query_blocked = true;
    }
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

/// The same composition with the wrapper declaration removed: the package
/// stays listed but the chain no longer names it.
[[nodiscard]] auto make_spec_without_wrapper()
    -> refrain::result<mod::gamepack_spec> {
    constexpr std::string_view text{R"([gamepack]
id = "refrain.test.permissions_lifecycle"
version = "1.0.0"
side = "server"
abi = "1"
auto_scan = false

[mods]
list = ["refrain.test.permissions_wrapper@1.0.0", "refrain.test.permissions_companion@1.0.0", "refrain.permissions@1.0.0"]

[providers]
permissions = { base = "refrain.permissions@^1.0.0" }
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
        // The base resolver is statically linked into the test binary; copy its
        // descriptor into this fixture's isolated registry so every host load
        // matches the manifest against the linked-in code.
        for (const auto &descriptor :
             mod::static_mod_registry::global().all()) {
            if (descriptor.id == "refrain.permissions") {
                if (auto added{registry_.add(descriptor)}; !added) {
                    return added;
                }
            }
        }
        host_.emplace(registry_);
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

    [[nodiscard]] auto image() const noexcept -> std::span<const std::byte> {
        return image_;
    }

  private:
    mod::static_mod_registry registry_{};
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
            error_code::not_found,
            "the composition has no refrain:permissions provider")};
    }
    const auto *api{static_cast<const mod::provider_api *>(provider->instance)};
    if (api == nullptr || api->vtable == nullptr || api->self == nullptr) {
        return std::unexpected{refrain::make_error(
            error_code::invalid_argument, "the provider table is incomplete")};
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
    "chain lifecycle/a wrapper reload applies the new generation at the next "
    "epoch"_test = [] {
        lifecycle_fixture fixture{base_policy};
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
        expect(client.query(player(), owned_home) ==
               permission_result::allowed);
        expect(client.query(player(), owned_second) ==
               permission_result::denied);
        auto *first_generation{fixture.loader().current()};
        expect(first_generation != nullptr);
        if (first_generation == nullptr) {
            return;
        }
        const auto first_queries{first_generation->queries.load()};
        const auto first_revision{client.revision()};

        // The wrapper keeps its own chain slot, so the staged generation is
        // verified against that slot instead of the base's default instance.
        fixture.loader().next_owned_node = std::string{owned_second};
        auto staged{fixture.host().stage_composition(
            fixture.plan(), fixture.resolver(),
            mod::stage_options{.reload = {std::string{wrapper_id}}})};
        expect(static_cast<bool>(staged))
            << (staged ? std::string{"staged"} : staged.error().message);
        if (!staged) {
            return;
        }
        // Nothing is published until the tick boundary: the committed chain
        // keeps answering with the previous wrapper generation.
        expect(client.query(player(), owned_home) ==
               permission_result::allowed);
        expect(first_generation->queries.load() > first_queries);
        expect(fixture.host().epoch() == 1U);

        auto committed{fixture.host().commit_epoch()};
        expect(static_cast<bool>(committed));
        if (!committed) {
            return;
        }
        expect(committed->epoch == 2U);
        expect(committed->reloaded ==
               std::vector<std::string>{std::string{wrapper_id}});
        expect(fixture.loader().unload_calls == 1);
        expect(fixture.loader().generations.size() == 2U);
        // The next query resolves the new generation's rules: the old node is
        // forwarded to the denying base, the new one is granted by the wrapper.
        expect(client.query(player(), owned_home) == permission_result::denied);
        expect(client.query(player(), owned_second) ==
               permission_result::allowed);
        expect(client.revision() > first_revision);
        expect(fixture.loader().current()->queries.load() > 0);
        // The committed slot points at the new generation even though the
        // retired candidate is still in the candidate table until the commit
        // finishes cleaning it up; a resolver that re-read first-registered
        // order would resurrect the predecessor.
        const auto wrapper_slot{fixture.host().services().active_provider(
            "refrain:permissions", mod::chain_wrapper_instance(0))};
        expect(wrapper_slot.has_value());
        if (wrapper_slot) {
            expect(wrapper_slot->id == wrapper_id);
            expect(wrapper_slot->generation == 2U);
        }
        expect(
            fixture.host().services().providers("refrain:permissions").size() ==
            2U);
        const auto old_queries{first_generation->queries.load()};
        static_cast<void>(client.query(player(), owned_second));
        expect(first_generation->queries.load() == old_queries);
    };

    "chain lifecycle/uninstalling the wrapper falls back to the base at the "
    "next epoch"_test = [] {
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
        expect(client.query(player(), owned_home) ==
               permission_result::allowed);

        auto staged{fixture.host().stage_uninstall(wrapper_id)};
        expect(static_cast<bool>(staged))
            << (staged ? std::string{"staged"} : staged.error().message);
        if (!staged) {
            return;
        }
        expect(client.query(player(), owned_home) ==
               permission_result::allowed);
        auto committed{fixture.host().commit_epoch()};
        expect(static_cast<bool>(committed));
        if (!committed) {
            return;
        }
        expect(committed->epoch == 2U);
        expect(committed->removed ==
               std::vector<std::string>{std::string{wrapper_id}});
        expect(fixture.loader().unload_calls == 1);
        // The base keeps the default slot; the wrapper's chain slot is gone.
        const auto base{
            fixture.host().services().active_provider("refrain:permissions")};
        expect(base.has_value());
        if (base) {
            expect(base->id == "refrain.permissions");
        }
        expect(!fixture.host()
                    .services()
                    .active_provider("refrain:permissions",
                                     mod::chain_wrapper_instance(0))
                    .has_value());
        expect(client.query(player(), owned_home) == permission_result::denied);
        expect(client.query(player(), "refrain.command.teleport") ==
               permission_result::denied);
    };

    "chain lifecycle/dropping the wrapper declaration retires its slot while "
    "the package stays loaded"_test = [] {
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
        expect(client.query(player(), owned_home) ==
               permission_result::allowed);

        auto spec{make_spec_without_wrapper()};
        expect(static_cast<bool>(spec));
        auto candidates{mod::scan_builtin_image(fixture.image())};
        expect(static_cast<bool>(candidates));
        if (!spec || !candidates) {
            return;
        }
        auto next{mod::resolve(*spec, *candidates)};
        expect(static_cast<bool>(next));
        if (!next) {
            return;
        }
        auto staged{
            fixture.host().stage_composition(*next, fixture.resolver())};
        expect(static_cast<bool>(staged))
            << (staged ? std::string{"staged"} : staged.error().message);
        if (!staged) {
            return;
        }
        // Dropping the declaration is not a mod change: the package stays
        // loaded, so only the chain slot is retired at the commit.
        expect(staged->added.empty());
        expect(staged->reloaded.empty());
        expect(staged->removed.empty());
        expect(client.query(player(), owned_home) ==
               permission_result::allowed);

        auto committed{fixture.host().commit_epoch()};
        expect(static_cast<bool>(committed));
        if (!committed) {
            return;
        }
        expect(committed->epoch == 2U);
        expect(fixture.host().find(wrapper_id) != nullptr);
        expect(!fixture.host()
                    .services()
                    .active_provider("refrain:permissions",
                                     mod::chain_wrapper_instance(0))
                    .has_value());
        expect(fixture.loader().unload_calls == 0);
        expect(client.query(player(), owned_home) == permission_result::denied);
    };

    "chain lifecycle/a failed stage leaves the running chain untouched"_test =
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
            const auto revision_before{client.revision()};
            const auto epoch_before{fixture.host().epoch()};

            fixture.loader().validate_fails = true;
            fixture.loader().next_owned_node = std::string{owned_second};
            auto staged{fixture.host().stage_composition(
                fixture.plan(), fixture.resolver(),
                mod::stage_options{.reload = {std::string{wrapper_id}}})};
            expect(!staged);
            if (staged) {
                return;
            }
            expect(fixture.host().epoch() == epoch_before);
            expect(!fixture.host().staged_change().has_value());
            expect(fixture.loader().unload_calls == 0);
            expect(fixture.loader().generations.size() == 1U);
            expect(client.query(player(), owned_home) ==
                   permission_result::allowed);
            expect(client.revision() == revision_before);

            // A later valid stage still commits normally.
            fixture.loader().validate_fails = false;
            auto retried{fixture.host().stage_composition(
                fixture.plan(), fixture.resolver(),
                mod::stage_options{.reload = {std::string{wrapper_id}}})};
            expect(static_cast<bool>(retried));
            if (retried) {
                expect(static_cast<bool>(fixture.host().commit_epoch()));
            }
            expect(client.query(player(), owned_second) ==
                   permission_result::allowed);
        };

    "chain lifecycle/a wrapper that never activates fails closed naming its "
    "chain slot"_test = [] {
        lifecycle_fixture fixture{base_policy};
        fixture.loader().skip_registration = true;
        const auto loaded{fixture.load()};
        expect(!loaded);
        if (loaded) {
            return;
        }
        // The base is loaded and registered; the failure names the wrapper's
        // chain slot, not the base's default provider.
        const auto &message{loaded.error().message};
        expect(message.find("refrain.test.permissions_wrapper") !=
               std::string::npos)
            << message;
        expect(message.find("chain:0") != std::string::npos) << message;
        expect(message.find("refrain.permissions") == std::string::npos)
            << message;
    };

    "chain lifecycle/a staged wrapper that never activates fails closed and "
    "rolls back"_test = [] {
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
        const auto revision_before{client.revision()};
        expect(client.query(player(), owned_home) ==
               permission_result::allowed);

        // The reloading loader reports success but registers no provider: the
        // staged view has the wrapper's chain slot retired, so the plan
        // verification fails closed instead of publishing a chain with a
        // missing link.
        fixture.loader().skip_registration = true;
        auto staged{fixture.host().stage_composition(
            fixture.plan(), fixture.resolver(),
            mod::stage_options{.reload = {std::string{wrapper_id}}})};
        expect(!staged);
        if (staged) {
            return;
        }
        expect(staged.error().message.find("chain:0") != std::string::npos)
            << staged.error().message;
        expect(staged.error().message.find(wrapper_id) != std::string::npos)
            << staged.error().message;
        expect(fixture.host().epoch() == 1U);
        expect(!fixture.host().staged_change().has_value());
        expect(client.query(player(), owned_home) ==
               permission_result::allowed);
        expect(client.revision() == revision_before);
        expect(fixture.host()
                   .services()
                   .active_provider("refrain:permissions",
                                    mod::chain_wrapper_instance(0))
                   .has_value());
    };

    "chain lifecycle/a query racing the commit keeps the retired table "
    "alive"_test = [] {
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
        expect(client.query(player(), owned_home) ==
               permission_result::allowed);
        auto *first_generation{fixture.loader().current()};
        expect(first_generation != nullptr);
        if (first_generation == nullptr) {
            return;
        }

        // Stage the reload first; it does not publish anything.
        fixture.loader().next_owned_node = std::string{owned_second};
        auto staged{fixture.host().stage_composition(
            fixture.plan(), fixture.resolver(),
            mod::stage_options{.reload = {std::string{wrapper_id}},
                               .drain_timeout =
                                   std::chrono::milliseconds{5000}})};
        expect(static_cast<bool>(staged))
            << (staged ? std::string{"staged"} : staged.error().message);
        if (!staged) {
            return;
        }

        // One query enters the wrapper table and blocks there, holding the
        // walk's lease; the commit must not retire the table until it returns.
        g_block_wrapper_query.store(true, std::memory_order_release);
        g_wrapper_query_entered.store(false, std::memory_order_release);
        g_release_wrapper_query.store(false, std::memory_order_release);
        permission_result raced_answer{permission_result::denied};
        std::thread querier{[&client, &raced_answer] {
            raced_answer = client.query(player(), owned_home);
        }};
        const auto entered_deadline{std::chrono::steady_clock::now() +
                                    std::chrono::seconds{2}};
        while (!g_wrapper_query_entered.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < entered_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        expect(g_wrapper_query_entered.load());

        std::atomic<bool> commit_done{false};
        std::optional<mod::composition_commit> commit_result{};
        std::string commit_error{};
        std::thread committer{
            [&fixture, &commit_done, &commit_result, &commit_error] {
                auto committed{fixture.host().commit_epoch()};
                if (committed) {
                    commit_result = *committed;
                } else {
                    commit_error = committed.error().message;
                }
                commit_done.store(true, std::memory_order_release);
            }};
        // The commit reaches the quiesce step while the query holds its lease
        // and waits there instead of switching the slot.
        const auto quiesce_deadline{std::chrono::steady_clock::now() +
                                    std::chrono::seconds{2}};
        auto quiesced{false};
        while (std::chrono::steady_clock::now() < quiesce_deadline) {
            const auto slot{fixture.host().services().query(
                "refrain:permissions", mod::chain_wrapper_instance(0))};
            if (!slot && slot.error().code == error_code::stale) {
                quiesced = true;
                break;
            }
            if (commit_done.load(std::memory_order_acquire)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        expect(quiesced);
        expect(!commit_done.load(std::memory_order_acquire));
        expect(!fixture.loader().unloaded_while_query_blocked);

        g_release_wrapper_query.store(true, std::memory_order_release);
        querier.join();
        committer.join();
        g_block_wrapper_query.store(false, std::memory_order_release);
        expect(commit_error.empty()) << commit_error;
        expect(commit_done.load());
        expect(commit_result.has_value());
        // The in-flight call finished on the previous generation, and only then
        // did the loader's unload hook run and the epoch switch.
        expect(raced_answer == permission_result::allowed);
        expect(fixture.loader().unload_calls == 1);
        expect(!fixture.loader().unloaded_while_query_blocked);
        expect(commit_result->epoch == 2U);
        expect(client.query(player(), owned_second) ==
               permission_result::allowed);
        expect(client.query(player(), owned_home) == permission_result::denied);
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
        expect(client.query(player(), owned_home) ==
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
        expect(client.query(player(), owned_home) ==
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
        expect(client.query(player(), owned_home) ==
               permission_result::allowed);

        // The probe runs after the query acquired its published chain snapshot
        // and its leases and before it walks the chain; triggering a rebuild
        // there makes the walk a use-after-free unless the query keeps the
        // snapshot alive.
        std::atomic<int> probe_calls{0};
        permissions::test_seam::query_probe = [&client, &probe_calls] {
            probe_calls.fetch_add(1, std::memory_order_relaxed);
            permissions::test_seam::forced_chain_rebuilds.fetch_add(
                1U, std::memory_order_relaxed);
            static_cast<void>(client.revision());
        };
        const auto answer{client.query(player(), owned_home)};
        permissions::test_seam::query_probe = {};
        expect(answer == permission_result::allowed);
        expect(probe_calls.load() > 0);
        expect(permissions::test_seam::forced_chain_rebuilds.load() == 0U);
        expect(client.query(player(), owned_home) ==
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
            expect(client.query(player(), owned_home) ==
                   permission_result::allowed);

            std::atomic<bool> stop{false};
            std::atomic<int> wrong_answers{0};
            std::thread worker{[&client, &stop, &wrong_answers] {
                while (!stop.load(std::memory_order_acquire)) {
                    if (client.query(player(), owned_home) !=
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
