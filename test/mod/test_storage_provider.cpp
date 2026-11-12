// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/builtin/image.hpp>
#include <refrain/contracts/abi.hpp>
#include <refrain/contracts/services/storage.hpp>
#include <refrain/mod/discovery.hpp>
#include <refrain/mod/gamepack.hpp>
#include <refrain/mod/host.hpp>
#include <refrain/world/chunk_codec.hpp>
#include <refrain/world/model.hpp>
#include <refrain/world/store.hpp>

#include <boost/ut.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>

namespace {
using namespace boost::ut;

namespace mod = refrain::mod;

namespace storage = refrain::services::storage;
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

[[nodiscard]] auto make_chunk() -> world::chunk {
    world::chunk value{};
    value.format_version = world::chunk_format_version;
    value.status = refrain::storage::chunk_status::full;
    value.y_pos = 0;
    refrain::storage::section_record section{};
    section.y = 0;
    section.kind = refrain::storage::section_kind::single_value;
    section.bits_per_entry = 0;
    section.block_count = 4096;
    section.palette.push_back(refrain::storage::palette_entry{
        .key = "minecraft:stone", .count = 4096, .state = 0});
    section.biome_kind = refrain::storage::section_kind::single_value;
    section.biome_bits_per_entry = 0;
    section.biome_palette.push_back("minecraft:plains");
    value.sections.push_back(std::move(section));
    return value;
}

[[maybe_unused]] const suite tests = [] {
    "storage/refrain.storage.rwf serves self-contained chunk payloads"_test =
        [] {
            auto plan{load_plan()};
            expect(static_cast<bool>(plan));
            if (!plan) {
                return;
            }
            refrain::mod::host host{};
            expect(static_cast<bool>(host.load(*plan, builtin_bytes())));
            const auto info{host.services().active_provider("refrain:storage")};
            expect(info.has_value());
            if (!info) {
                return;
            }
            expect(info->id == "refrain.storage.rwf");
            const auto *api{static_cast<const refrain::mod::provider_api *>(
                info->instance)};
            expect(api != nullptr);
            if (api == nullptr) {
                return;
            }
            const storage::provider provider{
                *static_cast<const refrain_services_storage_vtable *>(
                    api->vtable),
                api->self};

            const auto stamp{
                std::chrono::steady_clock::now().time_since_epoch().count()};
            const auto directory{
                std::filesystem::temp_directory_path() /
                ("refrain-storage-provider-" + std::to_string(stamp))};
            std::error_code error;
            std::filesystem::create_directories(directory, error);
            const auto world_path{directory / "world"};
            {
                auto created{world::world_store::create(world_path.string())};
                expect(created.has_value());
                if (!created) {
                    std::filesystem::remove_all(directory, error);
                    return;
                }
            }

            auto config{storage::make_world_open_config()};
            const std::string path{world_path.string()};
            config.world_path = refrain_str{
                .data = reinterpret_cast<const std::uint8_t *>(path.data()),
                .len = path.size(),
            };
            auto handle{provider.open_world(config)};
            expect(handle.has_value());
            if (!handle) {
                std::filesystem::remove_all(directory, error);
                return;
            }
            // A consumer identifies the store by the handle and the provider
            // that owns it, and can reach the in-process store directly.
            expect(handle->owner.vtable != nullptr);
            expect(handle->owner.self != nullptr);
            expect(handle->owner.abi_major ==
                   REFRAIN_SERVICES_STORAGE_ABI_MAJOR);
            expect(handle->store != nullptr);
            expect(
                provider.is_open(refrain::core::storage_world{handle->world}));

            const auto chunk{make_chunk()};
            auto payload{world::encode_chunk(
                chunk, nullptr, true, world::chunk_palette_mode::inline_keys)};
            expect(payload.has_value());
            if (payload) {
                auto key{storage::make_chunk_key()};
                key.pos.x = 0;
                key.pos.z = 0;
                key.format_version = world::chunk_format_version;
                const auto written{provider.write_chunk(
                    refrain::core::storage_world{handle->world}, key,
                    std::span<const std::uint8_t>{
                        reinterpret_cast<const std::uint8_t *>(payload->data()),
                        payload->size()})};
                expect(written.has_value());
                expect(
                    provider.sync(refrain::core::storage_world{handle->world})
                        .has_value());

                refrain::core::chunk_buffer buffer{};
                const auto read{provider.read_chunk(
                    refrain::core::storage_world{handle->world}, key, buffer)};
                expect(read.has_value());
                if (read) {
                    expect(read->bytes_read > 0U);
                    const auto bytes{refrain::contracts::as_bytes(buffer.data)};
                    auto decoded{world::decode_chunk(std::span<const std::byte>{
                        reinterpret_cast<const std::byte *>(bytes.data()),
                        bytes.size()})};
                    expect(decoded.has_value());
                    if (decoded) {
                        expect(decoded->y_pos == 0);
                        expect(decoded->status ==
                               refrain::storage::chunk_status::full);
                    }
                }
            }
            expect(provider
                       .close_world(refrain::core::storage_world{handle->world})
                       .has_value());
            expect(
                !provider.is_open(refrain::core::storage_world{handle->world}));
            std::filesystem::remove_all(directory, error);
        };

    "storage/refrain.storage.rwf opens a level directory once"_test = [] {
        auto plan{load_plan()};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        refrain::mod::host host{};
        expect(static_cast<bool>(host.load(*plan, builtin_bytes())));
        const auto info{host.services().active_provider("refrain:storage")};
        expect(info.has_value());
        if (!info) {
            return;
        }
        const auto *api{
            static_cast<const refrain::mod::provider_api *>(info->instance)};
        expect(api != nullptr);
        if (api == nullptr) {
            return;
        }
        const storage::provider provider{
            *static_cast<const refrain_services_storage_vtable *>(api->vtable),
            api->self};

        const auto stamp{
            std::chrono::steady_clock::now().time_since_epoch().count()};
        const auto directory{
            std::filesystem::temp_directory_path() /
            ("refrain-storage-single-open-" + std::to_string(stamp))};
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        const auto level_path{directory / "world"};
        const auto other_path{directory / "other"};
        {
            auto created{world::world_store::create(level_path.string())};
            auto other{world::world_store::create(other_path.string())};
            expect(created.has_value());
            expect(other.has_value());
            if (!created || !other) {
                std::filesystem::remove_all(directory, error);
                return;
            }
        }
        const std::string path{level_path.string()};
        const auto open_level = [&provider](const std::string &root) {
            auto config{storage::make_world_open_config()};
            config.world_path = refrain_str{
                .data = reinterpret_cast<const std::uint8_t *>(root.data()),
                .len = root.size(),
            };
            config.gamepack_id = refrain_str{};
            config.flags = 0;
            return provider.open_world(config);
        };

        auto first{open_level(path)};
        expect(first.has_value());
        if (!first) {
            std::filesystem::remove_all(directory, error);
            return;
        }
        const auto *first_store{first->store};

        // A second open of the same directory is refused: the store already
        // owns the level directory, and a second one would fight it for the
        // same region files.
        auto second{open_level(path)};
        expect(!second.has_value());
        if (!second) {
            expect(second.error() == storage::storage_error::already_open);
        }
        // The spelling of the path does not matter; the registry key is the
        // normalized directory.
        auto respelled{open_level(path + "/.")};
        expect(!respelled.has_value());

        // Another directory is a different store.
        auto other{open_level(other_path.string())};
        expect(other.has_value());
        if (other) {
            expect(other->store != first_store);
            expect(other->world != first->world);
            expect(
                provider.close_world(refrain::core::storage_world{other->world})
                    .has_value());
        }
        // The refused open left the first store untouched.
        expect(provider.is_open(refrain::core::storage_world{first->world}));
        expect(provider.close_world(refrain::core::storage_world{first->world})
                   .has_value());
        expect(!provider.is_open(refrain::core::storage_world{first->world}));

        // Closing released the directory, so the next open starts a new store.
        auto reopened{open_level(path)};
        expect(reopened.has_value());
        if (reopened) {
            expect(reopened->world != first->world);
            expect(
                provider
                    .close_world(refrain::core::storage_world{reopened->world})
                    .has_value());
        }
        std::filesystem::remove_all(directory, error);
    };
};

} // namespace
