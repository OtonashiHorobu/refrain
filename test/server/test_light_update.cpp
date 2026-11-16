// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Incremental update encoding: the `light_update` and `block_update` bodies the
// server fans out after an edit, and the column lookup the fan-out uses.

#include <refrain/data/vanilla.hpp>
#include <refrain/net/server.hpp>
#include <refrain/net/wire.hpp>
#include <refrain/protocol/current.hpp>
#include <refrain/server/chunk_packet.hpp>
#include <refrain/server/world_port.hpp>
#include <refrain/sim/world.hpp>
#include <refrain/world/store.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace {

using boost::ut::eq;
using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace net = refrain::net;
namespace play_cb = refrain::protocol::current::play::clientbound;
namespace server = refrain::server;
namespace sim = refrain::sim;
namespace storage = refrain::storage;
namespace world = refrain::world;

[[nodiscard]] auto scratch_directory() -> std::filesystem::path {
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    const auto path{std::filesystem::temp_directory_path() /
                    ("refrain_light_update_" + std::to_string(stamp))};
    std::error_code error{};
    std::filesystem::create_directories(path, error);
    return path;
}

/// One full-height column: stone below y=0, air above.
[[nodiscard]] auto make_column() -> storage::chunk {
    storage::chunk chunk{};
    chunk.format_version = world::chunk_format_version;
    chunk.status = storage::chunk_status::full;
    chunk.y_pos = -4;
    for (auto section_y{-4}; section_y <= 19; ++section_y) {
        storage::section_record section{};
        section.y = static_cast<std::int8_t>(section_y);
        section.kind = storage::section_kind::single_value;
        section.bits_per_entry = 0;
        const auto stone{section_y < 0};
        section.palette.push_back(storage::palette_entry{
            .key = stone ? "minecraft:stone" : "minecraft:air",
            .count = stone ? static_cast<std::uint16_t>(world::section_volume)
                           : static_cast<std::uint16_t>(0),
            .state = 0,
        });
        section.biome_kind = storage::section_kind::single_value;
        section.biome_bits_per_entry = 0;
        section.biome_palette.push_back("minecraft:plains");
        chunk.sections.push_back(std::move(section));
    }
    return chunk;
}

[[nodiscard]] auto make_tables() -> std::vector<std::byte> {
    refrain::data::vanilla_spec spec{};
    spec.data_version = 5023;
    refrain::data::block_spec air{};
    air.name = "minecraft:air";
    refrain::data::state_spec air_state{};
    air_state.vanilla_id = 0;
    air_state.is_default = true;
    air.states.push_back(std::move(air_state));
    spec.blocks.push_back(std::move(air));
    refrain::data::block_spec stone{};
    stone.name = "minecraft:stone";
    refrain::data::state_spec state{};
    state.vanilla_id = 1;
    state.is_default = true;
    stone.states.push_back(std::move(state));
    spec.blocks.push_back(std::move(stone));
    // Light behaviour of the two states: air is transparent, stone opaque.
    spec.block_lights = {
        refrain::data::block_light_spec{.vanilla_id = 0,
                                        .emission = 0,
                                        .dampening = 0,
                                        .can_occlude = false,
                                        .use_shape_for_light_occlusion = false,
                                        .shape = refrain::data::no_light_shape},
        refrain::data::block_light_spec{.vanilla_id = 1,
                                        .emission = 0,
                                        .dampening = 15,
                                        .can_occlude = true,
                                        .use_shape_for_light_occlusion = false,
                                        .shape = refrain::data::no_light_shape},
    };
    auto bytes{refrain::data::build_vanilla_tables(spec)};
    expect(bytes.has_value());
    return bytes ? std::move(*bytes) : std::vector<std::byte>{};
}

[[nodiscard]] auto bit_set(std::span<const std::byte> mask, std::size_t bit)
    -> bool {
    const auto index{bit / 8U};
    return index < mask.size() &&
           (static_cast<std::uint8_t>(mask[index]) & (1U << (bit % 8U))) != 0U;
}

struct fixture {
    std::filesystem::path directory{};
    std::shared_ptr<net::registry_catalog> registries{};

    fixture() : directory{scratch_directory()} {}
    fixture(const fixture &) = delete;
    auto operator=(const fixture &) -> fixture & = delete;
    fixture(fixture &&) = delete;
    auto operator=(fixture &&) -> fixture & = delete;
    ~fixture() {
        std::error_code error{};
        std::filesystem::remove_all(directory, error);
    }
};

} // namespace

suite<"server/light-update"> light_update_tests = [] {
    "a light update carries only the changed sections and their data"_test =
        [] {
            fixture scratch{};
            {
                auto store{
                    world::world_store::create(scratch.directory.string())};
                expect(store.has_value());
                if (!store) {
                    return;
                }
                expect(store->write_chunk(world::chunk_pos{0, 0}, make_column())
                           .has_value());
                expect(store->commit().has_value());
            }
            auto runtime{sim::world::open(scratch.directory.string())};
            expect(runtime.has_value());
            if (!runtime) {
                return;
            }
            auto port{server::make_borrowed_world_port(*runtime)};
            const auto table_bytes{make_tables()};
            auto tables{
                refrain::data::vanilla_tables::open(table_bytes, false)};
            expect(tables.has_value());
            if (!tables) {
                return;
            }
            runtime->set_block_light_table(
                sim::block_light_table::from_tables(*tables));
            const server::chunk_packet_source source{
                .world = port.get(),
                .tables = &*tables,
                .registries = nullptr,
            };
            // y=-4 is solid stone (no light), y=19 sees the sky (full light).
            const std::array<std::int8_t, 2> sections{-4, 19};
            auto body{server::encode_light_update(source, {0, 0}, sections)};
            expect(body.has_value());
            if (!body) {
                return;
            }
            net::reader in{*body};
            auto packet_id{in.read_varint()};
            expect(packet_id.has_value());
            if (!packet_id) {
                return;
            }
            expect(*packet_id == play_cb::light_update::packet_id);
            play_cb::light_update decoded{};
            auto ok{play_cb::decode_light_update(in, decoded)};
            expect(ok.has_value());
            if (!ok) {
                return;
            }
            expect(decoded.x == 0);
            expect(decoded.z == 0);
            const auto &light{decoded.light_data};
            // Section index = section Y - (min section Y - 1) = Y + 5.
            expect(bit_set(light.sky_y_mask, 24U));
            expect(bit_set(light.empty_sky_y_mask, 1U));
            expect(!bit_set(light.empty_sky_y_mask, 24U));
            expect(light.sky_updates.size() == 1U);
            expect(!bit_set(light.block_y_mask, 1U));
            expect(!bit_set(light.block_y_mask, 24U));
            expect(bit_set(light.empty_block_y_mask, 1U));
            expect(bit_set(light.empty_block_y_mask, 24U));
            expect(light.block_updates.empty());
            // No other section was requested, so no other bit is set.
            expect(light.sky_y_mask.size() == 4U);
            expect(!bit_set(light.sky_y_mask, 5U));
            expect(in.empty());
        };

    "a block update carries the packed position and the protocol state"_test =
        [] {
            fixture scratch{};
            {
                auto store{
                    world::world_store::create(scratch.directory.string())};
                expect(store.has_value());
                if (!store) {
                    return;
                }
                expect(store->write_chunk(world::chunk_pos{0, 0}, make_column())
                           .has_value());
                expect(store->commit().has_value());
            }
            auto runtime{sim::world::open(scratch.directory.string())};
            expect(runtime.has_value());
            if (!runtime) {
                return;
            }
            auto port{server::make_borrowed_world_port(*runtime)};
            const auto table_bytes{make_tables()};
            auto tables{
                refrain::data::vanilla_tables::open(table_bytes, false)};
            expect(tables.has_value());
            if (!tables) {
                return;
            }
            const server::chunk_packet_source source{
                .world = port.get(),
                .tables = &*tables,
                .registries = nullptr,
            };
            auto *registry{port->registry()};
            expect(registry != nullptr);
            if (registry == nullptr) {
                return;
            }
            const auto stone{registry->intern("minecraft:stone")};
            const sim::block_change change{
                .pos = sim::block_pos{3, 64, 2},
                .old_state = sim::air_block,
                .new_state = stone,
            };
            auto body{server::encode_block_update(source, change)};
            expect(body.has_value());
            if (!body) {
                return;
            }
            net::reader in{*body};
            auto packet_id{in.read_varint()};
            expect(packet_id.has_value());
            if (!packet_id) {
                return;
            }
            expect(*packet_id == play_cb::block_update::packet_id);
            play_cb::block_update decoded{};
            auto ok{play_cb::decode_block_update(in, decoded)};
            expect(ok.has_value());
            if (!ok) {
                return;
            }
            // Vanilla BlockPos.asLong layout.
            const auto expected{
                (static_cast<std::int64_t>(3 & 0x3FFFFFF) << 38) |
                (static_cast<std::int64_t>(2 & 0x3FFFFFF) << 12) |
                static_cast<std::int64_t>(64 & 0xFFF)};
            expect(decoded.position == expected);
            expect(decoded.state == 1U);
            expect(in.empty());
        };
};
