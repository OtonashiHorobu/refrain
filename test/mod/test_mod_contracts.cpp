// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Mod-defined service contracts: a package declares a custom service in its
// manifest, registers a provider whose generated vtable header matches the
// declaration, and another package consumes the table through the sealed host
// registry. The negative cases pin the fail-closed rules: a consumer range
// outside the declared compatibility band, two mods declaring one service id,
// an undeclared custom service, an untrusted provider and a mismatched vtable
// header.

#include <refrain/app/host_context.hpp>
#include <refrain/contracts/abi.hpp>
#include <refrain/contracts/services/mod_loader_abi.h>
#include <refrain/core/config.hpp>
#include <refrain/core/error.hpp>
#include <refrain/data/builtin_image.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/host/core_api.hpp>
#include <refrain/mod/discovery.hpp>
#include <refrain/mod/gamepack.hpp>
#include <refrain/mod/host.hpp>
#include <refrain/mod/manifest.hpp>
#include <refrain/mod/package.hpp>
#include <refrain/mod/static_mod.hpp>

#include <boost/ut.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace boost::ut;

namespace config = refrain::core::config;
namespace data = refrain::data;
namespace mod = refrain::mod;
using refrain::error_code;

/// Generated-vtable-shaped provider table for `example:shop` v1.0.0: the
/// leading 4 x `uint32_t` ABI header plus one contract slot. The descriptor
/// below declares exactly this `struct_size` and ABI version.
struct shop_vtable {
    std::uint32_t struct_size{sizeof(shop_vtable)};
    std::uint32_t abi_major{1};
    std::uint32_t abi_minor{0};
    std::uint32_t abi_patch{0};
    std::int64_t (*price)(void *self, std::int64_t item);
};

/// A malformed table that still passes the generic ABI header check but does
/// not match the declared descriptor size.
struct shop_vtable_short {
    std::uint32_t struct_size{16};
    std::uint32_t abi_major{1};
    std::uint32_t abi_minor{0};
    std::uint32_t abi_patch{0};
};

std::int64_t g_shop_self{0};
std::int64_t g_shop_price{-1};
int g_shop_factory_calls{0};
int g_consumer_factory_calls{0};
bool g_consumer_queried{false};
std::string g_consumer_failure{};

std::int64_t shop_price(void * /*self*/, std::int64_t item) { return item * 3; }

shop_vtable g_shop_table{.struct_size = sizeof(shop_vtable),
                         .abi_major = 1,
                         .abi_minor = 0,
                         .abi_patch = 0,
                         .price = &shop_price};
shop_vtable_short g_shop_table_short{};
refrain::app::host_context *g_context{nullptr};

/// The declaring mod: publishes `example:shop` v1 and registers its provider.
auto shop_factory(mod::host &host) -> refrain::status {
    ++g_shop_factory_calls;
    return host.register_provider("example:shop",
                                  mod::provider_api{.vtable = &g_shop_table,
                                                    .self = &g_shop_self,
                                                    .abi_major = 1});
}

/// A provider with a header the contract descriptor rejects.
auto short_header_factory(mod::host &host) -> refrain::status {
    ++g_shop_factory_calls;
    return host.register_provider(
        "example:shop", mod::provider_api{.vtable = &g_shop_table_short,
                                          .self = &g_shop_self,
                                          .abi_major = 1});
}

/// The consuming mod: reaches the custom provider through the sealed host
/// core API, exactly like a mod that ships a generated client wrapper. The
/// first call proves the registry hands out the custom table; the handle form
/// proves the generation identity is tracked like any other service.
auto consumer_factory(mod::host & /*host*/) -> refrain::status {
    ++g_consumer_factory_calls;
    if (g_context == nullptr) {
        g_consumer_failure = "host context is not installed";
        return refrain::status_ok();
    }
    const auto core{refrain::host::make_core_provider(*g_context)};
    const auto found{core.query_service("example:shop", {})};
    if (!found) {
        g_consumer_failure = "query_service failed";
        return refrain::status_ok();
    }
    const auto *table{static_cast<const shop_vtable *>(found->vtable)};
    g_shop_price = table->price(found->self, 7);
    g_consumer_queried = true;
    return refrain::status_ok();
}

auto noop_factory(mod::host & /*host*/) -> refrain::status {
    return refrain::status_ok();
}

[[nodiscard]] auto to_bytes(std::string_view text) -> std::vector<std::byte> {
    const auto bytes{
        std::as_bytes(std::span<const char>{text.data(), text.size()})};
    return {bytes.begin(), bytes.end()};
}

[[nodiscard]] auto
build_image(std::vector<std::pair<std::string, std::string>> manifests)
    -> std::vector<std::byte> {
    std::vector<data::builtin_image_input> inputs{};
    inputs.reserve(manifests.size());
    for (auto &[path, manifest] : manifests) {
        inputs.push_back(data::builtin_image_input{
            .path = "mods/" + std::move(path) + "/manifest.toml",
            .data = to_bytes(manifest)});
    }
    auto bytes{data::build_builtin_image(inputs)};
    expect(static_cast<bool>(bytes)) << "synthetic image must build";
    return bytes ? *bytes : std::vector<std::byte>{};
}

/// One mod manifest with the common identity fields.
[[nodiscard]] auto make_manifest(std::string_view id,
                                 std::string_view extras = {}) -> std::string {
    auto text{std::string{"[mod]\n"}};
    text += "id = \"" + std::string{id} + "\"\n";
    text += "version = \"1.0.0\"\n";
    text += "abi = \"1\"\n";
    text += "side = \"server\"\n";
    text += extras;
    return text;
}

[[nodiscard]] auto shop_manifest(std::string_view contract_version = "1.0.0",
                                 std::uint32_t vtable_size = 24,
                                 std::string_view compatible = ">=1.0.0 <2.0.0")
    -> std::string {
    auto text{make_manifest("example.shop")};
    text += "\n[provides]\nservices = [\"example:shop\"]\n";
    text += "\n[contracts.\"example:shop\"]\n";
    text += "version = \"" + std::string{contract_version} + "\"\n";
    text += "vtable_size = " + std::to_string(vtable_size) + "\n";
    text += "compatible = \"" + std::string{compatible} + "\"\n";
    return text;
}

[[nodiscard]] auto consumer_manifest(std::string_view required_range,
                                     bool require_mod = true) -> std::string {
    auto text{make_manifest("example.shop.client")};
    if (require_mod) {
        text += "\n[requires]\n\"example.shop\" = \"^1.0.0\"\n";
    }
    text += "\n[contract_requires]\n\"example:shop\" = \"" +
            std::string{required_range} + "\"\n";
    return text;
}

[[nodiscard]] auto make_gamepack(std::string_view mods_list,
                                 std::string_view providers = {})
    -> refrain::result<mod::composition> {
    auto text{std::string{"[gamepack]\n"}};
    text += "id = \"refrain.test.contracts\"\n";
    text += "version = \"1.0.0\"\n";
    text += "side = \"server\"\n";
    text += "abi = \"1\"\n";
    text += "auto_scan = false\n\n[mods]\nlist = [" + std::string{mods_list} +
            "]\n";
    if (!providers.empty()) {
        text += "\n[providers]\n" + std::string{providers};
    }
    auto doc{config::document::parse_text(text, "<gamepack>")};
    if (!doc) {
        return std::unexpected{doc.error()};
    }
    auto spec{mod::parse_gamepack(*doc, "server")};
    if (!spec) {
        return std::unexpected{spec.error()};
    }
    return refrain::mod::preview_composition(*spec);
}

/// Second contract generation: the additive ABI evolution used by the hot
/// epoch tests (`struct_size` 32, ABI 1.1.0).
struct shop_vtable_v11 {
    std::uint32_t struct_size{sizeof(shop_vtable_v11)};
    std::uint32_t abi_major{1};
    std::uint32_t abi_minor{1};
    std::uint32_t abi_patch{0};
    std::int64_t (*price)(void *self, std::int64_t item);
    std::int64_t (*reserved)(void *self);
};

shop_vtable_v11 g_shop_table_v11{.struct_size = sizeof(shop_vtable_v11),
                                 .abi_major = 1,
                                 .abi_minor = 1,
                                 .abi_patch = 0,
                                 .price = &shop_price,
                                 .reserved = nullptr};

// ---------------------------------------------------------------------------
// fake clean loader: a hot-swappable loader whose guest registers the custom
// contract provider during load/reload. The builtin loader declares
// restart-required unload, so hot epochs are exercised through this slot.
// ---------------------------------------------------------------------------

constexpr std::string_view contract_loader_id{"com.example.contracts"};

struct contract_loader_state {
    mod::host *owner{nullptr};
    std::string loader_id{contract_loader_id};
    std::string service{"example:shop"};
    std::string provide_mod{"example.shop"};
    const mod::provider_api *api{nullptr};
    int load_calls{0};
    int reload_calls{0};
    int unload_calls{0};
    std::string last_error{};
    std::uint64_t next_handle{1};
};

void contract_capabilities(
    void *self, refrain_services_mod_loader_mod_loader_capabilities *out) {
    const auto &state{*static_cast<contract_loader_state *>(self)};
    out->struct_size = REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_CAPABILITIES_SIZE;
    out->loader_id = refrain::contracts::to_borrowed(state.loader_id);
    out->abi_major = REFRAIN_SERVICES_MOD_LOADER_ABI_MAJOR;
    out->unload = REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_UNLOAD_CLEAN;
    out->sandboxed = 0;
    out->supports_hot_install = 1;
}

auto contract_validate(void *,
                       const refrain_services_mod_loader_mod_entry_manifest *)
    -> refrain_status {
    return REFRAIN_STATUS_OK;
}

[[nodiscard]] auto
register_guest(contract_loader_state &state,
               const refrain_services_mod_loader_mod_load_request *request)
    -> refrain_status {
    if (state.api == nullptr || request == nullptr ||
        request->manifest.activate_code == 0) {
        return REFRAIN_STATUS_OK;
    }
    const auto mod_id{
        refrain::contracts::as_string_view(request->manifest.mod_id)};
    if (mod_id != state.provide_mod) {
        return REFRAIN_STATUS_OK;
    }
    auto registered{state.owner->register_provider(state.service, *state.api)};
    if (!registered) {
        state.last_error = registered.error().message;
        return static_cast<refrain_status>(
            REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_ERROR_PERMISSION_DENIED);
    }
    return REFRAIN_STATUS_OK;
}

auto contract_load(void *self,
                   const refrain_services_mod_loader_mod_load_request *request,
                   refrain_services_mod_loader_handle_mod_instance *out)
    -> refrain_status {
    auto &state{*static_cast<contract_loader_state *>(self)};
    ++state.load_calls;
    *out = REFRAIN_SERVICES_MOD_LOADER_HANDLE_MOD_INSTANCE_INVALID;
    if (state.owner == nullptr) {
        return static_cast<refrain_status>(
            REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_ERROR_INVALID_ARGUMENT);
    }
    if (const auto registered{register_guest(state, request)};
        registered != REFRAIN_STATUS_OK) {
        return registered;
    }
    *out = state.next_handle++;
    return REFRAIN_STATUS_OK;
}

auto contract_reload(
    void *self, refrain_services_mod_loader_handle_mod_instance instance,
    const refrain_services_mod_loader_mod_load_request *request,
    refrain_services_mod_loader_handle_mod_instance *out) -> refrain_status {
    auto &state{*static_cast<contract_loader_state *>(self)};
    ++state.reload_calls;
    if (state.owner == nullptr ||
        instance == REFRAIN_SERVICES_MOD_LOADER_HANDLE_MOD_INSTANCE_INVALID) {
        return static_cast<refrain_status>(
            REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_ERROR_INVALID_ARGUMENT);
    }
    if (const auto registered{register_guest(state, request)};
        registered != REFRAIN_STATUS_OK) {
        return registered;
    }
    *out = state.next_handle++;
    return REFRAIN_STATUS_OK;
}

auto contract_unload(void *self,
                     refrain_services_mod_loader_handle_mod_instance,
                     refrain_services_mod_loader_mod_unload_reason)
    -> refrain_status {
    auto &state{*static_cast<contract_loader_state *>(self)};
    ++state.unload_calls;
    return REFRAIN_STATUS_OK;
}

[[nodiscard]] auto contract_loader_vtable()
    -> const refrain_services_mod_loader_vtable & {
    static const refrain_services_mod_loader_vtable table{
        .struct_size = REFRAIN_SERVICES_MOD_LOADER_VTABLE_SIZE,
        .abi_major = REFRAIN_SERVICES_MOD_LOADER_ABI_MAJOR,
        .abi_minor = REFRAIN_SERVICES_MOD_LOADER_ABI_MINOR,
        .abi_patch = REFRAIN_SERVICES_MOD_LOADER_ABI_PATCH,
        .capabilities = &contract_capabilities,
        .validate = &contract_validate,
        .load = &contract_load,
        .unload = &contract_unload,
        .reload = &contract_reload,
    };
    return table;
}

[[nodiscard]] auto register_contract_loader(mod::host &host,
                                            contract_loader_state &state)
    -> bool {
    state.owner = &host;
    const mod::provider_api api{.vtable = &contract_loader_vtable(),
                                .self = &state,
                                .abi_major =
                                    REFRAIN_SERVICES_MOD_LOADER_ABI_MAJOR};
    return host.register_mod_loader(contract_loader_id, api).has_value();
}

[[nodiscard]] auto
make_package_resolver_for(const std::vector<std::byte> &image)
    -> mod::package_resolver {
    auto resolver{mod::make_package_resolver(image)};
    expect(static_cast<bool>(resolver)) << "package resolver must build";
    return resolver ? *resolver : mod::package_resolver{};
}

/// Pin the real manifest digests into a preview plan: `stage_composition()`
/// treats a mod whose pinned digest matches the committed one as untouched,
/// which is how a hot epoch distinguishes a reloaded owner from an unchanged
/// trusted provider.
void pin_manifest_hashes(
    mod::composition &plan,
    const std::vector<std::pair<std::string, std::string>> &manifests) {
    for (auto &entry : plan.load_order) {
        const auto found{std::find_if(manifests.begin(), manifests.end(),
                                      [&entry](const auto &manifest) {
                                          return manifest.first == entry.id;
                                      })};
        if (found != manifests.end()) {
            const auto bytes{to_bytes(found->second)};
            entry.manifest_hash = data::blake3(bytes);
        }
    }
}

/// Route a package through the hot-swappable fake loader instead of the
/// restart-required builtin loader.
[[nodiscard]] auto
with_contract_loader(std::string manifest,
                     std::string_view loader = contract_loader_id)
    -> std::string {
    manifest += "\n[entry]\nloader = \"" + std::string{loader} + "\"\n";
    return manifest;
}

struct contract_fixture {
    mod::composition plan{};
    std::vector<std::byte> image{};
};

[[nodiscard]] auto make_contract_fixture(
    std::string_view mods_list, std::string_view providers,
    std::vector<std::pair<std::string, std::string>> manifests)
    -> std::optional<contract_fixture> {
    auto plan{make_gamepack(mods_list, providers)};
    if (!plan) {
        return std::nullopt;
    }
    pin_manifest_hashes(*plan, manifests);
    auto image{build_image(std::move(manifests))};
    return contract_fixture{.plan = std::move(*plan),
                            .image = std::move(image)};
}

/// The two-mod custom contract composition used by the hot epoch tests: the
/// owner declares and serves `example:shop` through the fake clean loader, the
/// consumer is data-only and requires the contract.
[[nodiscard]] auto
contract_pair_fixture(const std::string &shop_manifest_text,
                      const std::string &consumer_manifest_text,
                      bool external = false)
    -> std::optional<contract_fixture> {
    const auto route = [external](const std::string &manifest) {
        return external ? with_contract_loader(manifest) : manifest;
    };
    return make_contract_fixture(
        "\"example.shop@1.0.0\", \"example.shop.client@1.0.0\"",
        "\"example:shop\" = \"example.shop@^1.0.0\"\n",
        {{"example.shop", route(shop_manifest_text)},
         {"example.shop.client", route(consumer_manifest_text)}});
}

/// Reset the process-global observations of one test case.
void reset_observations() {
    g_shop_self = 0;
    g_shop_price = -1;
    g_shop_factory_calls = 0;
    g_consumer_factory_calls = 0;
    g_consumer_queried = false;
    g_consumer_failure.clear();
}

const suite custom_contract_tests = [] {
    "custom_contract/consumer queries the declared provider end to end"_test =
        [] {
            reset_observations();
            mod::static_mod_registry registry{};
            expect(static_cast<bool>(registry.add(
                mod::static_mod_descriptor{.id = "example.shop",
                                           .version = "1.0.0",
                                           .abi = 1,
                                           .factory = &shop_factory})));
            expect(static_cast<bool>(registry.add(
                mod::static_mod_descriptor{.id = "example.shop.client",
                                           .version = "1.0.0",
                                           .abi = 1,
                                           .factory = &consumer_factory})));
            auto plan{make_gamepack(
                "\"example.shop@1.0.0\", \"example.shop.client@1.0.0\"",
                "\"example:shop\" = \"example.shop@^1.0.0\"\n")};
            expect(static_cast<bool>(plan)) << "gamepack must accept the "
                                               "custom provider binding";
            if (!plan) {
                return;
            }
            const auto image{build_image(
                {{"example.shop", shop_manifest()},
                 {"example.shop.client", consumer_manifest("^1.0.0")}})};

            mod::host host{registry};
            refrain::app::host_context context{host};
            g_context = &context;
            expect(static_cast<bool>(host.register_sealed_provider(
                "refrain:host.core",
                mod::provider_api{.vtable = &refrain::host::core_vtable(),
                                  .self = &context,
                                  .abi_major = REFRAIN_HOST_CORE_ABI_MAJOR},
                "1.0.0")));

            auto loaded{host.load(*plan, image)};
            g_context = nullptr;
            expect(static_cast<bool>(loaded))
                << (loaded ? std::string{"load ok"} : loaded.error().message);
            if (!loaded) {
                return;
            }
            expect(g_shop_factory_calls == 1);
            expect(g_consumer_factory_calls == 1);
            expect(g_consumer_failure.empty()) << g_consumer_failure;
            expect(g_consumer_queried);
            // The consumer read the provider table through the registry and
            // called the contract function: 7 * 3.
            expect(g_shop_price == 21);

            // A consumer that caches the table across a tick boundary uses the
            // handle form: the committed slot carries a non-zero generation
            // and validates until the next composition epoch.
            const auto core{refrain::host::make_core_provider(context)};
            const auto handle{core.query_service_handle("example:shop", {})};
            expect(handle.has_value());
            if (handle) {
                expect(handle->generation != 0U);
                expect(static_cast<bool>(core.validate_handle(
                    "example:shop", {}, handle->generation)));
            }

            const auto *shop{host.find("example.shop")};
            expect(shop != nullptr);
            if (shop != nullptr) {
                expect(shop->contracts.size() == 1U);
                expect(shop->contracts.front().service == "example:shop");
                expect(shop->contracts.front().vtable_size == 24U);
            }
            const auto active{host.services().active_provider("example:shop")};
            expect(active.has_value());
            if (active) {
                expect(active->id == "example.shop");
                expect(active->version == "1.0.0");
            }
        };

    "custom_contract/consumer range outside the declared band fails"_test = [] {
        reset_observations();
        mod::static_mod_registry registry{};
        expect(static_cast<bool>(registry.add(
            mod::static_mod_descriptor{.id = "example.shop",
                                       .version = "1.0.0",
                                       .abi = 1,
                                       .factory = &shop_factory})));
        expect(static_cast<bool>(registry.add(
            mod::static_mod_descriptor{.id = "example.shop.client",
                                       .version = "1.0.0",
                                       .abi = 1,
                                       .factory = &consumer_factory})));
        auto plan{make_gamepack(
            "\"example.shop@1.0.0\", \"example.shop.client@1.0.0\"",
            "\"example:shop\" = \"example.shop@^1.0.0\"\n")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        const auto image{build_image(
            {{"example.shop", shop_manifest()},
             {"example.shop.client", consumer_manifest("^2.0.0")}})};

        mod::host host{registry};
        refrain::app::host_context context{host};
        g_context = &context;
        expect(static_cast<bool>(host.register_sealed_provider(
            "refrain:host.core",
            mod::provider_api{.vtable = &refrain::host::core_vtable(),
                              .self = &context,
                              .abi_major = REFRAIN_HOST_CORE_ABI_MAJOR},
            "1.0.0")));
        auto loaded{host.load(*plan, image)};
        g_context = nullptr;
        expect(!loaded);
        if (!loaded) {
            expect(loaded.error().code == error_code::version_mismatch);
            const auto &message{loaded.error().message};
            expect(message.find("example.shop.client") != std::string::npos);
            expect(message.find("example.shop") != std::string::npos);
            expect(message.find("^2.0.0") != std::string::npos);
            expect(message.find("1.0.0") != std::string::npos);
        }
        expect(!host.loaded());
        expect(host.services().provider_count() == 0U);
    };

    "custom_contract/two declarations of one service id fail naming both"_test =
        [] {
            reset_observations();
            mod::static_mod_registry registry{};
            expect(static_cast<bool>(registry.add(
                mod::static_mod_descriptor{.id = "example.shop.a",
                                           .version = "1.0.0",
                                           .abi = 1,
                                           .factory = &noop_factory})));
            expect(static_cast<bool>(registry.add(
                mod::static_mod_descriptor{.id = "example.shop.b",
                                           .version = "1.0.0",
                                           .abi = 1,
                                           .factory = &noop_factory})));
            auto first{make_manifest("example.shop.a")};
            first += "\n[contracts.\"example:shop\"]\nversion = \"1.0.0\"\n"
                     "vtable_size = 24\n";
            auto second{make_manifest("example.shop.b")};
            second += "\n[contracts.\"example:shop\"]\nversion = \"1.1.0\"\n"
                      "vtable_size = 32\n";
            auto plan{make_gamepack(
                "\"example.shop.a@1.0.0\", \"example.shop.b@1.0.0\"")};
            expect(static_cast<bool>(plan));
            if (!plan) {
                return;
            }
            const auto image{
                build_image({{"example.shop.a", std::move(first)},
                             {"example.shop.b", std::move(second)}})};
            mod::host host{registry};
            auto loaded{host.load(*plan, image)};
            expect(!loaded);
            if (!loaded) {
                expect(loaded.error().code == error_code::version_mismatch);
                const auto &message{loaded.error().message};
                expect(message.find("example.shop.a") != std::string::npos);
                expect(message.find("example.shop.b") != std::string::npos);
                expect(message.find("1.0.0") != std::string::npos);
                expect(message.find("1.1.0") != std::string::npos);
            }
        };

    "custom_contract/provider without a declaration is refused"_test = [] {
        reset_observations();
        mod::static_mod_registry registry{};
        expect(static_cast<bool>(registry.add(
            mod::static_mod_descriptor{.id = "example.shop",
                                       .version = "1.0.0",
                                       .abi = 1,
                                       .factory = &shop_factory})));
        auto manifest{make_manifest("example.shop")};
        manifest += "\n[provides]\nservices = [\"example:shop\"]\n";
        auto plan{
            make_gamepack("\"example.shop@1.0.0\"",
                          "\"example:shop\" = \"example.shop@^1.0.0\"\n")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        const auto image{build_image({{"example.shop", std::move(manifest)}})};
        mod::host host{registry};
        auto loaded{host.load(*plan, image)};
        expect(!loaded);
        if (!loaded) {
            expect(loaded.error().code == error_code::permission_denied);
            expect(loaded.error().message.find("[contracts]") !=
                   std::string::npos);
        }
    };

    "custom_contract/untrusted provider is refused naming the owner"_test = [] {
        reset_observations();
        mod::static_mod_registry registry{};
        expect(static_cast<bool>(registry.add(
            mod::static_mod_descriptor{.id = "example.shop.owner",
                                       .version = "1.0.0",
                                       .abi = 1,
                                       .factory = &noop_factory})));
        expect(static_cast<bool>(registry.add(
            mod::static_mod_descriptor{.id = "example.shop.other",
                                       .version = "1.0.0",
                                       .abi = 1,
                                       .factory = &shop_factory})));
        auto owner{make_manifest("example.shop.owner")};
        owner += "\n[contracts.\"example:shop\"]\nversion = \"1.0.0\"\n"
                 "vtable_size = 24\n";
        auto other{make_manifest("example.shop.other")};
        other += "\n[provides]\nservices = [\"example:shop\"]\n";
        other += "\n[requires]\n\"example.shop.owner\" = \"^1.0.0\"\n";
        auto plan{make_gamepack(
            "\"example.shop.owner@1.0.0\", \"example.shop.other@1.0.0\"",
            "\"example:shop\" = \"example.shop.other@^1.0.0\"\n")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        const auto image{
            build_image({{"example.shop.owner", std::move(owner)},
                         {"example.shop.other", std::move(other)}})};
        mod::host host{registry};
        auto loaded{host.load(*plan, image)};
        expect(!loaded);
        if (!loaded) {
            expect(loaded.error().code == error_code::permission_denied);
            const auto &message{loaded.error().message};
            expect(message.find("example.shop.owner") != std::string::npos);
            expect(message.find("trusted") != std::string::npos);
        }
    };

    "custom_contract/provider vtable must match the descriptor"_test = [] {
        reset_observations();
        mod::static_mod_registry registry{};
        expect(static_cast<bool>(registry.add(
            mod::static_mod_descriptor{.id = "example.shop",
                                       .version = "1.0.0",
                                       .abi = 1,
                                       .factory = &short_header_factory})));
        auto plan{
            make_gamepack("\"example.shop@1.0.0\"",
                          "\"example:shop\" = \"example.shop@^1.0.0\"\n")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        const auto image{build_image({{"example.shop", shop_manifest()}})};
        mod::host host{registry};
        auto loaded{host.load(*plan, image)};
        expect(!loaded);
        if (!loaded) {
            expect(loaded.error().code == error_code::version_mismatch);
            const auto &message{loaded.error().message};
            expect(message.find("struct_size 16") != std::string::npos);
            expect(message.find("struct_size 24") != std::string::npos);
        }
    };

    "custom_contract/required range must include the declared version"_test =
        [] {
            reset_observations();
            mod::static_mod_registry registry{};
            expect(static_cast<bool>(registry.add(
                mod::static_mod_descriptor{.id = "example.shop",
                                           .version = "1.0.0",
                                           .abi = 1,
                                           .factory = &shop_factory})));
            auto fixture{contract_pair_fixture(
                shop_manifest(), consumer_manifest(">=1.5.0 <2.0.0"))};
            expect(fixture.has_value());
            if (!fixture) {
                return;
            }
            mod::host host{registry};
            auto loaded{host.load(fixture->plan, fixture->image)};
            expect(!loaded);
            if (!loaded) {
                expect(loaded.error().code == error_code::version_mismatch);
                const auto &message{loaded.error().message};
                expect(message.find("example.shop.client") !=
                       std::string::npos);
                expect(message.find(">=1.5.0 <2.0.0") != std::string::npos);
                expect(message.find("1.0.0") != std::string::npos);
            }
        };

    "custom_contract/unbounded consumer range fails a bounded band"_test = [] {
        reset_observations();
        mod::static_mod_registry registry{};
        expect(static_cast<bool>(registry.add(
            mod::static_mod_descriptor{.id = "example.shop",
                                       .version = "1.0.0",
                                       .abi = 1,
                                       .factory = &shop_factory})));
        auto fixture{contract_pair_fixture(shop_manifest(),
                                           consumer_manifest(">=1.0.0"))};
        expect(fixture.has_value());
        if (!fixture) {
            return;
        }
        mod::host host{registry};
        auto loaded{host.load(fixture->plan, fixture->image)};
        expect(!loaded);
        if (!loaded) {
            expect(loaded.error().code == error_code::version_mismatch);
            expect(loaded.error().message.find(">=1.0.0") != std::string::npos);
        }
    };

    "custom_contract/one-sided band accepts a bounded requirement"_test = [] {
        reset_observations();
        mod::static_mod_registry registry{};
        expect(static_cast<bool>(registry.add(
            mod::static_mod_descriptor{.id = "example.shop",
                                       .version = "1.0.0",
                                       .abi = 1,
                                       .factory = &shop_factory})));
        auto fixture{
            contract_pair_fixture(shop_manifest("1.0.0", 24, "<2.0.0"),
                                  consumer_manifest(">=1.0.0 <2.0.0"))};
        expect(fixture.has_value());
        if (!fixture) {
            return;
        }
        mod::host host{registry};
        auto loaded{host.load(fixture->plan, fixture->image)};
        expect(static_cast<bool>(loaded))
            << (loaded ? std::string{"load ok"} : loaded.error().message);
        if (loaded) {
            expect(host.services().active_provider("example:shop").has_value());
        }
    };

    "custom_contract/identical duplicate declaration is a conflict"_test = [] {
        mod::static_mod_registry registry{};
        auto first{make_manifest("example.shop.a")};
        first += "\n[contracts.\"example:shop\"]\nversion = \"1.0.0\"\n"
                 "vtable_size = 24\n";
        auto second{make_manifest("example.shop.b")};
        second += "\n[contracts.\"example:shop\"]\nversion = \"1.0.0\"\n"
                  "vtable_size = 24\n";
        auto fixture{make_contract_fixture(
            "\"example.shop.a@1.0.0\", \"example.shop.b@1.0.0\"", "",
            {{"example.shop.a", std::move(first)},
             {"example.shop.b", std::move(second)}})};
        expect(fixture.has_value());
        if (!fixture) {
            return;
        }
        mod::host host{registry};
        auto loaded{host.load(fixture->plan, fixture->image)};
        expect(!loaded);
        if (!loaded) {
            expect(loaded.error().code == error_code::already_exists);
            expect(loaded.error().message.find("example.shop.a") !=
                   std::string::npos);
            expect(loaded.error().message.find("example.shop.b") !=
                   std::string::npos);
        }
    };

    "custom_contract/trusted non-owner provider registers the contract"_test =
        [] {
            reset_observations();
            mod::static_mod_registry registry{};
            expect(static_cast<bool>(registry.add(
                mod::static_mod_descriptor{.id = "example.shop.other",
                                           .version = "1.0.0",
                                           .abi = 1,
                                           .factory = &shop_factory})));
            auto owner{make_manifest("example.shop.owner")};
            owner += "\n[contracts.\"example:shop\"]\n"
                     "version = \"1.0.0\"\n"
                     "vtable_size = 24\n"
                     "trusted_providers = [\"example.shop.other\"]\n";
            auto other{make_manifest("example.shop.other")};
            other += "\n[provides]\nservices = [\"example:shop\"]\n";
            other += "\n[requires]\n\"example.shop.owner\" = \"^1.0.0\"\n";
            auto fixture{make_contract_fixture(
                "\"example.shop.owner@1.0.0\", \"example.shop.other@1.0.0\"",
                "\"example:shop\" = \"example.shop.other@^1.0.0\"\n",
                {{"example.shop.owner", std::move(owner)},
                 {"example.shop.other", std::move(other)}})};
            expect(fixture.has_value());
            if (!fixture) {
                return;
            }
            mod::host host{registry};
            auto loaded{host.load(fixture->plan, fixture->image)};
            expect(static_cast<bool>(loaded))
                << (loaded ? std::string{"load ok"} : loaded.error().message);
            if (!loaded) {
                return;
            }
            const auto active{host.services().active_provider("example:shop")};
            expect(active.has_value());
            if (active) {
                expect(active->id == "example.shop.other");
            }
        };
};

const suite custom_contract_epoch_tests = [] {
    "custom_contract/hot reload publishes the owner's new declaration"_test =
        [] {
            mod::static_mod_registry registry{};
            auto initial{contract_pair_fixture(
                shop_manifest(), consumer_manifest("^1.0.0"), true)};
            expect(initial.has_value());
            if (!initial) {
                return;
            }
            mod::host host{registry};
            contract_loader_state loader{};
            expect(register_contract_loader(host, loader));
            const mod::provider_api v1_api{
                .vtable = &g_shop_table, .self = &g_shop_self, .abi_major = 1};
            loader.api = &v1_api;
            auto loaded{host.load(initial->plan, initial->image)};
            expect(static_cast<bool>(loaded))
                << (loaded ? std::string{"load ok"} : loaded.error().message);
            if (!loaded) {
                return;
            }
            expect(host.epoch() == 1U);

            // A patch-level contract bump: same layout, new ABI patch.
            auto patched{shop_manifest("1.0.1", 24)};
            shop_vtable patched_table{g_shop_table};
            patched_table.abi_patch = 1;
            const mod::provider_api patched_api{
                .vtable = &patched_table, .self = &g_shop_self, .abi_major = 1};
            loader.api = &patched_api;
            auto next{contract_pair_fixture(std::move(patched),
                                            consumer_manifest("^1.0.0"), true)};
            expect(next.has_value());
            if (!next) {
                return;
            }
            auto staged{host.stage_composition(
                next->plan, make_package_resolver_for(next->image),
                mod::stage_options{.reload = {"example.shop"}})};
            expect(static_cast<bool>(staged))
                << (staged ? std::string{"staged"} : staged.error().message);
            if (!staged) {
                return;
            }
            expect(loader.reload_calls == 1);
            auto committed{host.commit_epoch()};
            expect(static_cast<bool>(committed))
                << (committed ? std::string{"committed"}
                              : committed.error().message);
            if (!committed) {
                return;
            }
            expect(host.epoch() == 2U);
            const auto *shop{host.find("example.shop")};
            expect(shop != nullptr);
            if (shop != nullptr) {
                expect(shop->contracts.size() == 1U);
                expect(shop->contracts.front().version.text() == "1.0.1");
            }
            expect(host.services().active_provider("example:shop").has_value());
        };

    "custom_contract/uninstall of a required declaration fails not_found"_test =
        [] {
            mod::static_mod_registry registry{};
            auto initial{contract_pair_fixture(
                shop_manifest(), consumer_manifest("^1.0.0", false), true)};
            expect(initial.has_value());
            if (!initial) {
                return;
            }
            mod::host host{registry};
            contract_loader_state loader{};
            expect(register_contract_loader(host, loader));
            const mod::provider_api v1_api{
                .vtable = &g_shop_table, .self = &g_shop_self, .abi_major = 1};
            loader.api = &v1_api;
            auto loaded{host.load(initial->plan, initial->image)};
            expect(static_cast<bool>(loaded))
                << (loaded ? std::string{"load ok"} : loaded.error().message);
            if (!loaded) {
                return;
            }
            auto uninstall{host.stage_uninstall("example.shop")};
            expect(!uninstall);
            if (!uninstall) {
                expect(uninstall.error().code == error_code::not_found);
                const auto &message{uninstall.error().message};
                expect(message.find("example.shop.client") !=
                       std::string::npos);
                expect(message.find("example:shop") != std::string::npos);
            }
            expect(host.mods().size() == 2U);
            expect(host.services().active_provider("example:shop").has_value());
        };

    "custom_contract/same-id reload must re-register a matching table"_test =
        [] {
            mod::static_mod_registry registry{};
            auto initial{contract_pair_fixture(
                shop_manifest(), consumer_manifest("^1.0.0"), true)};
            expect(initial.has_value());
            if (!initial) {
                return;
            }
            mod::host host{registry};
            contract_loader_state loader{};
            expect(register_contract_loader(host, loader));
            const mod::provider_api v1_api{
                .vtable = &g_shop_table, .self = &g_shop_self, .abi_major = 1};
            loader.api = &v1_api;
            auto loaded{host.load(initial->plan, initial->image)};
            expect(static_cast<bool>(loaded))
                << (loaded ? std::string{"load ok"} : loaded.error().message);
            if (!loaded) {
                return;
            }

            // A minor bump widens the vtable and the ABI version. A reload that
            // keeps the old table fails closed at registration.
            auto next{contract_pair_fixture(shop_manifest("1.1.0", 32),
                                            consumer_manifest("^1.0.0"), true)};
            expect(next.has_value());
            if (!next) {
                return;
            }
            loader.api = &v1_api;
            auto refused{host.stage_composition(
                next->plan, make_package_resolver_for(next->image))};
            expect(!refused);
            if (!refused) {
                // The loader ABI carries only loader error codes, so the host
                // reports the registration refusal as permission_denied; the
                // precise version diagnostic is the loader's last error.
                expect(refused.error().code == error_code::permission_denied);
                expect(loader.last_error.find("1.1.0") != std::string::npos);
                expect(loader.last_error.find("struct_size 32") !=
                       std::string::npos);
            }

            // Re-registering the matching table publishes the new declaration.
            const mod::provider_api v11_api{.vtable = &g_shop_table_v11,
                                            .self = &g_shop_self,
                                            .abi_major = 1};
            loader.api = &v11_api;
            auto accepted{host.stage_composition(
                next->plan, make_package_resolver_for(next->image))};
            expect(static_cast<bool>(accepted))
                << (accepted ? std::string{"staged"}
                             : accepted.error().message);
            if (!accepted) {
                return;
            }
            if (auto committed{host.commit_epoch()}; !committed) {
                expect(false) << committed.error().message;
                return;
            }
            const auto *shop{host.find("example.shop")};
            expect(shop != nullptr);
            if (shop != nullptr) {
                expect(shop->contracts.front().version.text() == "1.1.0");
                expect(shop->contracts.front().vtable_size == 32U);
            }
        };

    "custom_contract/reload re-checks an unchanged trusted provider"_test = [] {
        mod::static_mod_registry registry{};
        auto owner{make_manifest("example.shop.owner")};
        owner += "\n[contracts.\"example:shop\"]\n"
                 "version = \"1.0.0\"\n"
                 "vtable_size = 24\n"
                 "trusted_providers = [\"example.shop.provider\"]\n";
        auto provider{make_manifest("example.shop.provider")};
        provider += "\n[provides]\nservices = [\"example:shop\"]\n";
        provider += "\n[requires]\n\"example.shop.owner\" = \"^1.0.0\"\n";
        auto fixture{make_contract_fixture(
            "\"example.shop.owner@1.0.0\", \"example.shop.provider@1.0.0\"",
            "\"example:shop\" = \"example.shop.provider@^1.0.0\"\n",
            {{"example.shop.owner", with_contract_loader(std::move(owner))},
             {"example.shop.provider",
              with_contract_loader(std::move(provider))}})};
        expect(fixture.has_value());
        if (!fixture) {
            return;
        }
        mod::host host{registry};
        contract_loader_state loader{};
        loader.provide_mod = "example.shop.provider";
        expect(register_contract_loader(host, loader));
        const mod::provider_api v1_api{
            .vtable = &g_shop_table, .self = &g_shop_self, .abi_major = 1};
        loader.api = &v1_api;
        auto loaded{host.load(fixture->plan, fixture->image)};
        expect(static_cast<bool>(loaded))
            << (loaded ? std::string{"load ok"} : loaded.error().message);
        if (!loaded) {
            return;
        }
        // The owner reloads with a new contract version while the trusted
        // provider stays active on its old table.
        auto bumped_owner{make_manifest("example.shop.owner")};
        bumped_owner += "\n[contracts.\"example:shop\"]\n"
                        "version = \"1.1.0\"\n"
                        "vtable_size = 32\n"
                        "trusted_providers = [\"example.shop.provider\"]\n";
        auto provider_again{make_manifest("example.shop.provider")};
        provider_again += "\n[provides]\nservices = [\"example:shop\"]\n";
        provider_again += "\n[requires]\n\"example.shop.owner\" = \"^1.0.0\"\n";
        auto next{make_contract_fixture(
            "\"example.shop.owner@1.0.0\", \"example.shop.provider@1.0.0\"",
            "\"example:shop\" = \"example.shop.provider@^1.0.0\"\n",
            {{"example.shop.owner",
              with_contract_loader(std::move(bumped_owner))},
             {"example.shop.provider",
              with_contract_loader(std::move(provider_again))}})};
        expect(next.has_value());
        if (!next) {
            return;
        }
        loader.api = nullptr;
        auto staged{host.stage_composition(
            next->plan, make_package_resolver_for(next->image))};
        expect(!staged);
        if (!staged) {
            expect(staged.error().code == error_code::version_mismatch);
            const auto &message{staged.error().message};
            expect(message.find("example.shop.provider") != std::string::npos);
            expect(message.find("1.0.0") != std::string::npos);
            expect(message.find("1.1.0") != std::string::npos);
            expect(message.find("struct_size 24") != std::string::npos);
            expect(message.find("struct_size 32") != std::string::npos);
        }
    };
};

} // namespace
