// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// `refrain:mod.loader` dispatch: the host registers its `builtin` and `native`
// loader providers, routes every package through the active instance named by
// the manifest, records the declared unload/sandbox contract, and fails closed
// when the loader is missing, spoofed, sandboxed or cannot materialise the
// installed native entry from the content store.

#include <refrain/app/host_context.hpp>
#include <refrain/contracts/abi.hpp>
#include <refrain/contracts/services/mod_loader_abi.h>
#include <refrain/core/config.hpp>
#include <refrain/core/io.hpp>
#include <refrain/data/builtin_image.hpp>
#include <refrain/data/content_store.hpp>
#include <refrain/host/core_api.hpp>
#include <refrain/mod/discovery.hpp>
#include <refrain/mod/gamepack.hpp>
#include <refrain/mod/host.hpp>
#include <refrain/mod/manifest.hpp>
#include <refrain/mod/package.hpp>
#include <refrain/mod/rfmod.hpp>
#include <refrain/mod/static_mod.hpp>

#include "support/scratch_directory.hpp"

#include <boost/ut.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

namespace {

using namespace boost::ut;

namespace config = refrain::core::config;
namespace data = refrain::data;
namespace io = refrain::core::io;
namespace mod = refrain::mod;
using refrain::error_code;

namespace fs = std::filesystem;

int g_builtin_factory_calls{0};

auto counting_factory(refrain::mod::host &) -> refrain::status {
    ++g_builtin_factory_calls;
    return refrain::status_ok();
}

/// Minimal provider vtable with only the generated ABI header. It stands in for
/// a guest provider a sandboxed loader might try to register.
std::uint32_t g_guest_vtable[4]{16, 1, 0, 0};
int g_guest_self{0};

[[nodiscard]] auto guest_provider_api() -> mod::provider_api {
    return mod::provider_api{
        .vtable = g_guest_vtable, .self = &g_guest_self, .abi_major = 1};
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
    -> refrain::result<mod::composition> {
    auto text{std::string{"[gamepack]\n"}};
    text += "id = \"refrain.test.loader\"\n";
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

enum class fake_mode {
    normal,
    /// A guest asks the host to register a provider during `load`.
    register_provider,
};

/// State behind a fake `refrain:mod.loader` provider. The test chooses the
/// capability values and the mode before registering it.
struct fake_loader_state {
    refrain::mod::host *owner{nullptr};
    std::string loader_id{"test.fake"};
    std::uint32_t abi_major{1};
    std::uint8_t unload{REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_UNLOAD_RETIRE};
    std::uint8_t sandboxed{0};
    std::uint8_t hot_install{0};
    std::uint32_t struct_size{
        REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_CAPABILITIES_SIZE};
    fake_mode mode{fake_mode::normal};
    int capabilities_calls{0};
    int validate_calls{0};
    int load_calls{0};
    refrain::error inner_error{};
};

void fake_capabilities(
    void *self, refrain_services_mod_loader_mod_loader_capabilities *out) {
    auto &state{*static_cast<fake_loader_state *>(self)};
    ++state.capabilities_calls;
    out->struct_size = state.struct_size;
    out->loader_id = refrain::contracts::to_borrowed(state.loader_id);
    out->abi_major = state.abi_major;
    out->unload = state.unload;
    out->sandboxed = state.sandboxed;
    out->supports_hot_install = state.hot_install;
}

auto fake_validate(void *self,
                   const refrain_services_mod_loader_mod_entry_manifest *)
    -> refrain_status {
    auto &state{*static_cast<fake_loader_state *>(self)};
    ++state.validate_calls;
    return REFRAIN_STATUS_OK;
}

auto fake_load(void *self,
               const refrain_services_mod_loader_mod_load_request *request,
               refrain_services_mod_loader_handle_mod_instance *out)
    -> refrain_status {
    auto &state{*static_cast<fake_loader_state *>(self)};
    ++state.load_calls;
    *out = REFRAIN_HANDLE_INVALID;
    expect(request != nullptr);
    if (request != nullptr) {
        expect(refrain::contracts::as_string_view(request->manifest.loader) ==
               std::string_view{state.loader_id});
    }
    if (state.mode == fake_mode::register_provider) {
        // A sandboxed guest can only reach the host through its imports; a
        // direct provider registration must be refused before it is recorded.
        auto registered{state.owner->register_provider("refrain:data.packs",
                                                       guest_provider_api())};
        if (!registered) {
            state.inner_error = registered.error();
            return static_cast<refrain_status>(
                REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_ERROR_PERMISSION_DENIED);
        }
    }
    *out = 0x5150U;
    return REFRAIN_STATUS_OK;
}

auto fake_unload(void *, refrain_services_mod_loader_handle_mod_instance,
                 refrain_services_mod_loader_mod_unload_reason)
    -> refrain_status {
    return REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_ERROR_UNSUPPORTED;
}

auto fake_reload(void *, refrain_services_mod_loader_handle_mod_instance,
                 const refrain_services_mod_loader_mod_load_request *,
                 refrain_services_mod_loader_handle_mod_instance *)
    -> refrain_status {
    return REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_ERROR_UNSUPPORTED;
}

[[nodiscard]] auto fake_loader_vtable()
    -> const refrain_services_mod_loader_vtable & {
    static const refrain_services_mod_loader_vtable table{
        .struct_size = REFRAIN_SERVICES_MOD_LOADER_VTABLE_SIZE,
        .abi_major = REFRAIN_SERVICES_MOD_LOADER_ABI_MAJOR,
        .abi_minor = REFRAIN_SERVICES_MOD_LOADER_ABI_MINOR,
        .abi_patch = REFRAIN_SERVICES_MOD_LOADER_ABI_PATCH,
        .capabilities = &fake_capabilities,
        .validate = &fake_validate,
        .load = &fake_load,
        .unload = &fake_unload,
        .reload = &fake_reload,
    };
    return table;
}

[[nodiscard]] auto fake_provider_api_for(fake_loader_state &state)
    -> mod::provider_api {
    return mod::provider_api{
        .vtable = &fake_loader_vtable(),
        .self = &state,
        .abi_major = REFRAIN_SERVICES_MOD_LOADER_ABI_MAJOR,
    };
}

// ---------------------------------------------------------------------------
// installed package helpers (the receipt/CAS runtime source)
// ---------------------------------------------------------------------------

[[nodiscard]] auto write_text(const fs::path &path, std::string_view text)
    -> bool {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) {
        return false;
    }
    return io::write_text(path.string(), text).has_value();
}

/// Pack a mod directory into an `.rfmod`; `native_library` copies the external
/// test module beside the manifest as `lib/<name>`.
[[nodiscard]] auto pack_mod(refrain::test_support::scratch_directory &scratch,
                            std::string_view directory,
                            std::string_view manifest,
                            std::string_view native_library = {})
    -> std::optional<std::string> {
    const auto mod_dir{scratch.path() / std::string{directory}};
    if (!write_text(mod_dir / "manifest.toml", manifest)) {
        return std::nullopt;
    }
    if (!native_library.empty()) {
        std::error_code error;
        fs::create_directories(mod_dir / "lib", error);
        if (error) {
            return std::nullopt;
        }
        fs::copy_file(fs::path{REFRAIN_TEST_EXTERNAL_MOD},
                      mod_dir / "lib" / std::string{native_library},
                      fs::copy_options::overwrite_existing, error);
        if (error) {
            return std::nullopt;
        }
    }
    const auto archive{scratch.path() / (std::string{directory} + ".rfmod")};
    auto packed{mod::pack_mod_directory(mod_dir.string(), archive.string())};
    expect(static_cast<bool>(packed)) << "pack_mod_directory failed";
    if (!packed) {
        return std::nullopt;
    }
    return archive.string();
}

/// Install one archive into a fresh content store and return the store root.
[[nodiscard]] auto
install_mod(refrain::test_support::scratch_directory &scratch,
            const std::string &archive) -> std::optional<std::string> {
    const auto store_root{scratch.path() / "store"};
    auto store{data::content_store::open(store_root.string())};
    expect(static_cast<bool>(store)) << "content_store::open failed";
    if (!store) {
        return std::nullopt;
    }
    auto reader{mod::rfmod_reader::open_file(archive)};
    expect(static_cast<bool>(reader)) << "rfmod_reader::open_file failed";
    if (!reader) {
        return std::nullopt;
    }
    auto installed{mod::install_rfmod(*reader, *store)};
    expect(static_cast<bool>(installed)) << "install_rfmod failed";
    if (!installed) {
        return std::nullopt;
    }
    return store_root.string();
}

[[nodiscard]] auto installed_spec(std::string_view mod_id)
    -> refrain::result<mod::gamepack_spec> {
    auto text{std::string{"[gamepack]\n"}};
    text += "id = \"refrain.test.loader.installed\"\n";
    text += "version = \"1.0.0\"\n";
    text += "side = \"server\"\n";
    text += "abi = \"1\"\n";
    text += "auto_scan = false\n";
    text += "\n[mods]\nlist = [\"" + std::string{mod_id} + "@1.0.0\"]\n";
    text += "\n[providers]\ndata = \"" + std::string{mod_id} + "@^1.0.0\"\n";
    auto doc{config::document::parse_text(text, "<gamepack>")};
    if (!doc) {
        return std::unexpected{doc.error()};
    }
    return mod::parse_gamepack(*doc, "server");
}

[[nodiscard]] auto register_host_core(refrain::mod::host &host,
                                      refrain::app::host_context &context)
    -> bool {
    const mod::provider_api host_api{
        .vtable = &refrain::host::core_vtable(),
        .self = &context,
        .abi_major = REFRAIN_HOST_CORE_ABI_MAJOR,
    };
    return host.register_sealed_provider("refrain:host.core", host_api, "1.0.0")
        .has_value();
}

const suite mod_loader_tests = [] {
    "mod_loader/host bootstrap registers both loaders"_test = [] {
        mod::static_mod_registry registry{};
        auto plan{make_gamepack("\"refrain.test.loader.content@1.0.0\"")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        image_builder builder{};
        builder.add("refrain.test.loader.content",
                    make_manifest("refrain.test.loader.content"));
        mod::host host{registry};
        auto loaded{host.load(*plan, builder.build())};
        expect(static_cast<bool>(loaded))
            << (loaded ? std::string{"load ok"} : loaded.error().message);
        if (!loaded) {
            return;
        }
        expect(host.loaders().size() == 2U);
        const auto *builtin{host.find_loader(mod::builtin_loader_id)};
        const auto *native{host.find_loader(mod::native_loader_id)};
        expect(builtin != nullptr);
        expect(native != nullptr);
        if (builtin != nullptr) {
            expect(builtin->unload ==
                   mod::mod_unload_semantics::restart_required);
            expect(!builtin->sandboxed);
            expect(builtin->abi_major == 1U);
        }
        if (native != nullptr) {
            expect(native->unload == mod::mod_unload_semantics::retire);
            expect(!native->sandboxed);
        }
        expect(host.services()
                   .active_provider("refrain:mod.loader", "builtin")
                   .has_value());
        expect(host.services()
                   .active_provider("refrain:mod.loader", "native")
                   .has_value());
        const auto formatted{host.format()};
        expect(formatted.find("loaders:") != std::string::npos);
        expect(formatted.find("builtin") != std::string::npos);
        expect(formatted.find("native") != std::string::npos);
        expect(formatted.find("loader builtin") != std::string::npos);
    };

    "mod_loader/routes builtin mods through the builtin loader"_test = [] {
        mod::static_mod_registry registry{};
        g_builtin_factory_calls = 0;
        expect(static_cast<bool>(registry.add(
            mod::static_mod_descriptor{.id = "refrain.test.loader.provider",
                                       .version = "1.0.0",
                                       .abi = 1,
                                       .factory = &counting_factory})));
        auto plan{make_gamepack("\"refrain.test.loader.content@1.0.0\", "
                                "\"refrain.test.loader.provider@1.0.0\"")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        image_builder builder{};
        builder.add("refrain.test.loader.content",
                    make_manifest("refrain.test.loader.content"));
        builder.add("refrain.test.loader.provider",
                    make_manifest("refrain.test.loader.provider"));
        mod::host host{registry};
        auto loaded{host.load(*plan, builder.build())};
        expect(static_cast<bool>(loaded))
            << (loaded ? std::string{"load ok"} : loaded.error().message);
        if (!loaded) {
            return;
        }
        expect(g_builtin_factory_calls == 1);
        expect(host.mods().size() == 2U);
        if (host.mods().size() != 2U) {
            return;
        }
        for (const auto &loaded_mod : host.mods()) {
            expect(loaded_mod.loader == "builtin");
            expect(loaded_mod.loader_unload ==
                   mod::mod_unload_semantics::restart_required);
            expect(!loaded_mod.loader_sandboxed);
        }
        expect(!host.mods()[0].has_code);
        expect(host.mods()[1].has_code);
        expect(host.mods()[1].instance_handle != 0U);
    };

    "mod_loader/rejects a loader id with no active provider"_test = [] {
        mod::static_mod_registry registry{};
        auto plan{make_gamepack("\"refrain.test.loader.unknown@1.0.0\"")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        auto manifest{make_manifest("refrain.test.loader.unknown")};
        manifest += "\n[entry]\nloader = \"com.example.missing\"\n";
        image_builder builder{};
        builder.add("refrain.test.loader.unknown", std::move(manifest));
        mod::host host{registry};
        auto loaded{host.load(*plan, builder.build())};
        expect(!loaded);
        if (!loaded) {
            expect(loaded.error().code == error_code::not_found);
            expect(loaded.error().message.find("com.example.missing") !=
                   std::string::npos);
            // Fail closed names the active loaders so the operator sees what
            // the host actually has.
            expect(loaded.error().message.find("builtin") != std::string::npos);
            expect(loaded.error().message.find("native") != std::string::npos);
        }
        expect(!host.loaded());
    };

    "mod_loader/rejects a first-party loader that is not built"_test = [] {
        mod::static_mod_registry registry{};
        auto plan{make_gamepack("\"refrain.test.loader.wasm@1.0.0\"")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        auto manifest{make_manifest("refrain.test.loader.wasm")};
        manifest += "\n[entry]\nloader = \"wasm\"\nwasm = \"script.wasm\"\n";
        image_builder builder{};
        builder.add("refrain.test.loader.wasm", std::move(manifest));
        mod::host host{registry};
        auto loaded{host.load(*plan, builder.build())};
        expect(!loaded);
        if (!loaded) {
            expect(loaded.error().code == error_code::unsupported);
            expect(loaded.error().message.find("wasm") != std::string::npos);
            expect(loaded.error().message.find("active mod loaders") !=
                   std::string::npos);
        }
    };

    "mod_loader/rejects a spoofed entry shape before any loader runs"_test =
        [] {
            mod::static_mod_registry registry{};
            auto plan{make_gamepack("\"refrain.test.loader.spoof@1.0.0\"")};
            expect(static_cast<bool>(plan));
            if (!plan) {
                return;
            }
            auto manifest{make_manifest("refrain.test.loader.spoof")};
            manifest +=
                "\n[entry]\nloader = \"native\"\nwasm = \"script.wasm\"\n";
            image_builder builder{};
            builder.add("refrain.test.loader.spoof", std::move(manifest));
            mod::host host{registry};
            auto loaded{host.load(*plan, builder.build())};
            expect(!loaded);
            if (!loaded) {
                expect(loaded.error().code == error_code::invalid_argument);
                expect(loaded.error().message.find("native loader") !=
                       std::string::npos);
            }
            expect(!host.loaded());
        };

    "mod_loader/parses an explicit loader declaration"_test = [] {
        const auto parse{[&](std::string_view entry) {
            auto doc{config::document::parse_text(
                fmt::format("[mod]\nid = \"refrain.test.entry\"\n"
                            "version = \"1.0.0\"\n"
                            "abi = 1\n\n[entry]\n{}",
                            entry),
                "<manifest>")};
            expect(static_cast<bool>(doc));
            if (!doc) {
                return refrain::result<mod::mod_manifest>{
                    std::unexpected{doc.error()}};
            }
            return mod::parse_mod_manifest(*doc);
        }};

        // An entry table without `[entry].loader` is not a manifest form any
        // more: the load declares its execution environment or it is refused,
        // and the diagnostic names the field.
        auto missing_loader{parse("native = \"lib/libold.so\"\n")};
        expect(!missing_loader);
        if (!missing_loader) {
            expect(missing_loader.error().code == error_code::parse_error);
            expect(missing_loader.error().message.find("entry.loader") !=
                   std::string::npos);
        }
        auto explicit_native{
            parse("loader = \"native\"\nnative = \"lib/libnew.so\"\n")};
        expect(static_cast<bool>(explicit_native));
        if (explicit_native) {
            expect(explicit_native->loader == "native");
            expect(!explicit_native->loader_inferred);
            expect(explicit_native->entry->loader == "native");
        }
        auto internal{config::document::parse_text(
            "[mod]\nid = \"refrain.test.internal\"\n"
            "version = \"1.0.0\"\nabi = 1\n",
            "<manifest>")};
        expect(static_cast<bool>(internal));
        if (internal) {
            auto manifest{mod::parse_mod_manifest(*internal)};
            expect(static_cast<bool>(manifest));
            if (manifest) {
                expect(manifest->loader == "builtin");
                expect(!manifest->loader_inferred);
                expect(!manifest->is_external());
            }
        }
    };

    "mod_loader/rejects invalid loader declarations"_test = [] {
        const auto expect_entry_error{
            [](std::string_view entry, error_code code) {
                auto doc{config::document::parse_text(
                    fmt::format("[mod]\nid = \"refrain.test.bad\"\n"
                                "version = \"1.0.0\"\n"
                                "abi = 1\n\n[entry]\n{}",
                                entry),
                    "<manifest>")};
                expect(static_cast<bool>(doc));
                if (!doc) {
                    return;
                }
                auto manifest{mod::parse_mod_manifest(*doc)};
                expect(!manifest);
                if (!manifest) {
                    expect(manifest.error().code == code);
                }
            }};

        // `builtin` is implicit for a package without an entry table.
        expect_entry_error("loader = \"builtin\"\n",
                           error_code::invalid_argument);
        // An unqualified id is reserved for a first-party loader.
        expect_entry_error("loader = \"example\"\n",
                           error_code::invalid_argument);
        // The native loader owns the shared library shape.
        expect_entry_error("loader = \"native\"\n",
                           error_code::invalid_argument);
        // The wasm loader owns the module shape.
        expect_entry_error("loader = \"wasm\"\n", error_code::invalid_argument);
        // A third-party loader may define its own payload keys.
        auto third_party{config::document::parse_text(
            "[mod]\nid = \"refrain.test.third\"\nversion = \"1.0.0\"\n"
            "abi = 1\n\n[entry]\nloader = \"com.example.loader\"\n"
            "archive = \"payload.bin\"\n",
            "<manifest>")};
        expect(static_cast<bool>(third_party));
        if (third_party) {
            auto manifest{mod::parse_mod_manifest(*third_party)};
            expect(static_cast<bool>(manifest));
            if (manifest) {
                expect(manifest->loader == "com.example.loader");
                expect(!manifest->loader_inferred);
            }
        }
        // A known first-party loader keeps the shared entry keys strict.
        auto unknown_key{config::document::parse_text(
            "[mod]\nid = \"refrain.test.strict\"\nversion = \"1.0.0\"\n"
            "abi = 1\n\n[entry]\nloader = \"native\"\n"
            "native = \"lib/lib.so\"\nnatve = \"typo\"\n",
            "<manifest>")};
        expect(static_cast<bool>(unknown_key));
        if (unknown_key) {
            expect(!mod::parse_mod_manifest(*unknown_key));
        }
    };

    "mod_loader/validates loader capabilities at registration"_test = [] {
        mod::static_mod_registry registry{};
        mod::host host{registry};

        // The provider's capabilities must name the loader id it is active as.
        fake_loader_state wrong_id{};
        wrong_id.loader_id = "test.other";
        auto wrong_id_status{host.register_mod_loader(
            "test.requested", fake_provider_api_for(wrong_id))};
        expect(!wrong_id_status);
        if (!wrong_id_status) {
            expect(wrong_id_status.error().code ==
                   error_code::version_mismatch);
            expect(wrong_id_status.error().message.find("reports loader id") !=
                   std::string::npos);
        }

        // The loader ABI major must match the contract the host implements.
        fake_loader_state bad_abi{};
        bad_abi.loader_id = "test.abi";
        bad_abi.abi_major = 2;
        auto bad_abi_status{host.register_mod_loader(
            "test.abi", fake_provider_api_for(bad_abi))};
        expect(!bad_abi_status);
        if (!bad_abi_status) {
            expect(bad_abi_status.error().code == error_code::version_mismatch);
            expect(bad_abi_status.error().message.find("loader ABI 2") !=
                   std::string::npos);
        }

        // Unknown unload semantics are a contract violation, not a default.
        fake_loader_state bad_unload{};
        bad_unload.loader_id = "test.unload";
        bad_unload.unload = 7;
        auto bad_unload_status{host.register_mod_loader(
            "test.unload", fake_provider_api_for(bad_unload))};
        expect(!bad_unload_status);
        if (!bad_unload_status) {
            expect(bad_unload_status.error().code ==
                   error_code::version_mismatch);
            expect(bad_unload_status.error().message.find("unload semantics") !=
                   std::string::npos);
        }

        // An undersized capability struct cannot be trusted.
        fake_loader_state short_caps{};
        short_caps.loader_id = "test.short";
        short_caps.struct_size = 8;
        auto short_status{host.register_mod_loader(
            "test.short", fake_provider_api_for(short_caps))};
        expect(!short_status);
        if (!short_status) {
            expect(short_status.error().code == error_code::invalid_argument);
            expect(short_status.error().message.find("struct_size") !=
                   std::string::npos);
        }

        // Invalid or unqualified ids never reach a provider.
        expect(!host.register_mod_loader("Bad_ID",
                                         fake_provider_api_for(short_caps))
                    .has_value());
        fake_loader_state unqualified{};
        unqualified.loader_id = "example";
        auto unqualified_status{host.register_mod_loader(
            "example", fake_provider_api_for(unqualified))};
        expect(!unqualified_status);
        if (!unqualified_status) {
            expect(unqualified_status.error().code ==
                   error_code::invalid_argument);
            expect(unqualified_status.error().message.find("namespaced") !=
                   std::string::npos);
        }
    };

    "mod_loader/registers a namespaced third-party loader once"_test = [] {
        mod::static_mod_registry registry{};
        mod::host host{registry};
        fake_loader_state state{};
        state.owner = &host;
        state.loader_id = "com.example.loader";
        expect(static_cast<bool>(host.register_mod_loader(
            state.loader_id, fake_provider_api_for(state))));
        const auto *record{host.find_loader("com.example.loader")};
        expect(record != nullptr);
        if (record != nullptr) {
            expect(record->unload == mod::mod_unload_semantics::retire);
            expect(record->version == "1.0.0");
        }
        // The instance name is the loader id, so a second registration is a
        // duplicate instead of a silent replacement.
        fake_loader_state second{};
        second.loader_id = "com.example.loader";
        auto duplicate{host.register_mod_loader("com.example.loader",
                                                fake_provider_api_for(second))};
        expect(!duplicate);
        if (!duplicate) {
            expect(duplicate.error().code == error_code::already_exists);
        }
    };

    "mod_loader/rejects a loader whose capabilities drift after registration"_test =
        [] {
            mod::static_mod_registry registry{};
            auto plan{make_gamepack("\"refrain.test.loader.drift@1.0.0\"")};
            expect(static_cast<bool>(plan));
            if (!plan) {
                return;
            }
            auto manifest{make_manifest("refrain.test.loader.drift")};
            manifest += "\n[entry]\nloader = \"test.drift\"\n";
            image_builder builder{};
            builder.add("refrain.test.loader.drift", std::move(manifest));
            const auto image{builder.build()};

            mod::host host{registry};
            fake_loader_state state{};
            state.owner = &host;
            state.loader_id = "test.drift";
            expect(static_cast<bool>(host.register_mod_loader(
                "test.drift", fake_provider_api_for(state))));

            // A loader that renames itself after registration cannot be trusted
            // to execute a package.
            state.loader_id = "test.other";
            auto loaded{host.load(*plan, image)};
            expect(!loaded);
            if (!loaded) {
                expect(loaded.error().code == error_code::version_mismatch);
                expect(loaded.error().message.find("test.other") !=
                       std::string::npos);
            }
            expect(state.validate_calls == 0);
            expect(state.load_calls == 0);
        };

    "mod_loader/sandboxed loader cannot register a provider"_test = [] {
        mod::static_mod_registry registry{};
        auto plan{
            make_gamepack("\"refrain.test.loader.sandbox@1.0.0\"",
                          "data = \"refrain.test.loader.sandbox@^1.0.0\"\n")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        auto manifest{make_manifest("refrain.test.loader.sandbox", "1.0.0", 1,
                                    "server", "refrain:data.packs")};
        manifest += "\n[entry]\nloader = \"test.sandbox\"\n";
        image_builder builder{};
        builder.add("refrain.test.loader.sandbox", std::move(manifest));
        const auto image{builder.build()};

        mod::host host{registry};
        fake_loader_state state{};
        state.owner = &host;
        state.loader_id = "test.sandbox";
        state.sandboxed = 1;
        state.mode = fake_mode::register_provider;
        expect(static_cast<bool>(host.register_mod_loader(
            "test.sandbox", fake_provider_api_for(state))));

        auto loaded{host.load(*plan, image)};
        expect(!loaded);
        if (!loaded) {
            expect(loaded.error().code == error_code::permission_denied);
            expect(loaded.error().message.find("sandboxed") !=
                   std::string::npos);
            expect(loaded.error().message.find("test.sandbox") !=
                   std::string::npos);
        }
        expect(state.inner_error.code == error_code::permission_denied);
        expect(state.load_calls == 1);
        // The failed load must not leave the fake loader's provider behind.
        expect(
            !host.services().active_provider("refrain:data.packs").has_value());
    };

    "mod_loader/records unload semantics on the loaded mods"_test = [] {
        mod::static_mod_registry registry{};
        auto plan{make_gamepack("\"refrain.test.loader.semantics@1.0.0\"")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        auto manifest{make_manifest("refrain.test.loader.semantics")};
        manifest += "\n[entry]\nloader = \"test.semantics\"\n";
        image_builder builder{};
        builder.add("refrain.test.loader.semantics", std::move(manifest));
        const auto image{builder.build()};

        mod::host host{registry};
        fake_loader_state state{};
        state.owner = &host;
        state.loader_id = "test.semantics";
        state.unload = REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_UNLOAD_CLEAN;
        state.hot_install = 1;
        expect(static_cast<bool>(host.register_mod_loader(
            "test.semantics", fake_provider_api_for(state))));

        auto loaded{host.load(*plan, image)};
        expect(static_cast<bool>(loaded))
            << (loaded ? std::string{"load ok"} : loaded.error().message);
        if (!loaded) {
            return;
        }
        expect(host.mods().size() == 1U);
        if (host.mods().size() != 1U) {
            return;
        }
        const auto &loaded_mod{host.mods().front()};
        expect(loaded_mod.loader == "test.semantics");
        expect(loaded_mod.loader_unload == mod::mod_unload_semantics::clean);
        const auto *record{host.find_loader("test.semantics")};
        expect(record != nullptr);
        if (record != nullptr) {
            expect(record->supports_hot_install);
        }
        expect(loaded_mod.instance_handle == 0x5150U);
    };
};

const suite mod_loader_installed_tests = [] {
    "mod_loader/loads a native mod from an installed receipt"_test = [] {
        refrain::test_support::scratch_directory scratch;
        constexpr std::string_view manifest{
            "[mod]\n"
            "id = \"refrain.test.loader.installed\"\n"
            "version = \"1.0.0\"\n"
            "abi = \"1\"\n"
            "side = \"server\"\n"
            "\n[provides]\n"
            "services = [\"refrain:data.packs\"]\n"
            "\n[entry]\n"
            "loader = \"native\"\n"
            "native = \"lib/libexternal.so\"\n"};
        auto archive{pack_mod(scratch, "mod", manifest, "libexternal.so")};
        expect(archive.has_value());
        if (!archive) {
            return;
        }
        auto store_root{install_mod(scratch, *archive)};
        expect(store_root.has_value());
        if (!store_root) {
            return;
        }
        auto spec{installed_spec("refrain.test.loader.installed")};
        expect(static_cast<bool>(spec));
        if (!spec) {
            return;
        }
        const std::vector<std::string> roots{*store_root};
        auto plan{mod::resolve_installed_composition(
            *spec, roots,
            mod::composition_load_options{.host_side = "server"})};
        expect(static_cast<bool>(plan))
            << (plan ? std::string{"resolved"} : plan.error().message);
        if (!plan) {
            return;
        }
        expect(plan->load_order.size() == 1U);
        if (plan->load_order.size() == 1U) {
            expect(plan->load_order.front().kind == mod::package_kind::rfmod);
            expect(plan->load_order.front().is_external());
            expect(plan->load_order.front().entry.has_value());
            if (plan->load_order.front().entry) {
                expect(plan->load_order.front().entry->loader == "native");
            }
        }

        mod::static_mod_registry registry{};
        mod::host host{registry};
        refrain::app::host_context context{host};
        expect(register_host_core(host, context));
        auto loaded{host.load(*plan, std::span<const std::byte>{})};
        expect(static_cast<bool>(loaded))
            << (loaded ? std::string{"load ok"} : loaded.error().message);
        if (!loaded) {
            return;
        }
        expect(host.mods().size() == 1U);
        if (host.mods().size() != 1U) {
            return;
        }
        const auto &loaded_mod{host.mods().front()};
        expect(loaded_mod.external);
        expect(loaded_mod.has_code);
        expect(loaded_mod.loader == "native");
        expect(loaded_mod.loader_unload == mod::mod_unload_semantics::retire);
        expect(loaded_mod.instance_handle != 0U);
        const auto provider{
            host.services().active_provider("refrain:data.packs")};
        expect(provider.has_value());
        if (provider) {
            expect(provider->id == loaded_mod.id);
        }
        // The library was materialised outside the store; the host removes it
        // with its scratch directory when it goes away.
        const auto formatted{host.format()};
        expect(formatted.find("loader native") != std::string::npos);
        expect(formatted.find("unload retire") != std::string::npos);
    };

    "mod_loader/fails closed when the installed native entry is missing"_test =
        [] {
            refrain::test_support::scratch_directory scratch;
            constexpr std::string_view manifest{
                "[mod]\n"
                "id = \"refrain.test.loader.damaged\"\n"
                "version = \"1.0.0\"\n"
                "abi = \"1\"\n"
                "side = \"server\"\n"
                "\n[provides]\n"
                "services = [\"refrain:data.packs\"]\n"
                "\n[entry]\n"
                "loader = \"native\"\n"
                "native = \"lib/missing.so\"\n"};
            // Pack without copying the library: the manifest declares an entry
            // the archive does not contain.
            auto archive{pack_mod(scratch, "mod", manifest)};
            expect(archive.has_value());
            if (!archive) {
                return;
            }
            auto store_root{install_mod(scratch, *archive)};
            expect(store_root.has_value());
            if (!store_root) {
                return;
            }
            auto spec{installed_spec("refrain.test.loader.damaged")};
            expect(static_cast<bool>(spec));
            if (!spec) {
                return;
            }
            const std::vector<std::string> roots{*store_root};
            auto plan{mod::resolve_installed_composition(
                *spec, roots,
                mod::composition_load_options{.host_side = "server"})};
            expect(static_cast<bool>(plan))
                << (plan ? std::string{"resolved"} : plan.error().message);
            if (!plan) {
                return;
            }

            mod::static_mod_registry registry{};
            mod::host host{registry};
            refrain::app::host_context context{host};
            expect(register_host_core(host, context));
            auto loaded{host.load(*plan, std::span<const std::byte>{})};
            expect(!loaded);
            if (!loaded) {
                expect(loaded.error().code == error_code::not_found);
                // The refusal names the loader that failed, the mod and the
                // member it could not materialise.
                expect(loaded.error().message.find("native") !=
                       std::string::npos);
                expect(loaded.error().message.find(
                           "refrain.test.loader.damaged") != std::string::npos);
                expect(loaded.error().message.find("missing.so") !=
                       std::string::npos);
            }
            expect(!host.loaded());
        };
};

} // namespace
