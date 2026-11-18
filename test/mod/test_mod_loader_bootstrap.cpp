// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// A mod package provides a namespaced `refrain:mod.loader` instance that other
// packages are dispatched through: the host loads the declared provider in a
// bootstrap phase, gates the registration on the gamepack plan and the loader
// that executes the provider, dispatches its guests through the package
// instance, records the declared contract and refuses to unload or replace a
// loader provider while the process runs.

#include <refrain/contracts/abi.hpp>
#include <refrain/contracts/services/mod_loader_abi.h>
#include <refrain/core/config.hpp>
#include <refrain/core/io.hpp>
#include <refrain/data/builtin_image.hpp>
#include <refrain/data/content_store.hpp>
#include <refrain/mod/discovery.hpp>
#include <refrain/mod/gamepack.hpp>
#include <refrain/mod/host.hpp>
#include <refrain/mod/loader.hpp>
#include <refrain/mod/manifest.hpp>
#include <refrain/mod/package.hpp>
#include <refrain/mod/rfmod.hpp>
#include <refrain/mod/static_mod.hpp>

#include "support/scratch_directory.hpp"

#include <boost/ut.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace boost::ut;

namespace config = refrain::core::config;
namespace data = refrain::data;
namespace io = refrain::core::io;
namespace mod = refrain::mod;
using refrain::error_code;

namespace fs = std::filesystem;

constexpr std::string_view loader_service{"refrain:mod.loader"};
constexpr std::string_view fixture_mod_id{"refrain.test.loader.fixture"};
constexpr std::string_view fixture_loader_id{"com.example.fixture"};
constexpr std::string_view fixture_loader_id_two{"com.example.fixture.two"};
constexpr std::string_view guest_mod_id{"refrain.test.loader.guest"};
constexpr std::string_view guest_payload{"guest payload bytes"};

/// State behind the fixture loader. The fake provider implements the loader
/// contract; the static fixture mod's factory registers it through the package
/// registration path.
struct fixture_loader_state {
    mod::host *owner{nullptr};
    std::string loader_id{std::string{fixture_loader_id}};
    /// Capability `provider`; a package-provided loader must name the mod that
    /// registers it.
    std::string provider{std::string{fixture_mod_id}};
    std::uint32_t abi_major{REFRAIN_SERVICES_MOD_LOADER_ABI_MAJOR};
    std::uint8_t unload{REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_UNLOAD_CLEAN};
    std::uint8_t sandboxed{0};
    std::uint8_t hot_install{1};
    /// The fixture factory registers `loader_id` when true.
    bool register_self{true};
    /// Register the same loader id a second time, which the seal must refuse.
    bool register_twice{false};
    /// Simulate a guest asking the host to register `register_instance` as a
    /// loader from inside its load callback.
    bool register_instance_on_load{false};
    std::string register_instance{};
    /// Report the sandbox flag the other way after registration, so the next
    /// dispatch sees declared-capability drift.
    bool drift_after_register{false};
    /// Read the guest payload through the install receipt on load.
    bool read_payload{false};
    std::string last_package_root{};
    std::vector<std::byte> payload{};
    std::string inner_error{};
    int capabilities_calls{0};
    int validate_calls{0};
    int load_calls{0};
    int unload_calls{0};
    std::uint64_t next_handle{0x100};
};

fixture_loader_state *g_fixture{nullptr};
fixture_loader_state *g_fixture_second{nullptr};
std::string g_factory_error{};

void fixture_capabilities(
    void *self, refrain_services_mod_loader_mod_loader_capabilities *out) {
    auto &state{*static_cast<fixture_loader_state *>(self)};
    ++state.capabilities_calls;
    out->struct_size = REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_CAPABILITIES_SIZE;
    out->loader_id = refrain::contracts::to_borrowed(state.loader_id);
    out->abi_major = state.abi_major;
    out->unload = state.unload;
    out->sandboxed = state.sandboxed;
    out->supports_hot_install = state.hot_install;
    out->provider = refrain::contracts::to_borrowed(state.provider);
}

/// Read one guest member through the existing receipt/CAS package API. The
/// loader receives the package root; the member path is the loader's own
/// package convention because the contract does not forward custom `[entry]`
/// keys.
[[nodiscard]] auto read_guest_payload(fixture_loader_state &state,
                                      const refrain_str &root,
                                      const refrain_str &version) -> bool {
    const std::string package_root{refrain::contracts::as_string_view(root)};
    if (package_root.empty()) {
        state.inner_error = "loader received no package root for a guest";
        return false;
    }
    const auto receipt{
        fmt::format("{}/{}.json", package_root,
                    refrain::contracts::as_string_view(version))};
    auto source{mod::make_receipt_package_source(receipt)};
    if (!source) {
        state.inner_error = fmt::format("cannot open guest receipt '{}': {}",
                                        receipt, source.error().message);
        return false;
    }
    auto bytes{(*source)->read("data/payload.bin")};
    if (!bytes) {
        state.inner_error = fmt::format("cannot read guest payload member: {}",
                                        bytes.error().message);
        return false;
    }
    state.payload = std::move(*bytes);
    return true;
}

auto fixture_validate(void *self,
                      const refrain_services_mod_loader_mod_entry_manifest *)
    -> refrain_status {
    auto &state{*static_cast<fixture_loader_state *>(self)};
    ++state.validate_calls;
    return REFRAIN_STATUS_OK;
}

auto fixture_load(void *self,
                  const refrain_services_mod_loader_mod_load_request *request,
                  refrain_services_mod_loader_handle_mod_instance *out)
    -> refrain_status {
    auto &state{*static_cast<fixture_loader_state *>(self)};
    ++state.load_calls;
    *out = REFRAIN_HANDLE_INVALID;
    if (request == nullptr) {
        return static_cast<refrain_status>(
            REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_ERROR_INVALID_ARGUMENT);
    }
    expect(refrain::contracts::as_string_view(request->manifest.loader) ==
           std::string_view{state.loader_id});
    state.last_package_root = std::string{
        refrain::contracts::as_string_view(request->manifest.package_root)};
    if (state.read_payload && request->manifest.activate_code != 0) {
        if (!read_guest_payload(state, request->manifest.package_root,
                                request->manifest.version)) {
            mod::set_loader_diagnostic(state.inner_error);
            return static_cast<refrain_status>(
                REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_ERROR_IO_ERROR);
        }
    }
    if (state.register_instance_on_load && state.owner != nullptr) {
        auto registered{state.owner->register_provider(
            std::string{loader_service}, state.register_instance,
            mod::provider_api{
                .vtable = nullptr, .self = nullptr, .abi_major = 0})};
        if (!registered) {
            state.inner_error = registered.error().message;
            mod::set_loader_diagnostic(state.inner_error);
            return static_cast<refrain_status>(
                REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_ERROR_PERMISSION_DENIED);
        }
    }
    *out = state.next_handle++;
    return REFRAIN_STATUS_OK;
}

auto fixture_unload(void *self, refrain_services_mod_loader_handle_mod_instance,
                    refrain_services_mod_loader_mod_unload_reason)
    -> refrain_status {
    auto &state{*static_cast<fixture_loader_state *>(self)};
    ++state.unload_calls;
    return REFRAIN_STATUS_OK;
}

auto fixture_reload(void *, refrain_services_mod_loader_handle_mod_instance,
                    const refrain_services_mod_loader_mod_load_request *,
                    refrain_services_mod_loader_handle_mod_instance *)
    -> refrain_status {
    return REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_ERROR_UNSUPPORTED;
}

[[nodiscard]] auto fixture_vtable()
    -> const refrain_services_mod_loader_vtable & {
    static const refrain_services_mod_loader_vtable table{
        .struct_size = REFRAIN_SERVICES_MOD_LOADER_VTABLE_SIZE,
        .abi_major = REFRAIN_SERVICES_MOD_LOADER_ABI_MAJOR,
        .abi_minor = REFRAIN_SERVICES_MOD_LOADER_ABI_MINOR,
        .abi_patch = REFRAIN_SERVICES_MOD_LOADER_ABI_PATCH,
        .capabilities = &fixture_capabilities,
        .validate = &fixture_validate,
        .load = &fixture_load,
        .unload = &fixture_unload,
        .reload = &fixture_reload,
    };
    return table;
}

[[nodiscard]] auto fixture_api(fixture_loader_state &state)
    -> mod::provider_api {
    return mod::provider_api{
        .vtable = &fixture_vtable(),
        .self = &state,
        .abi_major = REFRAIN_SERVICES_MOD_LOADER_ABI_MAJOR,
    };
}

/// Static fixture mod: its factory registers the loader instance(s).
auto fixture_factory(mod::host &host) -> refrain::status {
    if (g_fixture == nullptr) {
        return std::unexpected{refrain::make_error(
            error_code::unknown, "fixture loader state is not set")};
    }
    const auto register_state{
        [&host](fixture_loader_state &state) -> refrain::status {
            auto registered{host.register_provider(std::string{loader_service},
                                                   state.loader_id,
                                                   fixture_api(state))};
            if (!registered) {
                g_factory_error = registered.error().message;
                return std::unexpected{registered.error()};
            }
            if (state.drift_after_register) {
                state.sandboxed = 1;
            }
            return refrain::status_ok();
        }};
    if (g_fixture->register_self) {
        if (auto registered{register_state(*g_fixture)}; !registered) {
            return registered;
        }
    }
    if (g_fixture->register_twice) {
        if (auto again{register_state(*g_fixture)}; !again) {
            return again;
        }
    }
    if (g_fixture_second != nullptr && g_fixture_second->register_self) {
        if (auto registered{register_state(*g_fixture_second)}; !registered) {
            return registered;
        }
    }
    return refrain::status_ok();
}

/// The impostor's factory registers the fixture provider's loader id.
auto impostor_factory(mod::host &host) -> refrain::status {
    if (g_fixture == nullptr) {
        return std::unexpected{refrain::make_error(
            error_code::unknown, "fixture loader state is not set")};
    }
    auto registered{host.register_provider(std::string{loader_service},
                                           std::string{fixture_loader_id},
                                           fixture_api(*g_fixture))};
    if (!registered) {
        g_factory_error = registered.error().message;
        return std::unexpected{registered.error()};
    }
    return refrain::status_ok();
}

/// A staged addition that tries to register a loader at runtime.
auto late_loader_factory(mod::host &host) -> refrain::status {
    if (g_fixture == nullptr) {
        return std::unexpected{refrain::make_error(
            error_code::unknown, "fixture loader state is not set")};
    }
    auto registered{host.register_provider(std::string{loader_service},
                                           "com.example.late",
                                           fixture_api(*g_fixture))};
    if (!registered) {
        g_factory_error = registered.error().message;
        return std::unexpected{registered.error()};
    }
    return refrain::status_ok();
}

[[nodiscard]] auto add_fixture_descriptor(mod::static_mod_registry &registry,
                                          mod::static_mod_factory factory)
    -> bool {
    return registry
        .add(mod::static_mod_descriptor{.id = fixture_mod_id,
                                        .version = "1.0.0",
                                        .abi = 1,
                                        .factory = factory})
        .has_value();
}

// ---------------------------------------------------------------------------
// manifests, image, plan, package helpers
// ---------------------------------------------------------------------------

[[nodiscard]] auto fixture_manifest() -> std::string {
    return "[mod]\n"
           "id = \"refrain.test.loader.fixture\"\n"
           "version = \"1.0.0\"\n"
           "abi = \"1\"\n"
           "side = \"server\"\n"
           "\n[provides]\n"
           "services = [\"refrain:mod.loader\"]\n";
}

[[nodiscard]] auto fixture_manifest_other() -> std::string {
    return "[mod]\n"
           "id = \"refrain.test.loader.impostor\"\n"
           "version = \"1.0.0\"\n"
           "abi = \"1\"\n"
           "side = \"server\"\n"
           "\n[provides]\n"
           "services = [\"refrain:mod.loader\"]\n";
}

[[nodiscard]] auto late_manifest() -> std::string {
    return "[mod]\n"
           "id = \"refrain.test.loader.late\"\n"
           "version = \"1.0.0\"\n"
           "abi = \"1\"\n"
           "side = \"server\"\n"
           "\n[provides]\n"
           "services = [\"refrain:mod.loader\"]\n";
}

[[nodiscard]] auto guest_manifest(std::string_view id, std::string_view loader)
    -> std::string {
    return fmt::format("[mod]\n"
                       "id = \"{}\"\n"
                       "version = \"1.0.0\"\n"
                       "abi = \"1\"\n"
                       "side = \"server\"\n"
                       "\n[entry]\n"
                       "loader = \"{}\"\n"
                       "payload = \"payload.bin\"\n",
                       id, loader);
}

struct image_builder {
    std::vector<data::builtin_image_input> inputs{};

    void add(std::string id, std::string manifest) {
        const auto bytes{std::as_bytes(
            std::span<const char>{manifest.data(), manifest.size()})};
        std::vector<std::byte> owned{bytes.begin(), bytes.end()};
        inputs.push_back(data::builtin_image_input{
            .path = "mods/" + std::move(id) + "/manifest.toml",
            .data = std::move(owned),
        });
    }

    [[nodiscard]] auto build() -> std::vector<std::byte> {
        auto bytes{data::build_builtin_image(inputs)};
        expect(static_cast<bool>(bytes)) << "synthetic image must build";
        return bytes ? *bytes : std::vector<std::byte>{};
    }
};

[[nodiscard]] auto make_spec(std::string_view mods_list,
                             std::string_view loaders = {},
                             std::string_view providers = {})
    -> refrain::result<mod::gamepack_spec> {
    auto text{std::string{"[gamepack]\n"}};
    text += "id = \"refrain.test.loader.bootstrap\"\n";
    text += "version = \"1.0.0\"\n";
    text += "side = \"server\"\n";
    text += "abi = \"1\"\n";
    text += "auto_scan = false\n\n[mods]\nlist = [";
    text += mods_list;
    text += "]\n";
    if (!providers.empty()) {
        text += "\n[providers]\n";
        text += providers;
    }
    if (!loaders.empty()) {
        text += "\n[loaders]\n";
        text += loaders;
    }
    auto doc{config::document::parse_text(text, "<gamepack>")};
    if (!doc) {
        return std::unexpected{doc.error()};
    }
    return mod::parse_gamepack(*doc, "server");
}

/// Runtime plan for a built-in image catalog: the candidates carry the real
/// manifest hashes and entry points a hot epoch diffs against.
[[nodiscard]] auto make_image_plan(std::string_view mods_list,
                                   std::string_view loaders,
                                   std::span<const std::byte> image,
                                   std::string_view providers = {})
    -> refrain::result<mod::composition> {
    auto spec{make_spec(mods_list, loaders, providers)};
    if (!spec) {
        return std::unexpected{spec.error()};
    }
    auto catalog{mod::scan_builtin_image(image)};
    if (!catalog) {
        return std::unexpected{catalog.error()};
    }
    return mod::resolve(*spec, *catalog);
}

/// Hand-built composition for a case the resolver must reject (a loader
/// provider executed by a sandboxed loader) but the host must still refuse.
/// Candidates keep real manifest hashes; only the loader declaration is
/// injected.
[[nodiscard]] auto
make_hand_built_plan(const mod::gamepack_spec &spec,
                     std::span<const std::byte> image,
                     std::vector<mod::resolved_loader> loaders)
    -> refrain::result<mod::composition> {
    auto catalog{mod::scan_builtin_image(image)};
    if (!catalog) {
        return std::unexpected{catalog.error()};
    }
    mod::composition plan{};
    plan.spec = spec;
    plan.format_version = mod::mod_lock_format_version;
    plan.gamepack_id = spec.id;
    plan.gamepack_version = spec.version;
    plan.loaders = std::move(loaders);
    for (const auto &entry : spec.mods) {
        const auto found{
            std::find_if(catalog->begin(), catalog->end(),
                         [&entry](const mod::candidate &candidate) {
                             return candidate.id == entry.ref.id;
                         })};
        if (found == catalog->end()) {
            return std::unexpected{refrain::make_error(
                error_code::not_found,
                fmt::format("catalog has no mod '{}'", entry.ref.id))};
        }
        plan.load_order.push_back(*found);
    }
    return plan;
}

[[nodiscard]] auto write_text_file(const fs::path &path, std::string_view text)
    -> bool {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) {
        return false;
    }
    return io::write_text(path.string(), text).has_value();
}

/// Pack one guest mod with a real payload member and install it into a fresh
/// content store; returns the store root.
[[nodiscard]] auto
install_guest(refrain::test_support::scratch_directory &scratch,
              std::string_view manifest, std::string_view payload)
    -> std::optional<std::string> {
    const auto mod_dir{scratch.path() / "guest"};
    if (!write_text_file(mod_dir / "manifest.toml", manifest)) {
        return std::nullopt;
    }
    const auto payload_bytes{
        std::as_bytes(std::span<const char>{payload.data(), payload.size()})};
    const std::vector<std::byte> bytes{payload_bytes.begin(),
                                       payload_bytes.end()};
    // The loader defines the guest package layout: a member must live under a
    // shipped top-level directory (`data/`, `assets/`, `scripts/`, `lib/`) for
    // the packer to record it.
    std::error_code error;
    fs::create_directories(mod_dir / "data", error);
    if (error) {
        return std::nullopt;
    }
    if (!io::write_file((mod_dir / "data" / "payload.bin").string(), bytes)
             .has_value()) {
        return std::nullopt;
    }
    const auto archive{scratch.path() / "guest.rfmod"};
    auto packed{mod::pack_mod_directory(mod_dir.string(), archive.string())};
    if (!packed) {
        return std::nullopt;
    }
    const auto store_root{scratch.path() / "store"};
    auto store{data::content_store::open(store_root.string())};
    if (!store) {
        return std::nullopt;
    }
    auto reader{mod::rfmod_reader::open_file(archive.string())};
    if (!reader) {
        return std::nullopt;
    }
    auto installed{mod::install_rfmod(*reader, *store)};
    if (!installed) {
        return std::nullopt;
    }
    return store_root.string();
}

const suite loader_bootstrap_tests = [] {
    "loader_bootstrap/loads and unloads an installed guest end to end"_test =
        [] {
            refrain::test_support::scratch_directory scratch;
            fixture_loader_state state{};
            state.read_payload = true;
            g_fixture = &state;
            g_fixture_second = nullptr;
            g_factory_error.clear();
            mod::static_mod_registry registry{};
            expect(add_fixture_descriptor(registry, &fixture_factory));

            image_builder builder{};
            builder.add(std::string{fixture_mod_id}, fixture_manifest());
            const auto image{builder.build()};
            auto store_root{install_guest(
                scratch, guest_manifest(guest_mod_id, fixture_loader_id),
                guest_payload)};
            expect(store_root.has_value());
            if (!store_root) {
                return;
            }
            auto spec{make_spec("\"refrain.test.loader.guest@1.0.0\", "
                                "\"refrain.test.loader.fixture@1.0.0\"",
                                "\"com.example.fixture\" = "
                                "\"refrain.test.loader.fixture@^1.0.0\"\n")};
            expect(static_cast<bool>(spec));
            if (!spec) {
                return;
            }
            const std::vector<std::string> roots{*store_root};
            auto plan{mod::resolve_installed_composition(
                *spec, roots,
                mod::composition_load_options{.builtin_image = image})};
            expect(static_cast<bool>(plan))
                << (plan ? std::string{"resolve ok"} : plan.error().message);
            if (!plan) {
                return;
            }
            expect(plan->loaders.size() == 1U);
            expect(plan->load_order.size() == 2U);
            if (plan->load_order.size() == 2U) {
                expect(plan->load_order[0].id == fixture_mod_id);
                expect(plan->load_order[1].kind == mod::package_kind::rfmod);
            }

            mod::host host{registry};
            state.owner = &host;
            auto loaded{host.load(*plan, image)};
            expect(static_cast<bool>(loaded))
                << (loaded ? std::string{"load ok"} : loaded.error().message);
            if (!loaded) {
                return;
            }
            // The fixture loader read the guest payload through the install
            // receipt and the content store.
            const auto expected_bytes{std::as_bytes(std::span<const char>{
                guest_payload.data(), guest_payload.size()})};
            const std::vector<std::byte> expected{expected_bytes.begin(),
                                                  expected_bytes.end()};
            expect(state.load_calls == 1);
            expect(state.payload == expected);
            expect(state.last_package_root ==
                   (fs::path{*store_root} / "mods" / std::string{guest_mod_id})
                       .string());

            expect(host.mods().size() == 2U);
            if (host.mods().size() == 2U) {
                expect(host.mods()[0].id == fixture_mod_id);
                const auto &guest{host.mods()[1]};
                expect(guest.id == guest_mod_id);
                expect(guest.loader == fixture_loader_id);
                expect(guest.loader_unload == mod::mod_unload_semantics::clean);
                expect(guest.instance_handle == 0x100U);
                expect(guest.external);
            }
            const auto *record{host.find_loader(fixture_loader_id)};
            expect(record != nullptr);
            if (record != nullptr) {
                expect(!record->host_owned);
                expect(record->provider == fixture_mod_id);
                expect(record->unload == mod::mod_unload_semantics::clean);
            }
            const auto active{host.services().active_provider(
                loader_service, fixture_loader_id)};
            expect(active.has_value());
            if (active.has_value()) {
                expect(active->id == fixture_loader_id);
                expect(active->source == fixture_mod_id);
            }

            // A loader provider with a live guest cannot be unloaded.
            auto live_guest_refused{
                host.stage_uninstall(std::string{fixture_mod_id})};
            expect(!live_guest_refused);
            if (!live_guest_refused) {
                expect(live_guest_refused.error().code ==
                       error_code::unsupported);
                expect(live_guest_refused.error().message.find(
                           fixture_loader_id) != std::string::npos);
                expect(live_guest_refused.error().message.find("restart") !=
                       std::string::npos);
            }

            // A staged composition cannot change the loader plan.
            auto repointed{host.active_composition()};
            repointed.loaders.push_back(mod::resolved_loader{
                .id = std::string{fixture_loader_id_two},
                .provider = fmt::format("{}@1.0.0", fixture_mod_id)});
            auto plan_changed{host.stage_composition(repointed, {})};
            expect(!plan_changed);
            if (!plan_changed) {
                expect(plan_changed.error().code == error_code::unsupported);
                expect(plan_changed.error().message.find(
                           fixture_loader_id_two) != std::string::npos);
                expect(plan_changed.error().message.find("restart") !=
                       std::string::npos);
            }
            // A staged reload of the provider is restart-required as well.
            auto reload_refused{host.stage_composition(
                host.active_composition(), {},
                mod::stage_options{.reload = {std::string{fixture_mod_id}}})};
            expect(!reload_refused);
            if (!reload_refused) {
                expect(reload_refused.error().code == error_code::unsupported);
                expect(reload_refused.error().message.find(fixture_loader_id) !=
                       std::string::npos);
            }

            // The guest unloads through the package-provided loader.
            auto staged{host.stage_uninstall(std::string{guest_mod_id})};
            expect(static_cast<bool>(staged))
                << (staged ? std::string{"stage ok"} : staged.error().message);
            if (!staged) {
                return;
            }
            expect(static_cast<bool>(host.commit_epoch()));
            expect(state.unload_calls == 1);
            expect(host.find(guest_mod_id) == nullptr);

            auto refused{host.stage_uninstall(std::string{fixture_mod_id})};
            expect(!refused);
            if (!refused) {
                expect(refused.error().code == error_code::unsupported);
                expect(refused.error().message.find(fixture_loader_id) !=
                       std::string::npos);
            }
            expect(host.find(fixture_mod_id) != nullptr);
        };

    "loader_bootstrap/one provider mod may register several loader ids"_test =
        [] {
            fixture_loader_state first{};
            fixture_loader_state second{};
            second.loader_id = std::string{fixture_loader_id_two};
            second.next_handle = 0x200;
            g_fixture = &first;
            g_fixture_second = &second;
            g_factory_error.clear();
            mod::static_mod_registry registry{};
            expect(add_fixture_descriptor(registry, &fixture_factory));

            image_builder builder{};
            builder.add(std::string{fixture_mod_id}, fixture_manifest());
            builder.add("refrain.test.loader.guest.one",
                        guest_manifest("refrain.test.loader.guest.one",
                                       fixture_loader_id));
            builder.add("refrain.test.loader.guest.two",
                        guest_manifest("refrain.test.loader.guest.two",
                                       fixture_loader_id_two));
            const auto image{builder.build()};
            auto plan{
                make_image_plan("\"refrain.test.loader.guest.one@1.0.0\", "
                                "\"refrain.test.loader.guest.two@1.0.0\", "
                                "\"refrain.test.loader.fixture@1.0.0\"",
                                "\"com.example.fixture\" = "
                                "\"refrain.test.loader.fixture@^1.0.0\"\n"
                                "\"com.example.fixture.two\" = "
                                "\"refrain.test.loader.fixture@^1.0.0\"\n",
                                image)};
            expect(static_cast<bool>(plan))
                << (plan ? std::string{"resolve ok"} : plan.error().message);
            if (!plan) {
                return;
            }
            expect(plan->loaders.size() == 2U);
            mod::host host{registry};
            first.owner = &host;
            second.owner = &host;
            auto loaded{host.load(*plan, image)};
            expect(static_cast<bool>(loaded))
                << (loaded ? std::string{"load ok"} : loaded.error().message);
            if (!loaded) {
                return;
            }
            expect(host.loaders().size() == 4U);
            expect(host.mods().size() == 3U);
            const auto *one{host.find_loader(fixture_loader_id)};
            const auto *two{host.find_loader(fixture_loader_id_two)};
            expect(one != nullptr);
            expect(two != nullptr);
            if (one != nullptr && two != nullptr) {
                expect(one->id == fixture_loader_id);
                expect(two->id == fixture_loader_id_two);
                expect(one->provider == fixture_mod_id);
                expect(two->provider == fixture_mod_id);
            }
            expect(host.services()
                       .active_provider(loader_service, fixture_loader_id)
                       .has_value());
            expect(host.services()
                       .active_provider(loader_service, fixture_loader_id_two)
                       .has_value());
            expect(host.find("refrain.test.loader.guest.one") != nullptr);
            expect(host.find("refrain.test.loader.guest.two") != nullptr);
            // Both guests ran through their own instance; a guest read from the
            // built-in image receives no package root.
            expect(first.validate_calls == 1);
            expect(second.validate_calls == 1);
            expect(first.last_package_root.empty());
            expect(second.last_package_root.empty());
        };

    "loader_bootstrap/refuses a declared loader that its provider did not "
    "register"_test = [] {
        fixture_loader_state state{};
        state.register_self = false;
        g_fixture = &state;
        g_fixture_second = nullptr;
        g_factory_error.clear();
        mod::static_mod_registry registry{};
        expect(add_fixture_descriptor(registry, &fixture_factory));

        image_builder builder{};
        builder.add(std::string{fixture_mod_id}, fixture_manifest());
        const auto image{builder.build()};
        auto plan{make_image_plan("\"refrain.test.loader.fixture@1.0.0\"",
                                  "\"com.example.fixture\" = "
                                  "\"refrain.test.loader.fixture@^1.0.0\"\n",
                                  image)};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        mod::host host{registry};
        state.owner = &host;
        auto loaded{host.load(*plan, image)};
        expect(!loaded);
        if (!loaded) {
            expect(loaded.error().code == error_code::not_found);
            expect(loaded.error().message.find(fixture_mod_id) !=
                   std::string::npos);
            expect(loaded.error().message.find(fixture_loader_id) !=
                   std::string::npos);
            expect(loaded.error().message.find("active mod loaders") !=
                   std::string::npos);
            expect(loaded.error().message.find("builtin") != std::string::npos);
        }
        expect(!host.loaded());
        expect(state.validate_calls == 0);
    };

    "loader_bootstrap/refuses a loader registration the plan does not "
    "declare"_test = [] {
        fixture_loader_state state{};
        state.loader_id = "com.example.other";
        g_fixture = &state;
        g_fixture_second = nullptr;
        g_factory_error.clear();
        mod::static_mod_registry registry{};
        expect(add_fixture_descriptor(registry, &fixture_factory));

        image_builder builder{};
        builder.add(std::string{fixture_mod_id}, fixture_manifest());
        const auto image{builder.build()};
        auto plan{
            make_image_plan("\"refrain.test.loader.fixture@1.0.0\"", {}, image,
                            "data = \"refrain.test.loader.fixture@^1.0.0\"\n")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        mod::host host{registry};
        state.owner = &host;
        auto loaded{host.load(*plan, image)};
        expect(!loaded);
        if (!loaded) {
            expect(loaded.error().code == error_code::permission_denied);
            expect(loaded.error().message.find("com.example.other") !=
                   std::string::npos);
            expect(loaded.error().message.find("declares no loader") !=
                   std::string::npos);
            expect(loaded.error().message.find("none") != std::string::npos);
        }
    };

    "loader_bootstrap/refuses a second registration of the same loader "
    "id"_test = [] {
        fixture_loader_state state{};
        state.register_twice = true;
        g_fixture = &state;
        g_fixture_second = nullptr;
        g_factory_error.clear();
        mod::static_mod_registry registry{};
        expect(add_fixture_descriptor(registry, &fixture_factory));

        image_builder builder{};
        builder.add(std::string{fixture_mod_id}, fixture_manifest());
        const auto image{builder.build()};
        auto plan{make_image_plan("\"refrain.test.loader.fixture@1.0.0\"",
                                  "\"com.example.fixture\" = "
                                  "\"refrain.test.loader.fixture@^1.0.0\"\n",
                                  image)};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        mod::host host{registry};
        state.owner = &host;
        auto loaded{host.load(*plan, image)};
        expect(!loaded);
        if (!loaded) {
            expect(loaded.error().code == error_code::already_exists);
            expect(loaded.error().message.find(fixture_loader_id) !=
                   std::string::npos);
        }
        expect(g_factory_error.find(fixture_loader_id) != std::string::npos);
    };

    "loader_bootstrap/refuses a loader registration attributed to another "
    "mod"_test = [] {
        fixture_loader_state state{};
        state.register_self = false;
        g_fixture = &state;
        g_fixture_second = nullptr;
        g_factory_error.clear();
        mod::static_mod_registry registry{};
        expect(add_fixture_descriptor(registry, &fixture_factory));
        expect(static_cast<bool>(registry.add(mod::static_mod_descriptor{
            .id = "refrain.test.loader.impostor",
            .version = "1.0.0",
            .abi = 1,
            .factory = &impostor_factory,
        })));

        image_builder builder{};
        builder.add(std::string{fixture_mod_id}, fixture_manifest());
        builder.add("refrain.test.loader.impostor", fixture_manifest_other());
        const auto image{builder.build()};
        auto plan{make_image_plan("\"refrain.test.loader.impostor@1.0.0\", "
                                  "\"refrain.test.loader.fixture@1.0.0\"",
                                  "\"com.example.impostor\" = "
                                  "\"refrain.test.loader.impostor@^1.0.0\"\n"
                                  "\"com.example.fixture\" = "
                                  "\"refrain.test.loader.fixture@^1.0.0\"\n",
                                  image)};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        mod::host host{registry};
        state.owner = &host;
        auto loaded{host.load(*plan, image)};
        expect(!loaded);
        if (!loaded) {
            expect(loaded.error().code == error_code::permission_denied);
            expect(loaded.error().message.find(
                       "refrain.test.loader.impostor") != std::string::npos);
            expect(loaded.error().message.find(fixture_mod_id) !=
                   std::string::npos);
            expect(loaded.error().message.find("declares mod") !=
                   std::string::npos);
        }
    };

    "loader_bootstrap/reserved loader ids stay host-owned"_test = [] {
        fixture_loader_state state{};
        state.loader_id = "native";
        g_fixture = &state;
        g_fixture_second = nullptr;
        g_factory_error.clear();
        mod::static_mod_registry registry{};
        expect(add_fixture_descriptor(registry, &fixture_factory));

        image_builder builder{};
        builder.add(std::string{fixture_mod_id}, fixture_manifest());
        const auto image{builder.build()};
        auto plan{
            make_image_plan("\"refrain.test.loader.fixture@1.0.0\"", {}, image,
                            "data = \"refrain.test.loader.fixture@^1.0.0\"\n")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        mod::host host{registry};
        state.owner = &host;
        auto loaded{host.load(*plan, image)};
        expect(!loaded);
        if (!loaded) {
            expect(loaded.error().code == error_code::permission_denied);
            expect(loaded.error().message.find("native") != std::string::npos);
            expect(loaded.error().message.find("reserved") !=
                   std::string::npos);
        }
        // A host bootstrap registration is host-owned: a provider that claims
        // a provider mod is refused instead of shadowing an embedder loader.
        fixture_loader_state foreign{};
        foreign.loader_id = "test.foreign";
        foreign.provider = "refrain.test.somebody";
        mod::host bootstrap_host{registry};
        auto foreign_status{bootstrap_host.register_mod_loader(
            "test.foreign", fixture_api(foreign))};
        expect(!foreign_status);
        if (!foreign_status) {
            expect(foreign_status.error().code == error_code::version_mismatch);
            expect(foreign_status.error().message.find("host bootstrap") !=
                   std::string::npos);
        }
    };

    "loader_bootstrap/a sandboxed guest cannot become a loader"_test = [] {
        fixture_loader_state sandbox{};
        sandbox.loader_id = "test.sandbox";
        sandbox.provider = {};
        sandbox.sandboxed = 1;
        sandbox.register_self = false;
        sandbox.register_instance_on_load = true;
        sandbox.register_instance = "com.example.inner";
        g_fixture = &sandbox;
        g_fixture_second = nullptr;
        g_factory_error.clear();
        mod::static_mod_registry registry{};

        image_builder builder{};
        builder.add("refrain.test.loader.sandboxed",
                    "[mod]\n"
                    "id = \"refrain.test.loader.sandboxed\"\n"
                    "version = \"1.0.0\"\n"
                    "abi = \"1\"\n"
                    "side = \"server\"\n"
                    "\n[provides]\n"
                    "services = [\"refrain:mod.loader\"]\n"
                    "\n[entry]\n"
                    "loader = \"test.sandbox\"\n"
                    "payload = \"guest.bin\"\n");
        const auto image{builder.build()};
        auto spec{make_spec("\"refrain.test.loader.sandboxed@1.0.0\"")};
        expect(static_cast<bool>(spec));
        if (!spec) {
            return;
        }
        auto plan{make_hand_built_plan(
            *spec, image,
            {mod::resolved_loader{.id = "com.example.inner",
                                  .provider =
                                      "refrain.test.loader.sandboxed@1.0.0"}})};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        mod::host host{registry};
        expect(static_cast<bool>(
            host.register_mod_loader("test.sandbox", fixture_api(sandbox))));
        sandbox.owner = &host;
        auto loaded{host.load(*plan, image)};
        expect(!loaded);
        if (!loaded) {
            expect(loaded.error().code == error_code::permission_denied);
            expect(loaded.error().message.find("sandboxed") !=
                   std::string::npos);
            expect(loaded.error().message.find("can never become a loader") !=
                   std::string::npos);
        }
        expect(sandbox.inner_error.find("sandboxed") != std::string::npos);
        expect(host.find_loader("test.sandbox") == nullptr);
    };

    "loader_bootstrap/capability drift after registration fails the next "
    "dispatch"_test = [] {
        fixture_loader_state state{};
        state.drift_after_register = true;
        g_fixture = &state;
        g_fixture_second = nullptr;
        g_factory_error.clear();
        mod::static_mod_registry registry{};
        expect(add_fixture_descriptor(registry, &fixture_factory));

        image_builder builder{};
        builder.add(std::string{fixture_mod_id}, fixture_manifest());
        builder.add("refrain.test.loader.drifting.guest",
                    guest_manifest("refrain.test.loader.drifting.guest",
                                   fixture_loader_id));
        const auto image{builder.build()};
        auto plan{
            make_image_plan("\"refrain.test.loader.fixture@1.0.0\", "
                            "\"refrain.test.loader.drifting.guest@1.0.0\"",
                            "\"com.example.fixture\" = "
                            "\"refrain.test.loader.fixture@^1.0.0\"\n",
                            image)};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        mod::host host{registry};
        state.owner = &host;
        auto loaded{host.load(*plan, image)};
        expect(!loaded);
        if (!loaded) {
            expect(loaded.error().code == error_code::version_mismatch);
            expect(loaded.error().message.find(fixture_loader_id) !=
                   std::string::npos);
            expect(loaded.error().message.find(fixture_mod_id) !=
                   std::string::npos);
            expect(loaded.error().message.find("capabilities") !=
                   std::string::npos);
        }
        // The drift is detected before the loader is called for the guest.
        expect(state.validate_calls == 0);
    };

    "loader_bootstrap/a staged addition cannot register a loader"_test = [] {
        fixture_loader_state state{};
        g_fixture = &state;
        g_fixture_second = nullptr;
        g_factory_error.clear();
        mod::static_mod_registry registry{};
        expect(add_fixture_descriptor(registry, &fixture_factory));
        expect(static_cast<bool>(registry.add(mod::static_mod_descriptor{
            .id = "refrain.test.loader.late",
            .version = "1.0.0",
            .abi = 1,
            .factory = &late_loader_factory,
        })));

        image_builder builder{};
        builder.add(std::string{fixture_mod_id}, fixture_manifest());
        builder.add("refrain.test.loader.base",
                    "[mod]\n"
                    "id = \"refrain.test.loader.base\"\n"
                    "version = \"1.0.0\"\n"
                    "abi = \"1\"\n"
                    "side = \"server\"\n");
        builder.add("refrain.test.loader.late", late_manifest());
        const auto image{builder.build()};
        auto initial{
            make_image_plan("\"refrain.test.loader.base@1.0.0\"", {}, image)};
        expect(static_cast<bool>(initial));
        if (!initial) {
            return;
        }
        auto next{make_image_plan(
            "\"refrain.test.loader.base@1.0.0\", "
            "\"refrain.test.loader.late@1.0.0\"",
            {}, image, "data = \"refrain.test.loader.late@^1.0.0\"\n")};
        expect(static_cast<bool>(next));
        if (!next) {
            return;
        }
        mod::host host{registry};
        state.owner = &host;
        auto loaded{host.load(*initial, image)};
        expect(static_cast<bool>(loaded))
            << (loaded ? std::string{"load ok"} : loaded.error().message);
        if (!loaded) {
            return;
        }
        auto resolver{mod::make_package_resolver(image)};
        expect(static_cast<bool>(resolver));
        if (!resolver) {
            return;
        }
        auto staged{host.stage_composition(*next, *resolver)};
        expect(!staged);
        if (!staged) {
            expect(staged.error().code == error_code::unsupported);
            expect(staged.error().message.find("staged") != std::string::npos);
            expect(staged.error().message.find("bootstrap composition") !=
                   std::string::npos);
        }
        expect(host.find("refrain.test.loader.late") == nullptr);
        expect(host.loaded());
    };

    "loader_bootstrap/a directory tree cannot provide a loader"_test = [] {
        fixture_loader_state state{};
        g_fixture = &state;
        g_fixture_second = nullptr;
        g_factory_error.clear();
        mod::static_mod_registry registry{};
        expect(add_fixture_descriptor(registry, &fixture_factory));

        refrain::test_support::scratch_directory scratch;
        const auto mod_dir{scratch.path() / "mods" /
                           std::string{fixture_mod_id}};
        expect(write_text_file(mod_dir / "manifest.toml", fixture_manifest()));
        auto scanned{mod::scan_mods_tree((scratch.path() / "mods").string())};
        expect(static_cast<bool>(scanned));
        if (!scanned) {
            return;
        }
        auto spec{make_spec("\"refrain.test.loader.fixture@1.0.0\"",
                            "\"com.example.fixture\" = "
                            "\"refrain.test.loader.fixture@^1.0.0\"\n")};
        expect(static_cast<bool>(spec));
        if (!spec) {
            return;
        }
        auto plan{mod::resolve(*spec, *scanned)};
        expect(static_cast<bool>(plan))
            << (plan ? std::string{"resolve ok"} : plan.error().message);
        if (!plan) {
            return;
        }
        mod::host host{registry};
        state.owner = &host;
        const mod::package_resolver resolver{
            [](const mod::candidate &entry)
                -> refrain::result<std::unique_ptr<mod::package_source>> {
                const auto root{fs::path{entry.path}.parent_path().string()};
                return mod::make_directory_package_source(root);
            }};
        auto loaded{host.load(*plan, resolver)};
        expect(!loaded);
        if (!loaded) {
            expect(loaded.error().code == error_code::permission_denied);
            expect(loaded.error().message.find("installed package") !=
                   std::string::npos);
            expect(loaded.error().message.find("install policy") !=
                   std::string::npos);
        }
        expect(!host.loaded());
    };
};

} // namespace
