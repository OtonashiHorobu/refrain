// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Opt-in biome oracle: compares the quart biome grid of generated chunks with
// the biomes stored by the reference server. The world keeps its normal
// multi-noise biome source (no data pack overrides); biomes are written at the
// `biomes` chunk status and are independent of surface rules, features and
// structures.
//
// `REFRAIN_VANILLA_WORLDGEN` points at the datapack `data` root and
// `REFRAIN_VANILLA_ORACLE_SEED` at the world seed. The overworld suite reads
// `REFRAIN_VANILLA_BIOME_REGION` and the nether suite
// `REFRAIN_VANILLA_NETHER_BIOME_REGION`; each names a region file of a plain
// world of that dimension. The nether suite takes the seed from the
// environment and compares every full chunk the region contains.

#include <refrain/core/io.hpp>
#include <refrain/data/nbt.hpp>
#include <refrain/worldgen/biome/biome_manager.hpp>
#include <refrain/worldgen/biome/biome_parameters.hpp>
#include <refrain/worldgen/biome/biome_source.hpp>
#include <refrain/worldgen/density/density_router.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace refrain;
using namespace refrain::worldgen;
using worldgen::algorithm;

constexpr jint oracle_min_y = -64;
constexpr jint oracle_height = 384;
constexpr jint oracle_min_section_y = -4;
constexpr jint oracle_max_section_y = 19;

auto as_u32(std::span<const std::byte> bytes, std::size_t offset)
    -> std::uint32_t {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

auto chunk_payload(std::span<const std::byte> region, jint chunk_x,
                   jint chunk_z) -> std::optional<std::span<const std::byte>> {
    const auto index =
        static_cast<std::size_t>((chunk_x & 31) + (chunk_z & 31) * 32);
    if (region.size() < 8192) {
        return std::nullopt;
    }
    const auto *entry = region.data() + index * 4U;
    const auto sector_offset = (static_cast<std::size_t>(entry[0]) << 16U) |
                               (static_cast<std::size_t>(entry[1]) << 8U) |
                               static_cast<std::size_t>(entry[2]);
    if (sector_offset == 0) {
        return std::nullopt;
    }
    const auto start = sector_offset * 4096U;
    if (start + 5U > region.size()) {
        return std::nullopt;
    }
    const auto length = as_u32(region, start);
    if (length < 1U || start + 4U + length > region.size()) {
        return std::nullopt;
    }
    if (static_cast<std::uint8_t>(region[start + 4U]) != 2U) {
        return std::nullopt;
    }
    return region.subspan(start + 5U, length - 1U);
}

auto ceil_log2(std::size_t value) -> int {
    int bits = 0;
    std::size_t limit = 1;
    while (limit < value) {
        limit <<= 1U;
        ++bits;
    }
    return bits;
}

auto int_value(const data::nbt::value &tag) -> std::optional<jint> {
    if (const auto value = tag.as_int(); value.has_value()) {
        return *value;
    }
    if (const auto value = tag.as_short(); value.has_value()) {
        return *value;
    }
    if (const auto value = tag.as_byte(); value.has_value()) {
        return *value;
    }
    return std::nullopt;
}

auto child(const data::nbt::compound *entries, std::string_view key)
    -> const data::nbt::value * {
    if (entries == nullptr) {
        return nullptr;
    }
    const auto found = entries->find(key);
    return found == entries->end() ? nullptr : &found->second;
}

/// Decodes one section's 4x4x4 biome palette into cell order
/// `x + z*4 + y*16`.
auto decode_section_biomes(const data::nbt::value &section)
    -> std::optional<std::vector<std::string>> {
    const auto *section_data = section.as_compound();
    if (section_data == nullptr) {
        return std::nullopt;
    }
    const auto *biomes_value = child(section_data, "biomes");
    const auto *biomes =
        biomes_value == nullptr ? nullptr : biomes_value->as_compound();
    if (biomes == nullptr) {
        return std::nullopt;
    }
    const auto *palette_value = child(biomes, "palette");
    const auto *palette =
        palette_value == nullptr ? nullptr : palette_value->as_list();
    if (palette == nullptr || palette->items.empty()) {
        return std::nullopt;
    }
    std::vector<std::string> names;
    names.reserve(palette->items.size());
    for (const auto &entry : palette->items) {
        const auto name = entry.as_string();
        if (!name) {
            return std::nullopt;
        }
        names.emplace_back(*name);
    }
    std::vector<std::string> cells(64);
    if (names.size() == 1) {
        std::fill(cells.begin(), cells.end(), names.front());
        return cells;
    }
    const auto *data_value = child(biomes, "data");
    const auto data =
        data_value == nullptr ? std::nullopt : data_value->as_long_array();
    if (!data) {
        return std::nullopt;
    }
    const int bits = std::max(1, ceil_log2(names.size()));
    const auto per_long = static_cast<std::size_t>(64 / bits);
    const auto mask = (std::uint64_t{1} << bits) - 1U;
    for (std::size_t i = 0; i < cells.size(); ++i) {
        if (i / per_long >= data->size()) {
            return std::nullopt;
        }
        const auto raw = static_cast<std::uint64_t>((*data)[i / per_long]) >>
                         ((i % per_long) * static_cast<std::size_t>(bits));
        const auto palette_index = raw & mask;
        if (palette_index >= names.size()) {
            return std::nullopt;
        }
        cells[i] = names[palette_index];
    }
    return cells;
}

} // namespace

using namespace boost::ut;

suite<"worldgen/nether-biome-oracle"> nether_biome_oracle_tests = [] {
    // The reference world keeps the normal nether preset, so the stored biome
    // grid is the output of the real multi-noise source. The region may hold
    // any number of force-loaded chunks; every chunk present is compared.
    "nether chunks store the reference quart biomes"_test = [] {
        const char *data_root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
        const char *region_path =
            std::getenv("REFRAIN_VANILLA_NETHER_BIOME_REGION");
        const char *raw_seed = std::getenv("REFRAIN_VANILLA_ORACLE_SEED");
        if (data_root == nullptr || region_path == nullptr ||
            raw_seed == nullptr) {
            return;
        }
        const jlong seed = std::strtoll(raw_seed, nullptr, 10);
        const auto region = core::io::read_file(region_path);
        expect(region.has_value()) << region.error().message;
        if (!region) {
            return;
        }
        auto settings =
            density::load_noise_settings(data_root, "minecraft/nether",
                                         worldgen::algorithm::xoroshiro, seed);
        expect(settings.has_value()) << settings.error().message;
        if (!settings) {
            return;
        }
        const density::sampler_context context;
        const climate_sampler sampler{*settings->router.temperature,
                                      *settings->router.vegetation,
                                      *settings->router.continents,
                                      *settings->router.erosion,
                                      *settings->router.depth,
                                      *settings->router.ridges,
                                      context};
        auto parameters =
            load_biome_parameters(data_root, dimension_kind::nether);
        expect(parameters.has_value()) << parameters.error().message;
        if (!parameters) {
            return;
        }
        multi_noise_biome_source source{std::move(*parameters)};
        const auto &profile = dimension_profile_for(dimension_kind::nether);

        std::size_t compared_chunks = 0;
        std::size_t compared_cells = 0;
        std::size_t mismatches = 0;
        for (jint chunk_z = 0; chunk_z < 32; ++chunk_z) {
            for (jint chunk_x = 0; chunk_x < 32; ++chunk_x) {
                const auto payload = chunk_payload(*region, chunk_x, chunk_z);
                if (!payload) {
                    continue;
                }
                const auto document =
                    data::nbt::read(*payload, data::nbt::compression::zlib);
                expect(document.has_value()) << document.error().message;
                if (!document) {
                    continue;
                }
                const auto *root = document->as_compound();
                const auto *status_value = child(root, "Status");
                if (root == nullptr || status_value == nullptr) {
                    expect(false) << "unexpected chunk document";
                    continue;
                }
                const auto status = status_value->as_string();
                if (!status || *status != "minecraft:full") {
                    // Force-loading can save neighbour chunks before they
                    // reach the full status; those carry no final biome grid.
                    continue;
                }
                const auto *x_pos = child(root, "xPos");
                const auto *z_pos = child(root, "zPos");
                if (x_pos == nullptr || z_pos == nullptr) {
                    expect(false) << "full chunk has no position";
                    continue;
                }
                const auto x = int_value(*x_pos);
                const auto z = int_value(*z_pos);
                if (!x || *x != chunk_x || !z || *z != chunk_z) {
                    expect(false) << "unexpected chunk header";
                    continue;
                }
                const auto *sections_value = child(root, "sections");
                const auto *sections = sections_value == nullptr
                                           ? nullptr
                                           : sections_value->as_list();
                if (sections == nullptr) {
                    expect(false) << "chunk has no sections";
                    continue;
                }
                biome_grid grid{source,
                                sampler,
                                chunk_x * 4,
                                profile.min_y >> 2,
                                chunk_z * 4,
                                4,
                                profile.height >> 2,
                                4};
                ++compared_chunks;
                for (const auto &section : sections->items) {
                    const auto *section_data = section.as_compound();
                    const auto *y_value = section_data == nullptr
                                              ? nullptr
                                              : child(section_data, "Y");
                    const auto section_y =
                        y_value == nullptr ? std::nullopt : int_value(*y_value);
                    if (!section_y) {
                        continue;
                    }
                    const auto expected = decode_section_biomes(section);
                    if (!expected) {
                        expect(false) << "section has no biome palette";
                        continue;
                    }
                    expect(expected->size() == 64U);
                    for (jint x_cell = 0; x_cell < 4; ++x_cell) {
                        for (jint y_cell = 0; y_cell < 4; ++y_cell) {
                            for (jint z_cell = 0; z_cell < 4; ++z_cell) {
                                const jint quart_y = *section_y * 4 + y_cell;
                                const jint quart_x = chunk_x * 4 + x_cell;
                                const jint quart_z = chunk_z * 4 + z_cell;
                                const auto id =
                                    grid.at(quart_x, quart_y, quart_z);
                                const auto &want =
                                    (*expected)[static_cast<std::size_t>(
                                        x_cell + z_cell * 4 + y_cell * 16)];
                                ++compared_cells;
                                if (source.biome(id) == want) {
                                    continue;
                                }
                                if (mismatches < 8) {
                                    std::fprintf(
                                        stderr,
                                        "chunk (%d,%d) quart (%d,%d,%d): got "
                                        "%.*s want %s\n",
                                        static_cast<int>(chunk_x),
                                        static_cast<int>(chunk_z),
                                        static_cast<int>(quart_x),
                                        static_cast<int>(quart_y),
                                        static_cast<int>(quart_z),
                                        static_cast<int>(
                                            source.biome(id).size()),
                                        source.biome(id).data(), want.c_str());
                                }
                                ++mismatches;
                            }
                        }
                    }
                }
            }
        }
        expect(compared_chunks > 0U) << "no reference chunk was compared";
        expect(compared_cells > 0U);
        expect(mismatches == 0) << "biome mismatches: " << mismatches;
    };
};

suite<"worldgen/biome-oracle"> biome_oracle_tests = [] {
    "generated chunks store the reference quart biomes"_test = [] {
        const char *data_root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
        const char *region_path = std::getenv("REFRAIN_VANILLA_BIOME_REGION");
        if (data_root == nullptr || region_path == nullptr) {
            return;
        }
        jlong seed = 12345;
        if (const char *raw = std::getenv("REFRAIN_VANILLA_ORACLE_SEED");
            raw != nullptr) {
            seed = std::strtoll(raw, nullptr, 10);
        }
        const auto region = core::io::read_file(region_path);
        expect(region.has_value()) << region.error().message;
        if (!region) {
            return;
        }
        auto settings =
            density::load_noise_settings(data_root, "minecraft/overworld",
                                         worldgen::algorithm::xoroshiro, seed);
        expect(settings.has_value()) << settings.error().message;
        if (!settings) {
            return;
        }

        const density::sampler_context context;
        const climate_sampler sampler{*settings->router.temperature,
                                      *settings->router.vegetation,
                                      *settings->router.continents,
                                      *settings->router.erosion,
                                      *settings->router.depth,
                                      *settings->router.ridges,
                                      context};
        auto parameters =
            load_biome_parameters(data_root, dimension_kind::overworld);
        expect(parameters.has_value()) << parameters.error().message;
        if (!parameters) {
            return;
        }
        multi_noise_biome_source source{std::move(*parameters)};

        constexpr std::array<std::array<jint, 2>, 5> chunks{{
            {0, 0},
            {1, 0},
            {0, 1},
            {1, 1},
            {2, 2},
        }};
        std::size_t mismatches = 0;
        std::size_t cells = 0;
        for (const auto &coordinates : chunks) {
            const jint chunk_x = coordinates[0];
            const jint chunk_z = coordinates[1];
            const auto payload = chunk_payload(*region, chunk_x, chunk_z);
            expect(payload.has_value())
                << "chunk is missing from the region file";
            if (!payload) {
                continue;
            }
            const auto document =
                data::nbt::read(*payload, data::nbt::compression::zlib);
            expect(document.has_value()) << document.error().message;
            if (!document) {
                continue;
            }
            const auto *root = document->as_compound();
            const auto *status_value = child(root, "Status");
            const auto *x_pos = child(root, "xPos");
            const auto *z_pos = child(root, "zPos");
            if (root == nullptr || status_value == nullptr ||
                x_pos == nullptr || z_pos == nullptr) {
                expect(false) << "unexpected chunk document";
                continue;
            }
            const auto status = status_value->as_string();
            const auto x = int_value(*x_pos);
            const auto z = int_value(*z_pos);
            if (!status || *status != "minecraft:full" || !x || *x != chunk_x ||
                !z || *z != chunk_z) {
                expect(false) << "unexpected chunk header";
                continue;
            }

            const auto *sections_value = child(root, "sections");
            const auto *sections =
                sections_value == nullptr ? nullptr : sections_value->as_list();
            if (sections == nullptr) {
                expect(false) << "chunk has no sections";
                continue;
            }
            std::map<jint, const data::nbt::value *> by_y;
            for (const auto &section : sections->items) {
                const auto *section_data = section.as_compound();
                const auto *y_value = section_data == nullptr
                                          ? nullptr
                                          : child(section_data, "Y");
                if (y_value == nullptr) {
                    continue;
                }
                const auto y = int_value(*y_value);
                if (y) {
                    by_y[*y] = &section;
                }
            }

            biome_grid grid{source,      sampler, chunk_x * 4, -16,
                            chunk_z * 4, 4,       96,          4};
            for (jint section_y = oracle_min_section_y;
                 section_y <= oracle_max_section_y; ++section_y) {
                const auto found = by_y.find(section_y);
                if (found == by_y.end()) {
                    expect(false) << "missing section";
                    continue;
                }
                const auto expected = decode_section_biomes(*found->second);
                if (!expected) {
                    expect(false) << "section has no biome palette";
                    continue;
                }
                for (jint x_cell = 0; x_cell < 4; ++x_cell) {
                    for (jint y_cell = 0; y_cell < 4; ++y_cell) {
                        for (jint z_cell = 0; z_cell < 4; ++z_cell) {
                            const jint quart_x = chunk_x * 4 + x_cell;
                            const jint quart_y = section_y * 4 + y_cell;
                            const jint quart_z = chunk_z * 4 + z_cell;
                            const auto id = grid.at(quart_x, quart_y, quart_z);
                            const auto &want =
                                (*expected)[static_cast<std::size_t>(
                                    x_cell + z_cell * 4 + y_cell * 16)];
                            ++cells;
                            if (source.biome(id) != want) {
                                if (mismatches < 8) {
                                    std::fprintf(
                                        stderr,
                                        "chunk (%d,%d) quart (%d,%d,%d): got "
                                        "%.*s want %s\n",
                                        static_cast<int>(chunk_x),
                                        static_cast<int>(chunk_z),
                                        static_cast<int>(quart_x),
                                        static_cast<int>(quart_y),
                                        static_cast<int>(quart_z),
                                        static_cast<int>(
                                            source.biome(id).size()),
                                        source.biome(id).data(), want.c_str());
                                }
                                ++mismatches;
                            }
                        }
                    }
                }
            }
        }
        expect(cells == 5U * 24U * 64U);
        expect(mismatches == 0) << "biome mismatches: " << mismatches;
    };
};
