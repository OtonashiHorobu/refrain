// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Opt-in dimension oracle: generates nether and end chunks with the dimension
// generator and compares them with reference worlds that the reference server
// wrote for the same seed. The fixed-biome reference worlds strip the biome's
// carvers and features and keep the real material rule, so the saved chunk is
// exactly the fill and surface output. A multi-noise nether world keeps the
// normal biome source and carvers but strips features; that suite compares the
// carved terrain of the selected chunk.
//
// `REFRAIN_VANILLA_WORLDGEN` points at the datapack `data` root,
// `REFRAIN_VANILLA_NETHER_REGION`, `REFRAIN_VANILLA_END_REGION` and
// `REFRAIN_VANILLA_NETHER_MULTI_REGION` at region files under
// `world/dimensions/minecraft/<dimension>/region`, and
// `REFRAIN_VANILLA_ORACLE_SEED` at the world seed (default 1). The multi-noise
// suite additionally reads `REFRAIN_VANILLA_NETHER_CHUNK_X` and `_Z` and
// requires the seed variable.
// `tools/worldgen/oracle/generate.py --dimension the_nether` writes the fixed
// world; `--carvers --dimension the_nether` writes the multi-noise world.

#include <refrain/core/io.hpp>
#include <refrain/data/nbt.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/chunk/chunk_region.hpp>
#include <refrain/worldgen/chunk/proto_chunk.hpp>
#include <refrain/worldgen/dimension_generator.hpp>
#include <refrain/worldgen/terrain/surface.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace boost::ut;
using namespace refrain;
using namespace refrain::worldgen;
using namespace refrain::worldgen::surface;

constexpr std::uint16_t id_air = 0;

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

auto int_value(const data::nbt::value &nbt_tag) -> std::optional<jint> {
    if (const auto value = nbt_tag.as_int(); value.has_value()) {
        return *value;
    }
    if (const auto value = nbt_tag.as_short(); value.has_value()) {
        return *value;
    }
    if (const auto value = nbt_tag.as_byte(); value.has_value()) {
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

/// Palette keys carry the default properties of a state while material rules
/// name blocks without them. Drop the defaults the compared rules can write.
auto canonical_state_name(std::string_view input) -> std::string {
    const auto canon_sort = [](std::string text) -> std::string {
        const auto open = text.find('[');
        if (open == std::string::npos) {
            return text;
        }
        const auto close = text.rfind(']');
        if (close == std::string::npos || close < open) {
            return text;
        }
        std::vector<std::pair<std::string, std::string>> properties;
        std::string_view body{text.data() + open + 1, close - open - 1};
        while (!body.empty()) {
            const auto comma = body.find(',');
            const auto entry = body.substr(0, comma);
            const auto equals = entry.find('=');
            if (equals != std::string_view::npos) {
                properties.emplace_back(std::string{entry.substr(0, equals)},
                                        std::string{entry.substr(equals + 1)});
            }
            if (comma == std::string_view::npos) {
                break;
            }
            body = body.substr(comma + 1);
        }
        std::sort(properties.begin(), properties.end());
        std::string rebuilt = text.substr(0, open);
        rebuilt.push_back('[');
        for (std::size_t i = 0; i < properties.size(); ++i) {
            if (i != 0) {
                rebuilt.push_back(',');
            }
            rebuilt.append(properties[i].first);
            rebuilt.push_back('=');
            rebuilt.append(properties[i].second);
        }
        rebuilt.push_back(']');
        return rebuilt;
    };
    const std::string owned = canon_sort(std::string{input});
    std::string_view name{owned};

    if (name.size() > 8 && name.ends_with("[axis=y]")) {
        const auto base = name.substr(0, name.size() - 8);
        if (base.ends_with("_log") || base.ends_with("_stem") ||
            base.ends_with("_wood") || base == "minecraft:basalt" ||
            base == "minecraft:polished_basalt" ||
            base == "minecraft:bone_block") {
            return std::string{base};
        }
    }
    constexpr std::string_view fluid_default = "[level=0]";
    if (name.ends_with(fluid_default) &&
        (name.starts_with("minecraft:lava") ||
         name.starts_with("minecraft:water"))) {
        return std::string{name.substr(0, name.size() - fluid_default.size())};
    }
    return std::string{name};
}

/// Reads a packed heightmap (`SimpleBitStorage`, 256 entries of
/// `ceil_log2(level height + 1)` bits) into absolute first-available heights.
auto unpack_heightmap(std::span<const std::int64_t> packed, jint min_y,
                      jint level_height)
    -> std::optional<std::array<jint, 256>> {
    const int bits = ceil_log2(static_cast<std::size_t>(level_height) + 1U);
    const auto per_long = static_cast<std::size_t>(64 / bits);
    const auto mask = (std::uint64_t{1} << bits) - 1U;
    std::array<jint, 256> heights{};
    for (std::size_t i = 0; i < heights.size(); ++i) {
        if (i / per_long >= packed.size()) {
            return std::nullopt;
        }
        const auto raw = static_cast<std::uint64_t>(packed[i / per_long]) >>
                         ((i % per_long) * static_cast<std::size_t>(bits));
        heights[i] = static_cast<jint>(raw & mask) + min_y;
    }
    return heights;
}

struct reference_chunk {
    std::vector<state_id> blocks{};
    std::array<jint, 256> world_surface{};
    std::array<jint, 256> ocean_floor{};
    std::array<jint, 256> motion_blocking{};
    std::array<jint, 256> motion_blocking_no_leaves{};

    [[nodiscard]] auto at(jint x, jint y, jint z, jint min_y) const
        -> state_id {
        return blocks[static_cast<std::size_t>(y - min_y) * 256U +
                      static_cast<std::size_t>(z) * 16U +
                      static_cast<std::size_t>(x)];
    }
};

auto decode_reference(const data::nbt::value &root, block_states &states,
                      jint chunk_x, jint chunk_z, jint min_y, jint height)
    -> std::optional<reference_chunk> {
    const auto *status = root.get("Status");
    const auto *x_pos = root.get("xPos");
    const auto *z_pos = root.get("zPos");
    if (status == nullptr || x_pos == nullptr || z_pos == nullptr) {
        return std::nullopt;
    }
    const auto status_name = status->as_string();
    const auto x = x_pos->as_int();
    const auto z = z_pos->as_int();
    if (!status_name.has_value() || *status_name != "minecraft:full" ||
        !x.has_value() || *x != chunk_x || !z.has_value() || *z != chunk_z) {
        return std::nullopt;
    }

    reference_chunk chunk;
    chunk.blocks.assign(static_cast<std::size_t>(height) * 256U, id_air);
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
        const auto section_y = int_value(*y_value);
        const auto *states_compound = states_value->as_compound();
        const auto *palette_value = child(states_compound, "palette");
        const auto *palette =
            palette_value == nullptr ? nullptr : palette_value->as_list();
        if (!section_y.has_value() || palette == nullptr ||
            palette->items.empty()) {
            return std::nullopt;
        }
        std::vector<state_id> palette_ids;
        for (const auto &raw_entry : palette->items) {
            const data::nbt::value *entry = &raw_entry;
            // 26.3 wraps heterogeneous list elements as `{"": value}`.
            if (const auto *wrapper = raw_entry.as_compound();
                wrapper != nullptr && wrapper->size() == 1) {
                const auto found = wrapper->find("");
                if (found != wrapper->end()) {
                    entry = &found->second;
                }
            }
            std::string name;
            if (const auto text = entry->as_string()) {
                name = std::string{*text};
            } else if (const auto *compound = entry->as_compound()) {
                const auto *id = child(compound, "id");
                const auto id_text =
                    id == nullptr ? std::nullopt : id->as_string();
                if (!id_text) {
                    return std::nullopt;
                }
                name = std::string{*id_text};
                const auto *properties = child(compound, "properties");
                if (properties != nullptr) {
                    const auto *property_map = properties->as_compound();
                    if (property_map == nullptr) {
                        return std::nullopt;
                    }
                    std::vector<std::pair<std::string, std::string>> sorted;
                    for (const auto &[property, raw] : *property_map) {
                        const auto property_text = raw.as_string();
                        if (!property_text) {
                            return std::nullopt;
                        }
                        sorted.emplace_back(property, *property_text);
                    }
                    std::sort(sorted.begin(), sorted.end());
                    name.push_back('[');
                    for (std::size_t i = 0; i < sorted.size(); ++i) {
                        if (i != 0) {
                            name.push_back(',');
                        }
                        name.append(sorted[i].first);
                        name.push_back('=');
                        name.append(sorted[i].second);
                    }
                    name.push_back(']');
                }
            } else {
                return std::nullopt;
            }
            palette_ids.push_back(states.intern(name));
        }
        std::array<state_id, 4096> ids{};
        if (palette_ids.size() == 1) {
            ids.fill(palette_ids[0]);
        } else {
            const auto *data_value = child(states_compound, "data");
            const auto data = data_value == nullptr
                                  ? std::nullopt
                                  : data_value->as_long_array();
            if (!data) {
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
            const auto block_y = *section_y * 16 + local_y;
            if (block_y < min_y || block_y >= min_y + height) {
                continue;
            }
            const auto local_x = static_cast<jint>(i % 16U);
            const auto local_z = static_cast<jint>((i % 256U) / 16U);
            chunk.blocks[static_cast<std::size_t>(block_y - min_y) * 256U +
                         static_cast<std::size_t>(local_z) * 16U +
                         static_cast<std::size_t>(local_x)] = ids[i];
        }
    }

    const auto *heightmaps_value = root.get("Heightmaps");
    const auto *heightmaps =
        heightmaps_value == nullptr ? nullptr : heightmaps_value->as_compound();
    if (heightmaps == nullptr) {
        return std::nullopt;
    }
    constexpr std::array<std::pair<std::string_view, std::size_t>, 4> names{{
        {"WORLD_SURFACE", 0},
        {"OCEAN_FLOOR", 1},
        {"MOTION_BLOCKING", 2},
        {"MOTION_BLOCKING_NO_LEAVES", 3},
    }};
    std::array<std::array<jint, 256>, 4> maps{};
    for (const auto &[name, index] : names) {
        const auto *value = child(heightmaps, name);
        const auto packed =
            value == nullptr ? std::nullopt : value->as_long_array();
        if (!packed) {
            return std::nullopt;
        }
        const auto heights = unpack_heightmap(*packed, min_y, height);
        if (!heights) {
            return std::nullopt;
        }
        maps[index] = *heights;
    }
    chunk.world_surface = maps[0];
    chunk.ocean_floor = maps[1];
    chunk.motion_blocking = maps[2];
    chunk.motion_blocking_no_leaves = maps[3];
    return chunk;
}

struct oracle_inputs {
    const char *data_root{nullptr};
    const char *region_path{nullptr};
    jlong seed{1};
};

auto read_inputs(const char *region_variable) -> std::optional<oracle_inputs> {
    oracle_inputs inputs;
    inputs.data_root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
    inputs.region_path = std::getenv(region_variable);
    if (inputs.data_root == nullptr || inputs.region_path == nullptr) {
        return std::nullopt;
    }
    if (const char *raw = std::getenv("REFRAIN_VANILLA_ORACLE_SEED");
        raw != nullptr) {
        inputs.seed = std::strtoll(raw, nullptr, 10);
    }
    return inputs;
}

void compare(const oracle_inputs &inputs, dimension_kind kind,
             std::string_view fixed_biome,
             const std::vector<std::array<jint, 2>> &chunks,
             bool run_carvers = false) {
    auto created = dimension_generator::create(
        dimension_generator_config{.data_root = inputs.data_root,
                                   .kind = kind,
                                   .seed = inputs.seed,
                                   .fixed_biome = std::string{fixed_biome}});
    expect(created.has_value()) << (created ? "" : created.error().message);
    if (!created) {
        return;
    }
    dimension_generator &generator = **created;
    block_states states{generator.states()};
    const auto region = core::io::read_file(inputs.region_path);
    expect(region.has_value()) << (region ? "" : region.error().message);
    if (!region) {
        return;
    }

    std::size_t compared_chunks = 0;
    std::size_t compared_all = 0;
    std::size_t mismatches = 0;
    std::size_t map_mismatches = 0;
    for (const auto &coordinates : chunks) {
        const jint chunk_x = coordinates[0];
        const jint chunk_z = coordinates[1];
        const auto payload = chunk_payload(*region, chunk_x, chunk_z);
        if (!payload) {
            continue;
        }
        const auto document =
            data::nbt::read(*payload, data::nbt::compression::zlib);
        expect(document.has_value())
            << (document ? "" : document.error().message);
        if (!document) {
            continue;
        }
        const auto reference = decode_reference(
            *document, states, chunk_x, chunk_z, generator.profile().min_y,
            generator.profile().height);
        expect(reference.has_value()) << "cannot decode reference chunk";
        if (!reference) {
            continue;
        }
        // A fixed-biome reference world strips the biome carvers, so compare
        // the fill and surface stages only; the carver oracle uses a world
        // that keeps them.
        const auto generated = generator.generate_terrain(
            chunk_pos{.x = chunk_x, .z = chunk_z},
            terrain_stages{.carvers =
                               run_carvers ? generator.carvers() : nullptr});
        expect(generated.has_value())
            << (generated ? "" : generated.error().message);
        if (!generated) {
            continue;
        }
        const proto_chunk &chunk = **generated;
        ++compared_chunks;
        for (jint y = chunk.min_y(); y < chunk.min_y() + chunk.height(); ++y) {
            for (jint z = 0; z < 16; ++z) {
                for (jint x = 0; x < 16; ++x) {
                    ++compared_all;
                    const auto actual = states.name(chunk.state_at(x, y, z));
                    const auto expected = states.name(
                        reference->at(x, y, z, generator.profile().min_y));
                    if (canonical_state_name(actual) ==
                        canonical_state_name(expected)) {
                        continue;
                    }
                    if (mismatches < 12) {
                        std::fprintf(stderr,
                                     "chunk (%d,%d) block (%d,%d,%d): got %s "
                                     "want %s\n",
                                     static_cast<int>(chunk_x),
                                     static_cast<int>(chunk_z),
                                     static_cast<int>(x), static_cast<int>(y),
                                     static_cast<int>(z),
                                     std::string{actual}.c_str(),
                                     std::string{expected}.c_str());
                    }
                    ++mismatches;
                }
            }
        }

        constexpr std::array<std::pair<std::string_view, heightmap_kind>, 4>
            maps{{
                {"WORLD_SURFACE", heightmap_kind::world_surface},
                {"OCEAN_FLOOR", heightmap_kind::ocean_floor},
                {"MOTION_BLOCKING", heightmap_kind::motion_blocking},
                {"MOTION_BLOCKING_NO_LEAVES",
                 heightmap_kind::motion_blocking_no_leaves},
            }};
        std::array<const std::array<jint, 256> *, 4> expected_maps{
            &reference->world_surface, &reference->ocean_floor,
            &reference->motion_blocking, &reference->motion_blocking_no_leaves};
        for (std::size_t index = 0; index < maps.size(); ++index) {
            const auto &[name, map_kind] = maps[index];
            for (jint z = 0; z < 16; ++z) {
                for (jint x = 0; x < 16; ++x) {
                    const auto actual = chunk.first_available(map_kind, x, z);
                    const auto expected =
                        (*expected_maps[index])[static_cast<std::size_t>(
                            z * 16 + x)];
                    if (actual == expected) {
                        continue;
                    }
                    if (map_mismatches < 12) {
                        std::fprintf(stderr,
                                     "map %.*s chunk %d,%d %d,%d: got %d want "
                                     "%d\n",
                                     static_cast<int>(name.size()), name.data(),
                                     static_cast<int>(chunk_x),
                                     static_cast<int>(chunk_z),
                                     static_cast<int>(x), static_cast<int>(z),
                                     static_cast<int>(actual),
                                     static_cast<int>(expected));
                    }
                    ++map_mismatches;
                }
            }
        }
    }
    expect(compared_chunks > 0U) << "no reference chunk was compared";
    expect(compared_all == compared_chunks * 256U * 256U);
    expect(mismatches == 0) << "block mismatches: " << mismatches;
    expect(map_mismatches == 0) << "heightmap mismatches: " << map_mismatches;
}

} // namespace

using namespace boost::ut;

suite<"worldgen/dimension-oracle"> dimension_oracle_tests = [] {
    "nether chunks match the reference fill and surface"_test = [] {
        const auto inputs = read_inputs("REFRAIN_VANILLA_NETHER_REGION");
        if (!inputs) {
            return;
        }
        compare(*inputs, dimension_kind::nether, "minecraft:nether_wastes",
                {{0, 0}, {1, 0}, {0, 1}});
    };

    "end chunks match the reference fill and surface"_test = [] {
        const auto inputs = read_inputs("REFRAIN_VANILLA_END_REGION");
        if (!inputs) {
            return;
        }
        compare(*inputs, dimension_kind::end, "minecraft:the_end",
                {{0, 0}, {1, 1}});
    };

    // A nether world that keeps the normal multi-noise preset and the biome
    // carvers but strips features. The compared chunk is the full terrain
    // output of biome selection, fill, surface and carving; its biome source
    // samples the real climate noises.
    "nether multi-noise terrain matches the reference"_test = [] {
        const auto inputs = read_inputs("REFRAIN_VANILLA_NETHER_MULTI_REGION");
        if (!inputs) {
            return;
        }
        const char *raw_x = std::getenv("REFRAIN_VANILLA_NETHER_CHUNK_X");
        const char *raw_z = std::getenv("REFRAIN_VANILLA_NETHER_CHUNK_Z");
        const char *raw_seed = std::getenv("REFRAIN_VANILLA_ORACLE_SEED");
        if (raw_x == nullptr || raw_z == nullptr || raw_seed == nullptr) {
            return;
        }
        const jint chunk_x = static_cast<jint>(std::strtol(raw_x, nullptr, 10));
        const jint chunk_z = static_cast<jint>(std::strtol(raw_z, nullptr, 10));
        compare(*inputs, dimension_kind::nether, "", {{chunk_x, chunk_z}},
                true);
    };
};
