// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// `refrain:world.migration` provider of the `refrain.world.datafix` built-in
// mod: the default composition activates it, the contract window agrees with
// the engine, and the contract path upgrades a synthetic older world.

#include <refrain/builtin/image.hpp>
#include <refrain/contracts/abi.hpp>
#include <refrain/contracts/services/world_migration.hpp>
#include <refrain/mod/discovery.hpp>
#include <refrain/mod/gamepack.hpp>
#include <refrain/mod/host.hpp>
#include <refrain/world/migration.hpp>
#include <refrain/world/store.hpp>
#include <refrain/world/version.hpp>

#include <boost/ut.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

#include "support/migration_world.hpp"
#include "support/scratch_directory.hpp"

namespace {

using namespace boost::ut;

namespace migration = refrain::services::world_migration;
namespace mod = refrain::mod;
namespace support = refrain::test_support;
namespace test_support = refrain::test_support;
namespace world = refrain::world;

constexpr std::int32_t fixture_version = 5013;

[[nodiscard]] auto builtin_bytes() -> std::span<const std::byte> {
    return std::span<const std::byte>{refrain::builtin::image_data(),
                                      refrain::builtin::image_size()};
}

[[nodiscard]] auto load_composition(std::string_view gamepack_path)
    -> refrain::result<mod::composition> {
    auto spec{mod::load_gamepack_spec(gamepack_path, "server")};
    if (!spec) {
        return std::unexpected{spec.error()};
    }
    return mod::preview_composition(*spec);
}

[[nodiscard]] auto load_default_composition()
    -> refrain::result<mod::composition> {
    return load_composition(std::string{REFRAIN_SOURCE_DIR} +
                            "/gamepack/gamepack.toml");
}

[[nodiscard]] auto make_wrapper(const mod::provider_api &api)
    -> migration::provider {
    return migration::provider{
        *static_cast<const refrain_services_world_migration_vtable *>(
            api.vtable),
        api.self};
}

[[nodiscard]] auto world_path(const test_support::scratch_directory &directory)
    -> std::string {
    return (directory.path() / "world").string();
}

[[nodiscard]] auto recorded_version(const std::string &root) -> std::int32_t {
    auto version{world::read_world_data_version(root)};
    expect(static_cast<bool>(version));
    return version ? *version : 0;
}

[[nodiscard]] auto make_request(const std::string &root, std::int32_t from,
                                std::int32_t to, bool dry_run)
    -> migration::world_migration_request {
    auto request{migration::make_world_migration_request()};
    request.world_path =
        refrain::contracts::to_borrowed(std::string_view{root});
    request.from_version = from;
    request.to_version = to;
    request.dry_run = dry_run ? 1 : 0;
    return request;
}

const suite world_migration_provider_tests = [] {
    "migration provider/default gamepack activates the provider"_test = [] {
        auto plan{load_default_composition()};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        refrain::mod::host host{};
        auto loaded{host.load(*plan, builtin_bytes())};
        expect(static_cast<bool>(loaded))
            << (loaded ? std::string{"load ok"} : loaded.error().message);
        if (!loaded) {
            return;
        }
        const auto provider{
            host.services().active_provider("refrain:world.migration")};
        expect(provider.has_value());
        if (!provider || provider->instance == nullptr) {
            return;
        }
        expect(provider->id == "refrain.world.datafix");
        expect(provider->version == "1.0.0");
        const auto *api{
            static_cast<const mod::provider_api *>(provider->instance)};
        expect(api != nullptr);
        if (api == nullptr || api->vtable == nullptr) {
            return;
        }
        const auto *vtable{
            static_cast<const refrain_services_world_migration_vtable *>(
                api->vtable)};
        expect(api->abi_major == REFRAIN_SERVICES_WORLD_MIGRATION_ABI_MAJOR);
        expect(vtable->struct_size ==
               REFRAIN_SERVICES_WORLD_MIGRATION_VTABLE_SIZE);
        const auto wrapper{make_wrapper(*api)};
        auto capabilities{wrapper.capabilities()};
        expect(static_cast<bool>(capabilities));
        if (capabilities) {
            expect(capabilities->min_version ==
                   world::oldest_migratable_data_version);
            expect(capabilities->target_version == world::target_data_version);
        }
    };

    "migration provider/a data-only composition has no migration provider"_test =
        [] {
            auto plan{load_composition(std::string{REFRAIN_SOURCE_DIR} +
                                       "/gamepack/presets/data-only.toml")};
            expect(static_cast<bool>(plan));
            if (!plan) {
                return;
            }
            refrain::mod::host host{};
            auto loaded{host.load(*plan, builtin_bytes())};
            expect(static_cast<bool>(loaded))
                << (loaded ? std::string{"load ok"} : loaded.error().message);
            if (!loaded) {
                return;
            }
            // The mod is not part of this composition, so the runtime sees no
            // migration provider and refuses an older world instead of
            // upgrading it.
            expect(!host.services()
                        .active_provider("refrain:world.migration")
                        .has_value());
        };

    "migration provider/plan agrees with the engine window"_test = [] {
        auto plan{load_default_composition()};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        refrain::mod::host host{};
        auto loaded{host.load(*plan, builtin_bytes())};
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        const auto provider{
            host.services().active_provider("refrain:world.migration")};
        expect(provider.has_value());
        if (!provider || provider->instance == nullptr) {
            return;
        }
        const auto wrapper{make_wrapper(
            *static_cast<const mod::provider_api *>(provider->instance))};
        auto planned{wrapper.plan(fixture_version, world::target_data_version)};
        expect(static_cast<bool>(planned));
        if (planned) {
            expect(planned->from_version == fixture_version);
            expect(planned->to_version == world::target_data_version);
            expect(planned->steps == 3U);
        }
        auto refused{wrapper.plan(5012, world::target_data_version)};
        expect(!refused);
        if (!refused) {
            expect(refused.error() ==
                   migration::world_migration_error::unsupported_version);
        }
        auto foreign{wrapper.plan(fixture_version, 5022)};
        expect(!foreign);
        if (!foreign) {
            expect(foreign.error() ==
                   migration::world_migration_error::unsupported_version);
        }
        auto down{wrapper.plan(5023, 5013)};
        expect(!down);
        if (!down) {
            expect(down.error() ==
                   migration::world_migration_error::invalid_request);
        }
    };

    "migration provider/migrates a synthetic older world over the contract"_test =
        [] {
            test_support::scratch_directory directory{};
            const auto root{world_path(directory)};
            expect(static_cast<bool>(
                support::create_world_at_version(root, fixture_version)));
            expect(static_cast<bool>(support::add_legacy_block_entity_chunk(
                root, world::chunk_pos{0, 0})));
            expect(static_cast<bool>(
                support::stamp_saved_data_version(root, fixture_version)));

            auto plan{load_default_composition()};
            expect(static_cast<bool>(plan));
            if (!plan) {
                return;
            }
            refrain::mod::host host{};
            auto loaded{host.load(*plan, builtin_bytes())};
            expect(static_cast<bool>(loaded));
            if (!loaded) {
                return;
            }
            const auto provider{
                host.services().active_provider("refrain:world.migration")};
            expect(provider.has_value());
            if (!provider || provider->instance == nullptr) {
                return;
            }
            const auto wrapper{make_wrapper(
                *static_cast<const mod::provider_api *>(provider->instance))};

            auto dry_request{make_request(root, fixture_version,
                                          world::target_data_version, true)};
            auto dry{wrapper.migrate(dry_request)};
            expect(static_cast<bool>(dry));
            if (dry) {
                expect(dry->dry_run == 1);
                expect(dry->chunks_scanned == 1U);
                expect(dry->chunks_migrated == 1U);
                expect(dry->steps == 3U);
            }
            expect(recorded_version(root) == fixture_version);

            auto request{make_request(root, fixture_version,
                                      world::target_data_version, false)};
            auto report{wrapper.migrate(request)};
            expect(static_cast<bool>(report));
            if (!report) {
                return;
            }
            expect(report->from_version == fixture_version);
            expect(report->to_version == world::target_data_version);
            expect(report->chunks_migrated == 1U);
            expect(report->no_op == 0);
            expect(recorded_version(root) == world::target_data_version);

            auto second_request{make_request(
                root, fixture_version, world::target_data_version, false)};
            auto second{wrapper.migrate(second_request)};
            expect(static_cast<bool>(second));
            if (second) {
                expect(second->no_op == 1);
                expect(second->steps == 0U);
            }
        };

    "migration provider/refuses an out-of-window world untouched"_test = [] {
        test_support::scratch_directory directory{};
        const auto root{world_path(directory)};
        expect(static_cast<bool>(support::create_world_at_version(root, 5012)));

        auto plan{load_default_composition()};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        refrain::mod::host host{};
        auto loaded{host.load(*plan, builtin_bytes())};
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        const auto provider{
            host.services().active_provider("refrain:world.migration")};
        expect(provider.has_value());
        if (!provider || provider->instance == nullptr) {
            return;
        }
        const auto wrapper{make_wrapper(
            *static_cast<const mod::provider_api *>(provider->instance))};
        auto request{
            make_request(root, 5012, world::target_data_version, false)};
        auto report{wrapper.migrate(request)};
        expect(!report);
        if (!report) {
            expect(report.error() ==
                   migration::world_migration_error::unsupported_version);
        }
        expect(recorded_version(root) == 5012);
    };
};

} // namespace
