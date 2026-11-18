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
#include <refrain/contracts/services/script_host.hpp>
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

#include <chrono>
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
namespace host = refrain::script::host;
using refrain::error_code;
namespace loader_abi = refrain::services::mod_loader;
namespace script_host_abi = refrain::services::script_host;

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
// The guest ABI has three capability-gated host imports (`refrain.log`,
// `refrain.config_get`, `refrain.tick`), a required `on_load: () -> i32`, an
// optional `on_unload: () -> i32` and optional `(i32, i32) -> i32` callbacks.
// A zero `on_load`/`on_unload` return reports success.

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

/// The mod id every installed guest fixture uses.
constexpr std::string_view guest_mod_id{"refrain.test.script.wasm"};

/// A guest that uses every granted host import and exports the callback the
/// host dispatches. `on_event` returns the sum of the staged argument bytes,
/// the configuration value length, the `log` import status and the tick index:
/// 10 + 5 + 0 + 42 = 57 with the fixture environment.
constexpr std::string_view import_guest_wat{R"((module
  (import "refrain" "log" (func $log (param i32 i32 i32) (result i32)))
  (import "refrain" "config_get" (func $config (param i32 i32 i32 i32) (result i32)))
  (import "refrain" "tick" (func $tick (result i64)))
  (memory (export "memory") 1)
  (data (i32.const 64) "hello from guest")
  (data (i32.const 128) "greeting")
  (func (export "on_load") (result i32)
    i32.const 2
    i32.const 64
    i32.const 16
    call $log
    drop
    i32.const 0)
  (func (export "on_unload") (result i32) i32.const 0)
  (func (export "on_event") (param i32 i32) (result i32)
    (local $sum i32)
    (local $index i32)
    (local $config_bytes i32)
    block $done
      loop $loop
        local.get $index
        local.get 1
        i32.ge_u
        br_if $done
        local.get $sum
        local.get 0
        local.get $index
        i32.add
        i32.load8_u
        i32.add
        local.set $sum
        local.get $index
        i32.const 1
        i32.add
        local.set $index
        br $loop
      end
    end
    i32.const 128
    i32.const 8
    i32.const 512
    i32.const 64
    call $config
    local.set $config_bytes
    i32.const 2
    i32.const 64
    i32.const 16
    call $log
    local.get $sum
    local.get $config_bytes
    i32.add
    i32.add
    call $tick
    i32.wrap_i64
    i32.add)))"};

/// A guest callback that never returns: the per-call instruction budget or the
/// wall-clock deadline must trap it.
constexpr std::string_view spin_callback_wat{R"((module
  (memory (export "memory") 1)
  (func (export "on_load") (result i32) i32.const 0)
  (func (export "on_unload") (result i32) i32.const 0)
  (func (export "spin") (param i32 i32) (result i32)
    (local $index i32)
    (loop $loop
      local.get $index
      i32.const 1
      i32.add
      local.set $index
      br $loop)
    local.get $index)))"};

/// A guest callback that traps immediately: the call must be reported as a
/// guest trap and quarantine the instance.
constexpr std::string_view trap_callback_wat{R"((module
  (memory (export "memory") 1)
  (func (export "on_load") (result i32) i32.const 0)
  (func (export "on_unload") (result i32) i32.const 0)
  (func (export "boom") (param i32 i32) (result i32) unreachable)))"};

/// A guest callback that burns a bounded amount of its per-call budget: two
/// dispatches both succeed only when every call provisions a fresh budget.
constexpr std::string_view burn_callback_wat{R"((module
  (memory (export "memory") 1)
  (func (export "on_load") (result i32) i32.const 0)
  (func (export "on_unload") (result i32) i32.const 0)
  (func (export "burn") (param i32 i32) (result i32)
    (local $index i32)
    (loop $loop
      local.get $index
      i32.const 1
      i32.add
      local.set $index
      local.get $index
      i32.const 3000
      i32.lt_u
      br_if $loop)
    local.get $index)))"};

/// A guest that accepts call arguments but exports no linear memory: the host
/// cannot stage the bytes and must refuse the call instead of guessing.
constexpr std::string_view no_memory_wat{R"((module
  (func (export "on_load") (result i32) i32.const 0)
  (func (export "on_event") (param i32 i32) (result i32) i32.const 0)))"};

/// A guest whose callback does not implement the `(i32, i32) -> i32` call ABI:
/// dispatching it must be a caller mistake, not a guest trap.
constexpr std::string_view bad_abi_wat{R"((module
  (memory (export "memory") 1)
  (func (export "on_load") (result i32) i32.const 0)
  (func (export "on_unload") (result i32) i32.const 0)
  (func (export "bad_abi") (param i32) (result i32) i32.const 0)))"};

/// A guest whose unload hook never returns: a clean unload of a quarantined
/// instance must not run it, or the host would hang in guest code.
constexpr std::string_view looping_unload_wat{R"((module
  (memory (export "memory") 1)
  (func (export "on_load") (result i32) i32.const 0)
  (func (export "on_unload") (result i32)
    (loop $loop
      br $loop)
    i32.const 0)
  (func (export "spin") (param i32 i32) (result i32)
    (loop $loop
      br $loop)
    i32.const 0)))"};

/// A guest that imports an unknown host function whose name contains a forged
/// log line: a refusal must escape the name instead of printing it raw.
constexpr std::string_view forged_import_wat{R"((module
  (import "refrain" "fake\0a[INFO] forged line\1b]0;title\07" (func $forged (result i32)))
  (func (export "on_load") (result i32) call $forged)))"};

/// A guest that imports a capability-gated host import; a loader that does not
/// grant it must refuse the package before any guest code runs.
constexpr std::string_view config_import_wat{R"((module
  (import "refrain" "config_get" (func $config (param i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (func (export "on_load") (result i32) i32.const 0)))"};

[[nodiscard]] auto assemble(std::string_view wat) -> std::vector<std::byte> {
    auto bytes{wasm::assemble_wat(wat)};
    expect(static_cast<bool>(bytes))
        << (bytes ? std::string{"WAT fixture assembled"}
                  : bytes.error().message);
    return bytes ? *bytes : std::vector<std::byte>{};
}

/// One guest, one callback, the value the callback returned and the loader
/// diagnostic the provider published for a failure.
struct direct_call {
    refrain_status status{REFRAIN_STATUS_OK};
    std::int32_t value{0};
    std::string diagnostic{};
};

/// Load one installed guest package through the provider directly, returning
/// the instance handle.
[[nodiscard]] auto load_guest_direct(wasm::loader &loader,
                                     std::string_view package_root)
    -> std::optional<std::uint64_t> {
    const auto provider_value{loader.provider()};
    const loader_abi::provider provider{
        *static_cast<const refrain_services_mod_loader_vtable *>(
            provider_value.vtable),
        provider_value.self};
    entry_fields fields{};
    fields.mod_id = std::string{guest_mod_id};
    fields.package_root = std::string{package_root};
    auto loaded{provider.load(fields.request())};
    expect(static_cast<bool>(loaded)) << "guest load failed";
    if (!loaded) {
        return std::nullopt;
    }
    return loaded->raw();
}

/// Dispatch one callback through the loader's sealed script host vtable.
[[nodiscard]] auto call_guest_direct(wasm::loader &loader, std::uint64_t handle,
                                     std::string_view callback,
                                     std::span<const std::byte> arguments,
                                     std::string_view loader_id = "wasm")
    -> direct_call {
    const auto provider_value{loader.script_host_provider()};
    const auto &table{*static_cast<const refrain_services_script_host_vtable *>(
        provider_value.vtable)};
    auto request{refrain::services::script_host::make_script_call_request()};
    request.callback = refrain::contracts::to_borrowed(callback);
    request.arguments =
        refrain::contracts::to_borrowed(std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t *>(arguments.data()),
            arguments.size()});
    request.loader = refrain::contracts::to_borrowed(loader_id);
    refrain_services_script_host_script_call_result result{};
    result.struct_size = REFRAIN_SERVICES_SCRIPT_HOST_SCRIPT_CALL_RESULT_SIZE;
    refrain::mod::clear_loader_diagnostic();
    direct_call called{};
    called.status = table.call(provider_value.self, handle, &request, &result);
    called.value = result.value;
    called.diagnostic = refrain::mod::take_loader_diagnostic();
    return called;
}

/// Pack the inline WAT module as an installed guest package and return the
/// content store root that `installed_plan()` and `guest_package_root()` take.
[[nodiscard]] auto
install_guest(refrain::test_support::scratch_directory &scratch,
              std::string_view wat) -> std::optional<std::string> {
    const auto module{assemble(wat)};
    if (module.empty()) {
        return std::nullopt;
    }
    auto archive{pack_wasm_mod(
        scratch, "mod", make_wasm_manifest(guest_mod_id, "scripts/mod.wasm"),
        "scripts/mod.wasm", module)};
    if (!archive) {
        return std::nullopt;
    }
    return install_package(scratch, *archive);
}

/// Package root of the single installed guest package.
[[nodiscard]] auto guest_package_root(std::string_view store_root)
    -> std::string {
    return (fs::path{store_root} / "mods" / std::string{guest_mod_id}).string();
}

/// The guest fixture environment: one configuration entry and a tick index.
[[nodiscard]] auto guest_environment() -> host::environment {
    host::environment environment{};
    environment.config.emplace("greeting", "hello");
    environment.tick_index = 42;
    return environment;
}

/// Unload one instance through the provider directly.
[[nodiscard]] auto unload_guest_direct(wasm::loader &loader,
                                       std::uint64_t handle) -> bool {
    const auto provider_value{loader.provider()};
    const loader_abi::provider provider{
        *static_cast<const refrain_services_mod_loader_vtable *>(
            provider_value.vtable),
        provider_value.self};
    return provider
        .unload(loader_abi::mod_instance{handle},
                loader_abi::mod_unload_reason::explicit_request)
        .has_value();
}

/// A loader whose guests hold every safe capability and read the fixture
/// environment.
[[nodiscard]] auto granted_loader(wasm::loader_budgets budgets = {})
    -> wasm::loader {
    return wasm::loader{
        budgets, wasm::loader_host_api{.grants = host::capability::log |
                                                 host::capability::config |
                                                 host::capability::clock,
                                       .environment = guest_environment()}};
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
            // The refusal names every import the sandbox does expose and the
            // grant set the guest ran with.
            expect(loaded.error().message.find("refrain.log") !=
                   std::string::npos)
                << loaded.error().message;
            expect(loaded.error().message.find("active grant set is [log]") !=
                   std::string::npos)
                << loaded.error().message;
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

// ---------------------------------------------------------------------------
// host imports and callback dispatch
// ---------------------------------------------------------------------------

const suite wasm_loader_call_tests = [] {
    "wasm_loader/calls a granted callback through the sealed script host"_test =
        [] {
            refrain::test_support::scratch_directory scratch;
            auto store_root{install_guest(scratch, import_guest_wat)};
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

            auto loader{granted_loader()};
            mod::host host{};
            expect(register_wasm_loader(host, loader));
            expect(static_cast<bool>(refrain::script::register_script_host(
                host, loader.script_host_provider())));
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
            const auto handle{host.mods().front().instance_handle};
            expect(handle != 0U);
            const std::array<std::byte, 4> arguments{
                std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
            auto called{script::dispatch_guest_call(host, handle, "on_event",
                                                    arguments)};
            expect(static_cast<bool>(called))
                << (called ? std::string{"called"} : called.error().message);
            if (called) {
                // argument sum 10 + config length 5 + log status 0 + tick 42.
                expect(called->value == 57);
            }
            // The host owns the tick index the guest reads; updating it
            // changes what the next dispatch observes.
            loader.set_tick_index(100);
            auto updated{script::dispatch_guest_call(host, handle, "on_event",
                                                     arguments)};
            expect(static_cast<bool>(updated))
                << (updated ? std::string{"called"} : updated.error().message);
            if (updated) {
                expect(updated->value == 115);
            }
            expect(loader.stats().total_calls == 2U);
            expect(loader.stats().failed_calls == 0U);
            expect(loader.stats().quarantined_instances == 0U);

            // A handle no loaded mod owns is refused before the script host
            // sees it: handles are loader-local and only valid for a live mod.
            auto bogus{script::dispatch_guest_call(host, handle + 777U,
                                                   "on_event", arguments)};
            expect(!bogus);
            if (!bogus) {
                expect(bogus.error().code == error_code::not_found);
                expect(bogus.error().message.find(
                           "no loaded mod owns guest instance handle") !=
                       std::string::npos)
                    << bogus.error().message;
            }
        };

    "wasm_loader/an unknown callback is refused by name without quarantining"_test =
        [] {
            refrain::test_support::scratch_directory scratch;
            auto store_root{install_guest(scratch, import_guest_wat)};
            expect(store_root.has_value());
            if (!store_root) {
                return;
            }
            auto loader{granted_loader()};
            auto handle{
                load_guest_direct(loader, guest_package_root(*store_root))};
            expect(handle.has_value());
            if (!handle) {
                return;
            }
            auto missing{call_guest_direct(loader, *handle, "missing", {})};
            expect(missing.status ==
                   static_cast<refrain_status>(
                       script_host_abi::script_error::not_found));
            expect(
                missing.diagnostic.find("does not export callback 'missing'") !=
                std::string::npos)
                << missing.diagnostic;
            expect(loader.stats().failed_calls == 1U);
            expect(loader.stats().quarantined_instances == 0U);

            // A caller mistake does not stop the instance.
            const std::array<std::byte, 4> arguments{
                std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
            auto called{
                call_guest_direct(loader, *handle, "on_event", arguments)};
            expect(called.status == REFRAIN_STATUS_OK);
            expect(called.value == 57);
        };

    "wasm_loader/an ungranted host import is refused at load with the grant set"_test =
        [] {
            refrain::test_support::scratch_directory scratch;
            auto store_root{install_guest(scratch, config_import_wat)};
            expect(store_root.has_value());
            if (!store_root) {
                return;
            }
            auto plan{installed_plan(wasm_mod_id, *store_root)};
            expect(static_cast<bool>(plan));
            if (!plan) {
                return;
            }
            // The guest imports `config_get` but the loader grants only `log`.
            wasm::loader loader{
                wasm::loader_budgets{},
                wasm::loader_host_api{.grants = host::capability::log}};
            mod::host host{};
            expect(register_wasm_loader(host, loader));
            auto loaded{host.load(*plan, std::span<const std::byte>{})};
            expect(!loaded);
            if (!loaded) {
                expect(loaded.error().code == error_code::permission_denied);
                expect(loaded.error().message.find("refrain.config_get") !=
                       std::string::npos)
                    << loaded.error().message;
                expect(loaded.error().message.find("capability 'config'") !=
                       std::string::npos)
                    << loaded.error().message;
                expect(loaded.error().message.find(
                           "active grant set is [log]") != std::string::npos)
                    << loaded.error().message;
            }
            expect(loader.stats().live_instances == 0U);
        };

    "wasm_loader/invalid call arguments are refused before the guest runs"_test =
        [] {
            refrain::test_support::scratch_directory scratch;
            auto store_root{install_guest(scratch, import_guest_wat)};
            expect(store_root.has_value());
            if (!store_root) {
                return;
            }
            auto loader{granted_loader()};
            auto handle{
                load_guest_direct(loader, guest_package_root(*store_root))};
            expect(handle.has_value());
            if (!handle) {
                return;
            }
            const std::vector<std::byte> oversized(
                host::max_call_argument_bytes + 1U);
            auto too_large{
                call_guest_direct(loader, *handle, "on_event", oversized)};
            expect(too_large.status ==
                   static_cast<refrain_status>(
                       script_host_abi::script_error::invalid_argument));
            expect(too_large.diagnostic.find("over the 256-byte call argument "
                                             "budget") != std::string::npos)
                << too_large.diagnostic;

            auto bad_name{call_guest_direct(loader, *handle, "bad name!", {})};
            expect(bad_name.status ==
                   static_cast<refrain_status>(
                       script_host_abi::script_error::invalid_argument));
            expect(bad_name.diagnostic.find("not a valid export name") !=
                   std::string::npos)
                << bad_name.diagnostic;

            auto empty_name{call_guest_direct(loader, *handle, "", {})};
            expect(empty_name.status ==
                   static_cast<refrain_status>(
                       script_host_abi::script_error::invalid_argument));

            // None of these reached the guest, so no dispatch is counted.
            expect(loader.stats().total_calls == 0U);
            expect(loader.stats().failed_calls == 0U);
        };

    "wasm_loader/a guest without a memory export cannot receive arguments"_test =
        [] {
            refrain::test_support::scratch_directory scratch;
            auto store_root{install_guest(scratch, no_memory_wat)};
            expect(store_root.has_value());
            if (!store_root) {
                return;
            }
            auto loader{granted_loader()};
            auto handle{
                load_guest_direct(loader, guest_package_root(*store_root))};
            expect(handle.has_value());
            if (!handle) {
                return;
            }
            const std::array<std::byte, 2> arguments{std::byte{1},
                                                     std::byte{2}};
            auto refused{
                call_guest_direct(loader, *handle, "on_event", arguments)};
            expect(refused.status ==
                   static_cast<refrain_status>(
                       script_host_abi::script_error::invalid_argument));
            expect(refused.diagnostic.find("linear memory") !=
                   std::string::npos)
                << refused.diagnostic;
            expect(loader.stats().quarantined_instances == 0U);

            auto empty{call_guest_direct(loader, *handle, "on_event", {})};
            expect(empty.status == REFRAIN_STATUS_OK);
            expect(empty.value == 0);
        };

    "wasm_loader/a callback that exhausts its call budget is quarantined"_test =
        [] {
            refrain::test_support::scratch_directory scratch;
            auto store_root{install_guest(scratch, spin_callback_wat)};
            expect(store_root.has_value());
            if (!store_root) {
                return;
            }
            auto loader{granted_loader(wasm::loader_budgets{
                .call_fuel = 10'000, .call_timeout_ms = 5'000})};
            auto handle{
                load_guest_direct(loader, guest_package_root(*store_root))};
            expect(handle.has_value());
            if (!handle) {
                return;
            }
            auto spin{call_guest_direct(loader, *handle, "spin", {})};
            expect(spin.status ==
                   static_cast<refrain_status>(
                       script_host_abi::script_error::budget_exceeded));
            expect(spin.diagnostic.find("instruction budget") !=
                   std::string::npos)
                << spin.diagnostic;
            expect(loader.stats().failed_calls == 1U);
            expect(loader.stats().quarantined_instances == 1U);

            auto again{call_guest_direct(loader, *handle, "spin", {})};
            expect(again.status ==
                   static_cast<refrain_status>(
                       script_host_abi::script_error::quarantined));
            expect(again.diagnostic.find("quarantined") != std::string::npos)
                << again.diagnostic;

            // Unload releases the quarantined store and every counter.
            expect(unload_guest_direct(loader, *handle));
            expect(loader.stats().live_instances == 0U);
            expect(loader.stats().live_module_bytes == 0U);
            expect(loader.stats().live_memory_bytes == 0U);
            expect(loader.stats().live_fuel == 0U);
            expect(loader.stats().quarantined_instances == 0U);
            expect(loader.stats().total_unloaded == 1U);
        };

    "wasm_loader/a callback that outlives its deadline is quarantined"_test =
        [] {
            refrain::test_support::scratch_directory scratch;
            auto store_root{install_guest(scratch, spin_callback_wat)};
            expect(store_root.has_value());
            if (!store_root) {
                return;
            }
            auto loader{granted_loader(wasm::loader_budgets{
                .call_fuel = 1'000'000'000, .call_timeout_ms = 10})};
            auto handle{
                load_guest_direct(loader, guest_package_root(*store_root))};
            expect(handle.has_value());
            if (!handle) {
                return;
            }
            const auto started{std::chrono::steady_clock::now()};
            auto spin{call_guest_direct(loader, *handle, "spin", {})};
            const auto elapsed{std::chrono::steady_clock::now() - started};
            expect(spin.status ==
                   static_cast<refrain_status>(
                       script_host_abi::script_error::deadline_exceeded));
            expect(spin.diagnostic.find("wall-clock deadline") !=
                   std::string::npos)
                << spin.diagnostic;
            expect(elapsed < std::chrono::seconds{10});
            expect(loader.stats().quarantined_instances == 1U);
        };

    "wasm_loader/a trapping callback is quarantined and reported"_test = [] {
        refrain::test_support::scratch_directory scratch;
        auto store_root{install_guest(scratch, trap_callback_wat)};
        expect(store_root.has_value());
        if (!store_root) {
            return;
        }
        auto loader{granted_loader()};
        auto handle{load_guest_direct(loader, guest_package_root(*store_root))};
        expect(handle.has_value());
        if (!handle) {
            return;
        }
        auto trapped{call_guest_direct(loader, *handle, "boom", {})};
        expect(trapped.status ==
               static_cast<refrain_status>(
                   script_host_abi::script_error::guest_trapped));
        expect(trapped.diagnostic.find("trapped") != std::string::npos)
            << trapped.diagnostic;
        expect(loader.stats().quarantined_instances == 1U);
    };

    "wasm_loader/each call provisions a fresh call budget"_test = [] {
        refrain::test_support::scratch_directory scratch;
        auto store_root{install_guest(scratch, burn_callback_wat)};
        expect(store_root.has_value());
        if (!store_root) {
            return;
        }
        // Measure one burn call's consumption with a generous budget.
        auto measuring{granted_loader(wasm::loader_budgets{
            .call_fuel = 1'000'000, .call_timeout_ms = 5'000})};
        auto measured_handle{
            load_guest_direct(measuring, guest_package_root(*store_root))};
        expect(measured_handle.has_value());
        if (!measured_handle) {
            return;
        }
        auto measured{
            call_guest_direct(measuring, *measured_handle, "burn", {})};
        expect(measured.status == REFRAIN_STATUS_OK);
        expect(measured.value == 3000);
        const auto remaining{measuring.stats().last_call_fuel_remaining};
        expect(remaining > 0U);
        expect(remaining < 1'000'000U);
        const auto consumed{1'000'000U - remaining};

        // A second loader starts with a load-path budget too small to run the
        // callback and provisions 1.5x one call per dispatch. Each call leaves
        // half a call of fuel behind, so the second success proves the budget
        // is re-provisioned rather than inherited from the first call.
        auto loader{granted_loader(
            wasm::loader_budgets{.load_fuel = 1'000,
                                 .call_fuel = consumed + consumed / 2,
                                 .call_timeout_ms = 5'000})};
        auto handle{load_guest_direct(loader, guest_package_root(*store_root))};
        expect(handle.has_value());
        if (!handle) {
            return;
        }
        auto first{call_guest_direct(loader, *handle, "burn", {})};
        expect(first.status == REFRAIN_STATUS_OK);
        expect(first.value == 3000);
        auto second{call_guest_direct(loader, *handle, "burn", {})};
        expect(second.status == REFRAIN_STATUS_OK);
        expect(second.value == 3000);
        expect(loader.stats().total_calls == 2U);
        expect(loader.stats().failed_calls == 0U);
    };

    "wasm_loader/a callback with the wrong ABI is refused without quarantine"_test =
        [] {
            refrain::test_support::scratch_directory scratch;
            auto store_root{install_guest(scratch, bad_abi_wat)};
            expect(store_root.has_value());
            if (!store_root) {
                return;
            }
            auto loader{granted_loader()};
            auto handle{
                load_guest_direct(loader, guest_package_root(*store_root))};
            expect(handle.has_value());
            if (!handle) {
                return;
            }
            const std::array<std::byte, 4> arguments{
                std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
            auto called{
                call_guest_direct(loader, *handle, "bad_abi", arguments)};
            expect(called.status ==
                   static_cast<refrain_status>(
                       script_host_abi::script_error::invalid_argument));
            expect(called.diagnostic.find("does not implement the (2 x i32) -> "
                                          "i32 call ABI") != std::string::npos)
                << called.diagnostic;
            expect(loader.stats().quarantined_instances == 0U);
            expect(loader.stats().failed_calls == 1U);
        };

    "wasm_loader/a forged import name is escaped in the refusal"_test = [] {
        refrain::test_support::scratch_directory scratch;
        auto store_root{install_guest(scratch, forged_import_wat)};
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
            expect(loaded.error().code == error_code::permission_denied);
            const auto &message{loaded.error().message};
            // The guest name bytes never reach the operator-facing message raw.
            expect(message.find('\n') == std::string::npos) << message;
            expect(message.find('\x1b') == std::string::npos) << message;
            expect(message.find("\\x0a") != std::string::npos) << message;
            expect(message.find("\\x1b") != std::string::npos) << message;
            expect(message.find("refrain.") != std::string::npos);
        }
        expect(loader.stats().live_instances == 0U);
    };

    "wasm_loader/a quarantined instance does not run its unload hook"_test =
        [] {
            refrain::test_support::scratch_directory scratch;
            auto store_root{install_guest(scratch, looping_unload_wat)};
            expect(store_root.has_value());
            if (!store_root) {
                return;
            }
            auto loader{granted_loader(wasm::loader_budgets{
                .call_fuel = 1'000'000'000, .call_timeout_ms = 10})};
            auto handle{
                load_guest_direct(loader, guest_package_root(*store_root))};
            expect(handle.has_value());
            if (!handle) {
                return;
            }
            auto spin{call_guest_direct(loader, *handle, "spin", {})};
            expect(spin.status ==
                   static_cast<refrain_status>(
                       script_host_abi::script_error::deadline_exceeded));
            expect(loader.stats().quarantined_instances == 1U);

            // The guest's `on_unload` loops forever; the clean unload must skip
            // it and release the store instead of entering guest code.
            const auto started{std::chrono::steady_clock::now()};
            expect(unload_guest_direct(loader, *handle));
            const auto elapsed{std::chrono::steady_clock::now() - started};
            expect(elapsed < std::chrono::seconds{5});
            expect(loader.stats().live_instances == 0U);
            expect(loader.stats().quarantined_instances == 0U);
        };

    "wasm_loader/a request naming another loader is refused"_test = [] {
        refrain::test_support::scratch_directory scratch;
        auto store_root{install_guest(scratch, import_guest_wat)};
        expect(store_root.has_value());
        if (!store_root) {
            return;
        }
        auto loader{granted_loader()};
        auto handle{load_guest_direct(loader, guest_package_root(*store_root))};
        expect(handle.has_value());
        if (!handle) {
            return;
        }
        const std::array<std::byte, 2> arguments{std::byte{1}, std::byte{2}};
        auto wrong_loader{call_guest_direct(loader, *handle, "on_event",
                                            arguments, "native")};
        expect(wrong_loader.status ==
               static_cast<refrain_status>(
                   script_host_abi::script_error::not_found));
        expect(wrong_loader.diagnostic.find("loader-local") !=
               std::string::npos)
            << wrong_loader.diagnostic;
        expect(loader.stats().failed_calls == 0U);
        expect(loader.stats().quarantined_instances == 0U);
    };

    "wasm_loader/call after unload reports a missing instance"_test = [] {
        refrain::test_support::scratch_directory scratch;
        auto store_root{install_guest(scratch, import_guest_wat)};
        expect(store_root.has_value());
        if (!store_root) {
            return;
        }
        auto loader{granted_loader()};
        auto handle{load_guest_direct(loader, guest_package_root(*store_root))};
        expect(handle.has_value());
        if (!handle) {
            return;
        }
        expect(unload_guest_direct(loader, *handle));
        auto called{call_guest_direct(loader, *handle, "on_event", {})};
        expect(called.status == static_cast<refrain_status>(
                                    script_host_abi::script_error::not_found));
        expect(called.diagnostic.find("owns no instance") != std::string::npos)
            << called.diagnostic;
    };
};

#endif // REFRAIN_WITH_WASM

} // namespace
