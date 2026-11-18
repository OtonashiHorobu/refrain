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
#include <refrain/core/config.hpp>
#include <refrain/core/error.hpp>
#include <refrain/data/builtin_image.hpp>
#include <refrain/host/core_api.hpp>
#include <refrain/mod/discovery.hpp>
#include <refrain/mod/gamepack.hpp>
#include <refrain/mod/host.hpp>
#include <refrain/mod/manifest.hpp>
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

[[nodiscard]] auto shop_manifest() -> std::string {
    auto text{make_manifest("example.shop")};
    text += "\n[provides]\nservices = [\"example:shop\"]\n";
    text += "\n[contracts.\"example:shop\"]\n";
    text += "version = \"1.0.0\"\n";
    text += "vtable_size = 24\n";
    text += "compatible = \">=1.0.0 <2.0.0\"\n";
    return text;
}

[[nodiscard]] auto consumer_manifest(std::string_view required_range)
    -> std::string {
    auto text{make_manifest("example.shop.client")};
    text += "\n[requires]\n\"example.shop\" = \"^1.0.0\"\n";
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
            expect(message.find("expects 24") != std::string::npos);
        }
    };
};

} // namespace
