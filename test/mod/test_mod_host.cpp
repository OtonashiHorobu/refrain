// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Auto-scan runtime wiring and plan-driven provider activation:
// `mod_host::loaded_mod` exposes the raw manifest hash plus the dependency
// lists so `refrain-server` can build a `mod::discovered_mod` for the builtin
// composition without the kernel header depending on the data layer, and
// provider code only runs for the mods `startup_plan.providers` selected.
// These tests pin both contracts, the fail-closed startup check and the
// failed-host cleanup.

#include <refrain/core/config.hpp>
#include <refrain/core/error.hpp>
#include <refrain/data/builtin_image.hpp>
#include <refrain/data/checksum.hpp>
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

int g_factory_calls{0};
int g_loser_calls{0};
error_code g_register_error{error_code::success};

/// Leading 4 x `uint32_t` header of a generated service vtable
/// (`struct_size`, `abi_major`, `abi_minor`, `abi_patch`). The slots after it
/// are contract specific; the host only inspects the header.
struct fake_vtable {
    std::uint32_t struct_size{16};
    std::uint32_t abi_major{1};
    std::uint32_t abi_minor{0};
    std::uint32_t abi_patch{0};
};

fake_vtable g_good_vtable{};
fake_vtable g_short_vtable{.struct_size = 8};
int g_provider_self{0};

[[nodiscard]] auto make_provider_api(const fake_vtable &vtable,
                                     std::uint32_t abi_major)
    -> refrain::mod::provider_api {
    return refrain::mod::provider_api{
        .vtable = &vtable, .self = &g_provider_self, .abi_major = abi_major};
}

[[nodiscard]] auto noop_factory(refrain::mod::host & /*host*/)
    -> refrain::status {
    ++g_factory_calls;
    return refrain::status_ok();
}

/// Plan-selected provider: publishes a `refrain:data.packs` provider.
[[nodiscard]] auto selected_factory(refrain::mod::host &host)
    -> refrain::status {
    ++g_factory_calls;
    return host.register_provider("refrain:data.packs",
                                  make_provider_api(g_good_vtable, 1));
}

/// Not selected by the plan: must never run. Registering here would be denied,
/// so this factory returning normally is only possible when it is skipped.
[[nodiscard]] auto loser_factory(refrain::mod::host &host) -> refrain::status {
    ++g_loser_calls;
    return host.register_provider("refrain:data.packs",
                                  make_provider_api(g_good_vtable, 1));
}

/// Mod without `[provides]`: the factory runs (no service claim in the
/// manifest), but `register_provider()` must deny the claim.
[[nodiscard]] auto unplanned_factory(refrain::mod::host &host)
    -> refrain::status {
    ++g_factory_calls;
    const auto status{host.register_provider(
        "refrain:data.packs", make_provider_api(g_good_vtable, 1))};
    g_register_error = status ? error_code::success : status.error().code;
    return refrain::status_ok();
}

[[nodiscard]] auto abi_mismatch_factory(refrain::mod::host &host)
    -> refrain::status {
    return host.register_provider("refrain:data.packs",
                                  make_provider_api(g_good_vtable, 2));
}

[[nodiscard]] auto short_header_factory(refrain::mod::host &host)
    -> refrain::status {
    return host.register_provider("refrain:data.packs",
                                  make_provider_api(g_short_vtable, 1));
}

[[nodiscard]] auto make_manifest(std::string_view id,
                                 std::string_view provides = {})
    -> std::string {
    auto text{std::string{"[mod]\n"}};
    text += "id = \"" + std::string{id} + "\"\n";
    text += "version = \"1.0.0\"\n";
    text += "abi = \"1\"\n";
    text += "side = \"server\"\n";
    if (!provides.empty()) {
        text +=
            "\n[provides]\nservices = [\"" + std::string{provides} + "\"]\n";
    }
    return text;
}

[[nodiscard]] auto make_gamepack(std::string_view mods_list,
                                 std::string_view providers = {})
    -> refrain::result<refrain::mod::composition> {
    auto text{std::string{"[gamepack]\n"}};
    text += "id = \"refrain.test.metadata\"\n";
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
    for (auto &[id, manifest] : manifests) {
        inputs.push_back(data::builtin_image_input{
            .path = "mods/" + std::move(id) + "/manifest.toml",
            .data = to_bytes(manifest)});
    }
    auto bytes{data::build_builtin_image(inputs)};
    expect(static_cast<bool>(bytes)) << "synthetic image must build";
    return bytes ? *bytes : std::vector<std::byte>{};
}

/// Load a one-mod gamepack whose `data` provider is `refrain.test.abi` with the
/// given factory, and report the load status plus the surviving candidate
/// count (the ABI checks run during registration, i.e. well before freeze).
struct abi_case_result {
    refrain::status status{};
    std::size_t providers{0};
};

[[nodiscard]] auto run_abi_case(mod::static_mod_factory factory)
    -> abi_case_result {
    mod::static_mod_registry registry{};
    static_cast<void>(
        registry.add(mod::static_mod_descriptor{.id = "refrain.test.abi",
                                                .version = "1.0.0",
                                                .abi = 1,
                                                .factory = factory}));
    auto plan{make_gamepack("\"refrain.test.abi@1.0.0\"",
                            "data = \"refrain.test.abi@^1.0.0\"\n")};
    if (!plan) {
        return abi_case_result{
            .status = refrain::status{std::unexpected{plan.error()}}};
    }
    const auto image{build_image(
        {{"refrain.test.abi",
          make_manifest("refrain.test.abi", "refrain:data.packs")}})};
    refrain::mod::host host{registry};
    auto status{host.load(*plan, image)};
    const auto providers{host.services().provider_count()};
    return abi_case_result{.status = std::move(status), .providers = providers};
}

const suite mod_host_metadata_tests = [] {
    "mod_host/exposes manifest hash and requirements"_test = [] {
        mod::static_mod_registry registry{};
        expect(static_cast<bool>(registry.add(
            mod::static_mod_descriptor{.id = "refrain.test.meta.provider",
                                       .version = "1.0.0",
                                       .abi = 1,
                                       .factory = &noop_factory})));

        auto plan{make_gamepack("\"refrain.test.meta.plain@1.0.0\", "
                                "\"refrain.test.meta.provider@1.0.0\"")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        g_factory_calls = 0;

        const std::string plain_manifest{
            "[mod]\n"
            "id = \"refrain.test.meta.plain\"\n"
            "version = \"1.0.0\"\n"
            "abi = \"1\"\n"
            "side = \"server\"\n"
            "\n[requires]\n"
            "\"refrain.test.meta.provider\" = \"^1.0.0\"\n"
            "\n[optional_requires]\n"
            "\"refrain.test.meta.extra\" = \"^2.0.0\"\n"};
        const std::string provider_manifest{
            "[mod]\n"
            "id = \"refrain.test.meta.provider\"\n"
            "version = \"1.0.0\"\n"
            "abi = \"1\"\n"
            "side = \"server\"\n"
            "\n[provides]\n"
            "services = [\"refrain:data.packs\"]\n"};
        const auto image{
            build_image({{"refrain.test.meta.plain", plain_manifest},
                         {"refrain.test.meta.provider", provider_manifest}})};

        refrain::mod::host host{registry};
        expect(static_cast<bool>(host.load(*plan, image)));
        if (!host.loaded()) {
            return;
        }

        const auto *plain{host.find("refrain.test.meta.plain")};
        expect(plain != nullptr);
        if (plain != nullptr) {
            expect(plain->manifest_hash_hex ==
                   data::to_hex(data::blake3(to_bytes(plain_manifest))));
            expect(plain->required.size() == 1U);
            if (plain->required.size() == 1U) {
                expect(plain->required[0].id == "refrain.test.meta.provider");
                expect(plain->required[0].range == "^1.0.0");
            }
            expect(plain->optional.size() == 1U);
            if (plain->optional.size() == 1U) {
                expect(plain->optional[0].id == "refrain.test.meta.extra");
                expect(plain->optional[0].range == "^2.0.0");
            }
        }

        const auto *provider{host.find("refrain.test.meta.provider")};
        expect(provider != nullptr);
        if (provider != nullptr) {
            // The gamepack has no `[providers]` entry, so the mod is a
            // data/content mod: its factory must not run even though it
            // declares `[provides]`.
            expect(!provider->has_code);
            expect(provider->manifest_hash_hex ==
                   data::to_hex(data::blake3(to_bytes(provider_manifest))));
            expect(provider->required.empty());
            expect(provider->optional.empty());
        }
        expect(g_factory_calls == 0)
            << "factory ran for an unselected provider";
        expect(
            !host.services().active_provider("refrain:data.packs").has_value());
    };
};

const suite mod_host_provider_activation_tests = [] {
    "mod_host/activates only the provider the plan selected"_test = [] {
        mod::static_mod_registry registry{};
        expect(static_cast<bool>(registry.add(
            mod::static_mod_descriptor{.id = "refrain.test.provider.a",
                                       .version = "1.0.0",
                                       .abi = 1,
                                       .factory = &selected_factory})));
        expect(static_cast<bool>(registry.add(
            mod::static_mod_descriptor{.id = "refrain.test.provider.b",
                                       .version = "1.0.0",
                                       .abi = 1,
                                       .factory = &loser_factory})));
        g_factory_calls = 0;
        g_loser_calls = 0;

        auto plan{make_gamepack("\"refrain.test.provider.a@1.0.0\", "
                                "\"refrain.test.provider.b@1.0.0\"",
                                "data = \"refrain.test.provider.a@^1.0.0\"\n")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        const auto image{build_image(
            {{"refrain.test.provider.a",
              make_manifest("refrain.test.provider.a", "refrain:data.packs")},
             {"refrain.test.provider.b",
              make_manifest("refrain.test.provider.b",
                            "refrain:data.packs")}})};

        refrain::mod::host host{registry};
        auto loaded{host.load(*plan, image)};
        expect(static_cast<bool>(loaded))
            << (loaded ? std::string{"load ok"} : loaded.error().message);
        if (!loaded) {
            return;
        }
        expect(g_factory_calls == 1);
        expect(g_loser_calls == 0) << "unselected provider factory ran";
        const auto *winner{host.find("refrain.test.provider.a")};
        const auto *loser{host.find("refrain.test.provider.b")};
        expect(winner != nullptr);
        expect(loser != nullptr);
        if (winner != nullptr) {
            expect(winner->has_code);
        }
        if (loser != nullptr) {
            expect(!loser->has_code);
        }
        const auto active{
            host.services().active_provider("refrain:data.packs")};
        expect(active.has_value());
        if (active) {
            expect(active->id == "refrain.test.provider.a");
        }
    };

    "mod_host/rejects a plan whose selected provider is not loaded"_test = [] {
        mod::static_mod_registry registry{};
        auto plan{make_gamepack("\"refrain.test.orphan@1.0.0\"",
                                "data = \"refrain.test.orphan@^1.0.0\"\n")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        // The resolver can only select listed mods; simulate a plan/image
        // mismatch by pointing the selected provider at a mod that is not in
        // the built-in image.
        for (auto &binding : plan->providers) {
            if (binding.service == "data") {
                binding.provider = "refrain.test.ghost@1.0.0";
            }
        }
        const auto image{build_image(
            {{"refrain.test.orphan", make_manifest("refrain.test.orphan")}})};

        refrain::mod::host host{registry};
        auto loaded{host.load(*plan, image)};
        expect(!loaded);
        if (!loaded) {
            expect(loaded.error().code == error_code::not_found);
            expect(loaded.error().message.find("data") != std::string::npos);
            expect(loaded.error().message.find("refrain.test.ghost") !=
                   std::string::npos);
        }
        expect(host.services().provider_count() == 0U);
        expect(!host.loaded());
    };

    "mod_host/denies provider registration outside the plan"_test = [] {
        mod::static_mod_registry registry{};
        expect(static_cast<bool>(registry.add(
            mod::static_mod_descriptor{.id = "refrain.test.unplanned",
                                       .version = "1.0.0",
                                       .abi = 1,
                                       .factory = &unplanned_factory})));
        g_factory_calls = 0;
        g_register_error = error_code::success;

        auto plan{make_gamepack("\"refrain.test.unplanned@1.0.0\"")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        const auto image{
            build_image({{"refrain.test.unplanned",
                          make_manifest("refrain.test.unplanned")}})};

        refrain::mod::host host{registry};
        expect(static_cast<bool>(host.load(*plan, image)));
        expect(g_factory_calls == 1) << "mod without [provides] keeps code";
        expect(g_register_error == error_code::permission_denied);
        expect(
            !host.services().active_provider("refrain:data.packs").has_value());
        const auto *mod{host.find("refrain.test.unplanned")};
        expect(mod != nullptr);
        if (mod != nullptr) {
            expect(mod->has_code);
        }
    };

    "mod_host/failed load drops registrations and refuses reuse"_test = [] {
        mod::static_mod_registry registry{};
        expect(static_cast<bool>(registry.add(
            mod::static_mod_descriptor{.id = "refrain.test.provider.a",
                                       .version = "1.0.0",
                                       .abi = 1,
                                       .factory = &selected_factory})));
        g_factory_calls = 0;

        auto plan{make_gamepack(
            "\"refrain.test.provider.a@1.0.0\", \"refrain.test.absent@1.0.0\"",
            "data = \"refrain.test.provider.a@^1.0.0\"\n")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        const auto image{build_image({{"refrain.test.provider.a",
                                       make_manifest("refrain.test.provider.a",
                                                     "refrain:data.packs")}})};

        refrain::mod::host host{registry};
        auto loaded{host.load(*plan, image)};
        expect(!loaded);
        if (!loaded) {
            expect(loaded.error().code == error_code::not_found);
            expect(loaded.error().message.find("refrain.test.absent") !=
                   std::string::npos);
        }
        // The selected provider registered before the missing manifest failed
        // the load; the failure path must drop it again.
        expect(g_factory_calls == 1);
        expect(host.services().provider_count() == 0U);
        expect(host.services().active_count() == 0U);
        expect(
            !host.services().active_provider("refrain:data.packs").has_value());
        expect(host.builtin_image_bytes().empty());
        expect(host.builtin_image_digest().empty());
        expect(host.mods().empty());
        expect(!host.loaded());

        auto again{host.load(*plan, image)};
        expect(!again);
        if (!again) {
            expect(again.error().code == error_code::unsupported);
            expect(again.error().message.find("recreate") != std::string::npos);
        }
    };

    "mod_host/validates the provider vtable ABI header"_test = [] {
        auto mismatch{run_abi_case(&abi_mismatch_factory)};
        expect(!mismatch.status);
        if (!mismatch.status) {
            expect(mismatch.status.error().code ==
                   error_code::version_mismatch);
            expect(mismatch.status.error().message.find("abi major") !=
                   std::string::npos);
        }
        expect(mismatch.providers == 0U);

        auto short_header{run_abi_case(&short_header_factory)};
        expect(!short_header.status);
        if (!short_header.status) {
            expect(short_header.status.error().code ==
                   error_code::version_mismatch);
            expect(short_header.status.error().message.find("struct_size") !=
                   std::string::npos);
        }
        expect(short_header.providers == 0U);

        auto good{run_abi_case(&selected_factory)};
        expect(static_cast<bool>(good.status))
            << (good.status ? std::string{"load ok"}
                            : good.status.error().message);
        expect(good.providers == 1U);
    };
};

} // namespace
