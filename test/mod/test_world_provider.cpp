// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/builtin/image.hpp>
#include <refrain/contracts/services/storage.hpp>
#include <refrain/contracts/services/world.hpp>
#include <refrain/mod/discovery.hpp>
#include <refrain/mod/gamepack.hpp>
#include <refrain/mod/host.hpp>
#include <refrain/sim/world.hpp>
#include <refrain/world/model.hpp>
#include <refrain/world/store.hpp>

#include <boost/ut.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <system_error>

namespace {
using namespace boost::ut;

namespace mod = refrain::mod;

namespace storage_service = refrain::services::storage;
namespace world_service = refrain::services::world;
namespace world = refrain::world;

[[nodiscard]] auto builtin_bytes() -> std::span<const std::byte> {
    return std::span<const std::byte>{refrain::builtin::image_data(),
                                      refrain::builtin::image_size()};
}

[[nodiscard]] auto load_plan() -> refrain::result<refrain::mod::composition> {
    auto spec{refrain::mod::load_gamepack_spec(
        std::string{REFRAIN_SOURCE_DIR} + "/gamepack/gamepack.toml", "server")};
    if (!spec) {
        return std::unexpected{spec.error()};
    }
    return refrain::mod::preview_composition(*spec);
}

[[nodiscard]] auto make_store(const std::filesystem::path &path)
    -> refrain::result<world::world_store> {
    auto store{world::world_store::create(path.string())};
    if (!store) {
        return std::unexpected{std::move(store.error())};
    }
    world::chunk chunk{};
    chunk.format_version = world::chunk_format_version;
    chunk.status = refrain::storage::chunk_status::full;
    chunk.y_pos = 0;
    refrain::storage::section_record section{};
    section.y = 0;
    section.kind = refrain::storage::section_kind::single_value;
    section.block_count = 4096;
    section.palette.push_back(refrain::storage::palette_entry{
        .key = "minecraft:stone", .count = 4096, .state = 0});
    section.biome_kind = refrain::storage::section_kind::single_value;
    section.biome_palette.push_back("minecraft:plains");
    chunk.sections.push_back(std::move(section));
    auto written{store->write_chunk({0, 0}, chunk)};
    if (!written) {
        return std::unexpected{std::move(written.error())};
    }
    auto committed{store->commit()};
    if (!committed) {
        return std::unexpected{std::move(committed.error())};
    }
    return store;
}

[[nodiscard]] auto open_level(const refrain::mod::host &host,
                              const std::string &path)
    -> std::optional<storage_service::storage_handle> {
    const auto info{host.services().active_provider("refrain:storage")};
    if (!info || info->instance == nullptr) {
        return std::nullopt;
    }
    const auto *api{
        static_cast<const refrain::mod::provider_api *>(info->instance)};
    if (api == nullptr || api->vtable == nullptr) {
        return std::nullopt;
    }
    const storage_service::provider storage{
        *static_cast<const refrain_services_storage_vtable *>(api->vtable),
        api->self};
    auto config{storage_service::make_world_open_config()};
    config.world_path = refrain_str{
        .data = reinterpret_cast<const std::uint8_t *>(path.data()),
        .len = path.size(),
    };
    config.gamepack_id = refrain_str{};
    config.flags = 0;
    auto opened{storage.open_world(config)};
    if (!opened) {
        return std::nullopt;
    }
    return *opened;
}

[[nodiscard]] auto close_level(const refrain::mod::host &host,
                               refrain_core_handle_storage_world handle)
    -> bool {
    const auto info{host.services().active_provider("refrain:storage")};
    if (!info || info->instance == nullptr) {
        return false;
    }
    const auto *api{
        static_cast<const refrain::mod::provider_api *>(info->instance)};
    if (api == nullptr || api->vtable == nullptr) {
        return false;
    }
    const storage_service::provider storage{
        *static_cast<const refrain_services_storage_vtable *>(api->vtable),
        api->self};
    return storage.close_world(refrain::core::storage_world{handle})
        .has_value();
}

[[maybe_unused]] const suite tests = [] {
    "world/refrain.world.vanilla serves chunks and block changes"_test = [] {
        auto plan{load_plan()};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        refrain::mod::host host{};
        expect(static_cast<bool>(host.load(*plan, builtin_bytes())));
        const auto info{host.services().active_provider("refrain:world")};
        expect(info.has_value());
        if (!info) {
            return;
        }
        expect(info->id == "refrain.world.vanilla");
        const auto *api{
            static_cast<const refrain::mod::provider_api *>(info->instance)};
        expect(api != nullptr);
        if (api == nullptr) {
            return;
        }
        const world_service::provider provider{
            *static_cast<const refrain_services_world_vtable *>(api->vtable),
            api->self};

        const auto stamp{
            std::chrono::steady_clock::now().time_since_epoch().count()};
        const auto directory{
            std::filesystem::temp_directory_path() /
            ("refrain-world-provider-" + std::to_string(stamp))};
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        const auto world_path{directory / "world"};
        {
            auto created{make_store(world_path)};
            expect(created.has_value());
            if (!created) {
                std::filesystem::remove_all(directory, error);
                return;
            }
        }

        const std::string path{world_path.string()};
        auto store{open_level(host, path)};
        expect(store.has_value());
        if (!store) {
            std::filesystem::remove_all(directory, error);
            return;
        }
        // The handle names the store and the provider that owns it, and it
        // carries the in-process store the world view attaches to.
        expect(store->store != nullptr);
        expect(store->owner.vtable != nullptr);
        expect(store->owner.self != nullptr);
        expect(store->owner.abi_major == REFRAIN_SERVICES_STORAGE_ABI_MAJOR);

        auto config{world_service::make_world_config()};
        config.storage = *store;
        config.seed = 7;
        expect(provider.open_world(config).has_value());

        refrain::core::chunk_pos pos{};
        refrain::contracts::init_struct(pos);
        expect(!provider.is_chunk_loaded(pos));
        const auto view_result{provider.chunk_at(pos)};
        expect(view_result.has_value());
        expect(provider.is_chunk_loaded(pos));
        if (view_result) {
            expect(view_result->section_count == 1U);
            expect(view_result->sections.len == 1U);
        }

        refrain::core::block_pos block{};
        refrain::contracts::init_struct(block);
        block.x = 0;
        block.y = 2;
        block.z = 3;
        expect(provider.block_at(block) != 0U);

        refrain::core::block_change change{};
        refrain::contracts::init_struct(change);
        change.pos = block;
        change.old_state = provider.block_at(block);
        change.new_state = 0;
        const refrain::core::block_change changes[]{change};
        const auto applied{provider.apply_block_changes(changes)};
        expect(applied.has_value());
        if (applied) {
            expect(*applied == 1U);
        }
        expect(provider.block_at(block) == 0U);
        expect(provider.world_tick() == 0);
        expect(provider.close_world().has_value());
        expect(!provider.is_chunk_loaded(pos));
        expect(close_level(host, store->world));
        std::filesystem::remove_all(directory, error);
    };

    "world/refrain.world.vanilla refuses a store the caller did not open"_test =
        [] {
            auto plan{load_plan()};
            expect(static_cast<bool>(plan));
            if (!plan) {
                return;
            }
            refrain::mod::host host{};
            expect(static_cast<bool>(host.load(*plan, builtin_bytes())));
            const auto info{host.services().active_provider("refrain:world")};
            expect(info.has_value());
            if (!info) {
                return;
            }
            const auto *api{static_cast<const refrain::mod::provider_api *>(
                info->instance)};
            expect(api != nullptr);
            if (api == nullptr) {
                return;
            }
            const world_service::provider provider{
                *static_cast<const refrain_services_world_vtable *>(
                    api->vtable),
                api->self};
            auto config{world_service::make_world_config()};
            config.storage = storage_service::make_storage_handle();
            config.seed = 7;
            const auto opened{provider.open_world(config)};
            expect(!opened.has_value());
            if (!opened) {
                expect(opened.error() ==
                       world_service::world_error::storage_unavailable);
            }
        };
};

} // namespace
