// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// `refrain:runtime` contract and provider activation.
//
// The default gamepack must select `refrain.server.vanilla` as the runtime
// provider; a plan that does not select it must leave the service inactive
// (the host turns that into a clean startup error) and a provider error must
// cross the generated wrapper unchanged. This test runs no tick loop: the real
// provider call uses an empty `world_path`, which is rejected before anything
// touches the filesystem.

#include <refrain/builtin/image.hpp>
#include <refrain/contracts/abi.h>
#include <refrain/contracts/services/runtime.hpp>
#include <refrain/mod/discovery.hpp>
#include <refrain/mod/gamepack.hpp>
#include <refrain/mod/host.hpp>

#include <boost/ut.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace boost::ut;

namespace mod = refrain::mod;
namespace runtime = refrain::services::runtime;

[[nodiscard]] auto builtin_bytes() -> std::span<const std::byte> {
    return std::span<const std::byte>{refrain::builtin::image_data(),
                                      refrain::builtin::image_size()};
}

[[nodiscard]] auto load_plan() -> refrain::result<mod::composition> {
    auto spec{mod::load_gamepack_spec(
        std::string{REFRAIN_SOURCE_DIR} + "/gamepack/gamepack.toml", "server")};
    if (!spec) {
        return std::unexpected{spec.error()};
    }
    return mod::preview_composition(*spec);
}

refrain_status fake_run_error(void *,
                              const refrain_services_runtime_runtime_config *,
                              std::int32_t *) {
    return REFRAIN_SERVICES_RUNTIME_RUNTIME_ERROR_INTERNAL;
}

refrain_status fake_run_exit(void *,
                             const refrain_services_runtime_runtime_config *,
                             std::int32_t *out_value) {
    *out_value = 7;
    return REFRAIN_STATUS_OK;
}

[[maybe_unused]] const suite tests = [] {
    "runtime/default gamepack selects refrain.server.vanilla"_test = [] {
        auto plan{load_plan()};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        mod::host host{};
        auto loaded{host.load(*plan, builtin_bytes())};
        expect(static_cast<bool>(loaded))
            << (loaded ? std::string{"load ok"} : loaded.error().message);
        if (!loaded) {
            return;
        }
        const auto info{host.services().active_provider("refrain:runtime")};
        expect(info.has_value());
        if (!info) {
            return;
        }
        expect(info->id == "refrain.server.vanilla");
        expect(info->version == "1.0.0");

        const auto *api{static_cast<const mod::provider_api *>(info->instance)};
        expect(api != nullptr);
        if (api == nullptr || api->vtable == nullptr) {
            return;
        }
        expect(api->abi_major == REFRAIN_SERVICES_RUNTIME_ABI_MAJOR);
        const runtime::provider provider{
            *static_cast<const refrain_services_runtime_vtable *>(api->vtable),
            api->self};

        // No `--world`: the provider rejects the config before starting the
        // tick loop, so this test never touches the filesystem.
        auto config{runtime::make_runtime_config()};
        const auto outcome{provider.run(config)};
        expect(!outcome);
        if (!outcome) {
            expect(outcome.error() == runtime::runtime_error::invalid_config);
        }
    };

    "runtime/plan without a runtime binding leaves the service inactive"_test =
        [] {
            auto plan{load_plan()};
            expect(static_cast<bool>(plan));
            if (!plan) {
                return;
            }
            const auto erased{std::erase_if(
                plan->providers, [](const mod::resolved_provider &provider) {
                    return provider.service == "runtime";
                })};
            expect(erased == 1U);

            mod::host host{};
            auto loaded{host.load(*plan, builtin_bytes())};
            expect(static_cast<bool>(loaded))
                << (loaded ? std::string{"load ok"} : loaded.error().message);
            if (!loaded) {
                return;
            }
            // The mod is still loaded as data (its factory is only called for a
            // selected provider), but the host has no runtime provider to call.
            expect(!host.services()
                        .active_provider("refrain:runtime")
                        .has_value());
            const auto *runtime_mod{host.find("refrain.server.vanilla")};
            expect(runtime_mod != nullptr);
            if (runtime_mod != nullptr) {
                expect(!runtime_mod->has_code);
            }
        };

    "runtime/missing provider is a clean required-service error"_test = [] {
        auto spec{mod::load_gamepack_spec(std::string{REFRAIN_SOURCE_DIR} +
                                              "/gamepack/gamepack.toml",
                                          "server")};
        expect(static_cast<bool>(spec));
        if (!spec) {
            return;
        }
        const auto erased{std::erase_if(
            spec->providers, [](const mod::provider_binding &provider) {
                return provider.service == "runtime";
            })};
        expect(erased == 1U);

        const std::array<std::string_view, 1> required{"runtime"};
        const auto plan{mod::preview_composition(*spec, required)};
        expect(!plan);
        if (!plan) {
            expect(plan.error().code == refrain::error_code::not_found);
        }
    };

    "runtime/provider errors and exit codes cross the generated wrapper"_test =
        [] {
            refrain_services_runtime_vtable error_table{};
            error_table.struct_size = REFRAIN_SERVICES_RUNTIME_VTABLE_SIZE;
            error_table.abi_major = REFRAIN_SERVICES_RUNTIME_ABI_MAJOR;
            error_table.abi_minor = REFRAIN_SERVICES_RUNTIME_ABI_MINOR;
            error_table.abi_patch = REFRAIN_SERVICES_RUNTIME_ABI_PATCH;
            error_table.run = &fake_run_error;
            const runtime::provider failing{error_table, nullptr};
            auto config{runtime::make_runtime_config()};
            const auto failed{failing.run(config)};
            expect(!failed);
            if (!failed) {
                expect(failed.error() == runtime::runtime_error::internal);
            }

            refrain_services_runtime_vtable exit_table{error_table};
            exit_table.run = &fake_run_exit;
            const runtime::provider exiting{exit_table, nullptr};
            const auto code{exiting.run(config)};
            expect(code.has_value());
            if (code) {
                expect(*code == 7);
            }
        };
};

} // namespace
