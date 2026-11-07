// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/chunk_packet.hpp>

#include <refrain/data/nbt.hpp>
#include <refrain/net/base.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

namespace refrain::server {
namespace {

using net::writer;

constexpr std::size_t section_entries{4096};
constexpr std::size_t biome_entries{64};
constexpr std::size_t light_bytes{2048};

[[nodiscard]] auto ceil_log2(std::size_t value) noexcept -> std::uint8_t {
    std::uint8_t bits{0};
    std::size_t capacity{1};
    while (capacity < value) {
        capacity <<= 1U;
        ++bits;
    }
    return bits;
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
    if (bits == 0) {
        return 0;
    }
    const auto per_word{64U / bits};
    const auto word{words[index / per_word]};
    const auto shift{(index % per_word) * bits};
    return static_cast<std::uint32_t>((word >> shift) &
                                      ((std::uint64_t{1} << bits) - 1U));
}

void write_bitset(std::vector<std::byte> &out,
                  std::span<const std::uint64_t> words) {
    // BitSet is transmitted as a byte array (little-endian words), not as a
    // long array: VarInt byte length followed by the bytes.
    writer target{out};
    target.write_varint(
        static_cast<std::uint32_t>(words.size() * sizeof(std::uint64_t)));
    for (const auto word : words) {
        target.write_i64(static_cast<std::int64_t>(word));
    }
}

void set_bit(std::vector<std::uint64_t> &words, std::size_t index) {
    words[index / 64] |= std::uint64_t{1} << (index % 64);
}

[[nodiscard]] auto registry_id(const net::registry_catalog *catalog,
                               std::string_view registry, std::string_view name)
    -> std::optional<std::uint32_t> {
    if (catalog == nullptr) {
        return std::nullopt;
    }
    for (const auto &definition : catalog->registries()) {
        if (definition.key != registry) {
            continue;
        }
        for (const auto &entry : definition.entries) {
            if (entry.name == name) {
                return entry.id;
            }
        }
    }
    return std::nullopt;
}

struct encoded_section {
    std::uint16_t non_empty{0};
    std::uint16_t fluids{0};
    std::uint8_t block_bits{0};
    bool block_global{false};
    std::vector<std::uint32_t> block_palette{};
    std::vector<std::uint64_t> block_words{};
    std::uint8_t biome_bits{0};
    bool biome_global{false};
    std::vector<std::uint32_t> biome_palette{};
    std::vector<std::uint64_t> biome_words{};
};

struct light_data {
    std::vector<std::uint64_t> sky_mask{};
    std::vector<std::uint64_t> block_mask{};
    std::vector<std::uint64_t> empty_sky{};
    std::vector<std::uint64_t> empty_block{};
    std::vector<std::vector<std::byte>> sky_updates{};
    std::vector<std::vector<std::byte>> block_updates{};
};

[[nodiscard]] auto block_state(const sim::section &section, std::size_t index)
    -> sim::block_state_id {
    return section.uniform ? section.value : section.blocks[index];
}

[[nodiscard]] auto encode_section(const sim::world &world,
                                  const data::vanilla_tables *tables,
                                  const net::registry_catalog *catalog,
                                  const sim::section &section)
    -> result<encoded_section> {
    encoded_section encoded{};
    std::map<std::uint32_t, std::uint32_t> palette_index{};
    std::vector<sim::block_state_id> runtime_palette{};
    std::vector<std::uint32_t> vanilla{};
    vanilla.reserve(section_entries);

    for (std::size_t index{0}; index < section_entries; ++index) {
        const auto state{block_state(section, index)};
        const auto key{world.registry().key(state)};
        std::uint32_t vanilla_id{0};
        if (!key.empty() && tables != nullptr) {
            if (const auto dense{tables->find_state(key)}; dense) {
                vanilla_id = tables->state_at(*dense).vanilla_id;
            }
        }
        vanilla.push_back(vanilla_id);
        const bool air{vanilla_id == 0};
        if (!air) {
            ++encoded.non_empty;
        }
        if (!key.empty() &&
            (key.find("water") != std::string_view::npos ||
             key.find("lava") != std::string_view::npos ||
             key.find("waterlogged=true") != std::string_view::npos)) {
            ++encoded.fluids;
        }
    }

    for (const auto id : vanilla) {
        if (!palette_index.contains(id)) {
            const auto next{static_cast<std::uint32_t>(palette_index.size())};
            palette_index.emplace(id, next);
            encoded.block_palette.push_back(id);
        }
    }

    if (encoded.block_palette.size() == 1) {
        encoded.block_bits = 0;
    } else if (encoded.block_palette.size() <= 256) {
        encoded.block_bits =
            std::max<std::uint8_t>(4, ceil_log2(encoded.block_palette.size()));
    } else {
        encoded.block_global = true;
        encoded.block_bits = 16;
    }
    encoded.block_words.assign(
        packed_words(section_entries, encoded.block_bits), 0);
    if (encoded.block_bits > 0) {
        const auto per_word{64U / encoded.block_bits};
        for (std::size_t index{0}; index < section_entries; ++index) {
            const auto value = encoded.block_global
                                   ? vanilla[index]
                                   : palette_index[vanilla[index]];
            encoded.block_words[index / per_word] |=
                static_cast<std::uint64_t>(value)
                << ((index % per_word) * encoded.block_bits);
        }
    }

    // Biomes: reuse the storage palette when its packed payload is valid,
    // otherwise use a single plains value. The wire palette stores ids while
    // the packed payload stores palette indices.
    const auto &base{section.base};
    if (base.biome_palette.empty()) {
        encoded.biome_palette.push_back(
            registry_id(catalog, "minecraft:worldgen/biome", "minecraft:plains")
                .value_or(41));
    } else {
        for (const auto &key : base.biome_palette) {
            encoded.biome_palette.push_back(
                registry_id(catalog, "minecraft:worldgen/biome", key)
                    .value_or(41));
        }
        if (base.biome_bits_per_entry == 0 || base.biome_data.empty() ||
            encoded.biome_palette.size() == 1) {
            encoded.biome_palette.resize(1);
        } else if (encoded.biome_palette.size() <= 8) {
            encoded.biome_bits = ceil_log2(encoded.biome_palette.size());
            const auto per_word{64U / encoded.biome_bits};
            encoded.biome_words.assign(
                packed_words(biome_entries, encoded.biome_bits), 0);
            for (std::size_t index{0}; index < biome_entries; ++index) {
                const auto palette_value{packed_value(
                    base.biome_data, index, base.biome_bits_per_entry)};
                const auto bounded{std::min<std::size_t>(
                    palette_value, encoded.biome_palette.size() - 1)};
                encoded.biome_words[index / per_word] |=
                    static_cast<std::uint64_t>(bounded)
                    << ((index % per_word) * encoded.biome_bits);
            }
        } else {
            encoded.biome_global = true;
            encoded.biome_bits = 7;
            encoded.biome_words.assign(
                packed_words(biome_entries, encoded.biome_bits), 0);
            const auto per_word{64U / encoded.biome_bits};
            for (std::size_t index{0}; index < biome_entries; ++index) {
                const auto palette_value{packed_value(
                    base.biome_data, index, base.biome_bits_per_entry)};
                const auto bounded{std::min<std::size_t>(
                    palette_value, encoded.biome_palette.size() - 1)};
                encoded.biome_words[index / per_word] |=
                    static_cast<std::uint64_t>(encoded.biome_palette[bounded])
                    << ((index % per_word) * encoded.biome_bits);
            }
        }
    }
    return encoded;
}

[[nodiscard]] auto computed_heightmap(const sim::world &world,
                                      const sim::chunk &chunk)
    -> std::vector<std::uint64_t> {
    constexpr std::uint8_t bits{9};
    constexpr std::size_t per_word{7};
    std::vector<std::uint64_t> words(packed_words(256, bits), 0);
    const auto base_x{chunk.pos.x * 16};
    const auto base_z{chunk.pos.z * 16};
    for (std::int32_t z{0}; z < 16; ++z) {
        for (std::int32_t x{0}; x < 16; ++x) {
            std::int32_t top{chunk.y_pos * 16};
            for (std::int32_t y{(chunk.y_pos + 24) * 16 - 1};
                 y >= chunk.y_pos * 16; --y) {
                if (world.block_at(
                        sim::block_pos{base_x + x, static_cast<std::int16_t>(y),
                                       base_z + z}) != sim::air_block) {
                    top = y + 1;
                    break;
                }
            }
            const auto index{static_cast<std::size_t>(z * 16 + x)};
            words[index / per_word] |= static_cast<std::uint64_t>(top)
                                       << ((index % per_word) * bits);
        }
    }
    return words;
}

[[nodiscard]] auto encode_block_entity(const sim::world & /*world*/,
                                       const net::registry_catalog *catalog,
                                       std::span<const std::byte> raw,
                                       std::vector<std::byte> &out) -> bool {
    auto parsed{data::nbt::read(raw)};
    if (!parsed) {
        return false;
    }
    const auto *id{parsed->get("id")};
    if (id == nullptr || !id->as_string()) {
        return false;
    }
    const auto type{
        registry_id(catalog, "minecraft:block_entity_type", *id->as_string())};
    if (!type) {
        return false;
    }
    const auto *x{parsed->get("x")};
    const auto *y{parsed->get("y")};
    const auto *z{parsed->get("z")};
    if (x == nullptr || y == nullptr || z == nullptr || !x->as_int() ||
        !y->as_int() || !z->as_int()) {
        return false;
    }
    writer target{out};
    const auto packed_xz{static_cast<std::uint8_t>(((*x->as_int() & 15) << 4) |
                                                   (*z->as_int() & 15))};
    target.write_u8(packed_xz);
    target.write_u16(static_cast<std::uint16_t>(*y->as_int()));
    target.write_varint(*type);
    target.write_bool(false); // no update tag in the first implementation
    return true;
}

} // namespace

auto encode_full_chunk(const chunk_packet_source &source, sim::chunk_pos pos)
    -> result<std::vector<std::byte>> {
    if (source.world == nullptr) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "chunk source has no world")};
    }
    auto loaded{source.world->ensure_chunk(pos)};
    if (!loaded) {
        return std::unexpected{std::move(loaded.error())};
    }
    const auto &chunk{**loaded};

    std::map<std::int32_t, const sim::section *> sections{};
    for (const auto &section : chunk.sections) {
        sections.emplace(section.y, &section);
    }

    std::vector<std::byte> packet;
    writer out{packet};
    out.write_varint(46); // level_chunk_with_light
    out.write_i32(pos.x);
    out.write_i32(pos.z);

    // Heightmaps. Prefer the stored payload when it uses the overworld's
    // nine-bit encoding; otherwise compute the four client maps.
    const auto computed{computed_heightmap(*source.world, chunk)};
    std::array<std::pair<std::string_view, std::uint32_t>, 4> requested{{
        {"WORLD_SURFACE", 1},
        {"OCEAN_FLOOR", 3},
        {"MOTION_BLOCKING", 4},
        {"MOTION_BLOCKING_NO_LEAVES", 5},
    }};
    out.write_varint(static_cast<std::uint32_t>(requested.size()));
    for (const auto [name, type] : requested) {
        std::span<const std::uint64_t> words{computed};
        for (const auto &stored : chunk.storage.heightmaps) {
            if (stored.name == name && stored.bits_per_entry == 9) {
                words = stored.data;
                break;
            }
        }
        out.write_varint(type);
        out.write_varint(static_cast<std::uint32_t>(words.size()));
        for (const auto word : words) {
            out.write_i64(static_cast<std::int64_t>(word));
        }
    }

    std::vector<std::byte> section_bytes;
    for (std::int32_t y{source.min_section_y}; y <= source.max_section_y; ++y) {
        encoded_section encoded{};
        const auto found{sections.find(y)};
        if (found != sections.end()) {
            auto result{encode_section(*source.world, source.tables,
                                       source.registries, *found->second)};
            if (!result) {
                return std::unexpected{std::move(result.error())};
            }
            encoded = std::move(*result);
        } else {
            encoded.non_empty = 0;
            encoded.fluids = 0;
            encoded.block_bits = 0;
            encoded.block_palette.push_back(0);
            encoded.biome_bits = 0;
            encoded.biome_palette.push_back(
                registry_id(source.registries, "minecraft:worldgen/biome",
                            "minecraft:plains")
                    .value_or(41));
        }

        writer section{section_bytes};
        section.write_u16(encoded.non_empty);
        section.write_u16(encoded.fluids);
        section.write_u8(encoded.block_bits);
        if (encoded.block_bits == 0) {
            section.write_varint(encoded.block_palette.empty()
                                     ? 0
                                     : encoded.block_palette.front());
        } else if (!encoded.block_global) {
            section.write_varint(
                static_cast<std::uint32_t>(encoded.block_palette.size()));
            for (const auto id : encoded.block_palette) {
                section.write_varint(id);
            }
        }
        for (const auto word : encoded.block_words) {
            section.write_i64(static_cast<std::int64_t>(word));
        }

        section.write_u8(encoded.biome_bits);
        if (encoded.biome_bits == 0) {
            section.write_varint(encoded.biome_palette.empty()
                                     ? 41
                                     : encoded.biome_palette.front());
        } else if (!encoded.biome_global) {
            section.write_varint(
                static_cast<std::uint32_t>(encoded.biome_palette.size()));
            for (const auto id : encoded.biome_palette) {
                section.write_varint(id);
            }
        }
        for (const auto word : encoded.biome_words) {
            section.write_i64(static_cast<std::int64_t>(word));
        }
    }
    out.write_varint(static_cast<std::uint32_t>(section_bytes.size()));
    out.write_bytes(section_bytes);

    std::vector<std::byte> entity_entries;
    std::uint32_t entity_count{0};
    for (const auto &raw : chunk.storage.block_entities) {
        if (encode_block_entity(*source.world, source.registries, raw,
                                entity_entries)) {
            ++entity_count;
        }
    }
    std::vector<std::byte> block_entities;
    writer entity_prefix{block_entities};
    entity_prefix.write_varint(entity_count);
    block_entities.insert(block_entities.end(), entity_entries.begin(),
                          entity_entries.end());
    out.write_bytes(block_entities);

    // Light: one mask entry per light section, with empty masks for layers the
    // storage does not carry.
    const auto light_min{source.min_section_y - 1};
    const auto light_count{static_cast<std::size_t>(source.max_section_y -
                                                    source.min_section_y + 3)};
    const auto word_count{(light_count + 63) / 64};
    light_data light{};
    light.sky_mask.assign(word_count, 0);
    light.block_mask.assign(word_count, 0);
    light.empty_sky.assign(word_count, 0);
    light.empty_block.assign(word_count, 0);
    std::map<std::int32_t, const refrain::storage::light_section *>
        light_by_y{};
    for (const auto &layer : chunk.storage.light_sections) {
        light_by_y.emplace(layer.y, &layer);
    }
    for (std::size_t index{0}; index < light_count; ++index) {
        const auto y{static_cast<std::int32_t>(light_min + index)};
        const auto found{light_by_y.find(y)};
        if (found == light_by_y.end()) {
            set_bit(light.empty_sky, index);
            set_bit(light.empty_block, index);
            continue;
        }
        const auto &layer{*found->second};
        if (layer.sky_light.size() == light_bytes) {
            set_bit(light.sky_mask, index);
            light.sky_updates.push_back(layer.sky_light);
        } else {
            set_bit(light.empty_sky, index);
        }
        if (layer.block_light.size() == light_bytes) {
            set_bit(light.block_mask, index);
            light.block_updates.push_back(layer.block_light);
        } else {
            set_bit(light.empty_block, index);
        }
    }
    write_bitset(packet, light.sky_mask);
    write_bitset(packet, light.block_mask);
    write_bitset(packet, light.empty_sky);
    write_bitset(packet, light.empty_block);
    out.write_varint(static_cast<std::uint32_t>(light.sky_updates.size()));
    for (const auto &layer : light.sky_updates) {
        out.write_varint(static_cast<std::uint32_t>(layer.size()));
        out.write_bytes(layer);
    }
    out.write_varint(static_cast<std::uint32_t>(light.block_updates.size()));
    for (const auto &layer : light.block_updates) {
        out.write_varint(static_cast<std::uint32_t>(layer.size()));
        out.write_bytes(layer);
    }

    if (packet.size() > net::max_frame_length) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       fmt::format("chunk ({}, {}) is too large for one frame",
                                   pos.x, pos.z))};
    }
    return packet;
}

auto build_spawn_chunks(const chunk_packet_source &source,
                        sim::chunk_pos center, std::int32_t radius)
    -> result<std::vector<std::vector<std::byte>>> {
    std::vector<std::vector<std::byte>> packets{};
    packets.reserve(
        static_cast<std::size_t>((radius * 2 + 1) * (radius * 2 + 1)));
    for (std::int32_t distance{0}; distance <= radius; ++distance) {
        for (std::int32_t dx{-distance}; dx <= distance; ++dx) {
            for (std::int32_t dz{-distance}; dz <= distance; ++dz) {
                if (distance > 0 &&
                    std::max(std::abs(dx), std::abs(dz)) != distance) {
                    continue;
                }
                const sim::chunk_pos pos{center.x + dx, center.z + dz};
                auto packet{encode_full_chunk(source, pos)};
                if (!packet) {
                    // A sparse world simply has no chunk at this position.
                    // Other failures mean a section could not be encoded and
                    // would send a chunk the client renders incorrectly.
                    if (packet.error().code == error_code::not_found) {
                        continue;
                    }
                    return std::unexpected{std::move(packet.error())};
                }
                packets.push_back(std::move(*packet));
            }
        }
    }
    return packets;
}

} // namespace refrain::server
