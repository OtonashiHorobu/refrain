// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Reference chunk decoding and comparison shared by the full-stage oracles.
// The reference worlds are written with an empty material rule and without
// carvers or features, so a compared chunk contains terrain fill and
// structure pieces only.

#pragma once

#include <refrain/data/nbt.hpp>
#include <refrain/worldgen/chunk/block_state_codec.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/chunk/proto_chunk.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::worldgen::test_support {

inline auto ceil_log2(std::size_t value) -> int {
    int bits = 0;
    std::size_t limit = 1;
    while (limit < value) {
        limit <<= 1U;
        ++bits;
    }
    return bits;
}

inline auto child(const data::nbt::compound *root, std::string_view key)
    -> const data::nbt::value * {
    if (root == nullptr) {
        return nullptr;
    }
    const auto found = root->find(key);
    return found == root->end() ? nullptr : &found->second;
}

inline auto chunk_payload(std::span<const std::byte> region, jint chunk_x,
                          jint chunk_z)
    -> std::optional<std::span<const std::byte>> {
    if (region.size() < 8192) {
        return std::nullopt;
    }
    const auto index =
        static_cast<std::size_t>((chunk_x & 31) + (chunk_z & 31) * 32);
    const auto *entry = region.data() + index * 4U;
    const auto offset = (static_cast<std::size_t>(entry[0]) << 16U) |
                        (static_cast<std::size_t>(entry[1]) << 8U) |
                        static_cast<std::size_t>(entry[2]);
    if (offset == 0) {
        return std::nullopt;
    }
    const auto start = offset * 4096U;
    if (start + 5U > region.size()) {
        return std::nullopt;
    }
    const auto length = (static_cast<std::size_t>(region[start]) << 24U) |
                        (static_cast<std::size_t>(region[start + 1]) << 16U) |
                        (static_cast<std::size_t>(region[start + 2]) << 8U) |
                        static_cast<std::size_t>(region[start + 3]);
    if (length < 1U || start + 4U + length > region.size()) {
        return std::nullopt;
    }
    if (static_cast<std::uint8_t>(region[start + 4U]) != 2U) {
        return std::nullopt;
    }
    return region.subspan(start + 5U, length - 1U);
}

struct decoded {
    block_state_registry registry{};
    std::vector<block_state_id> blocks{};
};

struct aquifer_holder {
    std::unique_ptr<positional_random_factory> factory{};
    std::unique_ptr<terrain::aquifer> aquifer{};
};

inline auto canonical(std::string name) -> std::string {
    const auto open = name.find('[');
    if (open == std::string::npos) {
        return name;
    }
    const auto close = name.rfind(']');
    if (close == std::string::npos) {
        return name;
    }
    const std::string base = name.substr(0, open);
    const bool fluid = base == "minecraft:lava" || base == "minecraft:water";
    const bool stairs = base.ends_with("_stairs");
    const bool slab = base.ends_with("_slab");
    const bool leaves = base.ends_with("_leaves");
    const bool bars = base == "minecraft:iron_bars";
    const bool fence = base.ends_with("_fence");
    const bool rail = base == "minecraft:rail" || base.ends_with("_rail");
    const bool snowy_block = base == "minecraft:grass_block" ||
                             base == "minecraft:mycelium" ||
                             base == "minecraft:podzol";
    const bool fence_gate = base.ends_with("_fence_gate");
    const bool pointed_dripstone = base == "minecraft:pointed_dripstone";
    std::vector<std::string> parts;
    std::string_view body{name.data() + open + 1, close - open - 1};
    while (true) {
        const auto comma = body.find(',');
        const std::string part{body.substr(0, comma)};
        const bool default_property =
            part == "waterlogged=false" || part == "hanging=false" ||
            (fluid && part == "level=0") ||
            (stairs && (part == "facing=north" || part == "half=bottom" ||
                        part == "shape=straight")) ||
            (slab && part == "type=bottom") ||
            (leaves && (part == "distance=7" || part == "persistent=false")) ||
            (bars && (part == "east=false" || part == "north=false" ||
                      part == "south=false" || part == "west=false")) ||
            (fence && (part == "east=false" || part == "north=false" ||
                       part == "south=false" || part == "west=false")) ||
            (rail && part == "shape=north_south") ||
            (snowy_block && part == "snowy=false") ||
            (fence_gate && (part == "facing=north" || part == "in_wall=false" ||
                            part == "open=false" || part == "powered=false")) ||
            (pointed_dripstone &&
             (part == "thickness=tip" || part == "vertical_direction=up"));
        if (!default_property) {
            parts.emplace_back(part);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        body = body.substr(comma + 1);
    }
    if (parts.empty()) {
        return base;
    }
    std::sort(parts.begin(), parts.end());
    std::string rebuilt = base;
    rebuilt.push_back('[');
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            rebuilt.push_back(',');
        }
        rebuilt.append(parts[i]);
    }
    rebuilt.push_back(']');
    if (rebuilt.find("_fence[") != std::string::npos) {
        const std::string fence_base = rebuilt.substr(0, rebuilt.find('['));
        const std::string water =
            rebuilt.find("waterlogged=true") != std::string::npos ? "true"
                                                                  : "false";
        return fence_base + "[waterlogged=" + water + "]";
    }
    if (rebuilt.starts_with("minecraft:chest[")) {
        return "minecraft:chest";
    }
    if (rebuilt == "minecraft:end_rod[facing=up]" ||
        rebuilt == "minecraft:ender_chest[facing=north]" ||
        rebuilt == "minecraft:ladder[facing=north]") {
        return base;
    }
    if (rebuilt == base + "[axis=y]" ||
        rebuilt.find("_door[") != std::string::npos) {
        return base;
    }
    return rebuilt;
}

/// How many block mismatches a comparison prints. The default keeps failures
/// readable; a larger value lists every differing position of a small diff.
inline auto mismatch_print_limit() -> std::size_t {
    const char *value = std::getenv("REFRAIN_FULL_STAGE_MISMATCHES");
    if (value == nullptr) {
        return 10;
    }
    const auto parsed = std::strtoul(value, nullptr, 10);
    return parsed == 0 ? 10 : parsed;
}

inline auto decode(const data::nbt::value &document,
                   const std::set<std::string> &motion,
                   const std::set<std::string> &motion_no_leaves, jint min_y,
                   jint height) -> std::optional<decoded> {
    const auto *sections = child(document.as_compound(), "sections");
    if (sections == nullptr || sections->as_list() == nullptr) {
        return std::nullopt;
    }
    decoded out;
    out.blocks.assign(static_cast<std::size_t>(height) * 256U,
                      out.registry.air());
    for (const auto &section_value : sections->as_list()->items) {
        const auto *section = section_value.as_compound();
        const auto *y_value = child(section, "Y");
        const auto *states_value = child(section, "block_states");
        if (y_value == nullptr || states_value == nullptr) {
            continue;
        }
        auto section_y = y_value->as_byte();
        if (!section_y) {
            section_y = y_value->as_short();
        }
        if (!section_y) {
            section_y = y_value->as_int();
        }
        const auto *states = states_value->as_compound();
        const auto *palette_value = child(states, "palette");
        const auto *palette =
            palette_value == nullptr ? nullptr : palette_value->as_list();
        if (!section_y || palette == nullptr || palette->items.empty()) {
            continue;
        }
        std::vector<block_state_id> ids;
        for (const auto &entry : palette->items) {
            std::string name;
            const auto text = entry.as_string();
            if (text) {
                name = *text;
            } else if (const auto *compound = entry.as_compound()) {
                if (compound->size() == 1) {
                    const auto unnamed = compound->find("");
                    if (unnamed != compound->end()) {
                        name = unnamed->second.as_string().value_or("");
                    }
                }
                const auto *id = child(compound, "id");
                const auto *legacy = child(compound, "Name");
                const auto *name_value = id != nullptr ? id : legacy;
                if (name.empty() && name_value != nullptr) {
                    name = name_value->as_string().value_or("minecraft:air");
                }
                if (name.empty()) {
                    continue;
                }
                const auto *properties = child(compound, "properties");
                if (properties == nullptr) {
                    properties = child(compound, "Properties");
                }
                if (properties != nullptr &&
                    properties->as_compound() != nullptr) {
                    std::vector<std::pair<std::string, std::string>> pairs;
                    for (const auto &[property, value] :
                         *properties->as_compound()) {
                        pairs.emplace_back(property,
                                           value.as_string().value_or(""));
                    }
                    std::sort(pairs.begin(), pairs.end());
                    name.push_back('[');
                    for (std::size_t i = 0; i < pairs.size(); ++i) {
                        if (i != 0) {
                            name.push_back(',');
                        }
                        name.append(pairs[i].first);
                        name.push_back('=');
                        name.append(pairs[i].second);
                    }
                    name.push_back(']');
                }
            }
            const auto block = state_block_name(name);
            const bool air = is_air_block_name(block);
            const bool water =
                block == "minecraft:water" ||
                name.find("waterlogged=true") != std::string::npos;
            const bool fluid = water || block == "minecraft:lava" ||
                               block == "minecraft:bubble_column";
            const auto interned = out.registry.intern(
                name, {.air = air,
                       .fluid = fluid,
                       .water = water,
                       .blocks_motion = motion.contains(std::string{block}),
                       .blocks_motion_no_leaves =
                           motion_no_leaves.contains(std::string{block})});
            if (!interned) {
                return std::nullopt;
            }
            ids.push_back(*interned);
        }
        std::array<block_state_id, 4096> section_ids{};
        if (ids.size() == 1) {
            section_ids.fill(ids[0]);
        } else {
            const auto *data_value = child(states, "data");
            const auto data = data_value == nullptr
                                  ? std::nullopt
                                  : data_value->as_long_array();
            if (!data) {
                return std::nullopt;
            }
            const int bits =
                std::max(4, static_cast<int>(std::bit_width(
                                std::max<std::size_t>(1U, ids.size() - 1U))));
            const auto per_long = static_cast<std::size_t>(64 / bits);
            const auto mask = (std::uint64_t{1} << bits) - 1U;
            for (std::size_t i = 0; i < section_ids.size(); ++i) {
                if (i / per_long >= data->size()) {
                    return std::nullopt;
                }
                const auto raw =
                    static_cast<std::uint64_t>((*data)[i / per_long]) >>
                    ((i % per_long) * static_cast<std::size_t>(bits));
                section_ids[i] = ids[raw & mask];
            }
        }
        for (std::size_t i = 0; i < section_ids.size(); ++i) {
            const auto block_y = static_cast<jint>(*section_y) * 16 +
                                 static_cast<jint>(i / 256U);
            const auto local_y = block_y - min_y;
            if (local_y < 0 || local_y >= height) {
                continue;
            }
            const auto local_x = static_cast<jint>(i % 16U);
            const auto local_z = static_cast<jint>((i % 256U) / 16U);
            out.blocks[static_cast<std::size_t>(local_y) * 256U +
                       static_cast<std::size_t>(local_z) * 16U +
                       static_cast<std::size_t>(local_x)] = section_ids[i];
        }
    }
    return out;
}

inline auto reference_heightmap(const data::nbt::value &document,
                                std::string_view key, std::size_t index,
                                jint min_y, jint level_height)
    -> std::optional<jint> {
    const auto *heightmaps = child(document.as_compound(), "Heightmaps");
    if (heightmaps == nullptr) {
        return std::nullopt;
    }
    const auto *encoded = child(heightmaps->as_compound(), key);
    if (encoded == nullptr) {
        return std::nullopt;
    }
    const auto data = encoded->as_long_array();
    if (!data) {
        return std::nullopt;
    }
    const int bits = ceil_log2(static_cast<std::size_t>(level_height) + 1U);
    const auto per_long = static_cast<std::size_t>(64 / bits);
    const std::size_t word = index / per_long;
    const std::size_t offset =
        (index % per_long) * static_cast<std::size_t>(bits);
    if (word >= data->size()) {
        return std::nullopt;
    }
    const auto mask = (std::uint64_t{1} << bits) - 1U;
    const auto value =
        (static_cast<std::uint64_t>((*data)[word]) >> offset) & mask;
    return static_cast<jint>(value) + min_y;
}

struct chunk_comparison {
    std::size_t block_mismatches{0};
    jint heightmap_mismatches{0};
};

/// Compares a generated chunk's block names and four final heightmaps with a
/// decoded reference chunk. Prints the first mismatches to stderr.
[[nodiscard]] inline auto
compare_chunks(const proto_chunk &actual, const decoded &reference,
               const data::nbt::value &document, jint min_y, jint level_height)
    -> chunk_comparison {
    chunk_comparison result;
    for (jint y = min_y; y < min_y + level_height; ++y) {
        for (jint z = 0; z < 16; ++z) {
            for (jint x = 0; x < 16; ++x) {
                const auto index = static_cast<std::size_t>(y - min_y) * 256U +
                                   static_cast<std::size_t>(z) * 16U +
                                   static_cast<std::size_t>(x);
                const auto expected = canonical(std::string{
                    reference.registry.name(reference.blocks[index])});
                const auto state = actual.state_at(x, y, z);
                const auto state_name = actual.table().name(state);
                const auto got = canonical(std::string{state_name});
                if (got == expected) {
                    continue;
                }
                if (result.block_mismatches < mismatch_print_limit()) {
                    std::fprintf(
                        stderr,
                        "full-stage mismatch (%d,%d,%d): got %s want %s\n",
                        static_cast<int>(x), static_cast<int>(y),
                        static_cast<int>(z), got.c_str(), expected.c_str());
                }
                ++result.block_mismatches;
            }
        }
    }
    for (const auto *kind_name :
         {"WORLD_SURFACE", "OCEAN_FLOOR", "MOTION_BLOCKING",
          "MOTION_BLOCKING_NO_LEAVES"}) {
        const auto kind = std::string_view{kind_name} == "WORLD_SURFACE"
                              ? heightmap_kind::world_surface
                          : std::string_view{kind_name} == "OCEAN_FLOOR"
                              ? heightmap_kind::ocean_floor
                          : std::string_view{kind_name} == "MOTION_BLOCKING"
                              ? heightmap_kind::motion_blocking
                              : heightmap_kind::motion_blocking_no_leaves;
        for (std::size_t index = 0; index < 256U; ++index) {
            const auto expected = reference_heightmap(
                document, kind_name, index, min_y, level_height);
            if (!expected.has_value()) {
                continue;
            }
            const auto x = static_cast<jint>(index % 16U);
            const auto z = static_cast<jint>(index / 16U);
            const auto got = actual.first_available(kind, x, z);
            if (got == *expected) {
                continue;
            }
            if (result.heightmap_mismatches <
                static_cast<jint>(mismatch_print_limit())) {
                std::fprintf(stderr, "heightmap %s (%d,%d): got %d want %d\n",
                             kind_name, static_cast<int>(x),
                             static_cast<int>(z), static_cast<int>(got),
                             static_cast<int>(*expected));
            }
            ++result.heightmap_mismatches;
        }
    }
    return result;
}

} // namespace refrain::worldgen::test_support
