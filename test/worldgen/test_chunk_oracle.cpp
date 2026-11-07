// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Opt-in chunk oracle: compares generated chunks against chunks written by the
// reference server with surface rules, carvers, features and structures
// disabled, so the block array is exactly the output of the fill stage.
//
// `REFRAIN_VANILLA_WORLDGEN` points at the datapack `data` root,
// `REFRAIN_VANILLA_REGION` at the overworld region file whose chunks (0,0) to
// (2,2) were force-loaded, and `REFRAIN_VANILLA_ORACLE_SEED` at the world seed
// (default 12345). `tools/worldgen/oracle/generate.py` writes such a world.

#include <refrain/core/io.hpp>
#include <refrain/data/nbt.hpp>
#include <refrain/worldgen/aquifer.hpp>
#include <refrain/worldgen/density_router.hpp>
#include <refrain/worldgen/terrain.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace boost::ut;
using namespace refrain;
using namespace refrain::worldgen;

constexpr jint oracle_seed = 12345;
constexpr jint oracle_chunk_x = 0;
constexpr jint oracle_chunk_z = 0;
constexpr jint oracle_min_y = -64;
constexpr jint oracle_height = 384;
constexpr std::uint16_t id_air = 0;
constexpr std::uint16_t id_stone = 1;
constexpr std::uint16_t id_water = 2;
constexpr std::uint16_t id_lava = 3;

auto as_u32(std::span<const std::byte> bytes, std::size_t offset)
    -> std::uint32_t {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

/// One chunk's compressed payload from an Anvil region file, or `nullopt` when
/// the chunk is absent, its sectors are out of bounds, or it is not zlib.
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
    const auto compression = static_cast<std::uint8_t>(region[start + 4U]);
    if (compression != 2U) {
        return std::nullopt;
    }
    return region.subspan(start + 5U, length - 1U);
}

/// Compound lookup; `value::get` only exists on values, compounds are maps.
auto child(const data::nbt::compound *entries, std::string_view key)
    -> const data::nbt::value * {
    if (entries == nullptr) {
        return nullptr;
    }
    const auto it = entries->find(key);
    return it == entries->end() ? nullptr : &it->second;
}

/// Section `Y` is a byte tag; coordinates accept the integer tag widths.
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

auto ceil_log2(std::size_t value) -> int {
    int bits = 0;
    std::size_t limit = 1;
    while (limit < value) {
        limit <<= 1U;
        ++bits;
    }
    return bits;
}

auto block_id(std::string_view name) -> std::optional<std::uint16_t> {
    if (name == "minecraft:air") {
        return id_air;
    }
    if (name == "minecraft:stone") {
        return id_stone;
    }
    if (name == "minecraft:water") {
        return id_water;
    }
    if (name == "minecraft:lava") {
        return id_lava;
    }
    return std::nullopt;
}

/// `PostProcessing` is a list of per-section short lists; entries are packed
/// `x | y<<4 | z<<8` relative to the chunk section.
auto decode_post_processing(const data::nbt::value &root)
    -> std::optional<std::vector<std::vector<std::uint16_t>>> {
    const auto *value = root.get("PostProcessing");
    if (value == nullptr) {
        return std::nullopt;
    }
    const auto *list = value->as_list();
    if (list == nullptr) {
        return std::nullopt;
    }
    std::vector<std::vector<std::uint16_t>> sections;
    sections.reserve(list->items.size());
    for (const auto &section : list->items) {
        std::vector<std::uint16_t> offsets;
        if (const auto *inner = section.as_list(); inner != nullptr) {
            offsets.reserve(inner->items.size());
            for (const auto &entry : inner->items) {
                const auto packed = entry.as_short();
                if (!packed) {
                    return std::nullopt;
                }
                offsets.push_back(static_cast<std::uint16_t>(*packed));
            }
        } else if (!section.is(data::nbt::tag_type::end)) {
            return std::nullopt;
        }
        sections.push_back(std::move(offsets));
    }
    return sections;
}

struct vanilla_chunk {
    std::vector<std::uint16_t> blocks{}; ///< index `(y-min_y)*256 + z*16 + x`
    std::array<jint, 256> world_surface{};
    std::array<jint, 256> ocean_floor{};
    std::vector<std::vector<std::uint16_t>> post_processing{};

    [[nodiscard]] auto at(jint x, jint y, jint z) const -> std::uint16_t {
        return blocks[static_cast<std::size_t>(y - oracle_min_y) * 256U +
                      static_cast<std::size_t>(z) * 16U +
                      static_cast<std::size_t>(x)];
    }
};

/// Reads a packed heightmap (`SimpleBitStorage`, 256 entries of
/// `ceil_log2(height + 1)` bits) into absolute first-available heights.
auto unpack_heightmap(std::span<const std::int64_t> packed)
    -> std::optional<std::array<jint, 256>> {
    const int bits = ceil_log2(oracle_height + 1);
    const auto per_long = static_cast<std::size_t>(64 / bits);
    const auto mask = (std::uint64_t{1} << bits) - 1U;
    std::array<jint, 256> heights{};
    for (std::size_t i = 0; i < heights.size(); ++i) {
        if (i / per_long >= packed.size()) {
            return std::nullopt;
        }
        const auto raw = static_cast<std::uint64_t>(packed[i / per_long]) >>
                         ((i % per_long) * static_cast<std::size_t>(bits));
        heights[i] = static_cast<jint>(raw & mask) + oracle_min_y;
    }
    return heights;
}

/// Decodes the parts of a chunk document the oracle compares: status, block
/// array and the two client heightmaps.
auto decode_chunk(const data::nbt::value &root, jint chunk_x, jint chunk_z)
    -> std::optional<vanilla_chunk> {
    const auto *status = root.get("Status");
    const auto *x_pos = root.get("xPos");
    const auto *z_pos = root.get("zPos");
    const auto *version = root.get("DataVersion");
    if (status == nullptr || x_pos == nullptr || z_pos == nullptr ||
        version == nullptr) {
        return std::nullopt;
    }
    const auto status_name = status->as_string();
    const auto x = x_pos->as_int();
    const auto z = z_pos->as_int();
    const auto data_version = version->as_int();
    if (!status_name.has_value() || *status_name != "minecraft:full" ||
        !x.has_value() || *x != chunk_x || !z.has_value() || *z != chunk_z ||
        !data_version.has_value() || *data_version != 5023) {
        return std::nullopt;
    }

    vanilla_chunk chunk;
    chunk.blocks.assign(static_cast<std::size_t>(oracle_height) * 256U, id_air);

    const auto *sections_value = root.get("sections");
    const auto *sections =
        sections_value == nullptr ? nullptr : sections_value->as_list();
    if (sections == nullptr) {
        return std::nullopt;
    }
    for (const auto &section_value : sections->items) {
        const auto *section = section_value.as_compound();
        if (section == nullptr) {
            return std::nullopt;
        }
        const auto *y_value = child(section, "Y");
        const auto *states_value = child(section, "block_states");
        if (y_value == nullptr || states_value == nullptr) {
            continue;
        }
        const auto local_y_base = int_value(*y_value);
        const auto *states = states_value->as_compound();
        if (!local_y_base.has_value() || states == nullptr) {
            return std::nullopt;
        }
        const auto *palette_value = child(states, "palette");
        const auto *palette =
            palette_value == nullptr ? nullptr : palette_value->as_list();
        if (palette == nullptr || palette->items.empty()) {
            return std::nullopt;
        }
        std::vector<std::uint16_t> palette_ids;
        palette_ids.reserve(palette->items.size());
        for (const auto &entry : palette->items) {
            const auto name = entry.as_string();
            if (!name.has_value()) {
                return std::nullopt;
            }
            const auto id = block_id(*name);
            if (!id.has_value()) {
                std::fprintf(stderr, "unexpected block state %.*s\n",
                             static_cast<int>(name->size()), name->data());
                return std::nullopt;
            }
            palette_ids.push_back(*id);
        }

        std::array<std::uint16_t, 4096> ids{};
        if (palette_ids.size() == 1) {
            ids.fill(palette_ids[0]);
        } else {
            const auto *data_value = child(states, "data");
            const auto data = data_value == nullptr
                                  ? std::nullopt
                                  : data_value->as_long_array();
            if (!data.has_value()) {
                return std::nullopt;
            }
            const int bits = std::max(4, ceil_log2(palette_ids.size()));
            const auto per_long = static_cast<std::size_t>(64 / bits);
            const auto mask = (std::uint64_t{1} << bits) - 1U;
            for (std::size_t i = 0; i < ids.size(); ++i) {
                if (i / per_long >= data->size()) {
                    return std::nullopt;
                }
                const auto raw =
                    static_cast<std::uint64_t>((*data)[i / per_long]) >>
                    ((i % per_long) * static_cast<std::size_t>(bits));
                const auto palette_index = raw & mask;
                if (palette_index >= palette_ids.size()) {
                    return std::nullopt;
                }
                ids[i] = palette_ids[palette_index];
            }
        }

        for (std::size_t i = 0; i < ids.size(); ++i) {
            const auto local_y = static_cast<jint>(i / 256U);
            const auto block_y = *local_y_base * 16 + local_y;
            if (block_y < oracle_min_y ||
                block_y >= oracle_min_y + oracle_height) {
                return std::nullopt;
            }
            const auto x_local = static_cast<jint>(i % 16U);
            const auto z_local = static_cast<jint>((i % 256U) / 16U);
            chunk.blocks[static_cast<std::size_t>(block_y - oracle_min_y) *
                             256U +
                         static_cast<std::size_t>(z_local) * 16U +
                         static_cast<std::size_t>(x_local)] = ids[i];
        }
    }

    const auto *heightmaps_value = root.get("Heightmaps");
    const auto *heightmaps =
        heightmaps_value == nullptr ? nullptr : heightmaps_value->as_compound();
    if (heightmaps == nullptr) {
        return std::nullopt;
    }
    const auto *surface_value = child(heightmaps, "WORLD_SURFACE");
    const auto *floor_value = child(heightmaps, "OCEAN_FLOOR");
    if (surface_value == nullptr || floor_value == nullptr) {
        return std::nullopt;
    }
    const auto surface = surface_value->as_long_array();
    const auto floor = floor_value->as_long_array();
    if (!surface.has_value() || !floor.has_value()) {
        return std::nullopt;
    }
    const auto surface_heights = unpack_heightmap(*surface);
    const auto floor_heights = unpack_heightmap(*floor);
    if (!surface_heights.has_value() || !floor_heights.has_value()) {
        return std::nullopt;
    }
    chunk.world_surface = *surface_heights;
    chunk.ocean_floor = *floor_heights;
    const auto post_processing = decode_post_processing(root);
    if (!post_processing) {
        return std::nullopt;
    }
    chunk.post_processing = *post_processing;
    return chunk;
}

} // namespace

suite<"worldgen/chunk-oracle"> chunk_oracle_tests = [] {
    "reference chunks match the fill stage block for block"_test = [] {
        const char *data_root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
        const char *region_path = std::getenv("REFRAIN_VANILLA_REGION");
        if (data_root == nullptr || region_path == nullptr) {
            return;
        }

        jlong seed = oracle_seed;
        if (const char *raw = std::getenv("REFRAIN_VANILLA_ORACLE_SEED");
            raw != nullptr) {
            seed = std::strtoll(raw, nullptr, 10);
        }
        const auto region = core::io::read_file(region_path);
        expect(region.has_value()) << region.error().message;
        if (!region) {
            return;
        }
        auto settings = density::load_noise_settings(
            data_root, "minecraft/overworld", algorithm::xoroshiro, seed);
        expect(settings.has_value()) << settings.error().message;
        if (!settings) {
            return;
        }

        const terrain::block_ids blocks{.air = id_air,
                                        .default_block = id_stone,
                                        .default_fluid = id_water,
                                        .lava = id_lava};
        const terrain::fluid_picker picker{.sea_level = settings->sea_level,
                                           .default_fluid = id_water,
                                           .lava = id_lava,
                                           .air = id_air};
        const terrain::fill_options options{.min_y = oracle_min_y,
                                            .height = oracle_height,
                                            .sea_level = settings->sea_level,
                                            .blocks = blocks};
        auto random = new_positional_factory(algorithm::xoroshiro, seed);
        auto aquifer_random = random->from_hash_of("minecraft:aquifer");
        auto factory = aquifer_random->fork_positional();
        const density::sampler_context context;

        constexpr std::array<std::array<jint, 2>, 5> chunks{{
            {0, 0},
            {1, 0},
            {0, 1},
            {1, 1},
            {2, 2},
        }};
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
            const auto vanilla = decode_chunk(*document, chunk_x, chunk_z);
            expect(vanilla.has_value()) << "unexpected chunk document";
            if (!vanilla) {
                continue;
            }

            const volume chunk_volume{16,           oracle_height,
                                      16,           chunk_x * 16,
                                      oracle_min_y, chunk_z * 16};
            terrain::noise_aquifer aquifer{settings->aquifer,
                                           *factory,
                                           chunk_volume,
                                           picker,
                                           blocks,
                                           context};
            const auto chunk =
                terrain::fill_chunk(options, *settings->router.final_density,
                                    context, chunk_x, chunk_z, aquifer);

            std::size_t mismatches = 0;
            for (jint y = oracle_min_y; y < oracle_min_y + oracle_height; ++y) {
                for (jint z = 0; z < 16; ++z) {
                    for (jint x = 0; x < 16; ++x) {
                        if (chunk.at(x, y, z) == vanilla->at(x, y, z)) {
                            continue;
                        }
                        if (mismatches < 8) {
                            std::fprintf(
                                stderr,
                                "chunk (%d, %d) block mismatch at (%d, %d, "
                                "%d): "
                                "got %u, want %u\n",
                                static_cast<int>(chunk_x),
                                static_cast<int>(chunk_z), static_cast<int>(x),
                                static_cast<int>(y), static_cast<int>(z),
                                static_cast<unsigned>(chunk.at(x, y, z)),
                                static_cast<unsigned>(vanilla->at(x, y, z)));
                        }
                        ++mismatches;
                    }
                }
            }
            expect(mismatches == 0) << "chunk (" << chunk_x << ", " << chunk_z
                                    << ") block mismatches: " << mismatches;

            std::size_t heightmap_mismatches = 0;
            for (std::size_t i = 0; i < 256U; ++i) {
                const auto expected_surface = chunk.world_surface[i] + 1;
                const auto expected_floor = chunk.ocean_floor[i] + 1;
                if (vanilla->world_surface[i] != expected_surface ||
                    vanilla->ocean_floor[i] != expected_floor) {
                    ++heightmap_mismatches;
                }
            }
            expect(heightmap_mismatches == 0)
                << "chunk (" << chunk_x << ", " << chunk_z
                << ") heightmap mismatches: " << heightmap_mismatches;
        }
    };
};
