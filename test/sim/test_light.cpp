// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Sky and block light over loaded columns: cave darkness, torches, borders
// between columns, incremental edits and reload order independence.
//
// The fixtures are synthetic flat worlds (stone below y=64, air above) with
// explicit block overrides; no game data is involved.

#include <refrain/sim/world.hpp>
#include <refrain/world/model.hpp>
#include <refrain/world/store.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using boost::ut::eq;
using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace sim = refrain::sim;
namespace storage = refrain::storage;
namespace world = refrain::world;

constexpr std::int32_t fixture_y_pos{-4};
constexpr std::int32_t fixture_top_section{19};
constexpr std::int32_t surface_y{64};

struct block_override {
    std::int32_t x;
    std::int32_t y;
    std::int32_t z;
    std::string_view key;
};

[[nodiscard]] auto scratch_directory() -> std::filesystem::path {
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    const auto path{std::filesystem::temp_directory_path() /
                    ("refrain_light_" + std::to_string(stamp))};
    std::error_code error{};
    std::filesystem::create_directories(path, error);
    return path;
}

struct world_fixture {
    std::filesystem::path directory{};
    std::optional<world::world_store> store{};

    world_fixture() : directory{scratch_directory()} {}
    world_fixture(const world_fixture &) = delete;
    auto operator=(const world_fixture &) -> world_fixture & = delete;
    world_fixture(world_fixture &&) = delete;
    auto operator=(world_fixture &&) -> world_fixture & = delete;
    ~world_fixture() {
        store.reset();
        std::error_code error{};
        std::filesystem::remove_all(directory, error);
    }
};

/// One column of the synthetic world: stone below `surface_y`, air above, with
/// the given overrides. `x`/`z` are world block coordinates.
[[nodiscard]] auto make_column(std::span<const block_override> overrides)
    -> storage::chunk {
    constexpr auto section_blocks{world::section_volume};
    constexpr std::string_view stone{"minecraft:stone"};
    constexpr std::string_view air{"minecraft:air"};
    constexpr std::string_view default_biome{"minecraft:plains"};
    std::map<std::int32_t, std::map<std::size_t, std::string_view>>
        by_section{};
    for (const auto &entry : overrides) {
        const auto section_y{entry.y >= 0
                                 ? entry.y / 16
                                 : (entry.y - (world::section_edge - 1)) /
                                       world::section_edge};
        const auto local_y{entry.y - section_y * 16};
        const auto local_x{entry.x & 15};
        const auto local_z{entry.z & 15};
        const auto index{static_cast<std::size_t>((local_y << 8) |
                                                  (local_z << 4) | local_x)};
        by_section[section_y][index] = entry.key;
    }
    storage::chunk chunk{};
    chunk.format_version = world::chunk_format_version;
    chunk.status = storage::chunk_status::full;
    chunk.y_pos = fixture_y_pos;
    for (auto section_y{fixture_y_pos}; section_y <= fixture_top_section;
         ++section_y) {
        storage::section_record section{};
        section.y = static_cast<std::int8_t>(section_y);
        section.biome_kind = storage::section_kind::single_value;
        section.biome_bits_per_entry = 0;
        section.biome_palette.emplace_back(default_biome);
        const auto section_top{section_y * 16 + 15};
        const auto section_bottom{section_y * 16};
        const auto default_key = [](std::int32_t y) -> std::string_view {
            return y < surface_y ? stone : air;
        };
        const auto overrides_here{by_section.find(section_y)};
        const bool has_overrides{overrides_here != by_section.end() &&
                                 !overrides_here->second.empty()};
        if (!has_overrides &&
            (section_top < surface_y || section_bottom >= surface_y)) {
            section.kind = storage::section_kind::single_value;
            section.bits_per_entry = 0;
            section.block_count =
                section_top < surface_y
                    ? static_cast<std::uint16_t>(section_blocks)
                    : 0;
            section.palette.push_back(storage::palette_entry{
                .key = std::string{default_key(section_bottom)},
                .count = section.block_count,
                .state = 0,
            });
            chunk.sections.push_back(std::move(section));
            continue;
        }
        std::vector<std::string_view> values(section_blocks);
        for (std::size_t index{0}; index < section_blocks; ++index) {
            values[index] = default_key(section_bottom +
                                        static_cast<std::int32_t>(index >> 8));
        }
        if (has_overrides) {
            for (const auto &[index, key] : overrides_here->second) {
                values[index] = key;
            }
        }
        std::vector<std::string_view> palette{};
        std::vector<std::uint32_t> indices(section_blocks, 0);
        std::map<std::string_view, std::uint32_t> palette_index{};
        std::uint16_t non_air{0};
        for (std::size_t index{0}; index < section_blocks; ++index) {
            const auto key{values[index]};
            const auto found{palette_index.find(key)};
            if (found == palette_index.end()) {
                const auto next{static_cast<std::uint32_t>(palette.size())};
                palette_index.emplace(key, next);
                palette.push_back(key);
                indices[index] = next;
            } else {
                indices[index] = found->second;
            }
            if (key != air) {
                ++non_air;
            }
        }
        section.block_count = non_air;
        section.kind = palette.size() == 1 ? storage::section_kind::single_value
                                           : storage::section_kind::palette;
        section.palette.reserve(palette.size());
        for (const auto key : palette) {
            section.palette.push_back(storage::palette_entry{
                .key = std::string{key}, .count = 0, .state = 0});
        }
        if (section.kind == storage::section_kind::palette) {
            section.bits_per_entry = world::bits_for_palette(palette.size());
            const auto packed{world::pack_palette_indices(
                indices, section.bits_per_entry, palette.size(), section.data)};
            expect(packed.has_value());
        }
        chunk.sections.push_back(std::move(section));
    }
    return chunk;
}

/// Write `columns` into a fresh store in `fixture.directory`.
void write_columns(
    world_fixture &fixture,
    std::span<const std::pair<world::chunk_pos, storage::chunk>> columns) {
    auto created{world::world_store::create(fixture.directory.string())};
    expect(created.has_value()) << "store creation failed";
    if (!created) {
        return;
    }
    for (const auto &[pos, column] : columns) {
        auto written{created->write_chunk(pos, column)};
        expect(written.has_value()) << (written ? "" : written.error().message);
    }
    auto committed{created->commit()};
    expect(committed.has_value());
    fixture.store.emplace(std::move(*created));
}

[[nodiscard]] auto open_world(world_fixture &fixture)
    -> std::optional<sim::world> {
    auto opened{sim::world::open(fixture.directory.string())};
    expect(opened.has_value()) << (opened ? "" : opened.error().message);
    if (!opened) {
        return std::nullopt;
    }
    return std::optional<sim::world>{std::move(*opened)};
}

constexpr block_override stone_at(std::int32_t x, std::int32_t y,
                                  std::int32_t z) {
    return block_override{.x = x, .y = y, .z = z, .key = "minecraft:stone"};
}

constexpr block_override air_at(std::int32_t x, std::int32_t y,
                                std::int32_t z) {
    return block_override{.x = x, .y = y, .z = z, .key = "minecraft:air"};
}

constexpr block_override torch_at(std::int32_t x, std::int32_t y,
                                  std::int32_t z) {
    return block_override{.x = x, .y = y, .z = z, .key = "minecraft:torch"};
}

[[nodiscard]] auto sky(const sim::world &world, std::int32_t x, std::int32_t y,
                       std::int32_t z) -> std::uint8_t {
    return world.light().sky_light_at(
        sim::block_pos{x, static_cast<std::int16_t>(y), z});
}

[[nodiscard]] auto block_light(const sim::world &world, std::int32_t x,
                               std::int32_t y, std::int32_t z) -> std::uint8_t {
    return world.light().block_light_at(
        sim::block_pos{x, static_cast<std::int16_t>(y), z});
}

/// Light arrays of one column as a flat byte vector, for order-independence
/// comparisons.
[[nodiscard]] auto light_bytes_of(const sim::world &world,
                                  const world::chunk_pos &pos)
    -> std::vector<std::byte> {
    const auto *column{world.find_chunk(pos)};
    expect(column != nullptr);
    if (column == nullptr) {
        return {};
    }
    std::vector<std::byte> bytes{};
    for (const auto &layer : column->storage.light_sections) {
        bytes.push_back(static_cast<std::byte>(layer.y));
        bytes.insert(bytes.end(), layer.sky_light.begin(),
                     layer.sky_light.end());
        bytes.insert(bytes.end(), layer.block_light.begin(),
                     layer.block_light.end());
    }
    return bytes;
}

} // namespace

suite<"sim/light"> light_tests = [] {
    "open sky above a flat world is full and the ground row is dark"_test = [] {
        world_fixture fixture{};
        const std::array<block_override, 1> overrides{{
            torch_at(8, 64, 8),
        }};
        std::vector<std::pair<world::chunk_pos, storage::chunk>> columns{};
        columns.emplace_back(world::chunk_pos{0, 0}, make_column(overrides));
        write_columns(fixture, columns);
        auto world{open_world(fixture)};
        expect(world.has_value());
        if (!world) {
            return;
        }
        expect(world->ensure_chunk(world::chunk_pos{0, 0}).has_value());
        expect(eq(static_cast<int>(sky(*world, 8, 319, 8)), 15));
        expect(eq(static_cast<int>(sky(*world, 8, 70, 8)), 15));
        expect(eq(static_cast<int>(sky(*world, 8, 64, 8)), 15));
        expect(eq(static_cast<int>(sky(*world, 8, 63, 8)), 0));
        expect(eq(static_cast<int>(sky(*world, 8, 40, 8)), 0));
    };

    "a sealed cave is dark and a torch lights it"_test = [] {
        world_fixture fixture{};
        std::vector<block_override> overrides{};
        // A 7×7×4 room from y=65 to y=68, sealed by a stone roof at y=69 and
        // stone walls, with a torch on its floor.
        for (auto z{4}; z <= 12; ++z) {
            for (auto x{4}; x <= 12; ++x) {
                overrides.push_back(stone_at(x, 69, z));
            }
        }
        for (auto y{64}; y <= 68; ++y) {
            for (auto lateral{4}; lateral <= 12; ++lateral) {
                overrides.push_back(stone_at(4, y, lateral));
                overrides.push_back(stone_at(12, y, lateral));
                overrides.push_back(stone_at(lateral, y, 4));
                overrides.push_back(stone_at(lateral, y, 12));
            }
        }
        overrides.push_back(torch_at(8, 65, 8));
        std::vector<std::pair<world::chunk_pos, storage::chunk>> columns{};
        columns.emplace_back(world::chunk_pos{0, 0}, make_column(overrides));
        write_columns(fixture, columns);
        auto world{open_world(fixture)};
        expect(world.has_value());
        if (!world) {
            return;
        }
        expect(world->ensure_chunk(world::chunk_pos{0, 0}).has_value());
        expect(eq(static_cast<int>(sky(*world, 8, 65, 8)), 0));
        expect(eq(static_cast<int>(sky(*world, 8, 68, 8)), 0));
        expect(eq(static_cast<int>(sky(*world, 10, 66, 9)), 0));
        // The torch emits 14 and falls off by one per step.
        expect(eq(static_cast<int>(block_light(*world, 8, 65, 8)), 14));
        expect(eq(static_cast<int>(block_light(*world, 9, 65, 8)), 13));
        expect(eq(static_cast<int>(block_light(*world, 8, 66, 8)), 13));
        expect(eq(static_cast<int>(block_light(*world, 7, 65, 8)), 13));
        expect(eq(static_cast<int>(block_light(*world, 11, 65, 8)), 11));
        // Outside the cave, the open sky is unaffected by the torch.
        expect(eq(static_cast<int>(sky(*world, 0, 64, 0)), 15));
    };

    "block light crosses a column border from the emitting side"_test = [] {
        world_fixture fixture{};
        std::vector<std::pair<world::chunk_pos, storage::chunk>> columns{};
        const std::array<block_override, 1> left{{
            torch_at(15, 64, 8),
        }};
        const std::array<block_override, 0> right{};
        columns.emplace_back(world::chunk_pos{0, 0}, make_column(left));
        columns.emplace_back(world::chunk_pos{1, 0}, make_column(right));
        write_columns(fixture, columns);
        auto world{open_world(fixture)};
        expect(world.has_value());
        if (!world) {
            return;
        }
        expect(world->ensure_chunk(world::chunk_pos{0, 0}).has_value());
        expect(world->ensure_chunk(world::chunk_pos{1, 0}).has_value());
        expect(eq(static_cast<int>(block_light(*world, 15, 64, 8)), 14));
        expect(eq(static_cast<int>(block_light(*world, 16, 64, 8)), 13));
        expect(eq(static_cast<int>(block_light(*world, 17, 64, 8)), 12));
    };

    "a column loaded after its neighbour receives the neighbour's light"_test =
        [] {
            world_fixture fixture{};
            std::vector<std::pair<world::chunk_pos, storage::chunk>> columns{};
            const std::array<block_override, 1> left{{
                torch_at(15, 64, 8),
            }};
            const std::array<block_override, 0> right{};
            columns.emplace_back(world::chunk_pos{0, 0}, make_column(left));
            columns.emplace_back(world::chunk_pos{1, 0}, make_column(right));
            write_columns(fixture, columns);
            auto world{open_world(fixture)};
            expect(world.has_value());
            if (!world) {
                return;
            }
            // Load the dark column first, then the lit one.
            expect(world->ensure_chunk(world::chunk_pos{1, 0}).has_value());
            expect(eq(static_cast<int>(block_light(*world, 16, 64, 8)), 0));
            expect(world->ensure_chunk(world::chunk_pos{0, 0}).has_value());
            expect(eq(static_cast<int>(block_light(*world, 16, 64, 8)), 13));
        };

    "breaking a block of a sealed roof lets sky light into the cave and "
    "placing it back removes it"_test = [] {
        world_fixture fixture{};
        std::vector<block_override> overrides{};
        for (auto z{4}; z <= 12; ++z) {
            for (auto x{4}; x <= 12; ++x) {
                overrides.push_back(stone_at(x, 69, z));
            }
        }
        for (auto y{64}; y <= 68; ++y) {
            for (auto lateral{4}; lateral <= 12; ++lateral) {
                overrides.push_back(stone_at(4, y, lateral));
                overrides.push_back(stone_at(12, y, lateral));
                overrides.push_back(stone_at(lateral, y, 4));
                overrides.push_back(stone_at(lateral, y, 12));
            }
        }
        std::vector<std::pair<world::chunk_pos, storage::chunk>> columns{};
        columns.emplace_back(world::chunk_pos{0, 0}, make_column(overrides));
        write_columns(fixture, columns);
        auto world{open_world(fixture)};
        expect(world.has_value());
        if (!world) {
            return;
        }
        expect(world->ensure_chunk(world::chunk_pos{0, 0}).has_value());
        expect(eq(static_cast<int>(sky(*world, 8, 65, 8)), 0));
        const auto air{world->registry().find("minecraft:air")};
        const auto stone{world->registry().find("minecraft:stone")};
        expect(air.has_value());
        expect(stone.has_value());
        if (!air || !stone) {
            return;
        }
        // A hole in the roof drops the full sky onto the room floor.
        expect(world->set_block(sim::block_pos{8, 69, 8}, *air).has_value());
        expect(eq(static_cast<int>(sky(*world, 8, 68, 8)), 15));
        expect(eq(static_cast<int>(sky(*world, 8, 65, 8)), 15));
        // Cover it again: the room goes dark.
        expect(world->set_block(sim::block_pos{8, 69, 8}, *stone).has_value());
        expect(eq(static_cast<int>(sky(*world, 8, 68, 8)), 0));
        expect(eq(static_cast<int>(sky(*world, 8, 65, 8)), 0));
    };

    "placing a torch lights the cells around it and breaking it darkens them"_test =
        [] {
            world_fixture fixture{};
            std::vector<std::pair<world::chunk_pos, storage::chunk>> columns{};
            const std::array<block_override, 0> empty{};
            columns.emplace_back(world::chunk_pos{0, 0}, make_column(empty));
            write_columns(fixture, columns);
            auto world{open_world(fixture)};
            expect(world.has_value());
            if (!world) {
                return;
            }
            expect(world->ensure_chunk(world::chunk_pos{0, 0}).has_value());
            expect(eq(static_cast<int>(block_light(*world, 8, 65, 8)), 0));
            const auto torch{world->registry().intern("minecraft:torch")};
            const auto air{world->registry().intern("minecraft:air")};
            expect(
                world->set_block(sim::block_pos{8, 64, 8}, torch).has_value());
            expect(eq(static_cast<int>(block_light(*world, 8, 64, 8)), 14));
            expect(eq(static_cast<int>(block_light(*world, 9, 64, 8)), 13));
            expect(world->set_block(sim::block_pos{8, 64, 8}, air).has_value());
            expect(eq(static_cast<int>(block_light(*world, 8, 64, 8)), 0));
            expect(eq(static_cast<int>(block_light(*world, 9, 64, 8)), 0));
        };

    "lighting a column is independent of the load order of its neighbours"_test =
        [] {
            world_fixture fixture{};
            std::vector<std::pair<world::chunk_pos, storage::chunk>> columns{};
            std::vector<block_override> lit{};
            lit.push_back(torch_at(0, 64, 0));
            lit.push_back(torch_at(4, 64, 5));
            for (auto x{0}; x < 16; ++x) {
                lit.push_back(stone_at(x, 70, 3));
            }
            const std::array<block_override, 0> empty{};
            columns.emplace_back(world::chunk_pos{-1, 0}, make_column(lit));
            columns.emplace_back(world::chunk_pos{0, 0}, make_column(lit));
            columns.emplace_back(world::chunk_pos{1, 0}, make_column(empty));
            columns.emplace_back(world::chunk_pos{0, -1}, make_column(empty));
            columns.emplace_back(world::chunk_pos{0, 1}, make_column(empty));
            write_columns(fixture, columns);

            auto first{open_world(fixture)};
            expect(first.has_value());
            if (!first) {
                return;
            }
            for (const auto &[pos, column] : columns) {
                static_cast<void>(column);
                expect(first->ensure_chunk(pos).has_value());
            }
            auto first_bytes{light_bytes_of(*first, world::chunk_pos{0, 0})};

            auto second{open_world(fixture)};
            expect(second.has_value());
            if (!second) {
                return;
            }
            for (auto index{columns.size()}; index-- > 0;) {
                expect(second->ensure_chunk(columns[index].first).has_value());
            }
            const auto second_bytes{
                light_bytes_of(*second, world::chunk_pos{0, 0})};
            expect(first_bytes == second_bytes)
                << "light depends on the column load order";
        };

    "an unloaded and reloaded column keeps the light its blocks imply"_test =
        [] {
            world_fixture fixture{};
            std::vector<block_override> overrides{};
            for (auto z{4}; z <= 12; ++z) {
                for (auto x{4}; x <= 12; ++x) {
                    overrides.push_back(stone_at(x, 69, z));
                }
            }
            for (auto y{64}; y <= 68; ++y) {
                for (auto lateral{4}; lateral <= 12; ++lateral) {
                    overrides.push_back(stone_at(4, y, lateral));
                    overrides.push_back(stone_at(12, y, lateral));
                    overrides.push_back(stone_at(lateral, y, 4));
                    overrides.push_back(stone_at(lateral, y, 12));
                }
            }
            overrides.push_back(torch_at(8, 65, 8));
            std::vector<std::pair<world::chunk_pos, storage::chunk>> columns{};
            columns.emplace_back(world::chunk_pos{0, 0},
                                 make_column(overrides));
            write_columns(fixture, columns);
            auto world{open_world(fixture)};
            expect(world.has_value());
            if (!world) {
                return;
            }
            expect(world->ensure_chunk(world::chunk_pos{0, 0}).has_value());
            const auto before{light_bytes_of(*world, world::chunk_pos{0, 0})};
            expect(world->flush().has_value());
            expect(world->unload_chunk(world::chunk_pos{0, 0}).has_value());
            expect(world->ensure_chunk(world::chunk_pos{0, 0}).has_value());
            const auto after{light_bytes_of(*world, world::chunk_pos{0, 0})};
            expect(before == after);
        };
};
