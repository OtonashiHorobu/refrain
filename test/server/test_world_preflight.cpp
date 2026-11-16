// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// World open preflight.
//
// The runtime checks a world's recorded data version before opening it: a
// current world opens unchanged, an older world is upgraded through the active
// migration provider, and anything else fails closed with the version and the
// supported window named in the message. A composition without a migration
// provider refuses an older world and leaves it untouched, which is what makes
// the migration a separable mod rather than a hidden storage behaviour.

#include <refrain/builtin/image.hpp>
#include <refrain/core/io.hpp>
#include <refrain/mod/discovery.hpp>
#include <refrain/mod/gamepack.hpp>
#include <refrain/mod/host.hpp>
#include <refrain/server/server_runtime.hpp>
#include <refrain/server/world_port.hpp>
#include <refrain/server/world_preflight.hpp>
#include <refrain/world/migration.hpp>
#include <refrain/world/store.hpp>
#include <refrain/world/version.hpp>

#include <boost/ut.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "support/migration_world.hpp"
#include "support/scratch_directory.hpp"

namespace {

using namespace boost::ut;

namespace mod = refrain::mod;
namespace server = refrain::server;
namespace support = refrain::test_support;
namespace test_support = refrain::test_support;
namespace world = refrain::world;

constexpr std::int32_t fixture_version = 5013;

[[nodiscard]] auto builtin_bytes() -> std::span<const std::byte> {
    return std::span<const std::byte>{refrain::builtin::image_data(),
                                      refrain::builtin::image_size()};
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

/// Load the default composition and project its migration provider into the
/// handle the open path takes. The host stays alive for the caller because the
/// provider tables live in it.
struct loaded_composition {
    mod::host host{};
    server::world_migration_handle migration{};
    bool ok{false};

    static auto load() -> std::unique_ptr<loaded_composition> {
        auto result{std::make_unique<loaded_composition>()};
        auto spec{mod::load_gamepack_spec(std::string{REFRAIN_SOURCE_DIR} +
                                              "/gamepack/gamepack.toml",
                                          "server")};
        if (!spec) {
            return result;
        }
        auto plan{mod::preview_composition(*spec)};
        if (!plan) {
            return result;
        }
        if (!result->host.load(*plan, builtin_bytes())) {
            return result;
        }
        const auto provider{
            result->host.services().active_provider("refrain:world.migration")};
        if (!provider || provider->instance == nullptr) {
            return result;
        }
        const auto *api{
            static_cast<const mod::provider_api *>(provider->instance)};
        if (api == nullptr || api->vtable == nullptr) {
            return result;
        }
        result->migration = server::world_migration_handle{
            .vtable =
                static_cast<const refrain_services_world_migration_vtable *>(
                    api->vtable),
            .self = api->self,
            .provider = provider->id,
        };
        result->ok = true;
        return result;
    }
};

const suite world_preflight_tests = [] {
    "preflight/leaves a current world openable without a provider"_test = [] {
        test_support::scratch_directory directory{};
        const auto root{world_path(directory)};
        expect(static_cast<bool>(support::create_world_at_version(
            root, world::target_data_version)));
        expect(static_cast<bool>(server::prepare_world_open(
            root, server::world_migration_handle{})));
        expect(recorded_version(root) == world::target_data_version);
    };

    "preflight/skips a directory that is not an RWF world"_test = [] {
        test_support::scratch_directory directory{};
        const auto root{directory.string()};
        // A port for another storage format owns its directory and any data
        // version upgrade; the preflight must not touch it.
        expect(static_cast<bool>(server::prepare_world_open(
            root, server::world_migration_handle{})));
    };

    "preflight/refuses an older world without a provider untouched"_test = [] {
        test_support::scratch_directory directory{};
        const auto root{world_path(directory)};
        expect(static_cast<bool>(
            support::create_world_at_version(root, fixture_version)));
        const auto meta_before{
            refrain::core::io::read_file(root + "/meta.rfw")};
        expect(static_cast<bool>(meta_before));

        auto prepared{
            server::prepare_world_open(root, server::world_migration_handle{})};
        expect(!prepared);
        if (!prepared) {
            expect(prepared.error().code == refrain::error_code::unsupported);
            const auto message{prepared.error().message};
            expect(message.find(std::to_string(fixture_version)) !=
                   std::string::npos);
            expect(message.find(std::to_string(world::target_data_version)) !=
                   std::string::npos);
            expect(message.find("migration provider") != std::string::npos);
        }
        expect(recorded_version(root) == fixture_version);
        auto meta_after{refrain::core::io::read_file(root + "/meta.rfw")};
        expect(static_cast<bool>(meta_after));
        if (meta_before && meta_after) {
            expect(*meta_after == *meta_before);
        }
    };

    "preflight/upgrades an older world through the active provider"_test = [] {
        test_support::scratch_directory directory{};
        const auto root{world_path(directory)};
        expect(static_cast<bool>(
            support::create_world_at_version(root, fixture_version)));
        expect(static_cast<bool>(support::add_legacy_block_entity_chunk(
            root, world::chunk_pos{0, 0})));

        auto composition{loaded_composition::load()};
        expect(composition->ok);
        if (!composition->ok) {
            return;
        }
        expect(static_cast<bool>(
            server::prepare_world_open(root, composition->migration)));
        expect(recorded_version(root) == world::target_data_version);
        // The upgraded world opens and passes the strict level validation.
        auto store{world::world_store::open(root)};
        expect(static_cast<bool>(store));
        if (store) {
            auto valid{store->validate_level()};
            expect(static_cast<bool>(valid));
            if (valid) {
                expect(valid->data_version == world::target_data_version);
            }
        }
        // A second preflight over the upgraded world is a no-op and needs no
        // provider call; it stays openable.
        expect(static_cast<bool>(server::prepare_world_open(
            root, server::world_migration_handle{})));
    };

    "preflight/refuses a version outside the provider window untouched"_test =
        [] {
            test_support::scratch_directory directory{};
            const auto root{world_path(directory)};
            expect(static_cast<bool>(
                support::create_world_at_version(root, 5012)));
            const auto meta_before{
                refrain::core::io::read_file(root + "/meta.rfw")};

            auto composition{loaded_composition::load()};
            expect(composition->ok);
            if (!composition->ok) {
                return;
            }
            auto prepared{
                server::prepare_world_open(root, composition->migration)};
            expect(!prepared);
            if (!prepared) {
                expect(prepared.error().code ==
                       refrain::error_code::unsupported);
                const auto message{prepared.error().message};
                expect(message.find("5012") != std::string::npos);
                expect(message.find(std::to_string(
                           world::oldest_migratable_data_version)) !=
                       std::string::npos);
                expect(message.find(std::to_string(
                           world::target_data_version)) != std::string::npos);
            }
            expect(recorded_version(root) == 5012);
            auto meta_after{refrain::core::io::read_file(root + "/meta.rfw")};
            expect(static_cast<bool>(meta_after));
            if (meta_before && meta_after) {
                expect(*meta_after == *meta_before);
            }
        };

    "preflight/refuses a world newer than this build"_test = [] {
        test_support::scratch_directory directory{};
        const auto root{world_path(directory)};
        expect(static_cast<bool>(support::create_world_at_version(
            root, world::target_data_version + 1)));

        auto composition{loaded_composition::load()};
        expect(composition->ok);
        if (!composition->ok) {
            return;
        }
        auto prepared{server::prepare_world_open(root, composition->migration)};
        expect(!prepared);
        if (!prepared) {
            const auto message{prepared.error().message};
            expect(message.find(std::to_string(world::target_data_version +
                                               1)) != std::string::npos);
        }
        expect(recorded_version(root) == world::target_data_version + 1);
    };

    "preflight/run_server refuses an older world without a provider"_test = [] {
        test_support::scratch_directory directory{};
        const auto root{world_path(directory)};
        expect(static_cast<bool>(
            support::create_world_at_version(root, fixture_version)));
        // The world holds a chunk so the runtime would not try to generate the
        // spawn area: the preflight is what stops it.
        expect(static_cast<bool>(support::add_legacy_block_entity_chunk(
            root, world::chunk_pos{0, 0})));

        auto port{server::make_world_port(server::server_service_handles{})};
        expect(port != nullptr);
        if (!port) {
            return;
        }
        server::server_run_options options{};
        options.world_path = root;
        options.ticks = 1;
        expect(server::run_server(options, server::server_service_handles{},
                                  *port) == 1);
        expect(recorded_version(root) == fixture_version);
    };
};

} // namespace
