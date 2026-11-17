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

/// Light table of the synthetic fixture. The values are the behaviour the
/// tests describe, not a copy of the game tables: they are installed before a
/// column loads, exactly like the runtime installs the content table.
[[nodiscard]] auto make_light_table() -> sim::block_light_table {
    sim::block_light_table table{};
    const auto add = [&table](std::string_view key, std::uint8_t emission,
                              std::uint8_t dampening) {
        table.insert(std::string{key},
                     sim::block_light_property{.emission = emission,
                                               .dampening = dampening});
    };
    add("minecraft:air", 0, 0);
    add("minecraft:cave_air", 0, 0);
    add("minecraft:void_air", 0, 0);
    add("minecraft:stone", 0, 15);
    add("minecraft:torch", 14, 0);
    add("minecraft:glass", 0, 0);
    add("minecraft:oak_leaves", 0, 1);
    add("minecraft:water", 0, 1);

    // A bottom slab: not solid enough to be opaque, with a full bottom face
    // shape. The reference shape has an empty top face (the open upper half),
    // so the slab cell itself still sees the sky while the edge below it is
    // closed.
    sim::block_light_shape slab{};
    sim::block_light_box full{};
    full.bounds = {0.0, 0.0, 0.0, 1.0, 1.0, 1.0};
    slab.faces[static_cast<std::size_t>(sim::light_face::down)].push_back(full);
    const auto *slab_shape = table.append_shape(std::move(slab));
    table.insert("minecraft:smooth_stone_slab[type=bottom,waterlogged=false]",
                 sim::block_light_property{
                     .emission = 0, .dampening = 0, .shape = slab_shape});
    add("minecraft:smooth_stone_slab[type=double,waterlogged=false]", 0, 15);
    return table;
}

[[nodiscard]] auto open_world(world_fixture &fixture)
    -> std::optional<sim::world> {
    auto opened{sim::world::open(fixture.directory.string())};
    expect(opened.has_value()) << (opened ? "" : opened.error().message);
    if (!opened) {
        return std::nullopt;
    }
    // A world without a light table runs on the fallback: every state opaque
    // and emitting nothing. Install one before the first column loads.
    opened->set_block_light_table(make_light_table());
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

    "glass lets the sky source reach the cells around and inside it"_test = [] {
        world_fixture fixture{};
        std::vector<block_override> overrides{};
        overrides.push_back(
            block_override{.x = 8, .y = 64, .z = 8, .key = "minecraft:glass"});
        std::vector<std::pair<world::chunk_pos, storage::chunk>> columns{};
        columns.emplace_back(world::chunk_pos{0, 0}, make_column(overrides));
        write_columns(fixture, columns);
        auto world{open_world(fixture)};
        expect(world.has_value());
        if (!world) {
            return;
        }
        expect(world->ensure_chunk(world::chunk_pos{0, 0}).has_value());
        expect(eq(static_cast<int>(sky(*world, 8, 65, 8)), 15));
        expect(eq(static_cast<int>(sky(*world, 8, 64, 8)), 15));
        expect(eq(static_cast<int>(sky(*world, 8, 63, 8)), 0));
    };

    "leaves lose one sky level per block in a shaft closed to the sides"_test =
        [] {
            world_fixture fixture{};
            std::vector<block_override> overrides{};
            constexpr std::string_view leaves{"minecraft:oak_leaves"};
            for (auto y{64}; y <= 66; ++y) {
                overrides.push_back(
                    block_override{.x = 8, .y = y, .z = 8, .key = leaves});
            }
            // Stone walls close the shaft so only the vertical path carries
            // sky light; the surrounding columns are opaque down to the floor.
            for (auto y{64}; y <= 66; ++y) {
                overrides.push_back(stone_at(7, y, 8));
                overrides.push_back(stone_at(9, y, 8));
                overrides.push_back(stone_at(8, y, 7));
                overrides.push_back(stone_at(8, y, 9));
            }
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
            expect(eq(static_cast<int>(sky(*world, 8, 67, 8)), 15));
            expect(eq(static_cast<int>(sky(*world, 8, 66, 8)), 14));
            expect(eq(static_cast<int>(sky(*world, 8, 65, 8)), 13));
            expect(eq(static_cast<int>(sky(*world, 8, 64, 8)), 12));
        };

    "a bottom slab closes the light step below its solid face"_test = [] {
        world_fixture fixture{};
        std::vector<block_override> overrides{};
        constexpr std::string_view slab{
            "minecraft:smooth_stone_slab[type=bottom,waterlogged=false]"};
        overrides.push_back(
            block_override{.x = 8, .y = 66, .z = 8, .key = slab});
        // A one-cell stone shaft from the torch up past the slab keeps both
        // kinds of light on the vertical path.
        for (auto y{64}; y <= 68; ++y) {
            for (const auto &[x, z] :
                 std::array<std::pair<std::int32_t, std::int32_t>, 4>{
                     std::pair{7, 8}, std::pair{9, 8}, std::pair{8, 7},
                     std::pair{8, 9}}) {
                overrides.push_back(stone_at(x, y, z));
            }
        }
        overrides.push_back(torch_at(8, 64, 8));
        std::vector<std::pair<world::chunk_pos, storage::chunk>> columns{};
        columns.emplace_back(world::chunk_pos{0, 0}, make_column(overrides));
        write_columns(fixture, columns);
        auto world{open_world(fixture)};
        expect(world.has_value());
        if (!world) {
            return;
        }
        expect(world->ensure_chunk(world::chunk_pos{0, 0}).has_value());
        // The slab's empty top face keeps its cell a sky source, and its full
        // bottom face shape closes the edge below: the cell under the slab is
        // dark even though the cell above the slab is full.
        expect(eq(static_cast<int>(sky(*world, 8, 67, 8)), 15));
        expect(eq(static_cast<int>(sky(*world, 8, 66, 8)), 15));
        expect(eq(static_cast<int>(sky(*world, 8, 65, 8)), 0));
        // The torch below the slab lights the cells up to the slab and stops:
        // the face shape blocks the step into the slab cell.
        expect(eq(static_cast<int>(block_light(*world, 8, 64, 8)), 14));
        expect(eq(static_cast<int>(block_light(*world, 8, 65, 8)), 13));
        expect(eq(static_cast<int>(block_light(*world, 8, 66, 8)), 0));
        expect(eq(static_cast<int>(block_light(*world, 8, 67, 8)), 0));
        // Removing the slab opens the path: both the sky and the torch light
        // reach the cells above.
        const auto air{world->registry().find("minecraft:air")};
        expect(air.has_value());
        if (!air) {
            return;
        }
        expect(world->set_block(sim::block_pos{8, 66, 8}, *air).has_value());
        expect(eq(static_cast<int>(sky(*world, 8, 66, 8)), 15));
        expect(eq(static_cast<int>(block_light(*world, 8, 66, 8)), 12));
        expect(eq(static_cast<int>(block_light(*world, 8, 67, 8)), 11));
    };

    "a state the table does not carry is opaque and emits nothing"_test = [] {
        world_fixture fixture{};
        std::vector<block_override> overrides{};
        overrides.push_back(block_override{
            .x = 8, .y = 64, .z = 8, .key = "minecraft:mystery_block"});
        std::vector<std::pair<world::chunk_pos, storage::chunk>> columns{};
        columns.emplace_back(world::chunk_pos{0, 0}, make_column(overrides));
        write_columns(fixture, columns);
        auto world{open_world(fixture)};
        expect(world.has_value());
        if (!world) {
            return;
        }
        expect(world->ensure_chunk(world::chunk_pos{0, 0}).has_value());
        expect(eq(static_cast<int>(world->block_light().size()), 10));
        // The fallback is opaque: the cell above the unknown block is still a
        // sky source, but no sky light passes into the block, and the state
        // emits nothing.
        expect(eq(static_cast<int>(sky(*world, 8, 65, 8)), 15));
        expect(eq(static_cast<int>(sky(*world, 8, 64, 8)), 0));
        expect(eq(static_cast<int>(block_light(*world, 8, 64, 8)), 0));
    };

    "a world without a content table falls back to opaque states"_test = [] {
        world_fixture fixture{};
        std::vector<std::pair<world::chunk_pos, storage::chunk>> columns{};
        const std::array<block_override, 0> empty{};
        columns.emplace_back(world::chunk_pos{0, 0}, make_column(empty));
        write_columns(fixture, columns);
        auto opened{sim::world::open(fixture.directory.string())};
        expect(opened.has_value());
        if (!opened) {
            return;
        }
        expect(opened->block_light().empty());
        expect(opened->ensure_chunk(world::chunk_pos{0, 0}).has_value());
        // No state of the fallback emits or lets light through, so the whole
        // column is dark: this is the documented degraded mode, not a guess.
        expect(eq(static_cast<int>(sky(*opened, 8, 319, 8)), 0));
        expect(eq(static_cast<int>(sky(*opened, 8, 64, 8)), 0));
    };

    "the light counters advance for a column load and an edit and reset "
    "cleanly"_test = [] {
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
        auto &engine{world->light()};
        engine.reset_stats();
        const auto empty_stats{engine.stats()};
        expect(eq(empty_stats.columns_lit, std::uint64_t{0}));
        expect(eq(empty_stats.initial_drain_calls, std::uint64_t{0}));
        expect(eq(empty_stats.initial_nodes, std::uint64_t{0}));
        expect(eq(empty_stats.initial_ns, std::uint64_t{0}));
        expect(eq(empty_stats.drain_calls, std::uint64_t{0}));
        expect(eq(empty_stats.drain_nodes, std::uint64_t{0}));
        expect(eq(empty_stats.drain_ns, std::uint64_t{0}));
        expect(eq(empty_stats.block_edits, std::uint64_t{0}));
        expect(eq(empty_stats.pending_peak, std::uint64_t{0}));
        expect(eq(empty_stats.pending, std::uint64_t{0}));

        expect(world->ensure_chunk(world::chunk_pos{0, 0}).has_value());
        const auto loaded{engine.stats()};
        expect(eq(loaded.columns_lit, std::uint64_t{1}));
        expect(loaded.initial_drain_calls > 0U);
        expect(loaded.initial_nodes > 0U);
        expect(loaded.drain_nodes >= loaded.initial_nodes);
        expect(loaded.pending_peak > 0U);
        expect(eq(loaded.block_edits, std::uint64_t{0}));

        // A torch above the flat surface is a block light source: the edit
        // queues an increase that the synchronous edit drain processes.
        const auto torch{world->registry().intern("minecraft:torch")};
        const auto nodes_before{loaded.drain_nodes};
        const auto calls_before{loaded.drain_calls};
        const auto edit_status{
            world->set_block(sim::block_pos{8, 65, 8}, torch)};
        expect(edit_status.has_value())
            << (edit_status ? "" : edit_status.error().message);
        expect(eq(static_cast<int>(block_light(*world, 8, 65, 8)), 14));
        const auto edited{engine.stats()};
        expect(edited.block_edits > loaded.block_edits);
        expect(edited.drain_nodes > nodes_before);
        expect(edited.drain_calls > calls_before);
        expect(edited.pending_peak > 0U);
        expect(eq(edited.pending, std::uint64_t{0}));

        // The counters are observation only: reading and resetting them does
        // not change the light the engine computed.
        engine.reset_stats();
        expect(eq(static_cast<int>(block_light(*world, 8, 65, 8)), 14));
        const auto cleared{engine.stats()};
        expect(eq(cleared.columns_lit, std::uint64_t{0}));
        expect(eq(cleared.initial_drain_calls, std::uint64_t{0}));
        expect(eq(cleared.initial_nodes, std::uint64_t{0}));
        expect(eq(cleared.initial_ns, std::uint64_t{0}));
        expect(eq(cleared.drain_calls, std::uint64_t{0}));
        expect(eq(cleared.drain_nodes, std::uint64_t{0}));
        expect(eq(cleared.drain_ns, std::uint64_t{0}));
        expect(eq(cleared.block_edits, std::uint64_t{0}));
        expect(eq(cleared.pending_peak, std::uint64_t{0}));
        expect(eq(cleared.pending, std::uint64_t{0}));
    };
};
