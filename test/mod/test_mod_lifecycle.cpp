// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Hot mod lifecycle: staged install/uninstall/reload, tick-boundary registry
// epochs, generation-stale handles, restart-required refusals, native retire
// counting and quarantine. The fixtures are synthetic: a fake clean loader for
// the leaf-service paths and the real external native test module for the
// retire cap.

#include <refrain/app/host_context.hpp>
#include <refrain/app/mod_lifecycle.hpp>
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
#include <refrain/platform/process.hpp>

#include "support/scratch_directory.hpp"

#include <boost/ut.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
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
namespace core = refrain::host::core;
using refrain::error_code;

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// synthetic package helpers
// ---------------------------------------------------------------------------

[[nodiscard]] auto make_manifest(std::string_view id,
                                 std::string_view version = "1.0.0",
                                 std::string_view provides = {},
                                 std::string_view loader = {}) -> std::string {
    auto text{
        fmt::format("[mod]\nid = \"{}\"\nversion = \"{}\"\nabi = \"1\"\nside = "
                    "\"server\"\n",
                    id, version)};
    if (!provides.empty()) {
        text += fmt::format("\n[provides]\nservices = [\"{}\"]\n", provides);
    }
    if (!loader.empty()) {
        text += fmt::format("\n[entry]\nloader = \"{}\"\n", loader);
    }
    return text;
}

[[nodiscard]] auto make_gamepack(std::string_view mods_list,
                                 std::string_view providers)
    -> refrain::result<mod::composition> {
    auto text{std::string{"[gamepack]\n"}};
    text += "id = \"refrain.test.lifecycle\"\n";
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
    return mod::preview_composition(*spec);
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

[[nodiscard]] auto
install_archive(refrain::test_support::scratch_directory &scratch,
                const std::string &archive,
                std::string_view store_name = "store")
    -> std::optional<std::string> {
    const auto store_root{scratch.path() / std::string{store_name}};
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

/// Install one archive into an existing store (the command layer's durable
/// transaction when the store already exists).
[[nodiscard]] auto install_into_store(std::string_view store_root,
                                      const std::string &archive) -> bool {
    auto store{data::content_store::open_existing(store_root)};
    if (!store) {
        return false;
    }
    auto reader{mod::rfmod_reader::open_file(archive)};
    if (!reader) {
        return false;
    }
    return mod::install_rfmod(*reader, *store).has_value();
}

// ---------------------------------------------------------------------------
// fake clean loader: a sandbox-free loader whose declared removal guarantee is
// `clean`. It is the stand-in for the script loaders (wasm/lua) that unload
// completely; the real script loaders are not built into this host, so the
// lifecycle behaviour is exercised through the same loader slot.
// ---------------------------------------------------------------------------

constexpr std::string_view clean_loader_id{"com.example.clean"};

std::uint32_t g_clean_vtable[4]{16, 1, 0, 0};

struct clean_loader_state {
    mod::host *owner{nullptr};
    std::string loader_id{clean_loader_id};
    int load_calls{0};
    int reload_calls{0};
    int unload_calls{0};
    std::uint32_t last_reason{999};
    bool fail_validate{false};
    bool fail_unload{false};
    std::string last_error{};
    std::string guest_service{"refrain:leaf"};
    std::uint64_t next_handle{1};
    std::vector<std::unique_ptr<int>> selves{};

    [[nodiscard]] auto register_guest(std::string_view service) -> bool {
        auto self{std::make_unique<int>(0)};
        *self = static_cast<int>(selves.size() + 1U);
        const mod::provider_api api{
            .vtable = g_clean_vtable,
            .self = self.get(),
            .abi_major = 1,
        };
        selves.push_back(std::move(self));
        auto registered{owner->register_provider(service, api)};
        if (!registered) {
            last_error = registered.error().message;
            return false;
        }
        return true;
    }
};

void clean_capabilities(
    void *self, refrain_services_mod_loader_mod_loader_capabilities *out) {
    auto &state{*static_cast<clean_loader_state *>(self)};
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
    auto &state{*static_cast<clean_loader_state *>(self)};
    if (state.fail_validate) {
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
    if (request == nullptr || state.owner == nullptr) {
        return static_cast<refrain_status>(
            REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_ERROR_INVALID_ARGUMENT);
    }
    if (request->manifest.activate_code == 0) {
        return REFRAIN_STATUS_OK;
    }
    if (!state.register_guest(state.guest_service)) {
        return static_cast<refrain_status>(
            REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_ERROR_PERMISSION_DENIED);
    }
    *out = state.next_handle++;
    return REFRAIN_STATUS_OK;
}

auto clean_reload(void *self,
                  refrain_services_mod_loader_handle_mod_instance instance,
                  const refrain_services_mod_loader_mod_load_request *request,
                  refrain_services_mod_loader_handle_mod_instance *out)
    -> refrain_status {
    auto &state{*static_cast<clean_loader_state *>(self)};
    ++state.reload_calls;
    if (request == nullptr || state.owner == nullptr ||
        instance == REFRAIN_SERVICES_MOD_LOADER_HANDLE_MOD_INSTANCE_INVALID) {
        return static_cast<refrain_status>(
            REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_ERROR_INVALID_ARGUMENT);
    }
    if (request->manifest.activate_code != 0 &&
        !state.register_guest(state.guest_service)) {
        return static_cast<refrain_status>(
            REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_ERROR_PERMISSION_DENIED);
    }
    *out = state.next_handle++;
    return REFRAIN_STATUS_OK;
}

auto clean_unload(void *self,
                  refrain_services_mod_loader_handle_mod_instance instance,
                  refrain_services_mod_loader_mod_unload_reason reason)
    -> refrain_status {
    auto &state{*static_cast<clean_loader_state *>(self)};
    ++state.unload_calls;
    state.last_reason = static_cast<std::uint32_t>(reason);
    static_cast<void>(instance);
    if (state.fail_unload) {
        return static_cast<refrain_status>(
            REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_ERROR_INTERNAL);
    }
    return REFRAIN_STATUS_OK;
}

[[nodiscard]] auto clean_loader_vtable()
    -> const refrain_services_mod_loader_vtable & {
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

[[nodiscard]] auto register_clean_loader(mod::host &host,
                                         clean_loader_state &state) -> bool {
    state.owner = &host;
    const mod::provider_api api{
        .vtable = &clean_loader_vtable(),
        .self = &state,
        .abi_major = REFRAIN_SERVICES_MOD_LOADER_ABI_MAJOR,
    };
    return host.register_mod_loader(clean_loader_id, api).has_value();
}

[[nodiscard]] auto register_host_core(mod::host &host,
                                      refrain::app::host_context &context)
    -> bool {
    const mod::provider_api api{
        .vtable = &refrain::host::core_vtable(),
        .self = &context,
        .abi_major = REFRAIN_HOST_CORE_ABI_MAJOR,
    };
    return host.register_sealed_provider("refrain:host.core", api, "1.0.0")
        .has_value();
}

[[nodiscard]] auto
make_package_resolver_for(const std::vector<std::byte> &image)
    -> mod::package_resolver {
    auto resolver{mod::make_package_resolver(image)};
    expect(static_cast<bool>(resolver)) << "package resolver must build";
    return resolver ? *resolver : mod::package_resolver{};
}

} // namespace

namespace {

int g_static_factory_calls{0};

auto static_provider_factory(mod::host &owner) -> refrain::status {
    ++g_static_factory_calls;
    const mod::provider_api api{
        .vtable = g_clean_vtable,
        .self = &g_static_factory_calls,
        .abi_major = 1,
    };
    return owner.register_provider("refrain:data.packs", api);
}

const suite mod_lifecycle_tests = [] {
    "mod_lifecycle/hot reload advances the generation and stales the old "
    "handle"_test = [] {
        mod::static_mod_registry registry{};
        auto plan{make_gamepack("\"refrain.test.life.leaf@1.0.0\"",
                                "leaf = \"refrain.test.life.leaf@^1.0.0\"\n")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        image_builder builder{};
        builder.add("refrain.test.life.leaf",
                    make_manifest("refrain.test.life.leaf", "1.0.0",
                                  "refrain:leaf", clean_loader_id));
        const auto image{builder.build()};
        clean_loader_state loader{};
        mod::host host{registry};
        expect(register_clean_loader(host, loader));
        auto loaded{host.load(*plan, image)};
        expect(static_cast<bool>(loaded))
            << (loaded ? std::string{"load ok"}
                       : loaded.error().message + " / " + loader.last_error);
        if (!loaded) {
            return;
        }
        expect(host.epoch() == 1U);
        expect(host.mods().size() == 1U);
        const auto initial_digest{host.composition_digest()};
        const auto initial_epoch{host.epoch()};
        const auto initial_providers{host.services().provider_count()};
        expect(initial_epoch == 1U);
        const auto record{host.lifecycle_record("refrain.test.life.leaf")};
        expect(record.has_value());
        if (record) {
            expect(record->state == mod::mod_lifecycle_state::active);
            expect(record->generation == 1U);
            expect(record->loader == clean_loader_id);
        }
        const auto old_handle{host.services().query("refrain:leaf")};
        expect(old_handle.has_value());
        if (!old_handle) {
            return;
        }
        expect(old_handle->provider.generation == 1U);

        auto resolver{make_package_resolver_for(image)};
        auto staged{host.stage_composition(
            *plan, resolver,
            mod::stage_options{.reload = {"refrain.test.life.leaf"}})};
        expect(static_cast<bool>(staged))
            << (staged ? std::string{"staged"} : staged.error().message);
        if (!staged) {
            return;
        }
        expect(staged->reloaded ==
               std::vector<std::string>{"refrain.test.life.leaf"});
        expect(staged->added.empty());
        expect(staged->removed.empty());
        // Staging does not publish: the old generation is still current.
        expect(host.epoch() == initial_epoch);
        expect(host.services().validate(*old_handle).has_value());
        expect(loader.reload_calls == 1);
        expect(loader.unload_calls == 0);

        auto committed{host.commit_epoch()};
        expect(static_cast<bool>(committed))
            << (committed ? std::string{"committed"}
                          : committed.error().message);
        if (!committed) {
            return;
        }
        expect(committed->epoch == initial_epoch + 1);
        expect(host.epoch() == committed->epoch);
        expect(committed->reloaded ==
               std::vector<std::string>{"refrain.test.life.leaf"});
        expect(host.composition_digest() != initial_digest);
        // The old generation is stale; the loader unloaded it with the reload
        // reason after the switch (never before).
        const auto stale{host.services().validate(*old_handle)};
        expect(!stale.has_value());
        if (!stale) {
            expect(stale.error().code == error_code::stale);
        }
        expect(loader.unload_calls == 1);
        expect(loader.last_reason ==
               REFRAIN_SERVICES_MOD_LOADER_MOD_UNLOAD_REASON_RELOAD);
        const auto current{host.services().query("refrain:leaf")};
        expect(current.has_value());
        if (current) {
            expect(current->generation == 2U);
            expect(host.services().validate(*current).has_value());
        }
        const auto reloaded{host.lifecycle_record("refrain.test.life.leaf")};
        expect(reloaded.has_value());
        if (reloaded) {
            expect(reloaded->generation == 2U) << reloaded->generation;
            expect(!reloaded->quarantined);
        }
        // The retired candidate is dropped, so a reload does not grow the
        // registry.
        expect(host.services().provider_count() == initial_providers);
        expect(host.quarantined_mods().empty());
    };

    "mod_lifecycle/host core ABI stamps the leaf generation and the reload "
    "stales it"_test = [] {
        mod::static_mod_registry registry{};
        auto plan{make_gamepack("\"refrain.test.life.leaf@1.0.0\"",
                                "leaf = \"refrain.test.life.leaf@^1.0.0\"\n")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        image_builder builder{};
        builder.add("refrain.test.life.leaf",
                    make_manifest("refrain.test.life.leaf", "1.0.0",
                                  "refrain:leaf", clean_loader_id));
        const auto image{builder.build()};
        clean_loader_state loader{};
        mod::host host{registry};
        refrain::app::host_context context{host};
        expect(register_host_core(host, context));
        expect(register_clean_loader(host, loader));
        auto loaded{host.load(*plan, image)};
        expect(static_cast<bool>(loaded))
            << (loaded ? std::string{"load ok"}
                       : loaded.error().message + " / " + loader.last_error);
        if (!loaded) {
            return;
        }
        // The ABI table a mod receives: the sealed host core provider table.
        const auto host_core{refrain::host::make_core_provider(context)};

        // A sealed host table is process lifetime and generation free.
        const auto sealed{
            host_core.query_service_handle("refrain:host.core", {})};
        expect(sealed.has_value());
        if (sealed) {
            expect(sealed->struct_size ==
                   REFRAIN_HOST_CORE_PROVIDER_HANDLE_SIZE);
            expect(sealed->provider.struct_size ==
                   REFRAIN_HOST_CORE_PROVIDER_REF_SIZE);
            expect(sealed->provider.vtable ==
                   static_cast<const void *>(&refrain::host::core_vtable()));
            expect(sealed->generation == 0U);
        }
        expect(
            host_core.validate_handle("refrain:host.core", {}, 0).has_value());

        // An ordinary leaf provider carries the committed slot generation.
        const auto first{host_core.query_service_handle("refrain:leaf", {})};
        expect(first.has_value());
        if (!first) {
            return;
        }
        expect(first->generation == 1U);
        expect(first->epoch == host.epoch());
        expect(host_core.validate_handle("refrain:leaf", {}, first->generation)
                   .has_value());

        // An unknown service and an unknown instance are not_found, not stale.
        const auto missing{
            host_core.query_service_handle("refrain:missing", {})};
        expect(!missing.has_value());
        if (!missing) {
            expect(missing.error() ==
                   refrain::host::core::host_error::not_found);
        }
        const auto typo{host_core.query_service_handle("refrain:leaf", "typo")};
        expect(!typo.has_value());
        if (!typo) {
            expect(typo.error() == refrain::host::core::host_error::not_found);
        }

        // A staged reload does not publish: the cached generation stays
        // current until the tick-boundary commit.
        auto resolver{make_package_resolver_for(image)};
        auto staged{host.stage_composition(
            *plan, resolver,
            mod::stage_options{.reload = {"refrain.test.life.leaf"}})};
        expect(static_cast<bool>(staged))
            << (staged ? std::string{"staged"} : staged.error().message);
        if (!staged) {
            return;
        }
        expect(host_core.validate_handle("refrain:leaf", {}, first->generation)
                   .has_value());
        auto committed{host.commit_epoch()};
        expect(static_cast<bool>(committed));
        if (!committed) {
            return;
        }
        // The pre-reload handle is stale through the ABI; re-querying returns
        // the new generation and the new table validates.
        const auto stale{
            host_core.validate_handle("refrain:leaf", {}, first->generation)};
        expect(!stale.has_value());
        if (!stale) {
            expect(stale.error() == refrain::host::core::host_error::stale);
        }
        const auto second{host_core.query_service_handle("refrain:leaf", {})};
        expect(second.has_value());
        if (!second) {
            return;
        }
        expect(second->generation == 2U);
        expect(host_core.validate_handle("refrain:leaf", {}, second->generation)
                   .has_value());

        // A retired instance: the query is not_found, the cached generation is
        // stale, and the sealed table is untouched.
        auto removal{host.stage_uninstall("refrain.test.life.leaf")};
        expect(static_cast<bool>(removal));
        if (!removal) {
            return;
        }
        expect(static_cast<bool>(host.commit_epoch()));
        const auto gone{host_core.query_service_handle("refrain:leaf", {})};
        expect(!gone.has_value());
        if (!gone) {
            expect(gone.error() == refrain::host::core::host_error::not_found);
        }
        const auto retired{
            host_core.validate_handle("refrain:leaf", {}, second->generation)};
        expect(!retired.has_value());
        if (!retired) {
            expect(retired.error() == refrain::host::core::host_error::stale);
        }
        expect(
            host_core.validate_handle("refrain:host.core", {}, 0).has_value());
    };

    "mod_lifecycle/hot uninstall drains the generation and reports the "
    "uninstall reason"_test = [] {
        mod::static_mod_registry registry{};
        auto plan{make_gamepack("\"refrain.test.life.leaf@1.0.0\"",
                                "leaf = \"refrain.test.life.leaf@^1.0.0\"\n")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        image_builder builder{};
        builder.add("refrain.test.life.leaf",
                    make_manifest("refrain.test.life.leaf", "1.0.0",
                                  "refrain:leaf", clean_loader_id));
        const auto image{builder.build()};
        clean_loader_state loader{};
        mod::host host{registry};
        expect(register_clean_loader(host, loader));
        auto loaded{host.load(*plan, image)};
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        const auto handle{host.services().query("refrain:leaf")};
        expect(handle.has_value());
        if (!handle) {
            return;
        }
        auto staged{host.stage_uninstall("refrain.test.life.leaf")};
        expect(static_cast<bool>(staged))
            << (staged ? std::string{"staged"} : staged.error().message);
        if (!staged) {
            return;
        }
        expect(staged->removed ==
               std::vector<std::string>{"refrain.test.life.leaf"});
        expect(host.mods().size() == 1U);
        expect(loader.unload_calls == 0);

        auto committed{host.commit_epoch()};
        expect(static_cast<bool>(committed));
        if (!committed) {
            return;
        }
        expect(host.mods().empty());
        expect(!host.services().active_provider("refrain:leaf").has_value());
        expect(!host.services().query("refrain:leaf").has_value());
        expect(loader.unload_calls == 1);
        expect(loader.last_reason ==
               REFRAIN_SERVICES_MOD_LOADER_MOD_UNLOAD_REASON_UNINSTALL);
        const auto stale{host.services().validate(*handle)};
        expect(!stale.has_value());
        if (!stale) {
            expect(stale.error().code == error_code::stale);
        }
        expect(!host.lifecycle_record("refrain.test.life.leaf").has_value());
        // A second removal is not found, not silently repeated.
        auto again{host.stage_uninstall("refrain.test.life.leaf")};
        expect(!again.has_value());
        if (!again) {
            expect(again.error().code == error_code::not_found);
        }
    };

    "mod_lifecycle/a call that does not drain quarantines the retired "
    "generation"_test = [] {
        mod::static_mod_registry registry{};
        auto plan{make_gamepack("\"refrain.test.life.leaf@1.0.0\"",
                                "leaf = \"refrain.test.life.leaf@^1.0.0\"\n")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        image_builder builder{};
        builder.add("refrain.test.life.leaf",
                    make_manifest("refrain.test.life.leaf", "1.0.0",
                                  "refrain:leaf", clean_loader_id));
        const auto image{builder.build()};
        clean_loader_state loader{};
        mod::host host{registry};
        expect(register_clean_loader(host, loader));
        auto loaded{host.load(*plan, image)};
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        // Hold one in-flight call across the commit: the bounded drain cannot
        // succeed, so the generation is quarantined instead of calling an
        // implementation the host can no longer trust.
        auto lease{host.services().acquire("refrain:leaf")};
        expect(lease.has_value());
        auto staged{host.stage_uninstall(
            "refrain.test.life.leaf",
            mod::stage_options{.drain_timeout =
                                   std::chrono::milliseconds{20}})};
        expect(static_cast<bool>(staged));
        if (!staged) {
            return;
        }
        auto committed{host.commit_epoch()};
        expect(static_cast<bool>(committed));
        if (!committed) {
            return;
        }
        expect(committed->quarantined ==
               std::vector<std::string>{"refrain.test.life.leaf"});
        expect(host.quarantined_mods().size() == 1U);
        // The unload hook must not run on an instance that may still be
        // executing: the mapping and the guest state stay until restart.
        expect(loader.unload_calls == 0);
        expect(host.mods().empty());
        expect(!host.services().query("refrain:leaf").has_value());
        const auto record{host.lifecycle_record("refrain.test.life.leaf")};
        expect(record.has_value());
        if (record) {
            expect(record->quarantined);
            expect(record->state == mod::mod_lifecycle_state::retired);
        }
        if (lease) {
            lease->release();
        }
        expect(host.services().in_flight("refrain:leaf") == 0U);
    };

    "mod_lifecycle/failed staging rolls back and keeps the running epoch"_test =
        [] {
            mod::static_mod_registry registry{};
            auto plan{
                make_gamepack("\"refrain.test.life.leaf@1.0.0\"",
                              "leaf = \"refrain.test.life.leaf@^1.0.0\"\n")};
            expect(static_cast<bool>(plan));
            if (!plan) {
                return;
            }
            image_builder builder{};
            builder.add("refrain.test.life.leaf",
                        make_manifest("refrain.test.life.leaf", "1.0.0",
                                      "refrain:leaf", clean_loader_id));
            const auto image{builder.build()};
            clean_loader_state loader{};
            mod::host host{registry};
            expect(register_clean_loader(host, loader));
            auto loaded{host.load(*plan, image)};
            expect(static_cast<bool>(loaded));
            if (!loaded) {
                return;
            }
            const auto epoch_before{host.epoch()};
            const auto digest_before{host.composition_digest()};
            const auto providers_before{host.services().provider_count()};
            const auto handle{host.services().query("refrain:leaf")};
            expect(handle.has_value());

            loader.fail_validate = true;
            auto resolver{make_package_resolver_for(image)};
            auto staged{host.stage_composition(
                *plan, resolver,
                mod::stage_options{.reload = {"refrain.test.life.leaf"}})};
            expect(!staged.has_value());
            expect(host.epoch() == epoch_before);
            expect(host.composition_digest() == digest_before);
            expect(!host.staged_change().has_value());
            expect(host.services().provider_count() == providers_before);
            if (handle) {
                expect(host.services().validate(*handle).has_value());
            }
            expect(loader.reload_calls == 0);
            expect(host.mods().size() == 1U);

            // The same change succeeds once the loader accepts it again.
            loader.fail_validate = false;
            auto retried{host.stage_composition(
                *plan, resolver,
                mod::stage_options{.reload = {"refrain.test.life.leaf"}})};
            expect(static_cast<bool>(retried));
            if (retried) {
                auto committed{host.commit_epoch()};
                expect(static_cast<bool>(committed));
                expect(host.epoch() == epoch_before + 1);
            }
        };

    "mod_lifecycle/static code and core providers are restart-required"_test =
        [] {
            // Static linked code cannot be unloaded in place.
            mod::static_mod_registry registry{};
            g_static_factory_calls = 0;
            expect(static_cast<bool>(registry.add(mod::static_mod_descriptor{
                .id = "refrain.test.life.static",
                .version = "1.0.0",
                .abi = 1,
                .factory = &static_provider_factory})));
            auto plan{
                make_gamepack("\"refrain.test.life.static@1.0.0\"",
                              "data = \"refrain.test.life.static@^1.0.0\"\n")};
            expect(static_cast<bool>(plan));
            if (!plan) {
                return;
            }
            image_builder builder{};
            builder.add("refrain.test.life.static",
                        make_manifest("refrain.test.life.static", "1.0.0",
                                      "refrain:data.packs"));
            mod::host host{registry};
            auto loaded{host.load(*plan, builder.build())};
            expect(static_cast<bool>(loaded))
                << (loaded ? std::string{"load ok"} : loaded.error().message);
            if (!loaded) {
                return;
            }
            expect(g_static_factory_calls == 1);
            auto removal{host.stage_uninstall("refrain.test.life.static")};
            expect(!removal.has_value());
            if (!removal) {
                expect(removal.error().code == error_code::unsupported);
                expect(removal.error().message.find("restart-required") !=
                       std::string::npos);
            }

            // A clean loader does not help when the mod provides a core
            // service the tick loop owns.
            mod::static_mod_registry core_registry{};
            auto core_plan{
                make_gamepack("\"refrain.test.life.sim@1.0.0\"",
                              "sim = \"refrain.test.life.sim@^1.0.0\"\n")};
            expect(static_cast<bool>(core_plan));
            if (!core_plan) {
                return;
            }
            image_builder core_builder{};
            core_builder.add("refrain.test.life.sim",
                             make_manifest("refrain.test.life.sim", "1.0.0",
                                           "refrain:sim", clean_loader_id));
            clean_loader_state core_loader{};
            core_loader.guest_service = "refrain:sim";
            mod::host core_host{core_registry};
            expect(register_clean_loader(core_host, core_loader));
            auto core_loaded{core_host.load(*core_plan, core_builder.build())};
            expect(static_cast<bool>(core_loaded))
                << (core_loaded ? std::string{"load ok"}
                                : core_loaded.error().message);
            if (!core_loaded) {
                return;
            }
            auto core_removal{
                core_host.stage_uninstall("refrain.test.life.sim")};
            expect(!core_removal.has_value());
            if (!core_removal) {
                expect(core_removal.error().code == error_code::unsupported);
                expect(core_removal.error().message.find("refrain:sim") !=
                       std::string::npos);
            }
            expect(core_host.epoch() == 1U);
        };

    "mod_lifecycle/sealed host services cannot be installed or replaced"_test =
        [] {
            mod::static_mod_registry registry{};
            auto plan{make_gamepack("\"refrain.test.life.anchor@1.0.0\"", "")};
            expect(static_cast<bool>(plan));
            if (!plan) {
                return;
            }
            image_builder builder{};
            builder.add("refrain.test.life.anchor",
                        make_manifest("refrain.test.life.anchor", "1.0.0",
                                      "refrain:leaf"));
            image_builder next_builder{};
            next_builder.add("refrain.test.life.anchor",
                             make_manifest("refrain.test.life.anchor", "1.0.0",
                                           "refrain:leaf"));
            next_builder.add("refrain.test.life.attacker",
                             make_manifest("refrain.test.life.attacker",
                                           "1.0.0", "refrain:host.core"));
            const auto image{builder.build()};
            clean_loader_state loader{};
            mod::host host{registry};
            refrain::app::host_context context{host};
            expect(register_host_core(host, context));
            expect(register_clean_loader(host, loader));
            auto loaded{host.load(*plan, image)};
            expect(static_cast<bool>(loaded))
                << (loaded ? std::string{"load ok"} : loaded.error().message);
            if (!loaded) {
                return;
            }
            const auto sealed_handle{
                host.services().query("refrain:host.core")};
            expect(sealed_handle.has_value());

            // The next plan adds a mod that claims the sealed host service.
            auto next_image{next_builder.build()};
            auto candidates{mod::scan_builtin_image(next_image)};
            expect(static_cast<bool>(candidates));
            if (!candidates) {
                return;
            }
            auto next{*plan};
            for (const auto &candidate : *candidates) {
                if (candidate.id == "refrain.test.life.attacker") {
                    next.load_order.push_back(candidate);
                }
            }
            auto resolver{make_package_resolver_for(next_image)};
            auto staged{host.stage_composition(next, resolver)};
            expect(!staged.has_value());
            if (!staged) {
                expect(staged.error().code == error_code::permission_denied ||
                       staged.error().code == error_code::unsupported);
                expect(staged.error().message.find("sealed") !=
                       std::string::npos);
            }
            expect(host.epoch() == 1U);
            if (sealed_handle) {
                expect(host.services().validate(*sealed_handle).has_value());
            }
        };

    "mod_lifecycle/repeated clean reloads stay bounded and leave no "
    "candidates"_test = [] {
        mod::static_mod_registry registry{};
        auto plan{make_gamepack("\"refrain.test.life.leaf@1.0.0\"",
                                "leaf = \"refrain.test.life.leaf@^1.0.0\"\n")};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        image_builder builder{};
        builder.add("refrain.test.life.leaf",
                    make_manifest("refrain.test.life.leaf", "1.0.0",
                                  "refrain:leaf", clean_loader_id));
        const auto image{builder.build()};
        clean_loader_state loader{};
        mod::host host{registry};
        expect(register_clean_loader(host, loader));
        auto loaded{host.load(*plan, image)};
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        const auto baseline{host.services().provider_count()};
        auto resolver{make_package_resolver_for(image)};
        constexpr int reloads{16};
        for (int index = 0; index < reloads; ++index) {
            auto staged{host.stage_composition(
                *plan, resolver,
                mod::stage_options{.reload = {"refrain.test.life.leaf"}})};
            expect(static_cast<bool>(staged));
            if (!staged) {
                return;
            }
            auto committed{host.commit_epoch()};
            expect(static_cast<bool>(committed));
        }
        expect(host.epoch() == static_cast<std::uint64_t>(reloads) + 1U);
        const auto record{host.lifecycle_record("refrain.test.life.leaf")};
        expect(record.has_value());
        if (record) {
            expect(record->generation ==
                   static_cast<std::uint64_t>(reloads) + 1U);
        }
        expect(loader.reload_calls == reloads);
        expect(loader.unload_calls == reloads);
        expect(host.services().provider_count() == baseline);
        expect(host.retired_native_generations() == 0U);
        // A clean loader unloads completely: nothing is quarantined and the
        // registry has exactly the current generation.
        expect(host.quarantined_mods().empty());
        expect(host.services().active_count() >= 1U);
        auto removal{host.stage_uninstall("refrain.test.life.leaf")};
        expect(static_cast<bool>(removal));
        if (removal) {
            expect(static_cast<bool>(host.commit_epoch()));
            expect(host.mods().empty());
            expect(loader.last_reason ==
                   REFRAIN_SERVICES_MOD_LOADER_MOD_UNLOAD_REASON_UNINSTALL);
        }
    };
};

} // namespace

// ---------------------------------------------------------------------------
// real native loader: retire counting, cap and quarantine
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] auto installed_spec(std::string_view mod_id)
    -> refrain::result<mod::gamepack_spec> {
    auto text{std::string{"[gamepack]\n"}};
    text += "id = \"refrain.test.lifecycle.installed\"\n";
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

constexpr std::string_view native_manifest{
    "[mod]\n"
    "id = \"refrain.test.life.native\"\n"
    "version = \"1.0.0\"\n"
    "abi = \"1\"\n"
    "side = \"server\"\n"
    "\n[provides]\n"
    "services = [\"refrain:data.packs\"]\n"
    "\n[entry]\n"
    "loader = \"native\"\n"
    "native = \"lib/libexternal.so\"\n"};

struct native_fixture {
    refrain::test_support::scratch_directory scratch{};
    std::string store_root{};
    mod::composition plan{};
};

[[nodiscard]] auto boot_native(native_fixture &fixture) -> bool {
    auto archive{
        pack_mod(fixture.scratch, "native", native_manifest, "libexternal.so")};
    if (!archive) {
        return false;
    }
    auto store{install_archive(fixture.scratch, *archive)};
    if (!store) {
        return false;
    }
    fixture.store_root = *store;
    auto spec{installed_spec("refrain.test.life.native")};
    if (!spec) {
        return false;
    }
    const std::vector<std::string> roots{fixture.store_root};
    auto plan{mod::resolve_installed_composition(
        *spec, roots, mod::composition_load_options{.host_side = "server"})};
    if (!plan) {
        return false;
    }
    fixture.plan = std::move(*plan);
    return true;
}

const suite mod_lifecycle_native_tests = [] {
    "mod_lifecycle/native reload retires mappings and the cap refuses"_test =
        [] {
            native_fixture fixture{};
            expect(boot_native(fixture)) << "native fixture must install";
            if (fixture.plan.load_order.empty()) {
                return;
            }
            mod::static_mod_registry registry{};
            mod::host host{registry};
            refrain::app::host_context context{host};
            expect(register_host_core(host, context));
            host.set_native_retire_limit(2);
            auto loaded{host.load(fixture.plan, std::span<const std::byte>{})};
            expect(static_cast<bool>(loaded))
                << (loaded ? std::string{"load ok"} : loaded.error().message);
            if (!loaded) {
                return;
            }
            expect(host.retired_native_generations() == 0U);
            auto resolver{make_package_resolver_for({})};

            for (int generation = 1; generation <= 2; ++generation) {
                auto staged{host.stage_composition(
                    fixture.plan, resolver,
                    mod::stage_options{
                        .reload = {"refrain.test.life.native"}})};
                expect(static_cast<bool>(staged))
                    << (staged ? std::string{"staged"}
                               : staged.error().message);
                if (!staged) {
                    return;
                }
                auto committed{host.commit_epoch()};
                expect(static_cast<bool>(committed));
                if (!committed) {
                    return;
                }
                expect(host.retired_native_generations(
                           "refrain.test.life.native") ==
                       static_cast<std::size_t>(generation))
                    << host.retired_native_generations(
                           "refrain.test.life.native");
                expect(host.retired_native_generations() ==
                       static_cast<std::size_t>(generation))
                    << host.retired_native_generations();
            }
            expect(host.retired_native_generations() == 2U);

            // The next reload would keep a third mapping: refused, and the
            // running epoch is untouched.
            const auto epoch_before{host.epoch()};
            auto refused{host.stage_composition(
                fixture.plan, resolver,
                mod::stage_options{.reload = {"refrain.test.life.native"}})};
            expect(!refused.has_value());
            if (!refused) {
                expect(refused.error().code == error_code::unsupported);
                expect(refused.error().message.find("restart") !=
                       std::string::npos);
            }
            expect(host.epoch() == epoch_before);
            expect(!host.staged_change().has_value());
            expect(host.retired_native_generations() == 2U);

            // Uninstalling still works: the mod is retired, not reloaded.
            auto removal{host.stage_uninstall("refrain.test.life.native")};
            expect(static_cast<bool>(removal))
                << (removal ? std::string{"staged"} : removal.error().message);
            if (removal) {
                expect(static_cast<bool>(host.commit_epoch()));
                expect(host.mods().empty());
                expect(host.retired_native_generations() == 3U);
                expect(host.quarantined_mods().empty());
            }
        };

    "mod_lifecycle/failing native on_unload quarantines the mod"_test = [] {
        native_fixture fixture{};
        expect(boot_native(fixture)) << "native fixture must install";
        if (fixture.plan.load_order.empty()) {
            return;
        }
        mod::static_mod_registry registry{};
        mod::host host{registry};
        refrain::app::host_context context{host};
        expect(register_host_core(host, context));
        auto loaded{host.load(fixture.plan, std::span<const std::byte>{})};
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        auto resolver{make_package_resolver_for({})};
        auto staged{host.stage_composition(
            fixture.plan, resolver,
            mod::stage_options{.reload = {"refrain.test.life.native"}})};
        expect(static_cast<bool>(staged));
        if (!staged) {
            return;
        }
        // The fixture reports a failed unload while this variable is set.
        static_cast<void>(refrain::platform::set_environment_variable(
            "REFRAIN_TEST_EXTERNAL_MOD_FAIL_UNLOAD", "1"));
        auto committed{host.commit_epoch()};
        static_cast<void>(refrain::platform::unset_environment_variable(
            "REFRAIN_TEST_EXTERNAL_MOD_FAIL_UNLOAD"));
        expect(static_cast<bool>(committed));
        if (!committed) {
            return;
        }
        expect(committed->quarantined ==
               std::vector<std::string>{"refrain.test.life.native"});
        expect(!host.quarantined_mods().empty());
        const auto record{host.lifecycle_record("refrain.test.life.native")};
        expect(record.has_value());
        if (record) {
            expect(record->quarantined);
        }
        // A quarantined id can never be installed or reloaded again in this
        // process; only a restart clears it.
        auto reinstall{host.stage_composition(
            host.active_composition(), resolver,
            mod::stage_options{.reload = {"refrain.test.life.native"}})};
        expect(!reinstall.has_value());
        if (!reinstall) {
            expect(reinstall.error().code == error_code::unsupported);
            expect(reinstall.error().message.find("quarantined") !=
                   std::string::npos);
        }
        expect(host.epoch() == 2U);
    };
};

} // namespace

// ---------------------------------------------------------------------------
// application service: command-side install/uninstall/reload and the
// tick-boundary hook
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] auto app_spec() -> refrain::result<mod::gamepack_spec> {
    auto text{std::string{"[gamepack]\n"}};
    text += "id = \"refrain.test.lifecycle.app\"\n";
    text += "version = \"1.0.0\"\n";
    text += "side = \"server\"\n";
    text += "abi = \"1\"\n";
    text += "auto_scan = true\n";
    text += "\n[mods]\nlist = [\"refrain.test.app.leaf@1.0.0\"]\n";
    text += "\n[providers]\nleaf = \"refrain.test.app.leaf@^1.0.0\"\n";
    auto doc{config::document::parse_text(text, "<gamepack>")};
    if (!doc) {
        return std::unexpected{doc.error()};
    }
    return mod::parse_gamepack(*doc, "server");
}

constexpr std::string_view app_leaf_manifest{
    "[mod]\n"
    "id = \"refrain.test.app.leaf\"\n"
    "version = \"1.0.0\"\n"
    "abi = \"1\"\n"
    "side = \"server\"\n"
    "\n[provides]\n"
    "services = [\"refrain:leaf\"]\n"
    "\n[entry]\n"
    "loader = \"com.example.clean\"\n"};

constexpr std::string_view app_data_manifest{
    "[mod]\n"
    "id = \"refrain.test.app.data\"\n"
    "version = \"1.0.0\"\n"
    "abi = \"1\"\n"
    "side = \"server\"\n"
    "\n[provides]\n"
    "services = [\"refrain:extra\"]\n"
    "\n[entry]\n"
    "loader = \"com.example.clean\"\n"};

const suite mod_lifecycle_app_tests = [] {
    "mod_lifecycle/app installs, reloads and uninstalls through the tick "
    "hook"_test = [] {
        refrain::test_support::scratch_directory scratch{};
        auto leaf_archive{pack_mod(scratch, "leaf", app_leaf_manifest)};
        auto data_archive{pack_mod(scratch, "data", app_data_manifest)};
        expect(leaf_archive.has_value());
        expect(data_archive.has_value());
        if (!leaf_archive || !data_archive) {
            return;
        }
        auto store_root{install_archive(scratch, *leaf_archive)};
        expect(store_root.has_value());
        if (!store_root) {
            return;
        }
        auto spec{app_spec()};
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
        clean_loader_state loader{};
        mod::static_mod_registry registry{};
        mod::host host{registry};
        expect(register_clean_loader(host, loader));
        auto loaded{host.load(*plan, std::span<const std::byte>{})};
        expect(static_cast<bool>(loaded))
            << (loaded ? std::string{"load ok"} : loaded.error().message);
        if (!loaded) {
            return;
        }
        refrain::app::mod_lifecycle lifecycle{
            host, refrain::app::mod_lifecycle_options{.store_roots = roots,
                                                      .actor = "test-console"}};
        expect(host.mods().size() == 1U);
        expect(host.epoch() == 1U);

        // Reload: staged by the service, published by the tick hook.
        auto reloaded{lifecycle.reload("refrain.test.app.leaf")};
        expect(static_cast<bool>(reloaded))
            << (reloaded ? std::string{"staged"} : reloaded.error().message);
        if (!reloaded) {
            return;
        }
        expect(lifecycle.pending());
        expect(host.epoch() == 1U);
        expect(static_cast<bool>(refrain::app::commit_staged_mod_changes()));
        expect(host.epoch() == 2U);
        expect(!lifecycle.pending());
        const auto record{host.lifecycle_record("refrain.test.app.leaf")};
        expect(record.has_value());
        if (record) {
            expect(record->generation == 2U);
        }

        // An install that records no receipt could never be loaded by the
        // running composition; the service refuses it before touching the
        // store.
        auto no_receipt{lifecycle.install(
            *data_archive, mod::rfmod_install_options{.write_receipt = false})};
        expect(!no_receipt.has_value());
        if (!no_receipt) {
            expect(no_receipt.error().code == error_code::invalid_argument);
        }

        // Install: the service runs the store transaction and stages the new
        // mod; the commit arrives with the next tick.
        auto installed{lifecycle.install(*data_archive)};
        expect(static_cast<bool>(installed))
            << (installed ? std::string{"staged"} : installed.error().message);
        if (!installed) {
            return;
        }
        expect(installed->action == "install");
        expect(installed->change.added ==
               std::vector<std::string>{"refrain.test.app.data"});
        expect(host.mods().size() == 1U);
        expect(static_cast<bool>(refrain::app::commit_staged_mod_changes()));
        expect(host.mods().size() == 2U);
        expect(host.epoch() == 3U);

        // Uninstall the installed mod: the receipt is removed and the tick
        // hook publishes the removal.
        auto removed{lifecycle.uninstall("refrain.test.app.data", "1.0.0")};
        expect(static_cast<bool>(removed))
            << (removed ? std::string{"staged"} : removed.error().message);
        if (!removed) {
            return;
        }
        expect(removed->action == "uninstall");
        expect(!removed->gc_hint.empty());
        expect(refrain::app::commit_staged_mod_changes().has_value());
        expect(host.mods().size() == 1U);
        expect(host.epoch() == 4U);
        auto receipts{mod::list_receipt_paths(*store_root)};
        expect(receipts.has_value());
        if (receipts) {
            expect(receipts->size() == 1U);
        }

        // The command surface reaches the same service through
        // `app::mod_lifecycle_port`: install (store already committed), reload
        // and uninstall (receipt already removed). The port only stages; the
        // tick hook publishes.
        auto port{refrain::app::make_mod_lifecycle_port(lifecycle)};
        expect(port.apply.operator bool());

        auto port_reload{port.apply(refrain::app::mod_lifecycle_request{
            .action = refrain::app::mod_lifecycle_action::reload,
            .id = "refrain.test.app.leaf"})};
        expect(port_reload.has_value())
            << (port_reload ? std::string{"staged"}
                            : port_reload.error().message);
        if (!port_reload) {
            return;
        }
        expect(port_reload->lines.size() == 2U);
        expect(lifecycle.pending());
        expect(refrain::app::commit_staged_mod_changes().has_value());
        expect(host.epoch() == 5U);

        // The command layer installs the archive into the store itself before
        // asking the running composition to follow.
        expect(install_into_store(*store_root, *data_archive));
        auto port_install{port.apply(refrain::app::mod_lifecycle_request{
            .action = refrain::app::mod_lifecycle_action::install,
            .id = "refrain.test.app.data"})};
        expect(port_install.has_value())
            << (port_install ? std::string{"staged"}
                             : port_install.error().message);
        if (!port_install) {
            return;
        }
        expect(refrain::app::commit_staged_mod_changes().has_value());
        expect(host.mods().size() == 2U);
        expect(host.epoch() == 6U);

        // The command layer removes the receipt first, then asks the running
        // composition to retire the generation.
        auto store{data::content_store::open_existing(*store_root)};
        expect(static_cast<bool>(store));
        if (store) {
            expect(mod::uninstall_rfmod(*store, "refrain.test.app.data")
                       .has_value());
        }
        auto port_uninstall{port.apply(refrain::app::mod_lifecycle_request{
            .action = refrain::app::mod_lifecycle_action::uninstall,
            .id = "refrain.test.app.data"})};
        expect(port_uninstall.has_value())
            << (port_uninstall ? std::string{"staged"}
                               : port_uninstall.error().message);
        if (port_uninstall) {
            expect(refrain::app::commit_staged_mod_changes().has_value());
            expect(host.mods().size() == 1U);
            expect(host.epoch() == 7U);
        }

        // A review-only reload that nothing consumes (no tick) must not
        // publish anything.
        const auto epoch_before_held{host.epoch()};
        auto held{lifecycle.reload("refrain.test.app.leaf")};
        expect(static_cast<bool>(held));
        if (held) {
            expect(host.epoch() == epoch_before_held);
            lifecycle.abort();
            expect(!lifecycle.pending());
            expect(host.epoch() == epoch_before_held);
        }
        // No staged change: the hook reports the committed epoch unchanged.
        const auto digest{host.composition_digest()};
        const auto quiet{refrain::app::commit_staged_mod_changes()};
        expect(quiet.has_value());
        expect(host.composition_digest() == digest);
    };
};

} // namespace
