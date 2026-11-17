// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/chunk_packet.hpp>

#include <refrain/core/io.hpp>
#include <refrain/data/vanilla.hpp>
#include <refrain/net/base.hpp>
#include <refrain/sim/world.hpp>
#include <refrain/world/store.hpp>

#include <boost/ut.hpp>

#include <fmt/format.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace data = refrain::data;
namespace net = refrain::net;
namespace server = refrain::server;
namespace sim = refrain::sim;
namespace storage = refrain::storage;
namespace world = refrain::world;

constexpr std::size_t section_entries{4096};
constexpr std::size_t biome_entries{64};
constexpr std::size_t light_bytes{2048};

[[nodiscard]] auto scratch_directory() -> std::filesystem::path {
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    auto path{std::filesystem::temp_directory_path() /
              fmt::format("refrain-chunk-packet-{}", stamp)};
    std::error_code error{};
    std::filesystem::create_directories(path, error);
    return path;
}

[[nodiscard]] auto packed_words(std::size_t entries, std::uint8_t bits)
    -> std::size_t {
    if (bits == 0) {
        return 0;
    }
    const auto per_word{64U / bits};
    return (entries + per_word - 1) / per_word;
}

[[nodiscard]] auto packed_value(const std::vector<std::uint64_t> &words,
                                std::size_t index, std::uint8_t bits)
    -> std::uint32_t {
    const auto per_word{64U / bits};
    const auto word{words[index / per_word]};
    const auto shift{(index % per_word) * bits};
    return static_cast<std::uint32_t>((word >> shift) &
                                      ((std::uint64_t{1} << bits) - 1U));
}

[[nodiscard]] auto heightmap_words(std::uint32_t value)
    -> std::vector<std::uint64_t> {
    constexpr std::uint8_t bits{9};
    constexpr std::size_t per_word{7};
    std::vector<std::uint64_t> words(packed_words(256, bits), 0);
    for (std::size_t index{0}; index < 256; ++index) {
        words[index / per_word] |= static_cast<std::uint64_t>(value)
                                   << ((index % per_word) * bits);
    }
    return words;
}

void add_section(storage::chunk &chunk, std::int8_t y) {
    storage::section_record section{};
    section.y = y;
    section.kind = storage::section_kind::single_value;
    section.bits_per_entry = 0;
    section.block_count = static_cast<std::uint16_t>(section_entries);
    section.palette.push_back(storage::palette_entry{
        .key = "minecraft:stone",
        .count = static_cast<std::uint16_t>(section_entries),
        .state = 0,
    });
    section.biome_kind = storage::section_kind::single_value;
    section.biome_bits_per_entry = 0;
    section.biome_palette.push_back("minecraft:plains");
    chunk.sections.push_back(std::move(section));
}

[[nodiscard]] auto make_chunk(bool full_column) -> storage::chunk {
    storage::chunk chunk{};
    chunk.format_version = world::chunk_format_version;
    chunk.status = storage::chunk_status::full;
    chunk.y_pos = -4;
    if (full_column) {
        for (std::int8_t y{-4}; y <= 19; ++y) {
            add_section(chunk, y);
        }
        for (const auto &[name, value] :
             std::array<std::pair<std::string_view, std::uint32_t>, 3>{
                 {std::pair{"WORLD_SURFACE", 7U},
                  std::pair{"MOTION_BLOCKING", 8U},
                  std::pair{"MOTION_BLOCKING_NO_LEAVES", 9U}}}) {
            storage::heightmap_record record{};
            record.name = name;
            record.bits_per_entry = 9;
            record.data = heightmap_words(value);
            chunk.heightmaps.push_back(std::move(record));
        }
        for (const auto y : {-4, 19}) {
            storage::light_section light{};
            light.y = static_cast<std::int8_t>(y);
            light.sky_light.assign(light_bytes, std::byte{0x11});
            light.block_light.assign(light_bytes, std::byte{0x22});
            chunk.light_sections.push_back(std::move(light));
        }
    } else {
        add_section(chunk, -4);
    }
    return chunk;
}

/// One stone section at the world bottom plus the four stored heightmaps the
/// generator writes. Every map starts at raw 16: the first cell above the
/// bottom stone section, 16 blocks above the chunk minimum Y of -64.
[[nodiscard]] auto heightmap_column() -> storage::chunk {
    storage::chunk chunk{};
    chunk.format_version = world::chunk_format_version;
    chunk.status = storage::chunk_status::full;
    chunk.y_pos = -4;
    add_section(chunk, -4);
    for (const auto *name : {"WORLD_SURFACE", "OCEAN_FLOOR", "MOTION_BLOCKING",
                             "MOTION_BLOCKING_NO_LEAVES"}) {
        storage::heightmap_record record{};
        record.name = name;
        record.bits_per_entry = 9;
        record.data = heightmap_words(16);
        chunk.heightmaps.push_back(std::move(record));
    }
    return chunk;
}

/// Raw value of one stored heightmap entry, or nullopt when the map is absent.
[[nodiscard]] auto stored_heightmap(const storage::chunk &chunk,
                                    std::string_view name, std::size_t index)
    -> std::optional<std::uint32_t> {
    for (const auto &record : chunk.heightmaps) {
        if (record.name == name && record.bits_per_entry == 9) {
            return packed_value(record.data, index, 9);
        }
    }
    return std::nullopt;
}

[[nodiscard]] auto make_tables() -> std::vector<std::byte> {
    data::vanilla_spec spec{};
    spec.data_version = 5023;
    data::block_spec stone{};
    stone.name = "minecraft:stone";
    data::state_spec state{};
    state.vanilla_id = 1;
    state.is_default = true;
    stone.states.push_back(std::move(state));
    spec.blocks.push_back(std::move(stone));
    auto bytes{data::build_vanilla_tables(spec)};
    expect(bytes.has_value());
    return bytes ? std::move(*bytes) : std::vector<std::byte>{};
}

[[nodiscard]] auto
parse_packet(std::span<const std::byte> body,
             std::vector<std::pair<std::uint32_t, std::vector<std::uint64_t>>>
                 &heightmaps,
             std::string &sky_mask, std::string &block_mask,
             std::string &empty_sky, std::string &empty_block,
             std::size_t &sky_updates, std::size_t &block_updates) -> bool {
    net::reader in{body};
    auto packet_id{in.read_varint()};
    auto x{in.read_i32()};
    auto z{in.read_i32()};
    if (!packet_id || *packet_id != 46 || !x || !z || *x != 0 || *z != 0) {
        return false;
    }
    auto heightmap_count{in.read_varint()};
    if (!heightmap_count || *heightmap_count != 3) {
        return false;
    }
    for (std::uint32_t index{0}; index < *heightmap_count; ++index) {
        auto type{in.read_varint()};
        auto words{in.read_varint()};
        if (!type || !words || *words != 37) {
            return false;
        }
        std::vector<std::uint64_t> values(*words);
        for (auto &word : values) {
            auto value{in.read_i64()};
            if (!value) {
                return false;
            }
            word = static_cast<std::uint64_t>(*value);
        }
        heightmaps.emplace_back(*type, std::move(values));
    }

    auto section_size{in.read_varint()};
    if (!section_size) {
        return false;
    }
    auto section_bytes{in.read_bytes(*section_size)};
    if (!section_bytes) {
        return false;
    }
    net::reader sections{*section_bytes};
    for (std::size_t index{0}; index < 24; ++index) {
        auto non_empty{sections.read_u16()};
        auto fluids{sections.read_u16()};
        auto bits{sections.read_u8()};
        if (!non_empty || !fluids || !bits) {
            return false;
        }
        if (*bits == 0) {
            auto palette{sections.read_varint()};
            if (!palette) {
                return false;
            }
        } else if (*bits <= 8) {
            auto palette_size{sections.read_varint()};
            if (!palette_size) {
                return false;
            }
            for (std::uint32_t entry{0}; entry < *palette_size; ++entry) {
                if (!sections.read_varint()) {
                    return false;
                }
            }
        }
        for (std::size_t word{0}; word < packed_words(section_entries, *bits);
             ++word) {
            if (!sections.read_i64()) {
                return false;
            }
        }
        auto biome_bits{sections.read_u8()};
        if (!biome_bits) {
            return false;
        }
        if (*biome_bits == 0) {
            if (!sections.read_varint()) {
                return false;
            }
        } else if (*biome_bits <= 3) {
            auto palette_size{sections.read_varint()};
            if (!palette_size) {
                return false;
            }
            for (std::uint32_t entry{0}; entry < *palette_size; ++entry) {
                if (!sections.read_varint()) {
                    return false;
                }
            }
        }
        for (std::size_t word{0};
             word < packed_words(biome_entries, *biome_bits); ++word) {
            if (!sections.read_i64()) {
                return false;
            }
        }
    }
    if (!sections.empty()) {
        return false;
    }

    auto entity_count{in.read_varint()};
    if (!entity_count || *entity_count != 0) {
        return false;
    }

    auto read_mask = [&in](std::string &mask) -> bool {
        auto bytes{in.read_varint()};
        if (!bytes) {
            return false;
        }
        auto payload{in.read_bytes(*bytes)};
        if (!payload) {
            return false;
        }
        mask.assign(reinterpret_cast<const char *>(payload->data()),
                    payload->size());
        return true;
    };
    if (!read_mask(sky_mask) || !read_mask(block_mask) ||
        !read_mask(empty_sky) || !read_mask(empty_block)) {
        return false;
    }
    auto updates = [&in](std::size_t &updates_count) -> bool {
        auto entries{in.read_varint()};
        if (!entries) {
            return false;
        }
        updates_count = *entries;
        for (std::uint32_t index{0}; index < *entries; ++index) {
            auto bytes{in.read_varint()};
            if (!bytes || *bytes != light_bytes) {
                return false;
            }
            if (!in.read_bytes(*bytes)) {
                return false;
            }
        }
        return true;
    };
    if (!updates(sky_updates) || !updates(block_updates)) {
        return false;
    }
    return in.empty();
}

suite chunk_packet_tests = [] {
    "server/chunk packet writes byte-array light masks and stored heightmaps"_test =
        [] {
            const auto directory{scratch_directory()};
            {
                auto store{world::world_store::create(directory.string())};
                expect(store.has_value());
                if (!store) {
                    return;
                }
                auto chunk{make_chunk(true)};
                expect(static_cast<bool>(
                    store->write_chunk(world::chunk_pos{0, 0}, chunk)));
                expect(static_cast<bool>(store->commit()));
            }

            auto table_bytes{make_tables()};
            auto tables{data::vanilla_tables::open(table_bytes, false)};
            expect(tables.has_value());
            auto runtime{sim::world::open(directory.string())};
            expect(runtime.has_value());
            if (!tables || !runtime) {
                return;
            }
            auto port{refrain::server::make_borrowed_world_port(*runtime)};
            refrain::server::chunk_packet_source source{
                .world = port.get(),
                .tables = &*tables,
                .registries = nullptr,
            };
            auto packet{refrain::server::encode_full_chunk(source, {0, 0})};
            expect(packet.has_value());
            if (!packet) {
                return;
            }
            std::vector<std::pair<std::uint32_t, std::vector<std::uint64_t>>>
                heightmaps{};
            std::string sky_mask{};
            std::string block_mask{};
            std::string empty_sky{};
            std::string empty_block{};
            std::size_t sky_updates{0};
            std::size_t block_updates{0};
            expect(parse_packet(*packet, heightmaps, sky_mask, block_mask,
                                empty_sky, empty_block, sky_updates,
                                block_updates));
            expect(heightmaps.size() == 3U);
            if (heightmaps.size() == 3U) {
                expect(heightmaps[0].first == 1U);
                expect(heightmaps[1].first == 4U);
                expect(heightmaps[2].first == 5U);
                expect(packed_value(heightmaps[0].second, 0, 9) == 7U);
                expect(packed_value(heightmaps[1].second, 0, 9) == 8U);
                expect(packed_value(heightmaps[2].second, 0, 9) == 9U);
            }
            const auto mask_bit = [](const std::string &mask, std::size_t bit) {
                const auto index{bit / 8U};
                return index < mask.size() &&
                       (static_cast<std::uint8_t>(mask[index]) &
                        (1U << (bit % 8U))) != 0U;
            };
            // The fixture column is solid stone: every light layer is empty,
            // so no data mask is set and every light section is marked empty.
            expect(sky_mask.empty());
            expect(block_mask.empty());
            expect(empty_sky.size() == 4U);
            expect(empty_block.size() == 4U);
            expect(mask_bit(empty_sky, 0));
            expect(mask_bit(empty_sky, 23));
            expect(mask_bit(empty_sky, 25));
            expect(mask_bit(empty_block, 0));
            expect(mask_bit(empty_block, 23));
            expect(mask_bit(empty_block, 25));
            expect(sky_updates == 0U);
            expect(block_updates == 0U);
        };

    "server/chunk packet computes relative heightmaps when none are stored"_test =
        [] {
            const auto directory{scratch_directory()};
            {
                auto store{world::world_store::create(directory.string())};
                expect(store.has_value());
                if (!store) {
                    return;
                }
                auto chunk{make_chunk(false)};
                expect(static_cast<bool>(
                    store->write_chunk(world::chunk_pos{0, 0}, chunk)));
                expect(static_cast<bool>(store->commit()));
            }

            auto table_bytes{make_tables()};
            auto tables{data::vanilla_tables::open(table_bytes, false)};
            expect(tables.has_value());
            auto runtime{sim::world::open(directory.string())};
            expect(runtime.has_value());
            if (!tables || !runtime) {
                return;
            }
            auto port{refrain::server::make_borrowed_world_port(*runtime)};
            refrain::server::chunk_packet_source source{
                .world = port.get(),
                .tables = &*tables,
                .registries = nullptr,
            };
            auto packet{refrain::server::encode_full_chunk(source, {0, 0})};
            expect(packet.has_value());
            if (!packet) {
                return;
            }
            std::vector<std::pair<std::uint32_t, std::vector<std::uint64_t>>>
                heightmaps{};
            std::string sky_mask{};
            std::string block_mask{};
            std::string empty_sky{};
            std::string empty_block{};
            std::size_t sky_updates{0};
            std::size_t block_updates{0};
            expect(parse_packet(*packet, heightmaps, sky_mask, block_mask,
                                empty_sky, empty_block, sky_updates,
                                block_updates));
            expect(heightmaps.size() == 3U);
            for (const auto &[type, words] : heightmaps) {
                expect(type == 1U || type == 4U || type == 5U);
                expect(packed_value(words, 0, 9) == 16U);
            }
            expect(sky_mask.empty());
            expect(block_mask.empty());
            // Only the light layers around the single block section exist; the
            // rest of the light range has no data at all.
            expect(empty_sky.size() == 1U);
            expect(empty_block.size() == 1U);
            expect(static_cast<std::uint8_t>(empty_sky[0]) == 0x07U);
            expect(static_cast<std::uint8_t>(empty_block[0]) == 0x07U);
            expect(sky_updates == 0U);
            expect(block_updates == 0U);
        };

    "server/chunk packet sends updated heightmaps after a block edit"_test =
        [] {
            const auto directory{scratch_directory()};
            {
                auto store{world::world_store::create(directory.string())};
                expect(store.has_value());
                if (!store) {
                    return;
                }
                auto chunk{heightmap_column()};
                expect(static_cast<bool>(
                    store->write_chunk(world::chunk_pos{0, 0}, chunk)));
                expect(static_cast<bool>(store->commit()));
            }

            auto table_bytes{make_tables()};
            auto tables{data::vanilla_tables::open(table_bytes, false)};
            expect(tables.has_value());
            auto runtime{sim::world::open(directory.string())};
            expect(runtime.has_value());
            if (!tables || !runtime) {
                return;
            }
            auto port{server::make_borrowed_world_port(*runtime)};
            server::chunk_packet_source source{
                .world = port.get(),
                .tables = &*tables,
                .registries = nullptr,
            };
            const auto encode = [&source] {
                auto packet{server::encode_full_chunk(source, {0, 0})};
                expect(packet.has_value())
                    << (packet ? "" : packet.error().message);
                std::vector<
                    std::pair<std::uint32_t, std::vector<std::uint64_t>>>
                    heightmaps{};
                std::string sky_mask{};
                std::string block_mask{};
                std::string empty_sky{};
                std::string empty_block{};
                std::size_t sky_updates{0};
                std::size_t block_updates{0};
                expect(packet.has_value() &&
                       parse_packet(*packet, heightmaps, sky_mask, block_mask,
                                    empty_sky, empty_block, sky_updates,
                                    block_updates));
                return heightmaps;
            };
            const auto value_of =
                [](const std::vector<
                       std::pair<std::uint32_t, std::vector<std::uint64_t>>>
                       &maps,
                   std::uint32_t type) -> std::uint32_t {
                for (const auto &[map_type, words] : maps) {
                    if (map_type == type) {
                        return packed_value(words, 0, 9);
                    }
                }
                return std::numeric_limits<std::uint32_t>::max();
            };

            // The stored maps are authoritative and start at the stone surface.
            const auto before{encode()};
            expect(value_of(before, 1U) == 16U); // WORLD_SURFACE
            expect(value_of(before, 4U) == 16U); // MOTION_BLOCKING
            expect(value_of(before, 5U) == 16U); // MOTION_BLOCKING_NO_LEAVES

            const auto stone{runtime->registry().find("minecraft:stone")};
            expect(stone.has_value());
            if (!stone) {
                return;
            }
            // Placing a block above the surface moves the stored maps; a full
            // chunk send after the edit has to carry the new surface, not the
            // pre-edit one.
            expect(runtime->set_block({0, 200, 0}, *stone).has_value());
            const auto *column{runtime->find_chunk({0, 0})};
            expect(column != nullptr);
            if (column == nullptr) {
                return;
            }
            // OCEAN_FLOOR is stored but not sent; the edit has to move it too.
            expect(stored_heightmap(column->storage, "OCEAN_FLOOR", 0) ==
                   std::optional<std::uint32_t>{265U});
            const auto raised{encode()};
            expect(value_of(raised, 1U) == 265U);
            expect(value_of(raised, 4U) == 265U);
            expect(value_of(raised, 5U) == 265U);

            // Removing the top block rescan the column downwards.
            expect(runtime->set_block({0, 200, 0}, sim::air_block).has_value());
            const auto restored{encode()};
            expect(value_of(restored, 1U) == 16U);
            expect(value_of(restored, 4U) == 16U);
            expect(value_of(restored, 5U) == 16U);

            // A fluid blocks motion but is not an ocean floor: MOTION_BLOCKING
            // rises, OCEAN_FLOOR stays at the stone surface.
            const auto water{runtime->registry().intern("minecraft:water")};
            expect(runtime->set_block({0, 200, 0}, water).has_value());
            const auto wet{encode()};
            expect(value_of(wet, 1U) == 265U);
            expect(value_of(wet, 4U) == 265U);
            expect(value_of(wet, 5U) == 265U);
            expect(stored_heightmap(column->storage, "OCEAN_FLOOR", 0) ==
                   std::optional<std::uint32_t>{16U});
        };

    "server/chunk packet section range matches the runtime build range"_test =
        [] {
            const server::chunk_packet_source source{};
            expect(source.min_section_y == sim::world::runtime_min_section_y);
            expect(source.max_section_y == sim::world::runtime_max_section_y);
            expect(sim::world::build_min_y() ==
                   sim::world::runtime_min_section_y * world::section_edge);
            expect(sim::world::build_max_y() ==
                   (sim::world::runtime_max_section_y + 1) *
                           world::section_edge -
                       1);
        };
};

} // namespace
