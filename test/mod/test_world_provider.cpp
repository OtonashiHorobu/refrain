// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/builtin/image.hpp>
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
#include <span>
#include <string>
#include <system_error>

namespace {
using namespace boost::ut;

namespace mod = refrain::mod;

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

        auto config{world_service::make_world_config()};
        const std::string path{world_path.string()};
        config.world_path = refrain_str{
            .data = reinterpret_cast<const std::uint8_t *>(path.data()),
            .len = path.size(),
        };
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
        std::filesystem::remove_all(directory, error);
    };
};

} // namespace
