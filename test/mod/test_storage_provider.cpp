// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/builtin.hpp>
#include <refrain/contracts/abi.hpp>
#include <refrain/contracts/services/storage.hpp>
#include <refrain/kernel/gamepack.hpp>
#include <refrain/mod/discovery.hpp>
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

namespace storage = refrain::services::storage;
namespace world = refrain::world;

[[nodiscard]] auto builtin_bytes() -> std::span<const std::byte> {
    return std::span<const std::byte>{refrain::builtin::image_data(),
                                      refrain::builtin::image_size()};
}

[[nodiscard]] auto load_plan() -> refrain::result<refrain::mod::composition> {
    auto spec{refrain::kernel::load_gamepack_spec(
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
            expect(provider.is_open(*handle));

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
                    *handle, key,
                    std::span<const std::uint8_t>{
                        reinterpret_cast<const std::uint8_t *>(payload->data()),
                        payload->size()})};
                expect(written.has_value());
                expect(provider.sync(*handle).has_value());

                refrain::core::chunk_buffer buffer{};
                const auto read{provider.read_chunk(*handle, key, buffer)};
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
            expect(provider.close_world(*handle).has_value());
            expect(!provider.is_open(*handle));
            std::filesystem::remove_all(directory, error);
        };
};

} // namespace
