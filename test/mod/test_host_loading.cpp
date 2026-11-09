// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/app/host_context.hpp>
#include <refrain/core/config.hpp>
#include <refrain/core/io.hpp>
#include <refrain/data/builtin_image.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/host/core_api.hpp>
#include <refrain/mod/discovery.hpp>
#include <refrain/mod/gamepack.hpp>
#include <refrain/mod/host.hpp>
#include <refrain/mod/manifest.hpp>
#include <refrain/mod/static_mod.hpp>

#include <boost/ut.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
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

/// Minimal 4 x `uint32_t` vtable header (the fake table only needs the ABI
/// header: this factory registers with `abi_major = 0`, which skips the check).
std::uint32_t g_fake_vtable[4]{16, 1, 0, 0};
int g_fake_self{0};

int g_instanced_self{0};

/// Registers the provider under the plan's `rw` instance (instanced binding).
auto instanced_factory(refrain::mod::host &host) -> refrain::status {
    ++g_factory_calls;
    const refrain::mod::provider_api api{
        .vtable = g_fake_vtable, .self = &g_instanced_self, .abi_major = 0};
    return host.register_provider("refrain:data.packs", "rw", api);
}

/// Tries the reserved default instance for an instanced binding: fail-closed.
auto default_on_instanced_factory(refrain::mod::host &host) -> refrain::status {
    const refrain::mod::provider_api api{
        .vtable = g_fake_vtable, .self = &g_instanced_self, .abi_major = 0};
    return host.register_provider("refrain:data.packs", api);
}

auto counting_factory(refrain::mod::host &host) -> refrain::status {
    ++g_factory_calls;
    const refrain::mod::provider_api api{
        .vtable = g_fake_vtable, .self = &g_fake_self, .abi_major = 0};
    return host.register_provider("refrain:data.packs", api);
}

[[nodiscard]] auto
make_manifest(std::string_view id, std::string_view version = "1.0.0",
              std::uint32_t abi = 1, std::string_view side = "server",
              std::string_view provides = {}) -> std::string {
    auto text{std::string{"[mod]\n"}};
    text += "id = \"" + std::string{id} + "\"\n";
    text += "version = \"" + std::string{version} + "\"\n";
    text += "abi = " + std::to_string(abi) + "\n";
    text += "side = \"" + std::string{side} + "\"\n";
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
    text += "id = \"refrain.test.host\"\n";
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

struct image_builder {
    std::vector<data::builtin_image_input> inputs{};

    void add(std::string id, std::string manifest) {
        const auto bytes{std::as_bytes(
            std::span<const char>{manifest.data(), manifest.size()})};
        std::vector<std::byte> owned{bytes.begin(), bytes.end()};
        inputs.push_back(data::builtin_image_input{
            .path = "mods/" + std::move(id) + "/manifest.toml",
            .data = std::move(owned)});
    }

    [[nodiscard]] auto build() -> std::vector<std::byte> {
        auto bytes{data::build_builtin_image(inputs)};
        expect(static_cast<bool>(bytes)) << "synthetic image must build";
        return bytes ? *bytes : std::vector<std::byte>{};
    }
};

const suite mod_host_tests = [] {
    "mod_host/loads internal data-only mods"_test = [] {
        mod::static_mod_registry registry{};

        auto plan{make_gamepack(
            "\"refrain.test.content@1.0.0\", \"refrain.test.data@1.0.0\"")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        image_builder builder{};
        builder.add("refrain.test.content",
                    make_manifest("refrain.test.content"));
        builder.add("refrain.test.data", make_manifest("refrain.test.data"));
        const auto image{builder.build()};

        refrain::mod::host host{registry};
        expect(static_cast<bool>(host.load(*plan, image)));
        expect(host.loaded());
        expect(host.mods().size() == 2U);
        if (host.mods().size() == 2U) {
            expect(host.mods()[0].id == "refrain.test.content");
            expect(host.mods()[1].id == "refrain.test.data");
            expect(!host.mods()[0].has_code);
            expect(!host.mods()[0].external);
        }
        expect(host.find("refrain.test.data") != nullptr);
        expect(host.find("refrain.test.missing") == nullptr);

        auto opened{data::builtin_image::open(image)};
        expect(static_cast<bool>(opened));
        if (opened) {
            expect(host.builtin_image_digest() ==
                   data::to_hex(opened->digest()));
        }
        const auto formatted{host.format()};
        expect(formatted.find("mod host: 2 mod(s) loaded") !=
               std::string::npos);
        expect(formatted.find("refrain.test.data@1.0.0") != std::string::npos);
        expect(formatted.find("data-only") != std::string::npos);
    };

    "mod_host/calls the static factory for a registered mod"_test = [] {
        mod::static_mod_registry registry{};
        g_factory_calls = 0;
        expect(static_cast<bool>(registry.add(
            mod::static_mod_descriptor{.id = "refrain.test.provider",
                                       .version = "1.0.0",
                                       .abi = 1,
                                       .factory = &counting_factory})));

        auto plan{make_gamepack("\"refrain.test.provider@1.0.0\"",
                                "data = \"refrain.test.provider@^1.0.0\"\n")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        expect(registry.find("refrain.test.provider") != nullptr);
        expect(registry.size() == 1U);
        image_builder builder{};
        builder.add("refrain.test.provider",
                    make_manifest("refrain.test.provider", "1.0.0", 1, "server",
                                  "refrain:data.packs"));
        const auto image{builder.build()};

        refrain::mod::host host{registry};
        auto loaded{host.load(*plan, image)};
        expect(static_cast<bool>(loaded))
            << (loaded ? std::string{"load ok"} : loaded.error().message);
        if (!loaded) {
            return;
        }
        expect(g_factory_calls == 1);
        expect(host.mods().size() == 1U);
        if (host.mods().size() == 1U) {
            expect(host.mods()[0].has_code);
            expect(host.mods()[0].provides_summary == "refrain:data.packs");
        }
    };

    "mod_host/rejects a missing manifest"_test = [] {
        mod::static_mod_registry registry{};
        auto plan{make_gamepack(
            "\"refrain.test.present@1.0.0\", \"refrain.test.absent@1.0.0\"")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        image_builder builder{};
        builder.add("refrain.test.present",
                    make_manifest("refrain.test.present"));
        const auto image{builder.build()};

        refrain::mod::host host{registry};
        auto loaded{host.load(*plan, image)};
        expect(!loaded);
        if (!loaded) {
            expect(loaded.error().code == error_code::not_found);
            expect(loaded.error().message.find("refrain.test.absent") !=
                   std::string::npos);
        }
        expect(!host.loaded());
        expect(host.mods().empty());
    };

    "mod_host/rejects version, abi and side drift"_test = [] {
        mod::static_mod_registry registry{};

        const auto run = [&registry](std::string manifest) {
            auto plan{make_gamepack("\"refrain.test.mod@1.0.0\"")};
            expect(static_cast<bool>(plan));
            if (!plan) {
                return refrain::status{};
            }
            image_builder builder{};
            builder.add("refrain.test.mod", std::move(manifest));
            refrain::mod::host host{registry};
            return host.load(*plan, builder.build());
        };

        auto version{run(make_manifest("refrain.test.mod", "1.0.1"))};
        expect(!version);
        if (!version) {
            expect(version.error().code == error_code::version_mismatch);
            expect(version.error().message.find("1.0.1") != std::string::npos);
        }
        auto abi{run(make_manifest("refrain.test.mod", "1.0.0", 2))};
        expect(!abi);
        if (!abi) {
            expect(abi.error().code == error_code::version_mismatch);
        }
        auto side{run(make_manifest("refrain.test.mod", "1.0.0", 1, "client"))};
        expect(!side);
        if (!side) {
            expect(side.error().code == error_code::version_mismatch);
        }
        auto id{run(make_manifest("refrain.test.other"))};
        expect(!id);
        if (!id) {
            expect(id.error().code == error_code::invalid_argument);
        }
    };

    "mod_host/rejects external mods"_test = [] {
        mod::static_mod_registry registry{};
        auto plan{make_gamepack("\"refrain.test.external@1.0.0\"")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        auto manifest{make_manifest("refrain.test.external")};
        manifest += "\n[entry]\nnative = \"lib/libexternal.so\"\n";
        image_builder builder{};
        builder.add("refrain.test.external", std::move(manifest));
        refrain::mod::host host{registry};
        auto loaded{host.load(*plan, builder.build())};
        expect(!loaded);
        if (!loaded) {
            expect(loaded.error().code == error_code::unsupported);
            expect(loaded.error().message.find("external") !=
                   std::string::npos);
        }
    };

    "mod_host/rejects provides without static code"_test = [] {
        mod::static_mod_registry registry{};
        // The provider must be selected by the plan for the missing factory to
        // be an error; an unselected one stays a data/content mod.
        auto plan{make_gamepack("\"refrain.test.ghost@1.0.0\"",
                                "data = \"refrain.test.ghost@^1.0.0\"\n")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        image_builder builder{};
        builder.add("refrain.test.ghost",
                    make_manifest("refrain.test.ghost", "1.0.0", 1, "server",
                                  "refrain:data.packs"));
        refrain::mod::host host{registry};
        auto loaded{host.load(*plan, builder.build())};
        expect(!loaded);
        if (!loaded) {
            expect(loaded.error().code == error_code::unsupported);
            expect(loaded.error().message.find("REFR_MOD_STATIC") !=
                   std::string::npos);
        }
    };

    "mod_host/keeps an unselected provider mod data-only"_test = [] {
        mod::static_mod_registry registry{};
        g_factory_calls = 0;
        expect(static_cast<bool>(registry.add(
            mod::static_mod_descriptor{.id = "refrain.test.unselected",
                                       .version = "1.0.0",
                                       .abi = 1,
                                       .factory = &counting_factory})));

        // No `[providers]` entry: the mod declares `[provides]` but the plan
        // does not select it, so it loads as a data/content mod.
        auto plan{make_gamepack("\"refrain.test.unselected@1.0.0\"")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        image_builder builder{};
        builder.add("refrain.test.unselected",
                    make_manifest("refrain.test.unselected", "1.0.0", 1,
                                  "server", "refrain:data.packs"));
        refrain::mod::host host{registry};
        auto loaded{host.load(*plan, builder.build())};
        expect(static_cast<bool>(loaded))
            << (loaded ? std::string{"load ok"} : loaded.error().message);
        expect(g_factory_calls == 0)
            << "factory ran for an unselected provider";
        expect(host.mods().size() == 1U);
        if (host.mods().size() == 1U) {
            expect(!host.mods()[0].has_code);
            expect(host.mods()[0].provides_summary == "refrain:data.packs");
        }
        expect(
            !host.services().active_provider("refrain:data.packs").has_value());
    };

    "mod_host/rejects corrupt images and double loads"_test = [] {
        mod::static_mod_registry registry{};
        auto plan{make_gamepack("\"refrain.test.mod@1.0.0\"")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        const std::vector<std::byte> garbage{std::byte{0x52}, std::byte{0x46}};
        refrain::mod::host broken{};
        auto bad{broken.load(*plan, garbage)};
        expect(!bad);
        if (!bad) {
            expect(bad.error().code == error_code::parse_error);
        }
        auto empty{broken.load(*plan, std::span<const std::byte>{})};
        expect(!empty);
        if (!empty) {
            // A failed load leaves the host unusable: every later load asks
            // the caller to recreate it instead of retrying on half state.
            expect(empty.error().code == error_code::unsupported);
            expect(empty.error().message.find("recreate") != std::string::npos);
        }
        expect(broken.services().provider_count() == 0U);

        image_builder builder{};
        builder.add("refrain.test.mod", make_manifest("refrain.test.mod"));
        const auto image{builder.build()};
        refrain::mod::host host{registry};
        expect(static_cast<bool>(host.load(*plan, image)));
        auto again{host.load(*plan, image)};
        expect(!again);
        if (!again) {
            expect(again.error().code == error_code::already_exists);
        }
    };

    "mod_host/activates planned provider instances"_test = [] {
        mod::static_mod_registry registry{};
        g_factory_calls = 0;
        expect(static_cast<bool>(registry.add(
            mod::static_mod_descriptor{.id = "refrain.test.instanced",
                                       .version = "1.0.0",
                                       .abi = 1,
                                       .factory = &instanced_factory})));
        auto plan{make_gamepack("\"refrain.test.instanced@1.0.0\"",
                                "data = { instances = { rw = "
                                "\"refrain.test.instanced@^1.0.0\" } }\n")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        image_builder builder{};
        builder.add("refrain.test.instanced",
                    make_manifest("refrain.test.instanced", "1.0.0", 1,
                                  "server", "refrain:data.packs"));
        refrain::mod::host host{registry};
        auto loaded{host.load(*plan, builder.build())};
        expect(static_cast<bool>(loaded))
            << (loaded ? std::string{"load ok"} : loaded.error().message);
        expect(g_factory_calls == 1);
        const auto rw{
            host.services().active_provider("refrain:data.packs", "rw")};
        expect(rw.has_value());
        if (rw) {
            expect(rw->id == "refrain.test.instanced");
        }
        // No default instance for an instanced binding.
        expect(
            !host.services().active_provider("refrain:data.packs").has_value());
        expect(host.services().active_instances("refrain:data.packs") ==
               std::vector<std::string>{"rw"});
    };

    "mod_host/denies an unplanned instance for an instanced binding"_test = [] {
        mod::static_mod_registry registry{};
        g_factory_calls = 0;
        expect(static_cast<bool>(registry.add(mod::static_mod_descriptor{
            .id = "refrain.test.instanced",
            .version = "1.0.0",
            .abi = 1,
            .factory = &default_on_instanced_factory})));
        auto plan{make_gamepack("\"refrain.test.instanced@1.0.0\"",
                                "data = { instances = { rw = "
                                "\"refrain.test.instanced@^1.0.0\" } }\n")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        image_builder builder{};
        builder.add("refrain.test.instanced",
                    make_manifest("refrain.test.instanced", "1.0.0", 1,
                                  "server", "refrain:data.packs"));
        refrain::mod::host host{registry};
        auto loaded{host.load(*plan, builder.build())};
        expect(!loaded);
        if (!loaded) {
            expect(loaded.error().code == error_code::permission_denied);
            expect(loaded.error().message.find("instance") !=
                   std::string::npos);
        }
    };

    "mod_host/loads an external native mod"_test = [] {
        namespace fs = std::filesystem;
        const auto root{fs::temp_directory_path() /
                        "refrain-host-external-test"};
        std::error_code error;
        fs::remove_all(root, error);
        expect(fs::create_directories(root, error));

        const auto module_path{root / "libexternal.so"};
        fs::copy_file(fs::path{REFRAIN_TEST_EXTERNAL_MOD}, module_path,
                      fs::copy_options::overwrite_existing, error);
        expect(!error) << error.message();

        constexpr std::string_view manifest_text = R"(
[mod]
id = "refrain.test.external"
version = "1.0.0"
abi = 1
side = "server"

[provides]
services = ["refrain:data.packs"]

[entry]
native = "libexternal.so"
)";
        auto written{refrain::core::io::write_text(
            (root / "manifest.toml").string(), manifest_text)};
        expect(static_cast<bool>(written)) << written.error().message;

        mod::composition plan{};
        plan.spec.id = "refrain.test.external.plan";
        plan.spec.version = "1.0.0";
        plan.spec.side = "server";
        plan.spec.abi = 1;
        plan.spec.auto_scan = false;
        mod::candidate entry{};
        entry.id = "refrain.test.external";
        entry.version = "1.0.0";
        entry.abi = 1;
        entry.origin = mod::mod_origin::user;
        entry.side = mod::manifest_side::server;
        entry.provides = {"refrain:data.packs"};
        entry.path = (root / "manifest.toml").string();
        entry.kind = mod::package_kind::directory;
        entry.entry = mod::manifest_entry_point{.native = "libexternal.so"};
        plan.load_order.push_back(entry);
        plan.providers.push_back(mod::resolved_provider{
            .service = "data",
            .mode = mod::provider_mode::exclusive,
            .provider = "refrain.test.external@1.0.0",
        });

        const auto resolver{
            [](const mod::candidate &candidate)
                -> refrain::result<std::unique_ptr<mod::package_source>> {
                const auto mod_root{
                    std::filesystem::path{candidate.path}.parent_path()};
                return mod::make_directory_package_source(mod_root.string());
            }};

        mod::static_mod_registry registry{};
        mod::host host{registry};
        refrain::app::host_context context{host};
        const mod::provider_api host_api{
            .vtable = &refrain::host::core_vtable(),
            .self = &context,
            .abi_major = REFRAIN_HOST_CORE_ABI_MAJOR,
        };
        expect(static_cast<bool>(host.register_sealed_provider(
            "refrain:host.core", host_api, "1.0.0")));

        auto loaded{host.load(plan, resolver)};
        expect(static_cast<bool>(loaded))
            << (loaded ? std::string{"load ok"} : loaded.error().message);
        expect(host.mods().size() == 1U);
        if (host.mods().size() == 1U) {
            expect(host.mods()[0].external);
            expect(host.mods()[0].has_code);
            expect(host.mods()[0].id == "refrain.test.external");
        }
        const auto active{
            host.services().active_provider("refrain:data.packs")};
        expect(active.has_value());
        if (active) {
            expect(active->id == "refrain.test.external");
        }
        fs::remove_all(root, error);
    };

    "mod_host/rejects an incompatible host_api"_test = [] {
        mod::static_mod_registry registry{};
        auto plan{make_gamepack("\"refrain.test.hostapi@1.0.0\"")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        image_builder builder{};
        auto manifest{make_manifest("refrain.test.hostapi")};
        manifest += "host_api = \"2.0.0\"\n";
        builder.add("refrain.test.hostapi", std::move(manifest));
        refrain::mod::host host{registry};
        auto loaded{host.load(*plan, builder.build())};
        expect(!loaded);
        if (!loaded) {
            expect(loaded.error().code == error_code::version_mismatch);
            expect(loaded.error().message.find("host_api") !=
                   std::string::npos);
        }
    };

    "mod_host/rejects an unknown host capability"_test = [] {
        mod::static_mod_registry registry{};
        auto plan{make_gamepack("\"refrain.test.hostapi@1.0.0\"")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        image_builder builder{};
        auto manifest{make_manifest("refrain.test.hostapi")};
        manifest += "host_capabilities = [\"nope\"]\n";
        builder.add("refrain.test.hostapi", std::move(manifest));
        refrain::mod::host host{registry};
        auto loaded{host.load(*plan, builder.build())};
        expect(!loaded);
        if (!loaded) {
            expect(loaded.error().code == error_code::unsupported);
            expect(loaded.error().message.find("capability") !=
                   std::string::npos);
        }
    };

    "mod_host/accepts a compatible host_api and capability"_test = [] {
        mod::static_mod_registry registry{};
        auto plan{make_gamepack("\"refrain.test.hostapi@1.0.0\"")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        image_builder builder{};
        auto manifest{make_manifest("refrain.test.hostapi")};
        manifest += "host_api = \"1.0.0\"\n";
        manifest += "host_capabilities = [\"log\", \"config\"]\n";
        builder.add("refrain.test.hostapi", std::move(manifest));
        refrain::mod::host host{registry};
        auto loaded{host.load(*plan, builder.build())};
        expect(static_cast<bool>(loaded))
            << (loaded ? std::string{"load ok"} : loaded.error().message);
        expect(host.loaded());
    };

    "mod_host/rejects a declared sealed service before the factory runs"_test =
        [] {
            mod::static_mod_registry registry{};
            g_factory_calls = 0;
            expect(static_cast<bool>(registry.add(
                mod::static_mod_descriptor{.id = "refrain.test.sealed",
                                           .version = "1.0.0",
                                           .abi = 1,
                                           .factory = &counting_factory})));
            auto plan{
                make_gamepack("\"refrain.test.sealed@1.0.0\"",
                              "sealed = \"refrain.test.sealed@^1.0.0\"\n")};
            expect(static_cast<bool>(plan));
            if (!plan) {
                return;
            }
            image_builder builder{};
            builder.add("refrain.test.sealed",
                        make_manifest("refrain.test.sealed", "1.0.0", 1,
                                      "server", "refrain:host.core"));
            refrain::mod::host host{registry};
            const mod::provider_api api{
                .vtable = g_fake_vtable, .self = &g_fake_self, .abi_major = 0};
            expect(static_cast<bool>(host.register_sealed_provider(
                "refrain:host.core", api, "1.0.0")));
            auto loaded{host.load(*plan, builder.build())};
            expect(!loaded)
                << (loaded ? std::string{"load ok"} : loaded.error().message);
            if (!loaded) {
                expect(loaded.error().code == error_code::permission_denied)
                    << loaded.error().message;
                expect(loaded.error().message.find("sealed service") !=
                       std::string::npos)
                    << loaded.error().message;
            }
            expect(g_factory_calls == 0);
        };
};

} // namespace
