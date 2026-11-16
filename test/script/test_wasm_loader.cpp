// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// `refrain:mod.loader` script loader providers: the wasm loader executes a
// sandboxed guest out of an installed package with module/memory/instruction
// budgets and clean unload, and the lua loader fails closed while no Lua
// runtime is linked. The cases cover the advertised capabilities, manifest
// entry validation, missing/corrupt/oversized packages, the sandbox refusal of
// host imports, the load/unload lifecycle with budget accounting, hot reload
// and the lua refusal.

#include <refrain/app/composition.hpp>
#include <refrain/contracts/services/mod_loader.hpp>
#include <refrain/core/config.hpp>
#include <refrain/core/error.hpp>
#include <refrain/core/io.hpp>
#include <refrain/data/builtin_image.hpp>
#include <refrain/data/content_store.hpp>
#include <refrain/mod/gamepack.hpp>
#include <refrain/mod/host.hpp>
#include <refrain/mod/manifest.hpp>
#include <refrain/mod/package.hpp>
#include <refrain/mod/rfmod.hpp>
#include <refrain/script/loaders.hpp>
#include <refrain/script/wasm.hpp>

#include "support/scratch_directory.hpp"

#include <boost/ut.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fmt/format.h>

namespace {

using namespace boost::ut;

namespace config = refrain::core::config;
namespace data = refrain::data;
namespace io = refrain::core::io;
namespace mod = refrain::mod;
namespace script = refrain::script;
namespace wasm = refrain::script::wasm;
using refrain::error_code;
namespace loader_abi = refrain::services::mod_loader;

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// shared helpers
// ---------------------------------------------------------------------------

#if REFRAIN_WITH_WASM

[[nodiscard]] auto write_text(const fs::path &path, std::string_view text)
    -> bool {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) {
        return false;
    }
    return io::write_text(path.string(), text).has_value();
}

[[nodiscard]] auto write_bytes(const fs::path &path,
                               std::span<const std::byte> bytes) -> bool {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) {
        return false;
    }
    return io::write_file(path.string(), bytes).has_value();
}

#endif // REFRAIN_WITH_WASM

[[nodiscard]] auto make_wasm_manifest(std::string_view id,
                                      std::string_view module,
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
    text += "\n[entry]\nloader = \"wasm\"\n";
    text += "wasm = \"" + std::string{module} + "\"\n";
    return text;
}

#if REFRAIN_WITH_WASM

/// Pack one wasm mod directory. `module`/`bytes` are the member the manifest
/// declares; an empty `bytes` packs the manifest without the member (the
/// missing-entry fail-closed case).
[[nodiscard]] auto
pack_wasm_mod(refrain::test_support::scratch_directory &scratch,
              std::string_view directory, std::string_view manifest,
              std::string_view module, std::span<const std::byte> bytes)
    -> std::optional<std::string> {
    const auto mod_dir{scratch.path() / std::string{directory}};
    if (!write_text(mod_dir / "manifest.toml", manifest)) {
        return std::nullopt;
    }
    if (!bytes.empty() && !write_bytes(mod_dir / std::string{module}, bytes)) {
        return std::nullopt;
    }
    const auto archive{scratch.path() / (std::string{directory} + ".rfmod")};
    auto packed{mod::pack_mod_directory(mod_dir.string(), archive.string())};
    expect(static_cast<bool>(packed)) << "pack_mod_directory failed";
    if (!packed) {
        return std::nullopt;
    }
    return archive.string();
}

[[nodiscard]] auto
install_package(refrain::test_support::scratch_directory &scratch,
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

/// Resolve a one-mod gamepack against the installed store; no provider slots.
[[nodiscard]] auto installed_plan(std::string_view mod_id,
                                  std::string_view store_root)
    -> refrain::result<mod::composition> {
    auto text{std::string{"[gamepack]\n"}};
    text += "id = \"refrain.test.script.loader\"\n";
    text += "version = \"1.0.0\"\n";
    text += "side = \"server\"\n";
    text += "abi = \"1\"\n";
    text += "auto_scan = false\n";
    text += "\n[mods]\nlist = [\"" + std::string{mod_id} + "@1.0.0\"]\n";
    auto doc{config::document::parse_text(text, "<gamepack>")};
    if (!doc) {
        return std::unexpected{doc.error()};
    }
    auto spec{mod::parse_gamepack(*doc, "server")};
    if (!spec) {
        return std::unexpected{spec.error()};
    }
    const std::vector<std::string> roots{std::string{store_root}};
    return mod::resolve_installed_composition(
        *spec, roots, mod::composition_load_options{.host_side = "server"});
}

#endif // REFRAIN_WITH_WASM

[[nodiscard]] auto register_wasm_loader(mod::host &host, wasm::loader &loader)
    -> bool {
    const auto provider{loader.provider()};
    return host
        .register_mod_loader(mod::wasm_loader_id,
                             mod::provider_api{.vtable = provider.vtable,
                                               .self = provider.self,
                                               .abi_major = provider.abi_major})
        .has_value();
}

[[nodiscard]] auto register_lua_loader(mod::host &host) -> bool {
    const auto provider{script::lua::default_mod_loader_provider()};
    return host
        .register_mod_loader(mod::lua_loader_id,
                             mod::provider_api{.vtable = provider.vtable,
                                               .self = provider.self,
                                               .abi_major = provider.abi_major})
        .has_value();
}

[[nodiscard]] auto make_gamepack(std::string_view mods_list)
    -> refrain::result<mod::composition> {
    auto text{std::string{"[gamepack]\n"}};
    text += "id = \"refrain.test.script.loader\"\n";
    text += "version = \"1.0.0\"\n";
    text += "side = \"server\"\n";
    text += "abi = \"1\"\n";
    text += "auto_scan = false\n\n[mods]\nlist = [" + std::string{mods_list} +
            "]\n";
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

/// Manifest entry the loader contract receives, with the strings kept alive by
/// the test object.
struct entry_fields {
    std::string mod_id{"refrain.test.script.entry"};
    std::string version{"1.0.0"};
    std::string loader_id{"wasm"};
    std::string package_root{};
    std::string native_path{};
    std::string wasm_path{"scripts/mod.wasm"};
    std::string symbol{};
    std::uint32_t abi{1};
    std::uint8_t activate_code{1};

    [[nodiscard]] auto manifest() const -> loader_abi::mod_entry_manifest {
        auto entry{loader_abi::make_mod_entry_manifest()};
        entry.mod_id = refrain::contracts::to_borrowed(mod_id);
        entry.version = refrain::contracts::to_borrowed(version);
        entry.abi = abi;
        entry.side = REFRAIN_SIDE_SERVER;
        entry.loader = refrain::contracts::to_borrowed(loader_id);
        entry.package_root = refrain::contracts::to_borrowed(package_root);
        entry.native_path = refrain::contracts::to_borrowed(native_path);
        entry.wasm_path = refrain::contracts::to_borrowed(wasm_path);
        entry.symbol = refrain::contracts::to_borrowed(symbol);
        entry.activate_code = activate_code;
        return entry;
    }

    [[nodiscard]] auto request() const -> loader_abi::mod_load_request {
        auto request{loader_abi::make_mod_load_request()};
        request.manifest = manifest();
        return request;
    }
};

#if REFRAIN_WITH_WASM

// --- guest fixtures (WAT, assembled at test time) -------------------------
//
// The guest ABI of this slice has no host imports: a module exports
// `on_load: () -> i32` (required) and may export `on_unload: () -> i32`. A zero
// return reports success.

constexpr std::string_view minimal_wat{R"((module
  (memory (export "memory") 1)
  (func (export "on_load") (result i32) i32.const 0)
  (func (export "on_unload") (result i32) i32.const 0)))"};

constexpr std::string_view trap_wat{R"((module
  (memory (export "memory") 1)
  (func $start unreachable)
  (start $start)
  (func (export "on_load") (result i32) i32.const 0)))"};

constexpr std::string_view spin_wat{R"((module
  (memory (export "memory") 1)
  (func (export "on_load") (result i32)
    (local $i i32)
    (loop $spin
      local.get $i
      i32.const 1
      i32.add
      local.set $i
      br $spin)
    i32.const 0)))"};

constexpr std::string_view oversized_memory_wat{R"((module
  (memory 2000)
  (func (export "on_load") (result i32) i32.const 0)))"};

/// A guest that tries to reach a host-provided provider-registration import:
/// the sandbox exposes no imports, so instantiation must fail closed.
constexpr std::string_view host_import_wat{R"((module
  (import "refrain" "register_provider" (func $register (result i32)))
  (func (export "on_load") (result i32) call $register)))"};

[[nodiscard]] auto assemble(std::string_view wat) -> std::vector<std::byte> {
    auto bytes{wasm::assemble_wat(wat)};
    expect(static_cast<bool>(bytes)) << "WAT fixture must assemble";
    return bytes ? *bytes : std::vector<std::byte>{};
}

#endif // REFRAIN_WITH_WASM

// ---------------------------------------------------------------------------
// capabilities and registration (both build configurations)
// ---------------------------------------------------------------------------

const suite script_loader_registration_tests = [] {
    "script_loaders/activation registers sandboxed clean wasm and lua "
    "loaders"_test = [] {
        mod::static_mod_registry registry{};
        auto plan{make_gamepack("\"refrain.test.script.content@1.0.0\"")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        image_builder builder{};
        builder.add("refrain.test.script.content",
                    "[mod]\nid = \"refrain.test.script.content\"\n"
                    "version = \"1.0.0\"\nabi = \"1\"\nside = \"server\"\n");
        const auto image{builder.build()};

        mod::host host{registry};
        auto activated{refrain::app::activate_server_mods(host, *plan, image)};
        expect(static_cast<bool>(activated))
            << (activated ? std::string{"activated"}
                          : activated.error().message);
        if (!activated) {
            return;
        }
        expect(host.loaders().size() == 4U);
        const auto *wasm_record{host.find_loader(mod::wasm_loader_id)};
        const auto *lua_record{host.find_loader(mod::lua_loader_id)};
        expect(wasm_record != nullptr);
        expect(lua_record != nullptr);
        if (wasm_record != nullptr) {
            expect(wasm_record->sandboxed);
            expect(wasm_record->unload == mod::mod_unload_semantics::clean);
#if REFRAIN_WITH_WASM
            expect(wasm_record->supports_hot_install);
#else
            expect(!wasm_record->supports_hot_install);
#endif
            expect(wasm_record->abi_major == 1U);
        }
        if (lua_record != nullptr) {
            expect(lua_record->sandboxed);
            expect(lua_record->unload == mod::mod_unload_semantics::clean);
            expect(!lua_record->supports_hot_install);
        }
        expect(host.services()
                   .active_provider(loader_abi::service_id, "wasm")
                   .has_value());
        expect(host.services()
                   .active_provider(loader_abi::service_id, "lua")
                   .has_value());
        // The registration is idempotent: a second activation attempt does not
        // replace the providers an embedder may already have installed.
        expect(static_cast<bool>(refrain::app::register_script_loaders(host)));
        expect(host.loaders().size() == 4U);
    };

    "script_loaders/embedder provider on the same id is kept"_test = [] {
        mod::static_mod_registry registry{};
        auto plan{make_gamepack("\"refrain.test.script.content@1.0.0\"")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        image_builder builder{};
        builder.add("refrain.test.script.content",
                    "[mod]\nid = \"refrain.test.script.content\"\n"
                    "version = \"1.0.0\"\nabi = \"1\"\nside = \"server\"\n");
        const auto image{builder.build()};

        mod::host host{registry};
        wasm::loader custom{wasm::loader_budgets{.max_module_bytes = 1024}};
        expect(register_wasm_loader(host, custom));
        expect(static_cast<bool>(refrain::app::register_script_loaders(host)));
        const auto *record{host.find_loader(mod::wasm_loader_id)};
        expect(record != nullptr);
        // Our provider was not installed over the embedder's one; the lua
        // provider was added beside it.
        expect(host.find_loader(mod::lua_loader_id) != nullptr);
        expect(host.loaders().size() == 2U);
        if (record != nullptr) {
            expect(record->sandboxed);
        }
        expect(static_cast<bool>(host.load(*plan, image)));
    };
};

// ---------------------------------------------------------------------------
// manifest entry validation (direct provider dispatch, both builds)
// ---------------------------------------------------------------------------

const suite script_loader_entry_tests = [] {
    "wasm_loader/rejects the manifest shapes it does not own"_test = [] {
        wasm::loader loader{};
        const auto provider_value{loader.provider()};
        const loader_abi::provider provider{
            *static_cast<const refrain_services_mod_loader_vtable *>(
                provider_value.vtable),
            provider_value.self};
        const auto expect_failure{[&provider](const entry_fields &fields,
                                              loader_abi::mod_loader_error code,
                                              std::string_view detail) {
            refrain::mod::clear_loader_diagnostic();
            auto validated{provider.validate(fields.manifest())};
            expect(!validated);
            if (!validated) {
                expect(validated.error() == code);
            }
            const auto diagnostic{refrain::mod::take_loader_diagnostic()};
            expect(!diagnostic.empty());
            expect(diagnostic.find(detail) != std::string::npos) << diagnostic;
        }};

        entry_fields wrong_loader{};
        wrong_loader.loader_id = "native";
        expect_failure(wrong_loader,
                       loader_abi::mod_loader_error::version_mismatch,
                       "loader 'native'");

        entry_fields missing_module{};
        missing_module.wasm_path.clear();
        expect_failure(missing_module,
                       loader_abi::mod_loader_error::invalid_argument,
                       "[entry].wasm");

        entry_fields unsafe_module{};
        unsafe_module.wasm_path = "../escape.wasm";
        expect_failure(unsafe_module,
                       loader_abi::mod_loader_error::invalid_argument,
                       "safe package-relative path");

        entry_fields native_payload{};
        native_payload.native_path = "lib/libmod.so";
        expect_failure(native_payload,
                       loader_abi::mod_loader_error::invalid_argument,
                       "[entry].native");

        entry_fields symbol_payload{};
        symbol_payload.symbol = "refrain_mod_main";
        expect_failure(symbol_payload,
                       loader_abi::mod_loader_error::invalid_argument,
                       "[entry].symbol");

        entry_fields wrong_abi{};
        wrong_abi.abi = 2;
        expect_failure(wrong_abi,
                       loader_abi::mod_loader_error::version_mismatch,
                       "guest ABI 2");

        // A data-only package is shape-checked without touching the package
        // bytes: the plan did not select it for code.
        entry_fields data_only{};
        data_only.activate_code = 0;
        data_only.wasm_path = "../still-not-read.wasm";
        refrain::mod::clear_loader_diagnostic();
        auto validated{provider.validate(data_only.manifest())};
        expect(!validated); // the path is still unsafe, so it is refused
    };
};

// ---------------------------------------------------------------------------
// lua fail-closed provider (both build configurations)
// ---------------------------------------------------------------------------

const suite lua_loader_tests = [] {
    "lua_loader/declares its contract and refuses every package naming lua"_test =
        [] {
            mod::static_mod_registry registry{};
            auto plan{make_gamepack("\"refrain.test.script.lua@1.0.0\"")};
            expect(static_cast<bool>(plan));
            if (!plan) {
                return;
            }
            auto manifest{std::string{"[mod]\n"}};
            manifest += "id = \"refrain.test.script.lua\"\n";
            manifest += "version = \"1.0.0\"\nabi = \"1\"\n";
            manifest += "side = \"server\"\n\n[entry]\nloader = \"lua\"\n";
            image_builder builder{};
            builder.add("refrain.test.script.lua", std::move(manifest));
            const auto image{builder.build()};

            mod::host host{registry};
            expect(register_lua_loader(host));
            const auto *record{host.find_loader(mod::lua_loader_id)};
            expect(record != nullptr);
            if (record != nullptr) {
                expect(record->sandboxed);
                expect(record->unload == mod::mod_unload_semantics::clean);
                expect(!record->supports_hot_install);
            }
            auto loaded{host.load(*plan, image)};
            expect(!loaded);
            if (!loaded) {
                expect(loaded.error().code == error_code::unsupported);
                // The refusal names the loader, the mod and the missing
                // runtime instead of leaving the operator with a bare code.
                expect(loaded.error().message.find("lua") != std::string::npos);
                expect(loaded.error().message.find("Lua runtime") !=
                       std::string::npos);
                expect(loaded.error().message.find("refrain.test.script.lua") !=
                       std::string::npos);
            }
            expect(!host.loaded());
        };

    "lua_loader/rejects a payload field owned by another first-party loader"_test =
        [] {
            const auto provider_value{
                script::lua::default_mod_loader_provider()};
            const loader_abi::provider provider{
                *static_cast<const refrain_services_mod_loader_vtable *>(
                    provider_value.vtable),
                provider_value.self};
            entry_fields fields{};
            fields.loader_id = "lua";
            fields.wasm_path.clear();
            refrain::mod::clear_loader_diagnostic();
            auto validated{provider.validate(fields.manifest())};
            expect(!validated);
            if (!validated) {
                expect(validated.error() ==
                       loader_abi::mod_loader_error::unsupported);
            }
            expect(refrain::mod::take_loader_diagnostic().find("lua") !=
                   std::string::npos);

            entry_fields wrong_loader{};
            wrong_loader.loader_id = "wasm";
            refrain::mod::clear_loader_diagnostic();
            auto refused{provider.validate(wrong_loader.manifest())};
            expect(!refused);
            if (!refused) {
                expect(refused.error() ==
                       loader_abi::mod_loader_error::version_mismatch);
            }
            entry_fields wasm_payload{};
            wasm_payload.loader_id = "lua";
            refrain::mod::clear_loader_diagnostic();
            auto shape{provider.validate(wasm_payload.manifest())};
            expect(!shape);
            if (!shape) {
                expect(shape.error() ==
                       loader_abi::mod_loader_error::invalid_argument);
            }
        };
};

// ---------------------------------------------------------------------------
// build without a WebAssembly runtime: fail closed
// ---------------------------------------------------------------------------

#if !REFRAIN_WITH_WASM

const suite wasm_loader_unavailable_tests = [] {
    "wasm_loader/unavailable build refuses a manifest naming wasm"_test = [] {
        mod::static_mod_registry registry{};
        auto plan{make_gamepack("\"refrain.test.script.wasm@1.0.0\"")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        auto manifest{
            make_wasm_manifest("refrain.test.script.wasm", "scripts/mod.wasm")};
        image_builder builder{};
        builder.add("refrain.test.script.wasm", std::move(manifest));
        const auto image{builder.build()};

        mod::host host{registry};
        wasm::loader loader{};
        expect(register_wasm_loader(host, loader));
        auto loaded{host.load(*plan, image)};
        expect(!loaded);
        if (!loaded) {
            expect(loaded.error().code == error_code::unsupported);
            expect(loaded.error().message.find("wasm") != std::string::npos);
            expect(loaded.error().message.find("REFRAIN_WITH_WASM=0") !=
                   std::string::npos);
        }
        expect(!host.loaded());
        expect(loader.stats().live_instances == 0U);
    };
};

#endif // !REFRAIN_WITH_WASM

// ---------------------------------------------------------------------------
// installed-package execution: budgets, sandbox, lifecycle, reload
// ---------------------------------------------------------------------------

#if REFRAIN_WITH_WASM

namespace {

constexpr std::string_view wasm_mod_id{"refrain.test.script.wasm"};

} // namespace

const suite wasm_loader_installed_tests = [] {
    "wasm_loader/loads an installed module and releases its budget on unload"_test =
        [] {
            refrain::test_support::scratch_directory scratch;
            const auto module{assemble(minimal_wat)};
            auto archive{pack_wasm_mod(
                scratch, "mod",
                make_wasm_manifest(wasm_mod_id, "scripts/mod.wasm"),
                "scripts/mod.wasm", module)};
            expect(archive.has_value());
            if (!archive) {
                return;
            }
            auto store_root{install_package(scratch, *archive)};
            expect(store_root.has_value());
            if (!store_root) {
                return;
            }
            auto plan{installed_plan(wasm_mod_id, *store_root)};
            expect(static_cast<bool>(plan))
                << (plan ? std::string{"resolved"} : plan.error().message);
            if (!plan) {
                return;
            }

            wasm::loader loader{};
            mod::host host{};
            expect(register_wasm_loader(host, loader));
            auto loaded{host.load(*plan, std::span<const std::byte>{})};
            expect(static_cast<bool>(loaded))
                << (loaded ? std::string{"load ok"} : loaded.error().message);
            if (!loaded) {
                return;
            }
            expect(host.mods().size() == 1U);
            if (host.mods().size() == 1U) {
                const auto &loaded_mod{host.mods().front()};
                expect(loaded_mod.loader == "wasm");
                expect(loaded_mod.loader_unload ==
                       mod::mod_unload_semantics::clean);
                expect(loaded_mod.loader_sandboxed);
                expect(loaded_mod.has_code);
                expect(loaded_mod.instance_handle != 0U);
            }
            const auto formatted{host.format()};
            expect(formatted.find("loader wasm sandboxed") !=
                   std::string::npos);
            expect(formatted.find("unload clean") != std::string::npos);

            // The guest holds its module, memory and fuel budget while it is
            // loaded.
            const auto &loaded_stats{loader.stats()};
            expect(loaded_stats.live_instances == 1U);
            expect(loaded_stats.live_module_bytes == module.size());
            expect(loaded_stats.live_memory_bytes > 0U);
            expect(loaded_stats.live_fuel > 0U);
            expect(loaded_stats.total_loaded == 1U);
            expect(loaded_stats.total_unloaded == 0U);

            // A clean unload destroys the store and releases every budget.
            auto staged{host.stage_uninstall(wasm_mod_id)};
            expect(static_cast<bool>(staged))
                << (staged ? std::string{"staged"} : staged.error().message);
            if (!staged) {
                return;
            }
            auto committed{host.commit_epoch()};
            expect(static_cast<bool>(committed))
                << (committed ? std::string{"committed"}
                              : committed.error().message);
            if (!committed) {
                return;
            }
            expect(committed->removed ==
                   std::vector<std::string>{std::string{wasm_mod_id}});
            expect(host.mods().empty());
            expect(loaded_stats.live_instances == 0U);
            expect(loaded_stats.live_module_bytes == 0U);
            expect(loaded_stats.live_memory_bytes == 0U);
            expect(loaded_stats.live_fuel == 0U);
            expect(loaded_stats.total_unloaded == 1U);
        };

    "wasm_loader/missing module member fails closed naming the entry"_test =
        [] {
            refrain::test_support::scratch_directory scratch;
            // The manifest declares the member; the package does not ship it.
            auto archive{pack_wasm_mod(
                scratch, "mod",
                make_wasm_manifest(wasm_mod_id, "scripts/missing.wasm"), "",
                {})};
            expect(archive.has_value());
            if (!archive) {
                return;
            }
            auto store_root{install_package(scratch, *archive)};
            expect(store_root.has_value());
            if (!store_root) {
                return;
            }
            auto plan{installed_plan(wasm_mod_id, *store_root)};
            expect(static_cast<bool>(plan));
            if (!plan) {
                return;
            }
            wasm::loader loader{};
            mod::host host{};
            expect(register_wasm_loader(host, loader));
            auto loaded{host.load(*plan, std::span<const std::byte>{})};
            expect(!loaded);
            if (!loaded) {
                expect(loaded.error().code == error_code::not_found);
                expect(loaded.error().message.find("scripts/missing.wasm") !=
                       std::string::npos);
                expect(loaded.error().message.find("wasm") !=
                       std::string::npos);
            }
            expect(loader.stats().live_instances == 0U);
            expect(!host.loaded());
        };

    "wasm_loader/corrupt module fails closed before any guest code runs"_test =
        [] {
            refrain::test_support::scratch_directory scratch;
            constexpr std::string_view corrupt{"this is not a wasm module"};
            const auto bytes{
                std::as_bytes(std::span{corrupt.data(), corrupt.size()})};
            auto archive{pack_wasm_mod(
                scratch, "mod",
                make_wasm_manifest(wasm_mod_id, "scripts/mod.wasm"),
                "scripts/mod.wasm",
                std::span<const std::byte>{bytes.data(), bytes.size()})};
            expect(archive.has_value());
            if (!archive) {
                return;
            }
            auto store_root{install_package(scratch, *archive)};
            expect(store_root.has_value());
            if (!store_root) {
                return;
            }
            auto plan{installed_plan(wasm_mod_id, *store_root)};
            expect(static_cast<bool>(plan));
            if (!plan) {
                return;
            }
            wasm::loader loader{};
            mod::host host{};
            expect(register_wasm_loader(host, loader));
            auto loaded{host.load(*plan, std::span<const std::byte>{})};
            expect(!loaded);
            if (!loaded) {
                expect(loaded.error().code == error_code::invalid_argument);
                expect(loaded.error().message.find("not a valid module") !=
                       std::string::npos);
            }
            expect(loader.stats().live_instances == 0U);
        };

    "wasm_loader/oversized module fails closed on its size budget"_test = [] {
        refrain::test_support::scratch_directory scratch;
        const auto module{assemble(minimal_wat)};
        auto archive{pack_wasm_mod(
            scratch, "mod", make_wasm_manifest(wasm_mod_id, "scripts/mod.wasm"),
            "scripts/mod.wasm", module)};
        expect(archive.has_value());
        if (!archive) {
            return;
        }
        auto store_root{install_package(scratch, *archive)};
        expect(store_root.has_value());
        if (!store_root) {
            return;
        }
        auto plan{installed_plan(wasm_mod_id, *store_root)};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        // A loader with a module budget smaller than the fixture.
        wasm::loader loader{wasm::loader_budgets{.max_module_bytes = 8}};
        mod::host host{};
        expect(register_wasm_loader(host, loader));
        auto loaded{host.load(*plan, std::span<const std::byte>{})};
        expect(!loaded);
        if (!loaded) {
            expect(loaded.error().code == error_code::permission_denied);
            expect(loaded.error().message.find("module budget") !=
                   std::string::npos)
                << loaded.error().message;
        }
        expect(loader.stats().live_instances == 0U);
    };

    "wasm_loader/guest cannot reach a host import from the sandbox"_test = [] {
        refrain::test_support::scratch_directory scratch;
        const auto module{assemble(host_import_wat)};
        auto archive{pack_wasm_mod(
            scratch, "mod", make_wasm_manifest(wasm_mod_id, "scripts/mod.wasm"),
            "scripts/mod.wasm", module)};
        expect(archive.has_value());
        if (!archive) {
            return;
        }
        auto store_root{install_package(scratch, *archive)};
        expect(store_root.has_value());
        if (!store_root) {
            return;
        }
        auto plan{installed_plan(wasm_mod_id, *store_root)};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        wasm::loader loader{};
        mod::host host{};
        expect(register_wasm_loader(host, loader));
        auto loaded{host.load(*plan, std::span<const std::byte>{})};
        expect(!loaded);
        if (!loaded) {
            // The refusal is the sandbox policy: the guest asked for an import
            // the loader does not expose.
            expect(loaded.error().code == error_code::permission_denied);
            expect(loaded.error().message.find("host import") !=
                   std::string::npos)
                << loaded.error().message;
            expect(loaded.error().message.find("sandboxed") !=
                   std::string::npos);
            expect(loaded.error().message.find("register_provider") !=
                   std::string::npos);
        }
        expect(loader.stats().live_instances == 0U);
    };

    "wasm_loader/runaway guest is stopped by its instruction budget"_test = [] {
        refrain::test_support::scratch_directory scratch;
        const auto module{assemble(spin_wat)};
        auto archive{pack_wasm_mod(
            scratch, "mod", make_wasm_manifest(wasm_mod_id, "scripts/mod.wasm"),
            "scripts/mod.wasm", module)};
        expect(archive.has_value());
        if (!archive) {
            return;
        }
        auto store_root{install_package(scratch, *archive)};
        expect(store_root.has_value());
        if (!store_root) {
            return;
        }
        auto plan{installed_plan(wasm_mod_id, *store_root)};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        // A tight loop in on_load: the load path's fuel budget traps it
        // instead of hanging the host.
        wasm::loader loader{wasm::loader_budgets{.load_fuel = 100'000}};
        mod::host host{};
        expect(register_wasm_loader(host, loader));
        auto loaded{host.load(*plan, std::span<const std::byte>{})};
        expect(!loaded);
        if (!loaded) {
            expect(loaded.error().code == error_code::permission_denied);
            expect(loaded.error().message.find("instruction budget") !=
                   std::string::npos)
                << loaded.error().message;
        }
        expect(loader.stats().live_instances == 0U);
        expect(loader.stats().live_fuel == 0U);
    };

    "wasm_loader/guest memory is capped by the store budget"_test = [] {
        refrain::test_support::scratch_directory scratch;
        const auto module{assemble(oversized_memory_wat)};
        auto archive{pack_wasm_mod(
            scratch, "mod", make_wasm_manifest(wasm_mod_id, "scripts/mod.wasm"),
            "scripts/mod.wasm", module)};
        expect(archive.has_value());
        if (!archive) {
            return;
        }
        auto store_root{install_package(scratch, *archive)};
        expect(store_root.has_value());
        if (!store_root) {
            return;
        }
        auto plan{installed_plan(wasm_mod_id, *store_root)};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        // The fixture's minimum memory is 128MiB; the budget is 1MiB, so
        // instantiation is refused inside the store.
        wasm::loader loader{
            wasm::loader_budgets{.max_memory_bytes = 1024ULL * 1024ULL}};
        mod::host host{};
        expect(register_wasm_loader(host, loader));
        auto loaded{host.load(*plan, std::span<const std::byte>{})};
        expect(!loaded);
        if (!loaded) {
            expect(loaded.error().code == error_code::permission_denied);
            expect(loaded.error().message.find("memory budget") !=
                   std::string::npos)
                << loaded.error().message;
        }
        expect(loader.stats().live_instances == 0U);
    };

    "wasm_loader/data-only package is validated but not executed"_test = [] {
        mod::static_mod_registry registry{};
        auto plan{make_gamepack("\"refrain.test.script.wasmdata@1.0.0\"")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        // The package declares a provider role the gamepack does not select,
        // so the plan keeps it data-only: the loader validates the entry shape
        // and must not instantiate anything.
        auto manifest{make_wasm_manifest("refrain.test.script.wasmdata",
                                         "scripts/mod.wasm",
                                         "refrain:data.packs")};
        image_builder builder{};
        builder.add("refrain.test.script.wasmdata", std::move(manifest));
        const auto image{builder.build()};

        wasm::loader loader{};
        mod::host host{registry};
        expect(register_wasm_loader(host, loader));
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
        expect(loaded_mod.loader == "wasm");
        expect(!loaded_mod.has_code);
        expect(loaded_mod.instance_handle == 0U);
        expect(loader.stats().live_instances == 0U);
    };

    "wasm_loader/hot reload replaces the guest and unloads the old "
    "generation"_test = [] {
        refrain::test_support::scratch_directory scratch;
        const auto module{assemble(minimal_wat)};
        auto archive{pack_wasm_mod(
            scratch, "mod", make_wasm_manifest(wasm_mod_id, "scripts/mod.wasm"),
            "scripts/mod.wasm", module)};
        expect(archive.has_value());
        if (!archive) {
            return;
        }
        auto store_root{install_package(scratch, *archive)};
        expect(store_root.has_value());
        if (!store_root) {
            return;
        }
        auto plan{installed_plan(wasm_mod_id, *store_root)};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        wasm::loader loader{};
        mod::host host{};
        expect(register_wasm_loader(host, loader));
        auto loaded{host.load(*plan, std::span<const std::byte>{})};
        expect(static_cast<bool>(loaded))
            << (loaded ? std::string{"load ok"} : loaded.error().message);
        if (!loaded) {
            return;
        }
        const auto first_handle{host.mods().front().instance_handle};
        expect(first_handle != 0U);
        expect(loader.stats().live_instances == 1U);

        auto resolver{mod::make_package_resolver(std::span<const std::byte>{})};
        expect(static_cast<bool>(resolver));
        if (!resolver) {
            return;
        }
        auto staged{host.stage_composition(
            *plan, *resolver,
            mod::stage_options{.reload = {std::string{wasm_mod_id}}})};
        expect(static_cast<bool>(staged))
            << (staged ? std::string{"staged"} : staged.error().message);
        if (!staged) {
            return;
        }
        expect(staged->reloaded ==
               std::vector<std::string>{std::string{wasm_mod_id}});
        // Staging builds the replacement next to the running guest; the old
        // instance is still live until the epoch commits.
        expect(loader.stats().live_instances == 2U);
        expect(loader.stats().total_loaded == 2U);
        expect(loader.stats().total_unloaded == 0U);

        auto committed{host.commit_epoch()};
        expect(static_cast<bool>(committed))
            << (committed ? std::string{"committed"}
                          : committed.error().message);
        if (!committed) {
            return;
        }
        expect(committed->reloaded ==
               std::vector<std::string>{std::string{wasm_mod_id}});
        // The old generation was unloaded after the switch, so exactly one
        // guest and one budget set remain.
        expect(loader.stats().live_instances == 1U);
        expect(loader.stats().total_loaded == 2U);
        expect(loader.stats().total_unloaded == 1U);
        expect(host.mods().size() == 1U);
        if (host.mods().size() == 1U) {
            expect(host.mods().front().instance_handle != first_handle);
            expect(host.mods().front().generation == 2U);
        }
        expect(host.quarantined_mods().empty());
    };

    "wasm_loader/reload builds a replacement before the old instance is "
    "unloaded"_test = [] {
        refrain::test_support::scratch_directory scratch;
        const auto module{assemble(minimal_wat)};
        auto archive{pack_wasm_mod(
            scratch, "mod", make_wasm_manifest(wasm_mod_id, "scripts/mod.wasm"),
            "scripts/mod.wasm", module)};
        expect(archive.has_value());
        if (!archive) {
            return;
        }
        auto store_root{install_package(scratch, *archive)};
        expect(store_root.has_value());
        if (!store_root) {
            return;
        }
        wasm::loader loader{};
        const auto provider_value{loader.provider()};
        const loader_abi::provider provider{
            *static_cast<const refrain_services_mod_loader_vtable *>(
                provider_value.vtable),
            provider_value.self};

        entry_fields fields{};
        fields.mod_id = std::string{wasm_mod_id};
        fields.package_root =
            (fs::path{*store_root} / "mods" / std::string{wasm_mod_id})
                .string();
        auto request{fields.request()};
        auto loaded{provider.load(request)};
        expect(static_cast<bool>(loaded))
            << (loaded ? std::string{"load ok"} : "load failed");
        if (!loaded) {
            return;
        }
        expect(loader.stats().live_instances == 1U);

        // The replacement generation is built next to the old one; the host
        // unloads the old instance only after the composition epoch commits.
        auto replaced{provider.reload(*loaded, request)};
        expect(static_cast<bool>(replaced));
        if (!replaced) {
            return;
        }
        expect(replaced->raw() != loaded->raw());
        expect(loader.stats().live_instances == 2U);
        expect(loader.stats().total_loaded == 2U);

        expect(static_cast<bool>(
            provider.unload(*loaded, loader_abi::mod_unload_reason::reload)));
        expect(loader.stats().live_instances == 1U);
        expect(loader.stats().total_unloaded == 1U);
        expect(static_cast<bool>(provider.unload(
            *replaced, loader_abi::mod_unload_reason::uninstall)));
        expect(loader.stats().live_instances == 0U);
        expect(loader.stats().live_module_bytes == 0U);
        expect(loader.stats().live_memory_bytes == 0U);
        expect(loader.stats().live_fuel == 0U);
        expect(loader.stats().total_unloaded == 2U);
    };
};

#endif // REFRAIN_WITH_WASM

} // namespace
