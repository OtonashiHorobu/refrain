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

    // Slabs are not solid enough to be opaque. The reference block light
    // report gives a bottom slab a full down face, an empty up face and
    // lower-half side faces; a top slab mirrors it. The side faces matter:
    // two slabs of opposite halves left and right of the same cell edge cover
    // it together, so no light may step across that edge.
    const auto slab_box = [](double low, double high) {
        sim::block_light_box box{};
        box.bounds = {0.0, low, 0.0, 1.0, high, 1.0};
        return box;
    };
    const auto full = slab_box(0.0, 1.0);
    const auto lower_half = slab_box(0.0, 0.5);
    const auto upper_half = slab_box(0.5, 1.0);
    sim::block_light_shape bottom_slab{};
    bottom_slab.faces[static_cast<std::size_t>(sim::light_face::down)]
        .push_back(full);
    sim::block_light_shape top_slab{};
    top_slab.faces[static_cast<std::size_t>(sim::light_face::up)].push_back(
        full);
    constexpr std::array<sim::light_face, 4> sides{{
        sim::light_face::north,
        sim::light_face::south,
        sim::light_face::west,
        sim::light_face::east,
    }};
    for (const auto side : sides) {
        bottom_slab.faces[static_cast<std::size_t>(side)].push_back(lower_half);
        top_slab.faces[static_cast<std::size_t>(side)].push_back(upper_half);
    }
    const auto *bottom_slab_shape = table.append_shape(std::move(bottom_slab));
    const auto *top_slab_shape = table.append_shape(std::move(top_slab));
    table.insert("minecraft:smooth_stone_slab[type=bottom,waterlogged=false]",
                 sim::block_light_property{.emission = 0,
                                           .dampening = 0,
                                           .shape = bottom_slab_shape});
    table.insert("minecraft:smooth_stone_slab[type=top,waterlogged=false]",
                 sim::block_light_property{
                     .emission = 0, .dampening = 0, .shape = top_slab_shape});
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

/// The same flat byte vector as `light_bytes_of`, read from a stored record.
[[nodiscard]] auto light_bytes_of_record(const storage::chunk &column)
    -> std::vector<std::byte> {
    std::vector<std::byte> bytes{};
    for (const auto &layer : column.light_sections) {
        bytes.push_back(static_cast<std::byte>(layer.y));
        bytes.insert(bytes.end(), layer.sky_light.begin(),
                     layer.sky_light.end());
        bytes.insert(bytes.end(), layer.block_light.begin(),
                     layer.block_light.end());
    }
    return bytes;
}

/// Read one committed record through a fresh store handle.
[[nodiscard]] auto read_stored_column(const world_fixture &fixture,
                                      const world::chunk_pos &pos)
    -> std::optional<storage::chunk> {
    auto store{world::world_store::open(fixture.directory.string())};
    expect(store.has_value());
    if (!store) {
        return std::nullopt;
    }
    auto record{store->read_chunk(pos)};
    expect(record.has_value());
    if (!record) {
        return std::nullopt;
    }
    return std::optional<storage::chunk>{std::move(*record)};
}

/// One sparse column with block sections only at `bottom_section` (stone) and
/// `top_section` (air): the sections between them are a vertical gap, which
/// RWF and Anvil both allow. No light layers and no heightmaps are stored;
/// the engine derives them on load.
[[nodiscard]] auto make_sparse_column(std::int32_t bottom_section,
                                      std::int32_t top_section)
    -> storage::chunk {
    constexpr auto section_blocks{world::section_volume};
    storage::chunk chunk{};
    chunk.format_version = world::chunk_format_version;
    chunk.status = storage::chunk_status::full;
    chunk.y_pos = fixture_y_pos;
    for (const auto [section_y, key] :
         std::array<std::pair<std::int32_t, std::string_view>, 2>{
             {std::pair{bottom_section, "minecraft:stone"},
              std::pair{top_section, "minecraft:air"}}}) {
        storage::section_record section{};
        section.y = static_cast<std::int8_t>(section_y);
        section.kind = storage::section_kind::single_value;
        section.bits_per_entry = 0;
        section.block_count = key == "minecraft:air"
                                  ? static_cast<std::uint16_t>(0)
                                  : static_cast<std::uint16_t>(section_blocks);
        section.biome_kind = storage::section_kind::single_value;
        section.biome_bits_per_entry = 0;
        section.biome_palette.emplace_back("minecraft:plains");
        section.palette.push_back(storage::palette_entry{
            .key = std::string{key},
            .count = section.block_count,
            .state = 0,
        });
        chunk.sections.push_back(std::move(section));
    }
    return chunk;
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

    "a top slab beside a bottom slab stays dark on every light path"_test = [] {
        // Column A: a top slab at y=65 under a stone roof at y=66. The slab
        // cell is below A's sky floor, so it only receives light from the side.
        // Column N: stone at y=64 and a bottom slab at y=65 with open air
        // above, so N's slab cell is a level-15 sky source. When both cells
        // share a chunk, the initial pass seeds A's slab cell from N's source
        // cell directly. The two slab side faces cover the shared edge, so no
        // sky light may step across it: A's slab cell and the cell below it
        // stay dark.
        //
        // The surrounding cells are solid stone from y=64 to y=66, so the slab
        // pair is the only path a level can take into A's cell.
        constexpr std::string_view top_slab{
            "minecraft:smooth_stone_slab[type=top,waterlogged=false]"};
        constexpr std::string_view bottom_slab{
            "minecraft:smooth_stone_slab[type=bottom,waterlogged=false]"};
        const auto fill_wall = [](std::vector<block_override> &overrides,
                                  std::int32_t chunk_x, std::int32_t chunk_z) {
            for (auto z{0}; z < 16; ++z) {
                for (auto x{0}; x < 16; ++x) {
                    for (auto y{64}; y <= 66; ++y) {
                        overrides.push_back(
                            stone_at(chunk_x * 16 + x, y, chunk_z * 16 + z));
                    }
                }
            }
        };
        const auto add_slab_pair = [&top_slab, &bottom_slab](
                                       std::vector<block_override> &overrides,
                                       std::int32_t slab_x,
                                       std::int32_t neighbour_x,
                                       std::int32_t z) {
            // A: air below the slab, the slab itself, the stone roof above.
            overrides.push_back(air_at(slab_x, 64, z));
            overrides.push_back(
                block_override{.x = slab_x, .y = 65, .z = z, .key = top_slab});
            overrides.push_back(stone_at(slab_x, 66, z));
            // N: stone below the slab, the slab itself, open air above.
            overrides.push_back(stone_at(neighbour_x, 64, z));
            overrides.push_back(block_override{
                .x = neighbour_x, .y = 65, .z = z, .key = bottom_slab});
            overrides.push_back(air_at(neighbour_x, 66, z));
        };
        const auto cell_sky = [](const sim::world &world, std::int32_t x,
                                 std::int32_t z) {
            return std::pair{static_cast<int>(sky(world, x, 65, z)),
                             static_cast<int>(sky(world, x, 64, z))};
        };

        // Same chunk: the two cells are (8,8) and (9,8) of chunk (0,0).
        world_fixture fixture{};
        std::vector<std::pair<world::chunk_pos, storage::chunk>> columns{};
        std::vector<block_override> overrides{};
        fill_wall(overrides, 0, 0);
        add_slab_pair(overrides, 8, 9, 8);
        columns.emplace_back(world::chunk_pos{0, 0}, make_column(overrides));
        write_columns(fixture, columns);
        auto world{open_world(fixture)};
        expect(world.has_value());
        if (!world) {
            return;
        }
        expect(world->ensure_chunk(world::chunk_pos{0, 0}).has_value());
        // N's slab cell is the column's lowest sky source; A's slab cell is
        // below the roof and dark.
        expect(eq(static_cast<int>(sky(*world, 9, 65, 8)), 15));
        const auto same_chunk{cell_sky(*world, 8, 8)};
        expect(same_chunk == std::pair{0, 0})
            << ("same-chunk seeding crossed the slab faces: " +
                std::to_string(same_chunk.first) + "/" +
                std::to_string(same_chunk.second));
        // The incremental path must agree with the initial pass: opening the
        // edge lets the light in, closing it again puts the cells back to dark.
        const auto air{world->registry().intern("minecraft:air")};
        const auto top_slab_id{world->registry().intern(std::string{top_slab})};
        expect(world->set_block(sim::block_pos{8, 65, 8}, air).has_value());
        expect(cell_sky(*world, 8, 8) == std::pair{14, 13});
        expect(world->set_block(sim::block_pos{8, 65, 8}, top_slab_id)
                   .has_value());
        expect(cell_sky(*world, 8, 8) == std::pair{0, 0})
            << "incremental relight disagrees with the initial pass";

        // Cross border: the same pair split between chunk (0,0) (slab cell at
        // local x=15) and chunk (1,0) (source cell at local x=0). The initial
        // pass of one chunk cannot see the other column's floor, so the step
        // goes through the shape-checked border path instead.
        world_fixture cross_fixture{};
        std::vector<std::pair<world::chunk_pos, storage::chunk>>
            cross_columns{};
        std::vector<block_override> left_overrides{};
        fill_wall(left_overrides, 0, 0);
        add_slab_pair(left_overrides, 15, 16, 8);
        std::vector<block_override> right_overrides{};
        fill_wall(right_overrides, 1, 0);
        cross_columns.emplace_back(world::chunk_pos{0, 0},
                                   make_column(left_overrides));
        cross_columns.emplace_back(world::chunk_pos{1, 0},
                                   make_column(right_overrides));
        write_columns(cross_fixture, cross_columns);
        for (const bool source_first : {false, true}) {
            auto cross_world{open_world(cross_fixture)};
            expect(cross_world.has_value());
            if (!cross_world) {
                return;
            }
            const auto slab_chunk{world::chunk_pos{0, 0}};
            const auto source_chunk{world::chunk_pos{1, 0}};
            const auto first{source_first ? source_chunk : slab_chunk};
            const auto second{source_first ? slab_chunk : source_chunk};
            expect(cross_world->ensure_chunk(first).has_value());
            expect(cross_world->ensure_chunk(second).has_value());
            const auto cross{cell_sky(*cross_world, 15, 8)};
            expect(cross == std::pair{0, 0})
                << ("cross-border light crossed the slab faces: " +
                    std::to_string(cross.first) + "/" +
                    std::to_string(cross.second));
        }
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
        expect(eq(static_cast<int>(world->block_light().size()), 11));
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

    "a section inserted into a vertical gap relights in place"_test = [] {
        world_fixture fixture{};
        std::vector<std::pair<world::chunk_pos, storage::chunk>> columns{};
        columns.emplace_back(world::chunk_pos{0, 0},
                             make_sparse_column(-4, 10));
        write_columns(fixture, columns);
        auto world{open_world(fixture)};
        expect(world.has_value());
        if (!world) {
            return;
        }
        expect(world->ensure_chunk(world::chunk_pos{0, 0}).has_value());
        // The load batch is not what this test observes.
        static_cast<void>(world->take_light_changes());
        auto *column{world->find_chunk(world::chunk_pos{0, 0})};
        expect(column != nullptr);
        if (column == nullptr) {
            return;
        }
        const auto has_layer = [](const storage::chunk &chunk, std::int32_t y) {
            return std::any_of(
                chunk.light_sections.begin(), chunk.light_sections.end(),
                [y](const storage::light_section &layer) {
                    return static_cast<std::int32_t>(layer.y) == y;
                });
        };
        // A gap section carries no stored light layer: it is air between two
        // block sections.
        expect(!has_layer(column->storage, 5));

        // A block edit that inserts a section into the gap must rebuild the
        // layer set; otherwise the new section has no layer and every light
        // write for it is dropped.
        const auto glass{world->registry().intern("minecraft:glass")};
        const auto applied{world->set_block(sim::block_pos{8, 80, 8}, glass)};
        expect(applied.has_value()) << (applied ? "" : applied.error().message);
        column = world->find_chunk(world::chunk_pos{0, 0});
        expect(column != nullptr);
        if (column == nullptr) {
            return;
        }
        expect(has_layer(column->storage, 5));
        // The section above the gap is open air, so the inserted layer is a
        // full sky source; a missing layer would read back as darkness. The
        // neighbouring gap sections keep no layer by contract, so only cells
        // of section 5 are asserted.
        expect(eq(static_cast<int>(sky(*world, 8, 80, 8)), 15));
        expect(eq(static_cast<int>(sky(*world, 8, 81, 8)), 15));
        // Clients holding the column learn about the new section.
        const auto changes{world->take_light_changes()};
        const auto reported{
            std::any_of(changes.begin(), changes.end(),
                        [](const sim::light_section_change &change) {
                            return change.pos == world::chunk_pos{0, 0} &&
                                   change.section_y == 5;
                        })};
        expect(reported);

        // Save and reload: the layer is written and a reload recomputes the
        // same light instead of leaving the section dark.
        expect(world->flush().has_value());
        expect(world->unload_chunk(world::chunk_pos{0, 0}).has_value());
        expect(world->ensure_chunk(world::chunk_pos{0, 0}).has_value());
        column = world->find_chunk(world::chunk_pos{0, 0});
        expect(column != nullptr);
        if (column == nullptr) {
            return;
        }
        expect(has_layer(column->storage, 5));
        expect(eq(static_cast<int>(sky(*world, 8, 80, 8)), 15));
        expect(eq(static_cast<int>(sky(*world, 8, 81, 8)), 15));
    };

    "a column saved with correct light reloads without an initial pass"_test =
        [] {
            world_fixture fixture{};
            auto created{
                world::world_store::create(fixture.directory.string())};
            expect(created.has_value())
                << (created ? "" : created.error().message);
            if (!created) {
                return;
            }
            fixture.store.emplace(std::move(*created));
            auto world{open_world(fixture)};
            expect(world.has_value());
            if (!world) {
                return;
            }
            const auto left{world::chunk_pos{0, 0}};
            const auto right{world::chunk_pos{1, 0}};
            const std::array<block_override, 1> left_overrides{{
                torch_at(15, 64, 8),
            }};
            const std::array<block_override, 0> right_overrides{};
            const auto left_column{make_column(left_overrides)};
            const auto right_column{make_column(right_overrides)};
            auto inserted_left{world->insert_chunk(left, left_column)};
            expect(inserted_left.has_value())
                << (inserted_left ? "" : inserted_left.error().message);
            auto inserted_right{world->insert_chunk(right, right_column)};
            expect(inserted_right.has_value())
                << (inserted_right ? "" : inserted_right.error().message);
            if (!inserted_left || !inserted_right) {
                return;
            }
            // The lighting pass completed before the record claimed
            // correctness, and both columns are saved with that claim.
            expect((*inserted_left)->storage.light_correct);
            expect((*inserted_right)->storage.light_correct);
            expect(eq(static_cast<int>(block_light(*world, 16, 64, 8)), 13));
            expect(world->flush().has_value());
            const auto left_lit{light_bytes_of(*world, left)};
            const auto right_lit{light_bytes_of(*world, right)};
            expect(!left_lit.empty());
            expect(!right_lit.empty());
            expect(world->unload_chunk(left).has_value());
            expect(world->unload_chunk(right).has_value());

            auto &engine{world->light()};
            engine.reset_stats();
            // Reload in the opposite order: a trusted column must not depend on
            // the order its neighbours joined the loaded set.
            expect(world->ensure_chunk(right).has_value());
            expect(world->ensure_chunk(left).has_value());
            const auto stats{engine.stats()};
            expect(eq(stats.columns_trusted, std::uint64_t{2}));
            expect(eq(stats.columns_lit, std::uint64_t{0}));
            expect(eq(stats.initial_drain_calls, std::uint64_t{0}));
            expect(eq(stats.initial_nodes, std::uint64_t{0}));
            expect(eq(stats.drain_calls, std::uint64_t{0}));
            expect(light_bytes_of(*world, left) == left_lit);
            expect(light_bytes_of(*world, right) == right_lit);
            expect(eq(static_cast<int>(block_light(*world, 16, 64, 8)), 13));
        };

    "a stored column without the correct-light mark is relit on load"_test =
        [] {
            world_fixture fixture{};
            std::vector<std::pair<world::chunk_pos, storage::chunk>> columns{};
            const std::array<block_override, 1> overrides{{
                torch_at(8, 64, 8),
            }};
            columns.emplace_back(world::chunk_pos{0, 0},
                                 make_column(overrides));
            write_columns(fixture, columns);
            auto world{open_world(fixture)};
            expect(world.has_value());
            if (!world) {
                return;
            }
            auto &engine{world->light()};
            engine.reset_stats();
            auto loaded{world->ensure_chunk(world::chunk_pos{0, 0})};
            expect(loaded.has_value());
            if (!loaded) {
                return;
            }
            const auto stats{engine.stats()};
            expect(eq(stats.columns_lit, std::uint64_t{1}));
            expect(eq(stats.columns_trusted, std::uint64_t{0}));
            expect(stats.initial_drain_calls > 0U);
            expect(stats.initial_nodes > 0U);
            expect(eq(static_cast<int>(sky(*world, 8, 70, 8)), 15));
            expect(eq(static_cast<int>(block_light(*world, 8, 64, 8)), 14));
            // The completed pass marked the runtime record correct; whether the
            // record is saved is the save path's concern.
            expect((*loaded)->storage.light_correct);
        };

    "a record that claims correct light with a partial layer set is relit"_test =
        [] {
            world_fixture fixture{};
            std::vector<std::pair<world::chunk_pos, storage::chunk>> columns{};
            const std::array<block_override, 1> overrides{{
                torch_at(8, 64, 8),
            }};
            auto column{make_column(overrides)};
            // The claim is not enough: the record carries one zeroed layer out
            // of the complete set, so the loader can prove nothing and
            // relights.
            column.light_correct = true;
            storage::light_section claimed{};
            claimed.y = 0;
            claimed.block_light.assign(world::light_nibble_bytes, std::byte{0});
            claimed.sky_light.assign(world::light_nibble_bytes, std::byte{0});
            column.light_sections.push_back(std::move(claimed));
            columns.emplace_back(world::chunk_pos{0, 0}, std::move(column));
            write_columns(fixture, columns);
            auto world{open_world(fixture)};
            expect(world.has_value());
            if (!world) {
                return;
            }
            auto &engine{world->light()};
            engine.reset_stats();
            expect(world->ensure_chunk(world::chunk_pos{0, 0}).has_value());
            const auto stats{engine.stats()};
            expect(eq(stats.columns_lit, std::uint64_t{1}));
            expect(eq(stats.columns_trusted, std::uint64_t{0}));
            expect(stats.initial_nodes > 0U);
            expect(eq(static_cast<int>(sky(*world, 8, 70, 8)), 15));
            expect(eq(static_cast<int>(block_light(*world, 8, 64, 8)), 14));
        };

    "an edit keeps the light mark dirty until the relight drains"_test = [] {
        world_fixture fixture{};
        auto created{world::world_store::create(fixture.directory.string())};
        expect(created.has_value());
        if (!created) {
            return;
        }
        fixture.store.emplace(std::move(*created));
        auto world{open_world(fixture)};
        expect(world.has_value());
        if (!world) {
            return;
        }
        const auto pos{world::chunk_pos{0, 0}};
        const std::array<block_override, 0> overrides{};
        auto inserted{world->insert_chunk(pos, make_column(overrides))};
        expect(inserted.has_value());
        if (!inserted) {
            return;
        }
        auto *column{world->find_chunk(pos)};
        expect(column != nullptr);
        if (column == nullptr) {
            return;
        }
        auto &engine{world->light()};
        // An edit makes the stored arrays stale; the record must not claim
        // correctness until the queued relight has actually been applied.
        engine.block_changed(sim::block_pos{8, 65, 8});
        expect(!column->storage.light_correct);
        expect(engine.pending());
        expect(eq(engine.run(0), std::size_t{0}));
        expect(!column->storage.light_correct);
        expect(engine.run() > std::size_t{0});
        expect(column->storage.light_correct);
        expect(!engine.pending());

        // The world edit path finishes the incremental relight before it
        // returns, so the record is correct again immediately.
        const auto torch{world->registry().intern("minecraft:torch")};
        expect(world->set_block(sim::block_pos{8, 64, 8}, torch).has_value());
        expect(column->storage.light_correct);
        expect(eq(static_cast<int>(block_light(*world, 8, 64, 8)), 14));
        expect(eq(static_cast<int>(block_light(*world, 9, 64, 8)), 13));
    };

    "the correct-light mark survives save and reload"_test = [] {
        world_fixture fixture{};
        const auto pos{world::chunk_pos{0, 0}};
        long long saved_layer_count{0};
        {
            auto created{
                world::world_store::create(fixture.directory.string())};
            expect(created.has_value());
            if (!created) {
                return;
            }
            fixture.store.emplace(std::move(*created));
            auto world{open_world(fixture)};
            expect(world.has_value());
            if (!world) {
                return;
            }
            const std::array<block_override, 0> overrides{};
            auto inserted{world->insert_chunk(pos, make_column(overrides))};
            expect(inserted.has_value());
            if (!inserted) {
                return;
            }
            saved_layer_count = static_cast<long long>(
                (*inserted)->storage.light_sections.size());
            expect(saved_layer_count > 0);
            expect(world->flush().has_value());
        }
        fixture.store.reset();
        {
            auto store{world::world_store::open(fixture.directory.string())};
            expect(store.has_value()) << (store ? "" : store.error().message);
            if (!store) {
                return;
            }
            auto record{store->read_chunk(pos)};
            expect(record.has_value());
            if (!record) {
                return;
            }
            expect(record->light_correct);
            expect(record->light_table != 0U);
            expect(eq(static_cast<long long>(record->light_sections.size()),
                      saved_layer_count));
        }
        {
            auto world{open_world(fixture)};
            expect(world.has_value());
            if (!world) {
                return;
            }
            world->light().reset_stats();
            expect(world->ensure_chunk(pos).has_value());
            expect(
                eq(world->light().stats().columns_trusted, std::uint64_t{1}));
            expect(eq(world->light().stats().columns_lit, std::uint64_t{0}));
        }
    };

    "a partial light drain is never persisted as complete"_test = [] {
        world_fixture fixture{};
        auto created{world::world_store::create(fixture.directory.string())};
        expect(created.has_value());
        if (!created) {
            return;
        }
        fixture.store.emplace(std::move(*created));
        auto world{open_world(fixture)};
        expect(world.has_value());
        if (!world) {
            return;
        }
        const auto edited{world::chunk_pos{0, 0}};
        const auto source{world::chunk_pos{1, 0}};
        const std::array<block_override, 0> edited_overrides{};
        // The torch sits on the border of the second column, so a propagation
        // reaches the first column's edge cells.
        const std::array<block_override, 1> source_overrides{{
            torch_at(16, 64, 8),
        }};
        expect(world->insert_chunk(edited, make_column(edited_overrides))
                   .has_value());
        expect(world->insert_chunk(source, make_column(source_overrides))
                   .has_value());
        // The initial passes complete before these records are saved, so this
        // is the complete light both columns settle on.
        expect(world->flush().has_value());
        auto complete_record{read_stored_column(fixture, edited)};
        expect(complete_record.has_value());
        if (!complete_record) {
            return;
        }
        expect(complete_record->light_correct);
        const auto complete{light_bytes_of_record(*complete_record)};

        // A block edit marks the first column dirty while its light is still
        // complete.
        const auto glass{world->registry().intern("minecraft:glass")};
        expect(world->set_block(sim::block_pos{3, 64, 3}, glass).has_value());
        auto *column{world->find_chunk(edited)};
        expect(column != nullptr);
        if (column == nullptr) {
            return;
        }
        expect(column->storage.light_correct);

        // Re-evaluating the emissive cell starts a fresh update of the same
        // size a large edit seeds. Running a budget below the queue size
        // reproduces a drain cut short between ticks: the decrease pass has
        // already written the first column's edge while the increase pass that
        // restores it is still queued.
        world->light().reset_stats();
        world->light().block_changed(sim::block_pos{16, 64, 8});
        expect(world->light().pending());
        expect(eq(world->light().run(3), std::size_t{3}));
        expect(world->light().pending());
        expect(!column->storage.light_correct);
        expect(light_bytes_of(*world, edited) != complete);

        // A save taken now must not hand out the partial arrays as complete.
        expect(world->flush().has_value());
        auto partial_record{read_stored_column(fixture, edited)};
        expect(partial_record.has_value());
        if (!partial_record) {
            return;
        }
        expect(!partial_record->light_correct);
        expect(light_bytes_of_record(*partial_record) != complete);

        // The drain finishes on the next tick; the corrected arrays are marked
        // for saving and the record claims correctness again.
        for (auto attempts{0}; attempts < 64 && world->light().pending();
             ++attempts) {
            expect(world->light().run() > std::size_t{0});
        }
        expect(!world->light().pending());
        expect(column->storage.light_correct);
        expect(light_bytes_of(*world, edited) == complete);
        expect(world->flush().has_value());
        auto corrected{read_stored_column(fixture, edited)};
        expect(corrected.has_value());
        if (!corrected) {
            return;
        }
        expect(corrected->light_correct);
        expect(corrected->light_table != 0U);
        expect(light_bytes_of_record(*corrected) == complete);

        // A reload trusts the corrected record instead of relighting.
        expect(world->unload_chunk(edited).has_value());
        world->light().reset_stats();
        expect(world->ensure_chunk(edited).has_value());
        expect(eq(world->light().stats().columns_trusted, std::uint64_t{1}));
        expect(light_bytes_of(*world, edited) == complete);
    };

    "a record lit under another light table is relit on load"_test = [] {
        world_fixture source_fixture{};
        const auto pos{world::chunk_pos{0, 0}};
        std::optional<storage::chunk> lit;
        {
            auto created{
                world::world_store::create(source_fixture.directory.string())};
            expect(created.has_value());
            if (!created) {
                return;
            }
            source_fixture.store.emplace(std::move(*created));
            auto world{open_world(source_fixture)};
            expect(world.has_value());
            if (!world) {
                return;
            }
            const std::array<block_override, 1> overrides{{
                torch_at(8, 64, 8),
            }};
            expect(
                world->insert_chunk(pos, make_column(overrides)).has_value());
            expect(world->flush().has_value());
            lit = read_stored_column(source_fixture, pos);
            expect(lit.has_value());
            if (!lit) {
                return;
            }
            expect(lit->light_correct);
            expect(lit->light_table != 0U);
        }

        // The same arrays, saved under a different light content: the
        // fingerprint no longer describes the runtime table.
        world_fixture tampered_fixture{};
        auto created{
            world::world_store::create(tampered_fixture.directory.string())};
        expect(created.has_value());
        if (!created) {
            return;
        }
        auto tampered{*lit};
        tampered.light_table ^= 0x9E3779B97F4A7C15ULL;
        expect(created->write_chunk(pos, tampered).has_value());
        expect(created->commit().has_value());
        tampered_fixture.store.emplace(std::move(*created));
        auto world{open_world(tampered_fixture)};
        expect(world.has_value());
        if (!world) {
            return;
        }
        world->light().reset_stats();
        expect(world->ensure_chunk(pos).has_value());
        expect(eq(world->light().stats().columns_lit, std::uint64_t{1}));
        expect(eq(world->light().stats().columns_trusted, std::uint64_t{0}));
        expect(eq(static_cast<int>(block_light(*world, 8, 64, 8)), 14));
    };

    "installing different light content relights a saved record"_test = [] {
        world_fixture fixture{};
        const auto pos{world::chunk_pos{0, 0}};
        {
            auto created{
                world::world_store::create(fixture.directory.string())};
            expect(created.has_value());
            if (!created) {
                return;
            }
            fixture.store.emplace(std::move(*created));
            auto world{open_world(fixture)};
            expect(world.has_value());
            if (!world) {
                return;
            }
            const std::array<block_override, 0> overrides{};
            expect(
                world->insert_chunk(pos, make_column(overrides)).has_value());
            expect(world->flush().has_value());
        }
        // Reopen the world, then install a table with different content before
        // the first column loads: production installs the table once at start,
        // an embedder or mod may change it between runs.
        auto world{sim::world::open(fixture.directory.string())};
        expect(world.has_value());
        if (!world) {
            return;
        }
        auto different_table{make_light_table()};
        different_table.insert("minecraft:probe_state",
                               sim::block_light_property{.emission = 7});
        world->set_block_light_table(std::move(different_table));
        world->light().reset_stats();
        expect(world->ensure_chunk(pos).has_value());
        expect(eq(world->light().stats().columns_lit, std::uint64_t{1}));
        expect(eq(world->light().stats().columns_trusted, std::uint64_t{0}));
        auto *column{world->find_chunk(pos)};
        expect(column != nullptr);
        if (column == nullptr) {
            return;
        }
        expect(column->storage.light_correct);
        expect(column->storage.light_table != 0U);
    };

    "moving a world relights a column whose stored light does not verify"_test =
        [] {
            world_fixture fixture{};
            auto created{
                world::world_store::create(fixture.directory.string())};
            expect(created.has_value());
            if (!created) {
                return;
            }
            fixture.store.emplace(std::move(*created));
            const auto pos{world::chunk_pos{0, 0}};
            std::vector<std::byte> complete{};
            auto world{open_world(fixture)};
            expect(world.has_value());
            if (!world) {
                return;
            }
            const std::array<block_override, 0> overrides{};
            expect(
                world->insert_chunk(pos, make_column(overrides)).has_value());
            expect(world->flush().has_value());
            expect(world->unload_chunk(pos).has_value());
            expect(world->ensure_chunk(pos).has_value());
            complete = light_bytes_of(*world, pos);
            auto *column{world->find_chunk(pos)};
            expect(column != nullptr);
            if (column == nullptr) {
                return;
            }
            expect(column->storage.light_correct);
            // Break the claim without clearing the flag: the moved engine must
            // run the same completeness proof as the load path.
            column->storage.light_sections.pop_back();
            sim::world moved{std::move(*world)};
            expect(eq(moved.light().stats().columns_lit, std::uint64_t{1}));
            expect(light_bytes_of(moved, pos) == complete);
            const auto *reloaded{moved.find_chunk(pos)};
            expect(reloaded != nullptr);
            if (reloaded == nullptr) {
                return;
            }
            expect(reloaded->storage.light_correct);
        };
};
