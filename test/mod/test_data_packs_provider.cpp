// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/builtin/image.hpp>
#include <refrain/contracts/services/data_packs.hpp>
#include <refrain/core/io.hpp>
#include <refrain/mod/discovery.hpp>
#include <refrain/mod/gamepack.hpp>
#include <refrain/mod/host.hpp>

#include <boost/ut.hpp>
#include <fmt/format.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using namespace boost::ut;

namespace contracts = refrain::contracts;
namespace data_packs = refrain::services::data_packs;
namespace mod = refrain::mod;

[[nodiscard]] auto builtin_bytes() -> std::span<const std::byte> {
    return std::span<const std::byte>{refrain::builtin::image_data(),
                                      refrain::builtin::image_size()};
}

[[nodiscard]] auto load_data_only_plan()
    -> refrain::result<refrain::mod::composition> {
    auto spec{mod::load_gamepack_spec(std::string{REFRAIN_SOURCE_DIR} +
                                          "/gamepack/presets/data-only.toml",
                                      "server")};
    if (!spec) {
        return std::unexpected{spec.error()};
    }
    return refrain::mod::preview_composition(*spec);
}

/// Default ten-mod vanilla gamepack (server), resolved against the embedded
/// built-in image; the `data` service is bound to `refrain.data.vanilla`.
[[nodiscard]] auto load_default_plan()
    -> refrain::result<refrain::mod::composition> {
    auto spec{mod::load_gamepack_spec(
        std::string{REFRAIN_SOURCE_DIR} + "/gamepack/gamepack.toml", "server")};
    if (!spec) {
        return std::unexpected{spec.error()};
    }
    return refrain::mod::preview_composition(*spec);
}

/// The provider is published on the frozen service registry as a
/// `provider_api`; build the typed wrapper around it.
[[nodiscard]] auto make_wrapper(const refrain::mod::provider_api &api)
    -> data_packs::provider {
    return data_packs::provider{
        *static_cast<const refrain_services_data_packs_vtable *>(api.vtable),
        api.self};
}

const suite data_packs_provider_tests = [] {
    "data_packs/refrain.data.vanilla registers the provider at load"_test = [] {
        auto plan{load_data_only_plan()};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        refrain::mod::host host{};
        expect(static_cast<bool>(host.load(*plan, builtin_bytes())));
        const auto provider{
            host.services().active_provider("refrain:data.packs")};
        expect(provider.has_value());
        if (!provider) {
            return;
        }
        expect(provider->id == "refrain.data.vanilla");
        expect(provider->version == "1.0.0");
        const auto *api{static_cast<const refrain::mod::provider_api *>(
            provider->instance)};
        expect(api != nullptr);
        if (api == nullptr) {
            return;
        }
        // The provider arms the host's vtable ABI-header check with the
        // generated contract major; a mismatch would have failed the load.
        expect(api->abi_major == REFRAIN_SERVICES_DATA_PACKS_ABI_MAJOR);
        auto wrapper{make_wrapper(*api)};
        // The provider mounts the embedded builtin RFSI image as its first
        // pack; its handle is 1 (the provider starts handing out handles at 1).
        expect(wrapper.mounted_count() == 1U);
        expect(host.builtin_image_digest().size() == 64U);
        const auto index{
            wrapper.read_resource(data_packs::pack{1}, "mods.index.toml")};
        expect(static_cast<bool>(index));
        if (index) {
            const std::string_view text{
                reinterpret_cast<const char *>(index->data()), index->size()};
            expect(text.find("format = \"rfsi\"") != std::string_view::npos);
            expect(text.find("refrain.data.vanilla") != std::string_view::npos);
        }
    };

    "data_packs/mount, read_resource and unmount a datapack directory"_test =
        [] {
            auto plan{load_data_only_plan()};
            expect(static_cast<bool>(plan));
            if (!plan) {
                return;
            }
            refrain::mod::host host{};
            expect(static_cast<bool>(host.load(*plan, builtin_bytes())));
            const auto provider{
                host.services().active_provider("refrain:data.packs")};
            expect(provider.has_value());
            if (!provider || provider->instance == nullptr) {
                return;
            }
            auto wrapper{
                make_wrapper(*static_cast<const refrain::mod::provider_api *>(
                    provider->instance))};

            const std::string fixture_path{
                (std::filesystem::path{REFRAIN_SOURCE_DIR} /
                 "test/fixtures/datapack")
                    .string()};
            auto source{data_packs::make_pack_source()};
            source.path = contracts::to_borrowed(fixture_path);
            source.kind =
                static_cast<std::uint32_t>(data_packs::pack_kind::data);
            source.pack_format = 121;
            auto mounted{wrapper.mount(source)};
            expect(static_cast<bool>(mounted));
            if (!mounted) {
                return;
            }
            expect(mounted->pack != REFRAIN_HANDLE_INVALID);
            expect(mounted->file_count == 9U);
            expect(mounted->pack_format == 121U);
            expect(wrapper.mounted_count() == 2U);

            const auto read{wrapper.read_resource(
                data_packs::pack{mounted->pack},
                "data/refrain_test/tags/block/metal.json")};
            expect(static_cast<bool>(read));
            if (read) {
                const std::string_view text{
                    reinterpret_cast<const char *>(read->data()), read->size()};
                expect(text.find("refrain_test:iron_block") !=
                       std::string_view::npos);
            }
            const auto missing{wrapper.read_resource(
                data_packs::pack{mounted->pack}, "data/does/not/exist.json")};
            expect(!missing);
            if (!missing) {
                expect(missing.error() == data_packs::pack_error::not_found);
            }

            expect(static_cast<bool>(
                wrapper.unmount(data_packs::pack{mounted->pack})));
            expect(wrapper.mounted_count() == 1U);
            const auto again{wrapper.read_resource(
                data_packs::pack{mounted->pack}, "pack.mcmeta")};
            expect(!again);
        };

    "data_packs/mount rejects duplicate and invalid sources"_test = [] {
        auto plan{load_data_only_plan()};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        refrain::mod::host host{};
        expect(static_cast<bool>(host.load(*plan, builtin_bytes())));
        const auto provider{
            host.services().active_provider("refrain:data.packs")};
        expect(provider.has_value());
        if (!provider || provider->instance == nullptr) {
            return;
        }
        auto wrapper{
            make_wrapper(*static_cast<const refrain::mod::provider_api *>(
                provider->instance))};

        auto empty{data_packs::make_pack_source()};
        empty.path = contracts::to_borrowed("");
        const auto invalid{wrapper.mount(empty)};
        expect(!invalid);
        if (!invalid) {
            expect(invalid.error() == data_packs::pack_error::invalid_format);
        }

        const std::string fixture_path{
            (std::filesystem::path{REFRAIN_SOURCE_DIR} /
             "test/fixtures/datapack")
                .string()};
        auto source{data_packs::make_pack_source()};
        source.path = contracts::to_borrowed(fixture_path);
        source.kind = static_cast<std::uint32_t>(data_packs::pack_kind::data);
        auto first{wrapper.mount(source)};
        expect(static_cast<bool>(first));
        if (!first) {
            return;
        }
        const auto duplicate{wrapper.mount(source)};
        expect(!duplicate);
        if (!duplicate) {
            expect(duplicate.error() ==
                   data_packs::pack_error::already_mounted);
        }
        static_cast<void>(wrapper.unmount(data_packs::pack{first->pack}));
    };

    "data_packs/provider is only activated when the plan selects it"_test = [] {
        auto plan{load_default_plan()};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        // Drop the `data` binding: `refrain.data.vanilla` still loads (it is a
        // content mod), but its factory must not run, so no provider claims
        // `refrain:data.packs`.
        const auto erased{std::erase_if(
            plan->providers, [](const mod::resolved_provider &provider) {
                return provider.service == "data";
            })};
        expect(erased == 1U);

        refrain::mod::host host{};
        auto loaded{host.load(*plan, builtin_bytes())};
        expect(static_cast<bool>(loaded))
            << (loaded ? std::string{"load ok"} : loaded.error().message);
        if (!loaded) {
            return;
        }
        expect(host.mods().size() == 12U)
            << "all twelve built-in mods must load";
        expect(
            !host.services().active_provider("refrain:data.packs").has_value());
        const auto *data_mod{host.find("refrain.data.vanilla")};
        expect(data_mod != nullptr);
        if (data_mod != nullptr) {
            expect(!data_mod->has_code) << "factory ran for an unselected "
                                           "provider";
            expect(data_mod->provides_summary == "refrain:data.packs");
        }
        expect(host.find("refrain.content.vanilla") != nullptr);
    };

    "data_packs/plan pointing data at an unknown mod fails closed"_test = [] {
        auto plan{load_default_plan()};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        // Resolve can only select listed mods, so simulate a plan/image
        // mismatch: the selected provider mod is absent from the image.
        constexpr std::string_view missing_mod{"refrain.data.missing"};
        for (auto &provider : plan->providers) {
            if (provider.service == "data") {
                provider.provider = std::string{missing_mod} + "@1.0.0";
            }
        }

        refrain::mod::host host{};
        auto loaded{host.load(*plan, builtin_bytes())};
        expect(!loaded);
        if (!loaded) {
            expect(loaded.error().code == refrain::error_code::not_found);
            expect(loaded.error().message.find("data") != std::string::npos);
            expect(loaded.error().message.find(missing_mod) !=
                   std::string::npos);
        }
        // The failed host exposes no partial activation and refuses reuse.
        expect(host.services().provider_count() == 0U);
        expect(!host.loaded());
        auto again{host.load(*plan, builtin_bytes())};
        expect(!again);
        if (!again) {
            expect(again.error().code == refrain::error_code::unsupported);
        }
    };
};

} // namespace
