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
#include <refrain/data/builtin_image.hpp>
#include <refrain/mod/discovery.hpp>
#include <refrain/mod/gamepack.hpp>
#include <refrain/mod/host.hpp>
#include <refrain/mod/loader.hpp>
#include <refrain/mod/manifest.hpp>
#include <refrain/mod/static_mod.hpp>

#include <boost/ut.hpp>

#include <fmt/format.h>

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

constexpr std::string_view loader_service{"refrain:mod.loader"};
constexpr std::string_view fixture_mod_id{"refrain.test.loader.fixture"};
constexpr std::string_view fixture_loader_id{"com.example.fixture"};

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
    /// Simulate a guest asking the host to register `register_instance` as a
    /// loader from inside its load callback.
    bool register_instance_on_load{false};
    std::string register_instance{};
    /// Report the sandbox flag the other way after registration, so the next
    /// dispatch sees declared-capability drift.
    bool drift_after_register{false};
    int capabilities_calls{0};
    int validate_calls{0};
    int load_calls{0};
    int unload_calls{0};
    std::uint64_t next_handle{0x100};
    std::string inner_error{};
};

fixture_loader_state *g_fixture{nullptr};
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
    if (request != nullptr) {
        expect(refrain::contracts::as_string_view(request->manifest.loader) ==
               std::string_view{state.loader_id});
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

/// Static fixture mod: its factory registers the loader instance.
auto fixture_factory(mod::host &host) -> refrain::status {
    if (g_fixture == nullptr) {
        return std::unexpected{refrain::make_error(
            error_code::unknown, "fixture loader state is not set")};
    }
    if (!g_fixture->register_self) {
        return refrain::status_ok();
    }
    auto registered{host.register_provider(std::string{loader_service},
                                           g_fixture->loader_id,
                                           fixture_api(*g_fixture))};
    if (!registered) {
        g_factory_error = registered.error().message;
        return std::unexpected{registered.error()};
    }
    if (g_fixture->drift_after_register) {
        g_fixture->sandboxed = 1;
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

[[nodiscard]] auto add_fixture_descriptor(mod::static_mod_registry &registry)
    -> bool {
    return registry
        .add(mod::static_mod_descriptor{.id = fixture_mod_id,
                                        .version = "1.0.0",
                                        .abi = 1,
                                        .factory = &fixture_factory})
        .has_value();
}

// ---------------------------------------------------------------------------
// manifests, image and plan helpers
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

[[nodiscard]] auto guest_manifest(std::string_view id, std::string_view loader)
    -> std::string {
    return fmt::format("[mod]\n"
                       "id = \"{}\"\n"
                       "version = \"1.0.0\"\n"
                       "abi = \"1\"\n"
                       "side = \"server\"\n"
                       "\n[entry]\n"
                       "loader = \"{}\"\n"
                       "payload = \"guest.bin\"\n",
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

/// Manifest-level plan: no package verification, all manifest hashes zero.
[[nodiscard]] auto make_plan(std::string_view mods_list,
                             std::string_view loaders = {},
                             std::string_view providers = {})
    -> refrain::result<mod::composition> {
    auto spec{make_spec(mods_list, loaders, providers)};
    if (!spec) {
        return std::unexpected{spec.error()};
    }
    return mod::preview_composition(*spec);
}

/// Runtime plan: the built-in image is the catalog, so the candidates carry
/// the real manifest hashes a hot epoch diffs against.
[[nodiscard]] auto make_resolved_plan(std::string_view mods_list,
                                      std::string_view loaders,
                                      std::span<const std::byte> image)
    -> refrain::result<mod::composition> {
    auto spec{make_spec(mods_list, loaders)};
    if (!spec) {
        return std::unexpected{spec.error()};
    }
    auto catalog{mod::scan_builtin_image(image)};
    if (!catalog) {
        return std::unexpected{catalog.error()};
    }
    return mod::resolve(*spec, *catalog);
}

const suite loader_bootstrap_tests = [] {
    "loader_bootstrap/loads and unloads a guest end to end"_test = [] {
        fixture_loader_state state{};
        g_fixture = &state;
        g_factory_error.clear();
        mod::static_mod_registry registry{};
        expect(add_fixture_descriptor(registry));

        image_builder builder{};
        builder.add(std::string{fixture_mod_id}, fixture_manifest());
        builder.add(
            "refrain.test.loader.guest",
            guest_manifest("refrain.test.loader.guest", fixture_loader_id));
        const auto image{builder.build()};
        auto plan{make_resolved_plan("\"refrain.test.loader.guest@1.0.0\", "
                                     "\"refrain.test.loader.fixture@1.0.0\"",
                                     "\"com.example.fixture\" = "
                                     "\"refrain.test.loader.fixture@^1.0.0\"\n",
                                     image)};
        expect(static_cast<bool>(plan))
            << (plan ? std::string{"resolve ok"} : plan.error().message);
        if (!plan) {
            return;
        }
        mod::host host{registry};
        state.owner = &host;
        auto loaded{host.load(*plan, image)};
        expect(static_cast<bool>(loaded))
            << (loaded ? std::string{"load ok"} : loaded.error().message);
        if (!loaded) {
            return;
        }
        // The bootstrap phase loaded the provider before the guest, even
        // though the gamepack lists the guest first.
        expect(host.mods().size() == 2U);
        if (host.mods().size() == 2U) {
            expect(host.mods()[0].id == fixture_mod_id);
            expect(host.mods()[1].id == "refrain.test.loader.guest");
            const auto &guest{host.mods()[1]};
            expect(guest.loader == fixture_loader_id);
            expect(guest.loader_unload == mod::mod_unload_semantics::clean);
            expect(guest.instance_handle == 0x100U);
        }
        expect(state.validate_calls == 1);
        expect(state.load_calls == 1);

        const auto *record{host.find_loader(fixture_loader_id)};
        expect(record != nullptr);
        if (record != nullptr) {
            expect(!record->host_owned);
            expect(record->provider == fixture_mod_id);
            expect(record->unload == mod::mod_unload_semantics::clean);
        }
        const auto active{
            host.services().active_provider(loader_service, fixture_loader_id)};
        expect(active.has_value());
        if (active.has_value()) {
            expect(active->id == fixture_mod_id);
        }
        expect(host.active_composition().loaders.size() == 1U);

        // A loader provider with a live guest cannot be unloaded.
        auto live_guest_refused{
            host.stage_uninstall(std::string{fixture_mod_id})};
        expect(!live_guest_refused);
        if (!live_guest_refused) {
            expect(live_guest_refused.error().code == error_code::unsupported);
            expect(live_guest_refused.error().message.find(fixture_loader_id) !=
                   std::string::npos);
            expect(live_guest_refused.error().message.find("restart") !=
                   std::string::npos);
        }

        // The guest unloads through the package-provided loader.
        auto staged{host.stage_uninstall("refrain.test.loader.guest")};
        expect(static_cast<bool>(staged))
            << (staged ? std::string{"stage ok"} : staged.error().message);
        if (staged) {
            expect(static_cast<bool>(host.commit_epoch()));
        }
        expect(state.unload_calls == 1);
        expect(host.find("refrain.test.loader.guest") == nullptr);

        // The loader provider is part of the bootstrap composition: it cannot
        // be hot-unloaded while, or after, its guests used it.
        auto refused{host.stage_uninstall(std::string{fixture_mod_id})};
        expect(!refused);
        if (!refused) {
            expect(refused.error().code == error_code::unsupported);
            expect(refused.error().message.find(fixture_loader_id) !=
                   std::string::npos);
            expect(refused.error().message.find(fixture_mod_id) !=
                   std::string::npos);
            expect(refused.error().message.find("restart") !=
                   std::string::npos);
        }
        expect(host.find(fixture_mod_id) != nullptr);
    };

    "loader_bootstrap/refuses a declared loader that its provider did not "
    "register"_test = [] {
        fixture_loader_state state{};
        state.register_self = false;
        g_fixture = &state;
        g_factory_error.clear();
        mod::static_mod_registry registry{};
        expect(add_fixture_descriptor(registry));

        auto plan{make_plan("\"refrain.test.loader.fixture@1.0.0\"",
                            "\"com.example.fixture\" = "
                            "\"refrain.test.loader.fixture@^1.0.0\"\n")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        image_builder builder{};
        builder.add(std::string{fixture_mod_id}, fixture_manifest());
        mod::host host{registry};
        state.owner = &host;
        auto loaded{host.load(*plan, builder.build())};
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
        g_factory_error.clear();
        mod::static_mod_registry registry{};
        expect(add_fixture_descriptor(registry));

        auto plan{make_plan("\"refrain.test.loader.fixture@1.0.0\"",
                            "\"com.example.fixture\" = "
                            "\"refrain.test.loader.fixture@^1.0.0\"\n")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        image_builder builder{};
        builder.add(std::string{fixture_mod_id}, fixture_manifest());
        mod::host host{registry};
        state.owner = &host;
        auto loaded{host.load(*plan, builder.build())};
        expect(!loaded);
        if (!loaded) {
            expect(loaded.error().code == error_code::permission_denied);
            expect(loaded.error().message.find("com.example.other") !=
                   std::string::npos);
            expect(loaded.error().message.find("declares no loader") !=
                   std::string::npos);
            expect(loaded.error().message.find("com.example.fixture") !=
                   std::string::npos);
        }
    };

    "loader_bootstrap/refuses a loader registration attributed to another "
    "mod"_test = [] {
        fixture_loader_state state{};
        state.register_self = false;
        g_fixture = &state;
        g_factory_error.clear();
        mod::static_mod_registry registry{};
        expect(add_fixture_descriptor(registry));
        // The impostor is a declared loader provider itself; its factory
        // registers the loader the plan attributes to the fixture provider.
        expect(static_cast<bool>(registry.add(mod::static_mod_descriptor{
            .id = "refrain.test.loader.impostor",
            .version = "1.0.0",
            .abi = 1,
            .factory = &impostor_factory,
        })));

        auto plan{make_plan("\"refrain.test.loader.impostor@1.0.0\", "
                            "\"refrain.test.loader.fixture@1.0.0\"",
                            "\"com.example.impostor\" = "
                            "\"refrain.test.loader.impostor@^1.0.0\"\n"
                            "\"com.example.fixture\" = "
                            "\"refrain.test.loader.fixture@^1.0.0\"\n")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        image_builder builder{};
        builder.add(std::string{fixture_mod_id}, fixture_manifest());
        builder.add("refrain.test.loader.impostor", fixture_manifest_other());
        mod::host host{registry};
        state.owner = &host;
        // The impostor is loaded first (it is first in the plan) and tries to
        // register the fixture's loader id; the gate rejects the attribution.
        auto loaded{host.load(*plan, builder.build())};
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
        g_factory_error.clear();
        mod::static_mod_registry registry{};
        expect(add_fixture_descriptor(registry));

        auto plan{make_plan("\"refrain.test.loader.fixture@1.0.0\"", {},
                            "data = \"refrain.test.loader.fixture@^1.0.0\"\n")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        image_builder builder{};
        builder.add(std::string{fixture_mod_id}, fixture_manifest());
        mod::host host{registry};
        state.owner = &host;
        auto loaded{host.load(*plan, builder.build())};
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
        // The bootstrap fake loader is sandboxed and, on load, simulates guest
        // code that asks the host to register itself as a loader.
        fixture_loader_state sandbox{};
        sandbox.loader_id = "test.sandbox";
        sandbox.provider = {};
        sandbox.sandboxed = 1;
        sandbox.register_self = false;
        sandbox.register_instance_on_load = true;
        sandbox.register_instance = "com.example.inner";
        g_fixture = &sandbox;
        g_factory_error.clear();
        mod::static_mod_registry registry{};

        auto plan{make_plan("\"refrain.test.loader.sandboxed@1.0.0\"",
                            "\"com.example.inner\" = "
                            "\"refrain.test.loader.sandboxed@^1.0.0\"\n")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
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
        mod::host host{registry};
        expect(static_cast<bool>(
            host.register_mod_loader("test.sandbox", fixture_api(sandbox))));
        sandbox.owner = &host;
        auto loaded{host.load(*plan, builder.build())};
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
        g_factory_error.clear();
        mod::static_mod_registry registry{};
        expect(add_fixture_descriptor(registry));

        auto plan{make_plan("\"refrain.test.loader.fixture@1.0.0\", "
                            "\"refrain.test.loader.drifting.guest@1.0.0\"",
                            "\"com.example.fixture\" = "
                            "\"refrain.test.loader.fixture@^1.0.0\"\n")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        image_builder builder{};
        builder.add(std::string{fixture_mod_id}, fixture_manifest());
        builder.add("refrain.test.loader.drifting.guest",
                    guest_manifest("refrain.test.loader.drifting.guest",
                                   fixture_loader_id));
        mod::host host{registry};
        state.owner = &host;
        auto loaded{host.load(*plan, builder.build())};
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
};

} // namespace
