// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/nbt.hpp>
#include <refrain/worldgen/chunk/block_state_codec.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/core/rng.hpp>
#include <refrain/worldgen/feature/feature.hpp>
#include <refrain/worldgen/structure/generation.hpp>
#include <refrain/worldgen/structure/placement_stage.hpp>
#include <refrain/worldgen/structure/template.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace {

using namespace refrain;
using namespace refrain::worldgen;

auto child(const data::nbt::compound *entries, std::string_view key)
    -> const data::nbt::value * {
    if (entries == nullptr) {
        return nullptr;
    }
    const auto found = entries->find(key);
    return found == entries->end() ? nullptr : &found->second;
}

auto read_chunk(std::string_view path, jint chunk_x, jint chunk_z)
    -> std::optional<data::nbt::value> {
    std::ifstream stream{std::string{path}, std::ios::binary};
    if (!stream) {
        return std::nullopt;
    }
    std::vector<char> raw((std::istreambuf_iterator<char>(stream)),
                          std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    const auto local_x = static_cast<std::size_t>(((chunk_x % 32) + 32) % 32);
    const auto local_z = static_cast<std::size_t>(((chunk_z % 32) + 32) % 32);
    const auto index = local_x + local_z * 32;
    if ((index + 1) * 4 > bytes.size()) {
        return std::nullopt;
    }
    const auto read_u32 = [&](std::size_t offset) {
        return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
               (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
               (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) |
               static_cast<std::uint32_t>(bytes[offset + 3]);
    };
    const auto offset = static_cast<std::size_t>(read_u32(index * 4) >> 8U);
    if (offset == 0 || offset * 4096 + 5 > bytes.size()) {
        return std::nullopt;
    }
    const auto length = read_u32(offset * 4096);
    if (length < 1 || offset * 4096 + 4 + length > bytes.size()) {
        return std::nullopt;
    }
    if (std::to_integer<int>(bytes[offset * 4096 + 4]) != 2) {
        return std::nullopt;
    }
    const auto payload =
        std::span{bytes.data() + offset * 4096 + 5, length - 1};
    auto parsed = data::nbt::read(payload, data::nbt::compression::zlib);
    if (!parsed) {
        return std::nullopt;
    }
    return std::move(*parsed);
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

auto ceil_log2(std::size_t value) -> int {
    int bits = 0;
    std::size_t limit = 1;
    while (limit < value) {
        limit <<= 1U;
        ++bits;
    }
    return bits;
}

auto canonical_key(std::string_view name) -> std::string {
    const auto open = name.find('[');
    if (open == std::string_view::npos) {
        return std::string{name};
    }
    const auto close = name.rfind(']');
    if (close == std::string_view::npos || close < open) {
        return std::string{name};
    }
    std::vector<std::pair<std::string, std::string>> properties;
    std::string_view body = name.substr(open + 1, close - open - 1);
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
    std::string key{name.substr(0, open)};
    key.push_back('[');
    for (std::size_t i = 0; i < properties.size(); ++i) {
        if (i != 0) {
            key.push_back(',');
        }
        key.append(properties[i].first);
        key.push_back('=');
        key.append(properties[i].second);
    }
    key.push_back(']');
    return key;
}

struct decoded_chunk {
    block_state_registry registry{};
    std::vector<block_state_id> blocks{};
    std::array<jint, 256> surface_wg{};
};

auto decode_chunk(const data::nbt::value &root, decoded_chunk &out) -> bool {
    const auto *sections = child(root.as_compound(), "sections");
    if (sections == nullptr || sections->as_list() == nullptr) {
        return false;
    }
    out.blocks.assign(static_cast<std::size_t>(384 * 256), out.registry.air());
    for (const auto &section_value : sections->as_list()->items) {
        const auto *section = section_value.as_compound();
        const auto *y_value = child(section, "Y");
        const auto *states_value = child(section, "block_states");
        if (y_value == nullptr || states_value == nullptr) {
            continue;
        }
        const auto section_y = int_value(*y_value);
        const auto *states = states_value->as_compound();
        const auto *palette_value = child(states, "palette");
        const auto *palette =
            palette_value == nullptr ? nullptr : palette_value->as_list();
        if (!section_y || palette == nullptr || palette->items.empty()) {
            continue;
        }
        std::vector<block_state_id> palette_ids;
        for (const auto &raw_entry : palette->items) {
            const data::nbt::value *entry = &raw_entry;
            if (const auto *wrapper = raw_entry.as_compound();
                wrapper != nullptr && wrapper->size() == 1) {
                const auto found = wrapper->find("");
                if (found != wrapper->end()) {
                    entry = &found->second;
                }
            }
            std::string name;
            if (const auto text = entry->as_string()) {
                name = canonical_key(*text);
            } else if (const auto *compound = entry->as_compound()) {
                const auto *id = child(compound, "id");
                const auto id_text =
                    id == nullptr ? std::nullopt : id->as_string();
                if (!id_text) {
                    return false;
                }
                name = std::string{*id_text};
                const auto *properties = child(compound, "properties");
                if (properties != nullptr &&
                    properties->as_compound() != nullptr) {
                    for (const auto &[property, raw] :
                         *properties->as_compound()) {
                        const auto property_text = raw.as_string();
                        if (!property_text) {
                            return false;
                        }
                        name.push_back(
                            name.find('[') == std::string::npos ? '[' : ',');
                        name.append(property);
                        name.push_back('=');
                        name.append(*property_text);
                    }
                    name.push_back(']');
                    name = canonical_key(name);
                }
            } else {
                return false;
            }
            const auto block_name = state_block_name(name);
            const bool air = is_air_block_name(block_name);
            const bool fluid = name.starts_with("minecraft:water") ||
                               name.starts_with("minecraft:lava");
            const auto interned = out.registry.intern(
                name, {.air = air,
                       .fluid = fluid,
                       .water = name.starts_with("minecraft:water"),
                       .blocks_motion = !air && !fluid});
            if (!interned) {
                return false;
            }
            palette_ids.push_back(*interned);
        }
        std::array<block_state_id, 4096> ids{};
        if (palette_ids.size() == 1) {
            ids.fill(palette_ids.front());
        } else {
            const auto *data_value = child(states, "data");
            const auto data = data_value == nullptr
                                  ? std::nullopt
                                  : data_value->as_long_array();
            if (!data) {
                return false;
            }
            const int bits = std::max(4, ceil_log2(palette_ids.size()));
            const auto per_long = static_cast<std::size_t>(64 / bits);
            const auto mask = (std::uint64_t{1} << bits) - 1U;
            for (std::size_t i = 0; i < ids.size(); ++i) {
                const auto word = i / per_long;
                if (word >= data->size()) {
                    return false;
                }
                const auto raw =
                    static_cast<std::uint64_t>((*data)[word]) >>
                    ((i % per_long) * static_cast<std::size_t>(bits));
                const auto index = raw & mask;
                if (index >= palette_ids.size()) {
                    return false;
                }
                ids[i] = palette_ids[index];
            }
        }
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const auto local_y = static_cast<jint>(i / 256U);
            const auto local_z = static_cast<jint>((i % 256U) / 16U);
            const auto local_x = static_cast<jint>(i % 16U);
            const jint y = *section_y * 16 + local_y;
            if (y < -64 || y >= 320) {
                continue;
            }
            out.blocks[static_cast<std::size_t>(y + 64) * 256U +
                       static_cast<std::size_t>(local_z * 16 + local_x)] =
                ids[i];
        }
    }

    const auto *heightmaps = child(root.as_compound(), "Heightmaps");
    const auto *surface =
        heightmaps == nullptr
            ? nullptr
            : child(heightmaps->as_compound(), "WORLD_SURFACE");
    const auto packed =
        surface == nullptr ? std::nullopt : surface->as_long_array();
    if (!packed) {
        return false;
    }
    for (std::size_t i = 0; i < out.surface_wg.size(); ++i) {
        const auto word = i / 7;
        const auto shift = (i % 7) * 9;
        if (word >= packed->size()) {
            return false;
        }
        out.surface_wg[i] = static_cast<jint>(
            (static_cast<std::uint64_t>((*packed)[word]) >> shift) & 0x1FFU);
    }
    return true;
}

auto canonical_state(std::string name) -> std::string {
    // The chunk palette elides properties that equal the block's default
    // state; keep the comparison on the semantic state.
    if (name == "minecraft:redstone_torch[lit=true]") {
        return "minecraft:redstone_torch";
    }
    if (name == "minecraft:red_bed[facing=north,occupied=false,part=foot]") {
        return "minecraft:red_bed";
    }
    return name;
}

} // namespace

using namespace boost::ut;

suite<"worldgen/igloo-blocks"> igloo_blocks_tests = [] {
    "the igloo placement writes the reference blocks"_test = [] {
        if (std::getenv("REFRAIN_VANILLA_IGLOO_REAL") == nullptr) {
            return;
        }
        const char *region = std::getenv("REFRAIN_VANILLA_IGLOO_REGION");
        const char *root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
        const char *templates_root = std::getenv("REFRAIN_VANILLA_STRUCTURES");
        if (region == nullptr || root == nullptr || templates_root == nullptr) {
            return;
        }
        jint chunk_x = 0;
        jint chunk_z = 10;
        if (const char *value = std::getenv("REFRAIN_VANILLA_IGLOO_CHUNK_X");
            value != nullptr) {
            chunk_x = std::atoi(value);
        }
        if (const char *value = std::getenv("REFRAIN_VANILLA_IGLOO_CHUNK_Z");
            value != nullptr) {
            chunk_z = std::atoi(value);
        }
        jlong seed = 1;
        if (const char *value = std::getenv("REFRAIN_VANILLA_IGLOO_SEED");
            value != nullptr) {
            seed = std::strtoll(value, nullptr, 10);
        }

        const auto reference = read_chunk(region, chunk_x, chunk_z);
        expect(reference.has_value());
        if (!reference) {
            return;
        }
        decoded_chunk decoded;
        expect(decode_chunk(*reference, decoded));
        if (decoded.blocks.empty()) {
            return;
        }

        proto_chunk chunk{decoded.registry, chunk_x, chunk_z, -64, 384};
        for (jint y = -64; y < 320; ++y) {
            for (jint z = 0; z < 16; ++z) {
                for (jint x = 0; x < 16; ++x) {
                    const auto index = static_cast<std::size_t>(y + 64) * 256U +
                                       static_cast<std::size_t>(z * 16 + x);
                    chunk.set_state(x, y, z, decoded.blocks[index]);
                }
            }
        }

        structure::structure_template_manager manager{templates_root,
                                                      decoded.registry};
        const auto start = structure::igloo_start(
            chunk_pos{.x = chunk_x, .z = chunk_z}, seed, manager);
        expect(start.pieces.size() == 1U);
        if (start.pieces.empty()) {
            return;
        }
        expect(start.pieces.front().template_id == "minecraft:igloo/top");
        expect(start.pieces.front().template_rotation == 2);

        structure_start record{
            .structure = "minecraft:igloo",
            .step = "surface_structures",
            .chunk_x = chunk_x,
            .chunk_z = chunk_z,
            .min_x = chunk_x * 16,
            .min_y = start.bounds.min_y,
            .min_z = chunk_z * 16,
            .max_x = chunk_x * 16 + 15,
            .max_y = start.bounds.max_y,
            .max_z = chunk_z * 16 + 15,
        };
        for (const auto &piece : start.pieces) {
            record.pieces.push_back(structure_piece_record{
                .type = piece.type,
                .min_x = piece.bounds.min_x,
                .min_y = piece.bounds.min_y,
                .min_z = piece.bounds.min_z,
                .max_x = piece.bounds.max_x,
                .max_y = piece.bounds.max_y,
                .max_z = piece.bounds.max_z,
                .orientation = piece.orientation,
                .template_id = piece.template_id,
                .template_x = piece.template_origin.x,
                .template_y = piece.template_origin.y,
                .template_z = piece.template_origin.z,
                .template_rotation = piece.template_rotation,
                .template_mirror = piece.template_mirror,
                .template_pivot_x = piece.template_pivot.x,
                .template_pivot_y = piece.template_pivot.y,
                .template_pivot_z = piece.template_pivot.z,
            });
        }
        chunk.add_structure_start(std::move(record));
        chunk.add_structure_reference("minecraft:igloo", chunk_x, chunk_z);

        const auto &piece = start.pieces.front();
        const auto &templ = manager.get_or_create(piece.template_id);
        expect(!templ.palettes.empty() && !templ.palettes.front().empty());
        if (templ.palettes.empty() || templ.palettes.front().empty()) {
            return;
        }
        const auto transform = structure::template_transform{
            .mirror = structure::template_mirror::none,
            .rotation = static_cast<structure::template_rotation>(
                piece.template_rotation),
            .pivot = piece.template_pivot,
        };
        // The saved piece box is the reference's postProcess result; its
        // y offset is the shift the placement has to apply.
        const auto *structures_value =
            child(reference->as_compound(), "structures");
        const auto *starts_value =
            structures_value == nullptr
                ? nullptr
                : child(structures_value->as_compound(), "starts");
        const auto *igloo_value =
            starts_value == nullptr
                ? nullptr
                : child(starts_value->as_compound(), "minecraft:igloo");
        const auto *children_value =
            igloo_value == nullptr
                ? nullptr
                : child(igloo_value->as_compound(), "Children");
        const auto *children_list =
            children_value == nullptr ? nullptr : children_value->as_list();
        if (children_list == nullptr || children_list->items.empty()) {
            return;
        }
        const auto *child_compound = children_list->items.front().as_compound();
        const auto *bb = child(child_compound, "BB");
        const auto bb_values =
            bb == nullptr ? std::nullopt : bb->as_int_array();
        if (!bb_values || bb_values->size() != 6U) {
            return;
        }
        const jint shift = (*bb_values)[1] - piece.bounds.min_y;
        const auto entrance_local = structure::transform_position(
            block_pos{.x = 3, .y = 0, .z = 0}, transform);
        std::vector<block_pos> positions;
        for (const auto &block : templ.palettes.front()) {
            const auto local = structure::transform_position(
                block_pos{.x = block.x, .y = block.y, .z = block.z}, transform);
            positions.push_back(
                block_pos{.x = piece.template_origin.x + local.x,
                          .y = piece.template_origin.y + shift + local.y,
                          .z = piece.template_origin.z + local.z});
        }
        for (const auto &position : positions) {
            (void)chunk.set_state(position.x & 15, position.y, position.z & 15,
                                  decoded.registry.air());
        }

        chunk.heightmap_at(heightmap_kind::world_surface_wg)
            .set_first_available(
                (piece.template_origin.x + entrance_local.x) & 15,
                (piece.template_origin.z + entrance_local.z) & 15,
                shift + 90 + 1);
        chunk_region region_view{chunk, 0};
        const auto resolve = [&](std::string_view key) -> block_state_id {
            if (const auto found = decoded.registry.find(key);
                found.has_value()) {
                return *found;
            }
            const auto block = state_block_name(key);
            const bool air = is_air_block_name(block);
            const auto inserted = decoded.registry.intern(
                key, {.air = air, .blocks_motion = !air});
            return inserted.has_value() ? *inserted : no_block_state;
        };
        const structure::structure_place_context context{
            .region = &region_view,
            .chunk = &chunk,
            .states = &decoded.registry,
            .state = resolve,
            .templates = &manager,
            .registry = &decoded.registry,
        };
        auto random =
            worldgen_random{std::make_unique<legacy_random_source>(7)};
        structure::place_structures(chunk, context, "surface_structures",
                                    random);

        jint mismatches = 0;
        for (const auto &position : positions) {
            const auto index =
                static_cast<std::size_t>(position.y + 64) * 256U +
                static_cast<std::size_t>((position.z & 15) * 16 +
                                         (position.x & 15));
            const auto expected = decoded.blocks[index];
            const auto actual =
                chunk.state_at(position.x & 15, position.y, position.z & 15);
            const auto actual_name =
                canonical_state(std::string{decoded.registry.name(actual)});
            const auto expected_name =
                canonical_state(std::string{decoded.registry.name(expected)});
            if (actual_name != expected_name) {
                if (mismatches < 8) {
                    std::fprintf(stderr,
                                 "igloo mismatch (%d,%d,%d): got %s want %s\n",
                                 static_cast<int>(position.x),
                                 static_cast<int>(position.y),
                                 static_cast<int>(position.z),
                                 actual_name.c_str(), expected_name.c_str());
                }
                ++mismatches;
            }
        }
        expect(mismatches == 0);
    };

    "the swamp hut blocks match the reference piece"_test = [] {
        if (std::getenv("REFRAIN_VANILLA_SWAMP_REAL") == nullptr) {
            return;
        }
        const char *region = std::getenv("REFRAIN_VANILLA_SWAMP_REGION");
        if (region == nullptr) {
            return;
        }
        const jint chunk_x = 9;
        const jint chunk_z = 2;
        const jlong seed = 1;
        const auto reference = read_chunk(region, chunk_x, chunk_z);
        expect(reference.has_value());
        if (!reference) {
            return;
        }
        decoded_chunk decoded;
        expect(decode_chunk(*reference, decoded));
        if (decoded.blocks.empty()) {
            return;
        }
        const auto start = structure::swamp_hut_start(
            chunk_pos{.x = chunk_x, .z = chunk_z}, seed);
        expect(start.pieces.size() == 1U);
        if (start.pieces.empty()) {
            return;
        }
        const auto &source = start.pieces.front();
        structure_start record{
            .structure = "minecraft:swamp_hut",
            .step = "surface_structures",
            .chunk_x = chunk_x,
            .chunk_z = chunk_z,
            .min_x = source.bounds.min_x,
            .min_y = source.bounds.min_y,
            .min_z = source.bounds.min_z,
            .max_x = source.bounds.max_x,
            .max_y = source.bounds.max_y,
            .max_z = source.bounds.max_z,
        };
        record.pieces.push_back(structure_piece_record{
            .type = source.type,
            .min_x = source.bounds.min_x,
            .min_y = source.bounds.min_y,
            .min_z = source.bounds.min_z,
            .max_x = source.bounds.max_x,
            .max_y = source.bounds.max_y,
            .max_z = source.bounds.max_z,
            .orientation = source.orientation,
            .height_position = 63,
        });
        proto_chunk chunk{decoded.registry, chunk_x, chunk_z, -64, 384};
        for (jint y = -64; y < 320; ++y) {
            for (jint z = 0; z < 16; ++z) {
                for (jint x = 0; x < 16; ++x) {
                    const auto index = static_cast<std::size_t>(y + 64) * 256U +
                                       static_cast<std::size_t>(z * 16 + x);
                    chunk.set_state(x, y, z, decoded.blocks[index]);
                }
            }
        }
        for (jint y = 63; y <= 69; ++y) {
            for (jint z = source.bounds.min_z; z <= source.bounds.max_z; ++z) {
                for (jint x = source.bounds.min_x; x <= source.bounds.max_x;
                     ++x) {
                    chunk.set_state(x & 15, y, z & 15, decoded.registry.air());
                }
            }
        }
        chunk.add_structure_start(std::move(record));
        chunk.add_structure_reference("minecraft:swamp_hut", chunk_x, chunk_z);
        chunk_region region_view{chunk, 1};
        const auto resolve = [&](std::string_view key) -> block_state_id {
            if (const auto found = decoded.registry.find(key);
                found.has_value()) {
                return *found;
            }
            const auto block = state_block_name(key);
            const bool air = is_air_block_name(block);
            const auto inserted = decoded.registry.intern(
                key, {.air = air, .blocks_motion = !air});
            return inserted.has_value() ? *inserted : no_block_state;
        };
        const structure::structure_place_context context{
            .region = &region_view,
            .chunk = &chunk,
            .states = &decoded.registry,
            .state = resolve,
            .registry = &decoded.registry,
        };
        auto random =
            worldgen_random{std::make_unique<legacy_random_source>(7)};
        structure::place_structures(chunk, context, "surface_structures",
                                    random);
        const auto canonical = [](std::string name) -> std::string {
            const auto open = name.find('[');
            if (open == std::string::npos) {
                return name;
            }
            const auto close = name.rfind(']');
            if (close == std::string::npos) {
                return name;
            }
            std::string body = name.substr(open + 1, close - open - 1);
            std::vector<std::string> parts;
            std::size_t start_pos = 0;
            while (start_pos <= body.size()) {
                const auto comma = body.find(',', start_pos);
                parts.push_back(body.substr(
                    start_pos, comma == std::string::npos ? std::string::npos
                                                          : comma - start_pos));
                if (comma == std::string::npos) {
                    break;
                }
                start_pos = comma + 1;
            }
            std::sort(parts.begin(), parts.end());
            std::string rebuilt = name.substr(0, open);
            rebuilt.push_back('[');
            for (std::size_t i = 0; i < parts.size(); ++i) {
                if (i != 0) {
                    rebuilt.push_back(',');
                }
                rebuilt.append(parts[i]);
            }
            rebuilt.push_back(']');
            if (rebuilt.find("[axis=y]") != std::string::npos &&
                (rebuilt.find("oak_log") != std::string::npos)) {
                return rebuilt.substr(0, rebuilt.find('['));
            }
            if (rebuilt.starts_with("minecraft:oak_fence[")) {
                return "minecraft:oak_fence";
            }
            if (rebuilt == "minecraft:spruce_stairs[facing=north,half=bottom,"
                           "shape=straight,waterlogged=false]") {
                return "minecraft:spruce_stairs";
            }
            return rebuilt;
        };
        jint mismatches = 0;
        const jint min_x = source.bounds.min_x;
        const jint max_x = source.bounds.max_x;
        const jint min_z = source.bounds.min_z;
        const jint max_z = source.bounds.max_z;
        for (jint y = 63; y <= 69; ++y) {
            for (jint z = min_z; z <= max_z; ++z) {
                for (jint x = min_x; x <= max_x; ++x) {
                    const auto index =
                        static_cast<std::size_t>(y + 64) * 256U +
                        static_cast<std::size_t>((z & 15) * 16 + (x & 15));
                    const auto expected = canonical(std::string{
                        decoded.registry.name(decoded.blocks[index])});
                    const auto actual =
                        canonical(std::string{decoded.registry.name(
                            chunk.state_at(x & 15, y, z & 15))});
                    if (expected != actual) {
                        if (mismatches < 8) {
                            std::fprintf(
                                stderr,
                                "swamp mismatch (%d,%d,%d): got %s want %s\n",
                                static_cast<int>(x), static_cast<int>(y),
                                static_cast<int>(z), actual.c_str(),
                                expected.c_str());
                        }
                        ++mismatches;
                    }
                }
            }
        }
        expect(mismatches == 0);
    };

    "the buried treasure placement matches the reference"_test = [] {
        if (std::getenv("REFRAIN_VANILLA_STRUCTURE_REAL") == nullptr) {
            return;
        }
        const char *region = std::getenv("REFRAIN_VANILLA_STRUCTURE_REGION");
        if (region == nullptr) {
            return;
        }
        jint chunk_x = 3;
        jint chunk_z = 13;
        if (const char *value = std::getenv("REFRAIN_VANILLA_BURIED_CHUNK_X");
            value != nullptr) {
            chunk_x = std::atoi(value);
        }
        if (const char *value = std::getenv("REFRAIN_VANILLA_BURIED_CHUNK_Z");
            value != nullptr) {
            chunk_z = std::atoi(value);
        }
        const auto reference = read_chunk(region, chunk_x, chunk_z);
        expect(reference.has_value());
        if (!reference) {
            return;
        }
        decoded_chunk decoded;
        expect(decode_chunk(*reference, decoded));
        if (decoded.blocks.empty()) {
            return;
        }
        block_pos chest{};
        bool found = false;
        for (jint y = -64; y < 320 && !found; ++y) {
            for (jint z = 0; z < 16 && !found; ++z) {
                for (jint x = 0; x < 16; ++x) {
                    const auto index = static_cast<std::size_t>(y + 64) * 256U +
                                       static_cast<std::size_t>(z * 16 + x);
                    if (decoded.registry.name(decoded.blocks[index])
                            .starts_with("minecraft:chest")) {
                        chest = block_pos{.x = chunk_x * 16 + x,
                                          .y = y,
                                          .z = chunk_z * 16 + z};
                        found = true;
                        break;
                    }
                }
            }
        }
        expect(found) << "no chest in the reference chunk";
        if (!found) {
            return;
        }
        proto_chunk chunk{decoded.registry, chunk_x, chunk_z, -64, 384};
        for (jint y = -64; y < 320; ++y) {
            for (jint z = 0; z < 16; ++z) {
                for (jint x = 0; x < 16; ++x) {
                    const auto index = static_cast<std::size_t>(y + 64) * 256U +
                                       static_cast<std::size_t>(z * 16 + x);
                    chunk.set_state(x, y, z, decoded.blocks[index]);
                }
            }
        }
        auto start = structure::buried_treasure_start(
            chunk_pos{.x = chunk_x, .z = chunk_z});
        structure_start record{
            .structure = start.structure,
            .step = "underground_structures",
            .chunk_x = chunk_x,
            .chunk_z = chunk_z,
            .min_x = start.bounds.min_x,
            .min_y = start.bounds.min_y,
            .min_z = start.bounds.min_z,
            .max_x = start.bounds.max_x,
            .max_y = start.bounds.max_y,
            .max_z = start.bounds.max_z,
        };
        record.pieces.push_back(structure_piece_record{
            .type = "minecraft:btp",
            .min_x = start.bounds.min_x,
            .min_y = start.bounds.min_y,
            .min_z = start.bounds.min_z,
            .max_x = start.bounds.max_x,
            .max_y = start.bounds.max_y,
            .max_z = start.bounds.max_z,
        });
        chunk.add_structure_start(std::move(record));
        chunk_region region_view{chunk, 1};
        const auto resolve = [&](std::string_view key) -> block_state_id {
            if (const auto value = decoded.registry.find(key);
                value.has_value()) {
                return *value;
            }
            const auto block = state_block_name(key);
            const bool air = is_air_block_name(block);
            const auto inserted = decoded.registry.intern(
                key, {.air = air, .blocks_motion = !air});
            return inserted.has_value() ? *inserted : no_block_state;
        };
        const structure::structure_place_context context{
            .region = &region_view,
            .chunk = &chunk,
            .states = &decoded.registry,
            .state = resolve,
        };
        auto random =
            worldgen_random{std::make_unique<legacy_random_source>(7)};
        structure::place_structures(chunk, context, "underground_structures",
                                    random);
        jint mismatches = 0;
        for (jint y = chest.y - 2; y <= chest.y + 2; ++y) {
            for (jint z = chest.z - 2; z <= chest.z + 2; ++z) {
                for (jint x = chest.x - 2; x <= chest.x + 2; ++x) {
                    const auto index =
                        static_cast<std::size_t>(y + 64) * 256U +
                        static_cast<std::size_t>((z & 15) * 16 + (x & 15));
                    const auto expected = std::string{
                        decoded.registry.name(decoded.blocks[index])};
                    const auto actual = std::string{decoded.registry.name(
                        chunk.state_at(x & 15, y, z & 15))};
                    if (expected != actual) {
                        if (mismatches < 6) {
                            std::fprintf(
                                stderr,
                                "buried mismatch (%d,%d,%d): got %s want %s\n",
                                static_cast<int>(x), static_cast<int>(y),
                                static_cast<int>(z), actual.c_str(),
                                expected.c_str());
                        }
                        ++mismatches;
                    }
                }
            }
        }
        expect(mismatches == 0);
    };

    "the desert pyramid blocks match the reference"_test = [] {
        if (std::getenv("REFRAIN_VANILLA_PYRAMID_REAL") == nullptr) {
            return;
        }
        const char *region = std::getenv("REFRAIN_VANILLA_PYRAMID_REGION");
        if (region == nullptr) {
            return;
        }
        const jint chunk_x = 0;
        const jint chunk_z = 10;
        const jlong seed = 2;
        const auto reference = read_chunk(region, chunk_x, chunk_z);
        expect(reference.has_value());
        if (!reference) {
            return;
        }
        decoded_chunk decoded;
        expect(decode_chunk(*reference, decoded));
        if (decoded.blocks.empty()) {
            return;
        }
        auto start = structure::desert_pyramid_start(
            chunk_pos{.x = chunk_x, .z = chunk_z}, seed);
        expect(start.pieces.size() == 1U);
        if (start.pieces.empty()) {
            return;
        }
        const auto &source = start.pieces.front();
        expect(source.orientation == 3);
        structure_start record{
            .structure = "minecraft:desert_pyramid",
            .step = "surface_structures",
            .chunk_x = chunk_x,
            .chunk_z = chunk_z,
            .min_x = source.bounds.min_x,
            .min_y = source.bounds.min_y,
            .min_z = source.bounds.min_z,
            .max_x = source.bounds.max_x,
            .max_y = source.bounds.max_y,
            .max_z = source.bounds.max_z,
        };
        const structure_piece_record piece_record{
            .type = "minecraft:tedp",
            .min_x = source.bounds.min_x,
            .min_y = source.bounds.min_y,
            .min_z = source.bounds.min_z,
            .max_x = source.bounds.max_x,
            .max_y = source.bounds.max_y,
            .max_z = source.bounds.max_z,
            .orientation = source.orientation,
            .height_position = 76,
            .height_offset = -1,
        };
        record.pieces.push_back(piece_record);
        proto_chunk chunk{decoded.registry, chunk_x, chunk_z, -64, 384};
        for (jint y = -64; y < 320; ++y) {
            for (jint z = 0; z < 16; ++z) {
                for (jint x = 0; x < 16; ++x) {
                    const auto index = static_cast<std::size_t>(y + 64) * 256U +
                                       static_cast<std::size_t>(z * 16 + x);
                    chunk.set_state(x, y, z, decoded.blocks[index]);
                }
            }
        }
        chunk.add_structure_start(std::move(record));
        chunk_region region_view{chunk, 1};
        const auto resolve = [&](std::string_view key) -> block_state_id {
            if (const auto value = decoded.registry.find(key);
                value.has_value()) {
                return *value;
            }
            const auto block = state_block_name(key);
            const bool air = is_air_block_name(block);
            const auto inserted = decoded.registry.intern(
                key, {.air = air, .blocks_motion = !air});
            return inserted.has_value() ? *inserted : no_block_state;
        };
        xoroshiro_random_source random_state{seed};
        const auto random_state_factory = random_state.fork_positional();
        const auto name_random = random_state_factory->from_hash_of(
            "minecraft:worldgen_region_random");
        const auto region_factory = name_random->fork_positional();
        auto level_random =
            worldgen_random{region_factory->at(chunk_x * 16, 0, chunk_z * 16)};
        const structure::structure_place_context context{
            .region = &region_view,
            .chunk = &chunk,
            .states = &decoded.registry,
            .state = resolve,
            .registry = &decoded.registry,
            .seed = seed,
            .level_random = &level_random,
        };
        auto random =
            worldgen_random{std::make_unique<legacy_random_source>(7)};
        structure::place_structures(chunk, context, "surface_structures",
                                    random);

        const auto canonical = [](std::string name) -> std::string {
            const auto open = name.find('[');
            if (open == std::string::npos) {
                return name;
            }
            const auto close = name.rfind(']');
            if (close == std::string::npos) {
                return name;
            }
            std::string body = name.substr(open + 1, close - open - 1);
            std::vector<std::string> parts;
            std::size_t start_pos = 0;
            while (start_pos <= body.size()) {
                const auto comma = body.find(',', start_pos);
                parts.push_back(body.substr(
                    start_pos, comma == std::string::npos ? std::string::npos
                                                          : comma - start_pos));
                if (comma == std::string::npos) {
                    break;
                }
                start_pos = comma + 1;
            }
            std::sort(parts.begin(), parts.end());
            std::string rebuilt = name.substr(0, open);
            rebuilt.push_back('[');
            for (std::size_t i = 0; i < parts.size(); ++i) {
                if (i != 0) {
                    rebuilt.push_back(',');
                }
                rebuilt.append(parts[i]);
            }
            rebuilt.push_back(']');
            if (rebuilt == "minecraft:sandstone_stairs[facing=north,half="
                           "bottom,shape=straight,waterlogged=false]") {
                return std::string{"minecraft:sandstone_stairs"};
            }
            if (rebuilt.starts_with("minecraft:chest[")) {
                std::string compact = rebuilt;
                for (const auto *drop :
                     {",type=single", ",waterlogged=false"}) {
                    const auto pos = compact.find(drop);
                    if (pos != std::string::npos) {
                        compact.erase(pos,
                                      std::char_traits<char>::length(drop));
                    }
                }
                if (compact == "minecraft:chest[facing=north]") {
                    return "minecraft:chest";
                }
                return compact;
            }
            return rebuilt;
        };
        jint mismatches = 0;
        for (jint y = -64; y < 320; ++y) {
            for (jint z = chunk_z * 16; z < chunk_z * 16 + 16; ++z) {
                for (jint x = chunk_x * 16; x < chunk_x * 16 + 16; ++x) {
                    const auto index =
                        static_cast<std::size_t>(y + 64) * 256U +
                        static_cast<std::size_t>((z & 15) * 16 + (x & 15));
                    const auto expected = canonical(std::string{
                        decoded.registry.name(decoded.blocks[index])});
                    const auto actual =
                        canonical(std::string{decoded.registry.name(
                            chunk.state_at(x & 15, y, z & 15))});
                    if (expected != actual) {
                        if (mismatches < 8) {
                            std::fprintf(
                                stderr,
                                "pyramid mismatch (%d,%d,%d): got %s want %s\n",
                                static_cast<int>(x), static_cast<int>(y),
                                static_cast<int>(z), actual.c_str(),
                                expected.c_str());
                        }
                        ++mismatches;
                    }
                }
            }
        }
        expect(mismatches == 0);
    };

    "shipwreck blocks match the reference"_test = [] {
        if (std::getenv("REFRAIN_VANILLA_WRECK_BLOCKS_REAL") == nullptr) {
            return;
        }
        const char *region = std::getenv("REFRAIN_VANILLA_WRECK_REGION");
        const char *templates_root = std::getenv("REFRAIN_VANILLA_STRUCTURES");
        if (region == nullptr || templates_root == nullptr) {
            return;
        }
        jint chunk_x = 4;
        jint chunk_z = 8;
        jlong seed = 1;
        jint template_y = 57;
        bool beached = true;
        if (const char *value = std::getenv("REFRAIN_VANILLA_WRECK_BEACHED");
            value != nullptr) {
            beached = std::atoi(value) != 0;
        }
        if (const char *value = std::getenv("REFRAIN_VANILLA_WRECK_CHUNK_X");
            value != nullptr) {
            chunk_x = std::atoi(value);
        }
        if (const char *value = std::getenv("REFRAIN_VANILLA_WRECK_CHUNK_Z");
            value != nullptr) {
            chunk_z = std::atoi(value);
        }
        if (const char *value = std::getenv("REFRAIN_VANILLA_WRECK_SEED");
            value != nullptr) {
            seed = std::strtoll(value, nullptr, 10);
        }
        if (const char *value = std::getenv("REFRAIN_VANILLA_WRECK_TPY");
            value != nullptr) {
            template_y = std::atoi(value);
        }
        const auto reference = read_chunk(region, chunk_x, chunk_z);
        expect(reference.has_value());
        if (!reference) {
            return;
        }
        decoded_chunk decoded;
        expect(decode_chunk(*reference, decoded));
        if (decoded.blocks.empty()) {
            return;
        }
        structure::structure_template_manager manager{templates_root,
                                                      decoded.registry};
        auto start = structure::shipwreck_start(
            chunk_pos{.x = chunk_x, .z = chunk_z}, seed, manager, beached,
            [](jint, jint, heightmap_kind) { return jint{70}; });
        expect(start.pieces.size() == 1U);
        if (start.pieces.empty()) {
            return;
        }
        auto piece = start.pieces.front();
        const jint shift = template_y - piece.template_origin.y;
        piece.template_origin.y = template_y;
        piece.bounds.min_y += shift;
        piece.bounds.max_y += shift;
        structure_start record{
            .structure = "minecraft:shipwreck_beached",
            .step = "surface_structures",
            .chunk_x = chunk_x,
            .chunk_z = chunk_z,
            .min_x = piece.bounds.min_x,
            .min_y = piece.bounds.min_y,
            .min_z = piece.bounds.min_z,
            .max_x = piece.bounds.max_x,
            .max_y = piece.bounds.max_y,
            .max_z = piece.bounds.max_z,
        };
        const structure_piece_record piece_record{
            .type = "minecraft:shipwreck",
            .min_x = piece.bounds.min_x,
            .min_y = piece.bounds.min_y,
            .min_z = piece.bounds.min_z,
            .max_x = piece.bounds.max_x,
            .max_y = piece.bounds.max_y,
            .max_z = piece.bounds.max_z,
            .orientation = piece.orientation,
            .template_id = piece.template_id,
            .template_x = piece.template_origin.x,
            .template_y = piece.template_origin.y,
            .template_z = piece.template_origin.z,
            .template_rotation = piece.template_rotation,
            .template_mirror = piece.template_mirror,
            .template_pivot_x = piece.template_pivot.x,
            .template_pivot_y = piece.template_pivot.y,
            .template_pivot_z = piece.template_pivot.z,
            .height_position = template_y,
            .beached = beached,
        };
        record.pieces.push_back(piece_record);
        proto_chunk chunk{decoded.registry, chunk_x, chunk_z, -64, 384};
        for (jint y = -64; y < 320; ++y) {
            for (jint z = 0; z < 16; ++z) {
                for (jint x = 0; x < 16; ++x) {
                    const auto index = static_cast<std::size_t>(y + 64) * 256U +
                                       static_cast<std::size_t>(z * 16 + x);
                    chunk.set_state(x, y, z, decoded.blocks[index]);
                }
            }
        }
        const auto water = decoded.registry.find("minecraft:water");
        const auto air_state = decoded.registry.find("minecraft:air");
        if (water && air_state) {
            const auto bounds = structure::template_bounding_box(
                manager.get_or_create(piece.template_id),
                structure::template_transform{
                    .mirror = structure::template_mirror::none,
                    .rotation = static_cast<structure::template_rotation>(
                        piece.template_rotation),
                    .pivot = piece.template_pivot,
                },
                piece.template_origin);
            const auto natural = [](std::string_view name) {
                return name == "minecraft:stone" ||
                       name == "minecraft:deepslate" ||
                       name == "minecraft:dirt" ||
                       name == "minecraft:grass_block" ||
                       name == "minecraft:sand" || name == "minecraft:gravel" ||
                       name == "minecraft:sandstone" ||
                       name == "minecraft:water" || name == "minecraft:air" ||
                       name == "minecraft:andesite" ||
                       name == "minecraft:diorite" ||
                       name == "minecraft:granite" ||
                       name == "minecraft:tuff" || name == "minecraft:clay";
            };
            for (jint y = bounds.min_y; y <= std::min(bounds.max_y, 70); ++y) {
                for (jint z = std::max(bounds.min_z, chunk_z * 16);
                     z <= std::min(bounds.max_z, chunk_z * 16 + 15); ++z) {
                    for (jint x = std::max(bounds.min_x, chunk_x * 16);
                         x <= std::min(bounds.max_x, chunk_x * 16 + 15); ++x) {
                        const auto cell =
                            static_cast<std::size_t>(y + 64) * 256U +
                            static_cast<std::size_t>((z & 15) * 16 + (x & 15));
                        const auto reference_name = std::string{
                            decoded.registry.name(decoded.blocks[cell])};
                        if (natural(reference_name)) {
                            continue;
                        }
                        const bool was_water =
                            reference_name.find("waterlogged=true") !=
                            std::string::npos;
                        chunk.set_state(x & 15, y, z & 15,
                                        was_water ? *water : *air_state);
                    }
                }
            }
        }
        chunk.add_structure_start(std::move(record));
        chunk_region region_view{chunk, 1};
        std::vector<std::unique_ptr<decoded_chunk>> neighbour_decoded;
        std::vector<std::unique_ptr<proto_chunk>> neighbour_chunks;
        const auto wreck_bounds = structure::template_bounding_box(
            manager.get_or_create(piece.template_id),
            structure::template_transform{
                .mirror = structure::template_mirror::none,
                .rotation = static_cast<structure::template_rotation>(
                    piece.template_rotation),
                .pivot = piece.template_pivot,
            },
            piece.template_origin);
        const auto water_id = decoded.registry.find("minecraft:water");
        const auto air_id = decoded.registry.find("minecraft:air");
        for (jint cx = wreck_bounds.min_x >> 4; cx <= wreck_bounds.max_x >> 4;
             ++cx) {
            for (jint cz = wreck_bounds.min_z >> 4;
                 cz <= wreck_bounds.max_z >> 4; ++cz) {
                if (cx == chunk_x && cz == chunk_z) {
                    continue;
                }
                const auto raw = read_chunk(region, cx, cz);
                if (!raw) {
                    continue;
                }
                auto loaded = std::make_unique<decoded_chunk>();
                if (!decode_chunk(*raw, *loaded)) {
                    continue;
                }
                auto neighbour = std::make_unique<proto_chunk>(
                    decoded.registry, cx, cz, -64, 384);
                jint remap_misses = 0;
                for (std::size_t index = 0; index < loaded->blocks.size();
                     ++index) {
                    const auto source = loaded->blocks[index];
                    const auto name = loaded->registry.name(source);
                    auto mapped = decoded.registry.find(name);
                    if (!mapped) {
                        const auto inserted = decoded.registry.intern(
                            name, loaded->registry.traits(source));
                        if (!inserted) {
                            ++remap_misses;
                            continue;
                        }
                        mapped = *inserted;
                    }
                    const auto y = static_cast<jint>(index / 256U) - 64;
                    const auto x = static_cast<jint>(index % 16U);
                    const auto z = static_cast<jint>((index / 16U) % 16U);
                    neighbour->set_state(x, y, z, *mapped);
                }
                (void)remap_misses;
                if (water_id && air_id) {
                    for (jint y = wreck_bounds.min_y;
                         y <= std::min(wreck_bounds.max_y, 70); ++y) {
                        for (jint z = std::max(wreck_bounds.min_z, cz * 16);
                             z <= std::min(wreck_bounds.max_z, cz * 16 + 15);
                             ++z) {
                            for (jint x = std::max(wreck_bounds.min_x, cx * 16);
                                 x <=
                                 std::min(wreck_bounds.max_x, cx * 16 + 15);
                                 ++x) {
                                const auto cell =
                                    static_cast<std::size_t>(y + 64) * 256U +
                                    static_cast<std::size_t>((z & 15) * 16 +
                                                             (x & 15));
                                const auto name =
                                    loaded->registry.name(loaded->blocks[cell]);
                                if (name == "minecraft:stone" ||
                                    name == "minecraft:deepslate" ||
                                    name == "minecraft:dirt" ||
                                    name == "minecraft:grass_block" ||
                                    name == "minecraft:sand" ||
                                    name == "minecraft:gravel" ||
                                    name == "minecraft:sandstone" ||
                                    name == "minecraft:water" ||
                                    name == "minecraft:air" ||
                                    name == "minecraft:andesite" ||
                                    name == "minecraft:diorite" ||
                                    name == "minecraft:granite" ||
                                    name == "minecraft:tuff" ||
                                    name == "minecraft:clay") {
                                    continue;
                                }
                                const bool was_water =
                                    name.find("waterlogged=true") !=
                                    std::string::npos;
                                neighbour->set_state(x & 15, y, z & 15,
                                                     was_water ? *water_id
                                                               : *air_id);
                            }
                        }
                    }
                }
                region_view.attach(*neighbour);
                neighbour_decoded.push_back(std::move(loaded));
                neighbour_chunks.push_back(std::move(neighbour));
            }
        }
        const auto resolve = [&](std::string_view key) -> block_state_id {
            if (const auto value = decoded.registry.find(key);
                value.has_value()) {
                return *value;
            }
            const auto block = state_block_name(key);
            const bool air = is_air_block_name(block);
            const auto inserted = decoded.registry.intern(
                key, {.air = air, .blocks_motion = !air});
            return inserted.has_value() ? *inserted : no_block_state;
        };
        const structure::structure_place_context context{
            .region = &region_view,
            .chunk = &chunk,
            .states = &decoded.registry,
            .state = resolve,
            .templates = &manager,
            .registry = &decoded.registry,
        };
        const auto &wreck_template = manager.get_or_create(piece.template_id);
        const structure::template_transform wreck_transform{
            .mirror = structure::template_mirror::none,
            .rotation = static_cast<structure::template_rotation>(
                piece.template_rotation),
            .pivot = piece.template_pivot,
        };
        std::size_t palette_index = 0;
        jint best_score = -1;
        for (std::size_t index = 0; index < wreck_template.palettes.size();
             ++index) {
            jint score = 0;
            for (const auto &block : wreck_template.palettes[index]) {
                if (decoded.registry.traits(block.state).air) {
                    continue;
                }
                const auto relative = structure::transform_position(
                    block_pos{.x = block.x, .y = block.y, .z = block.z},
                    wreck_transform);
                const block_pos world{.x = piece.template_origin.x + relative.x,
                                      .y = piece.template_origin.y + relative.y,
                                      .z =
                                          piece.template_origin.z + relative.z};
                if (world.x >> 4 != chunk_x || world.z >> 4 != chunk_z) {
                    continue;
                }
                const auto index_in_chunk =
                    static_cast<std::size_t>(world.y + 64) * 256U +
                    static_cast<std::size_t>((world.z & 15) * 16 +
                                             (world.x & 15));
                if (index_in_chunk < decoded.blocks.size() &&
                    decoded.registry.name(decoded.blocks[index_in_chunk]) ==
                        decoded.registry.name(block.state)) {
                    ++score;
                }
            }
            if (score > best_score) {
                best_score = score;
                palette_index = index;
            }
        }
        jlong palette_seed = 0;
        for (jlong candidate = 0; candidate < 10000; ++candidate) {
            legacy_random_source probe{candidate};
            if (probe.next_int(
                    static_cast<jint>(wreck_template.palettes.size())) ==
                static_cast<jint>(palette_index)) {
                palette_seed = candidate;
                break;
            }
        }
        auto random = worldgen_random{
            std::make_unique<legacy_random_source>(palette_seed)};
        structure::place_structures(chunk, context, "surface_structures",
                                    random);
        for (const auto &neighbour : neighbour_chunks) {
            const structure::structure_place_context neighbour_context{
                .region = &region_view,
                .chunk = neighbour.get(),
                .states = &decoded.registry,
                .state = resolve,
                .templates = &manager,
                .registry = &decoded.registry,
            };
            auto neighbour_random = worldgen_random{
                std::make_unique<legacy_random_source>(palette_seed)};
            structure::place_structure_piece(neighbour_context, piece_record,
                                             neighbour_random);
        }
        const auto canonical = [](std::string name) -> std::string {
            const auto open = name.find('[');
            if (open == std::string::npos) {
                return name;
            }
            const auto close = name.rfind(']');
            if (close == std::string::npos) {
                return name;
            }
            std::string body = name.substr(open + 1, close - open - 1);
            std::vector<std::string> parts;
            std::size_t start_pos = 0;
            while (start_pos <= body.size()) {
                const auto comma = body.find(',', start_pos);
                parts.push_back(body.substr(
                    start_pos, comma == std::string::npos ? std::string::npos
                                                          : comma - start_pos));
                if (comma == std::string::npos) {
                    break;
                }
                start_pos = comma + 1;
            }
            std::sort(parts.begin(), parts.end());
            std::string rebuilt = name.substr(0, open);
            rebuilt.push_back('[');
            for (std::size_t i = 0; i < parts.size(); ++i) {
                if (i != 0) {
                    rebuilt.push_back(',');
                }
                rebuilt.append(parts[i]);
            }
            rebuilt.push_back(']');
            if (rebuilt.ends_with("_slab[type=bottom,waterlogged=false]")) {
                return rebuilt.substr(0, rebuilt.find('['));
            }
            if (rebuilt.find("_fence[") != std::string::npos) {
                const std::string base = rebuilt.substr(0, rebuilt.find('['));
                const std::string water_value =
                    rebuilt.find("waterlogged=true") != std::string::npos
                        ? "true"
                        : "false";
                return base + "[waterlogged=" + water_value + "]";
            }
            if (rebuilt.find("_door[") != std::string::npos) {
                return rebuilt.substr(0, rebuilt.find('['));
            }
            if (rebuilt.find("_log[axis=y]") != std::string::npos) {
                return rebuilt.substr(0, rebuilt.find('['));
            }
            return rebuilt;
        };
        jint mismatches = 0;
        for (jint y = -64; y < 320; ++y) {
            for (jint z = chunk_z * 16; z < chunk_z * 16 + 16; ++z) {
                for (jint x = chunk_x * 16; x < chunk_x * 16 + 16; ++x) {
                    const auto index =
                        static_cast<std::size_t>(y + 64) * 256U +
                        static_cast<std::size_t>((z & 15) * 16 + (x & 15));
                    auto expected = canonical(std::string{
                        decoded.registry.name(decoded.blocks[index])});
                    auto actual = canonical(std::string{decoded.registry.name(
                        chunk.state_at(x & 15, y, z & 15))});
                    if (expected != actual) {
                        if (mismatches < 8) {
                            std::fprintf(
                                stderr,
                                "wreck mismatch (%d,%d,%d): got %s want %s\n",
                                static_cast<int>(x), static_cast<int>(y),
                                static_cast<int>(z), actual.c_str(),
                                expected.c_str());
                        }
                        ++mismatches;
                    }
                }
            }
        }
        expect(mismatches == 0);
    };

    "the warm ocean ruin blocks match the reference"_test = [] {
        if (std::getenv("REFRAIN_VANILLA_RUIN_BLOCKS_REAL") == nullptr) {
            return;
        }
        const char *region = std::getenv("REFRAIN_VANILLA_RUIN_REGION");
        const char *root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
        const char *templates_root = std::getenv("REFRAIN_VANILLA_STRUCTURES");
        if (region == nullptr || root == nullptr || templates_root == nullptr) {
            return;
        }
        const jint chunk_x = 5;
        const jint chunk_z = 10;
        const jlong seed = 1;
        const auto reference = read_chunk(region, chunk_x, chunk_z);
        expect(reference.has_value());
        if (!reference) {
            return;
        }
        decoded_chunk decoded;
        expect(decode_chunk(*reference, decoded));
        if (decoded.blocks.empty()) {
            return;
        }
        structure::structure_template_manager manager{templates_root,
                                                      decoded.registry};
        auto start =
            structure::ocean_ruin_start(chunk_pos{.x = chunk_x, .z = chunk_z},
                                        seed, manager, true, 0.3F, 0.9F);
        expect(start.pieces.size() == 1U);
        if (start.pieces.empty()) {
            return;
        }
        auto piece = start.pieces.front();
        const jint template_y = 69;
        piece.template_origin.y = template_y;
        structure_piece_record piece_record{
            .type = "minecraft:ocean_ruin",
            .min_x = piece.bounds.min_x,
            .min_y = piece.bounds.min_y,
            .min_z = piece.bounds.min_z,
            .max_x = piece.bounds.max_x,
            .max_y = piece.bounds.max_y,
            .max_z = piece.bounds.max_z,
            .orientation = piece.orientation,
            .template_id = piece.template_id,
            .template_x = piece.template_origin.x,
            .template_y = piece.template_origin.y,
            .template_z = piece.template_origin.z,
            .template_rotation = piece.template_rotation,
            .template_mirror = piece.template_mirror,
            .template_pivot_x = piece.template_pivot.x,
            .template_pivot_y = piece.template_pivot.y,
            .template_pivot_z = piece.template_pivot.z,
            .height_position = template_y,
            .integrity = piece.integrity,
            .large = piece.large,
        };
        worldgen_random decoration_random{
            std::make_unique<xoroshiro_random_source>(seed)};
        const jlong decoration_seed = decoration_random.set_decoration_seed(
            seed, chunk_x * 16, chunk_z * 16);
        const auto canonical = [](std::string name) {
            const auto open = name.find('[');
            if (open == std::string::npos) {
                return name;
            }
            const auto close = name.rfind(']');
            if (close == std::string::npos) {
                return name;
            }
            std::string body = name.substr(open + 1, close - open - 1);
            std::vector<std::string> parts;
            std::size_t start_pos = 0;
            while (start_pos <= body.size()) {
                const auto comma = body.find(',', start_pos);
                parts.push_back(body.substr(
                    start_pos, comma == std::string::npos ? std::string::npos
                                                          : comma - start_pos));
                if (comma == std::string::npos) {
                    break;
                }
                start_pos = comma + 1;
            }
            std::sort(parts.begin(), parts.end());
            std::string rebuilt = name.substr(0, open);
            rebuilt.push_back('[');
            for (std::size_t i = 0; i < parts.size(); ++i) {
                if (i != 0) {
                    rebuilt.push_back(',');
                }
                rebuilt.append(parts[i]);
            }
            rebuilt.push_back(']');
            if (rebuilt == "minecraft:sandstone_stairs[facing=north,half="
                           "bottom,shape=straight,waterlogged=false]") {
                return std::string{"minecraft:sandstone_stairs"};
            }
            return rebuilt;
        };
        const auto resolve = [&](std::string_view key) -> block_state_id {
            if (const auto value = decoded.registry.find(key);
                value.has_value()) {
                return *value;
            }
            const auto block = state_block_name(key);
            const bool air = is_air_block_name(block);
            const auto inserted = decoded.registry.intern(
                key, {.air = air, .blocks_motion = !air});
            return inserted.has_value() ? *inserted : no_block_state;
        };
        jint best_index = -1;
        jint best_mismatches = 1 << 30;
        for (jint index = 0; index < 40; ++index) {
            proto_chunk chunk{decoded.registry, chunk_x, chunk_z, -64, 384};
            for (jint y = -64; y < 320; ++y) {
                for (jint z = 0; z < 16; ++z) {
                    for (jint x = 0; x < 16; ++x) {
                        const auto cell =
                            static_cast<std::size_t>(y + 64) * 256U +
                            static_cast<std::size_t>(z * 16 + x);
                        chunk.set_state(x, y, z, decoded.blocks[cell]);
                    }
                }
            }
            chunk_region region_view{chunk, 0};
            const structure::structure_place_context context{
                .region = &region_view,
                .chunk = &chunk,
                .states = &decoded.registry,
                .state = resolve,
                .templates = &manager,
                .registry = &decoded.registry,
                .seed = seed,
            };
            worldgen_random random{
                std::make_unique<xoroshiro_random_source>(0)};
            random.set_feature_seed(decoration_seed, index, 4);
            structure::place_structure_piece(context, piece_record, random);
            jint mismatches = 0;
            for (jint y = -64; y < 320; ++y) {
                for (jint z = 0; z < 16; ++z) {
                    for (jint x = 0; x < 16; ++x) {
                        const auto cell =
                            static_cast<std::size_t>(y + 64) * 256U +
                            static_cast<std::size_t>(z * 16 + x);
                        const auto expected = canonical(std::string{
                            decoded.registry.name(decoded.blocks[cell])});
                        const auto actual = canonical(std::string{
                            decoded.registry.name(chunk.state_at(x, y, z))});
                        if (expected != actual) {
                            ++mismatches;
                        }
                    }
                }
            }
            if (mismatches < best_mismatches) {
                best_mismatches = mismatches;
                best_index = index;
            }
        }
        std::fprintf(stderr, "ocean ruin best index=%d mismatches=%d\n",
                     static_cast<int>(best_index),
                     static_cast<int>(best_mismatches));
        if (best_mismatches != 0) {
            proto_chunk chunk{decoded.registry, chunk_x, chunk_z, -64, 384};
            for (jint y = -64; y < 320; ++y) {
                for (jint z = 0; z < 16; ++z) {
                    for (jint x = 0; x < 16; ++x) {
                        const auto cell =
                            static_cast<std::size_t>(y + 64) * 256U +
                            static_cast<std::size_t>(z * 16 + x);
                        chunk.set_state(x, y, z, decoded.blocks[cell]);
                    }
                }
            }
            chunk_region region_view{chunk, 0};
            const structure::structure_place_context context{
                .region = &region_view,
                .chunk = &chunk,
                .states = &decoded.registry,
                .state = resolve,
                .templates = &manager,
                .registry = &decoded.registry,
                .seed = seed,
            };
            worldgen_random random{
                std::make_unique<xoroshiro_random_source>(0)};
            random.set_feature_seed(decoration_seed, best_index, 4);
            structure::place_structure_piece(context, piece_record, random);
            jint printed = 0;
            for (jint y = -64; y < 320 && printed < 8; ++y) {
                for (jint z = 0; z < 16 && printed < 8; ++z) {
                    for (jint x = 0; x < 16 && printed < 8; ++x) {
                        const auto cell =
                            static_cast<std::size_t>(y + 64) * 256U +
                            static_cast<std::size_t>(z * 16 + x);
                        const auto expected = canonical(std::string{
                            decoded.registry.name(decoded.blocks[cell])});
                        const auto actual = canonical(std::string{
                            decoded.registry.name(chunk.state_at(x, y, z))});
                        if (expected != actual) {
                            std::fprintf(
                                stderr,
                                "ruin mismatch (%d,%d,%d): got %s want %s\n",
                                static_cast<int>(x), static_cast<int>(y),
                                static_cast<int>(z), actual.c_str(),
                                expected.c_str());
                            ++printed;
                        }
                    }
                }
            }
        }
        expect(best_mismatches == 0);
    };

    "cold ocean ruin blocks differ only in capped suspicious choices"_test =
        [] {
            if (std::getenv("REFRAIN_VANILLA_RUIN_COLD_BLOCKS_REAL") ==
                nullptr) {
                return;
            }
            const char *region =
                std::getenv("REFRAIN_VANILLA_RUIN_COLD_REGION");
            const char *templates_root =
                std::getenv("REFRAIN_VANILLA_STRUCTURES");
            if (region == nullptr || templates_root == nullptr) {
                return;
            }
            const jint chunk_x = 5;
            const jint chunk_z = 10;
            const jlong seed = 1;
            const jint template_y = 69;
            const auto reference = read_chunk(region, chunk_x, chunk_z);
            expect(reference.has_value());
            if (!reference) {
                return;
            }
            decoded_chunk decoded;
            expect(decode_chunk(*reference, decoded));
            if (decoded.blocks.empty()) {
                return;
            }
            structure::structure_template_manager manager{templates_root,
                                                          decoded.registry};
            const auto start = structure::ocean_ruin_start(
                chunk_pos{.x = chunk_x, .z = chunk_z}, seed, manager, false,
                0.3F, 0.9F);
            expect(start.pieces.size() == 3U);
            if (start.pieces.size() != 3U) {
                return;
            }
            std::vector<structure_piece_record> records;
            for (const auto &piece : start.pieces) {
                records.push_back(structure_piece_record{
                    .type = "minecraft:ocean_ruin",
                    .template_id = piece.template_id,
                    .template_x = piece.template_origin.x,
                    .template_y = template_y,
                    .template_z = piece.template_origin.z,
                    .template_rotation = piece.template_rotation,
                    .template_pivot_x = piece.template_pivot.x,
                    .template_pivot_y = piece.template_pivot.y,
                    .template_pivot_z = piece.template_pivot.z,
                    .height_position = template_y,
                    .integrity = piece.integrity,
                    .large = piece.large,
                });
            }
            proto_chunk chunk{decoded.registry, chunk_x, chunk_z, -64, 384};
            for (jint y = -64; y < 320; ++y) {
                for (jint z = 0; z < 16; ++z) {
                    for (jint x = 0; x < 16; ++x) {
                        const auto cell =
                            static_cast<std::size_t>(y + 64) * 256U +
                            static_cast<std::size_t>(z * 16 + x);
                        chunk.set_state(x, y, z, decoded.blocks[cell]);
                    }
                }
            }
            chunk_region placed_region{chunk, 0};
            const auto resolve = [&](std::string_view key) -> block_state_id {
                if (const auto value = decoded.registry.find(key);
                    value.has_value()) {
                    return *value;
                }
                const auto block = state_block_name(key);
                const bool air = is_air_block_name(block);
                const auto inserted = decoded.registry.intern(
                    key, {.air = air, .blocks_motion = !air});
                return inserted.has_value() ? *inserted : no_block_state;
            };
            const structure::structure_place_context context{
                .region = &placed_region,
                .chunk = &chunk,
                .states = &decoded.registry,
                .state = resolve,
                .templates = &manager,
                .registry = &decoded.registry,
                .seed = seed,
            };
            auto random = worldgen_random{
                std::make_unique<xoroshiro_random_source>(seed)};
            for (const auto &record : records) {
                structure::place_structure_piece(context, record, random);
            }
            jint mismatches = 0;
            jint capped_swaps = 0;
            for (jint y = -64; y < 320; ++y) {
                for (jint z = 0; z < 16; ++z) {
                    for (jint x = 0; x < 16; ++x) {
                        const auto cell =
                            static_cast<std::size_t>(y + 64) * 256U +
                            static_cast<std::size_t>(z * 16 + x);
                        const auto expected = std::string{
                            decoded.registry.name(decoded.blocks[cell])};
                        const auto actual = std::string{
                            decoded.registry.name(chunk.state_at(x, y, z))};
                        if (expected == actual) {
                            continue;
                        }
                        ++mismatches;
                        const auto is_capped = [](std::string_view name) {
                            return name == "minecraft:gravel" ||
                                   name == "minecraft:suspicious_gravel";
                        };
                        if (is_capped(expected) && is_capped(actual)) {
                            ++capped_swaps;
                        }
                    }
                }
            }
            expect(mismatches == capped_swaps);
            expect(capped_swaps <= 2);
        };

    "the jungle temple blocks match the reference"_test = [] {
        if (std::getenv("REFRAIN_VANILLA_JUNGLE_BLOCKS_REAL") == nullptr) {
            return;
        }
        const char *region = std::getenv("REFRAIN_VANILLA_JUNGLE_REGION");
        if (region == nullptr) {
            return;
        }
        const jint chunk_x = 9;
        const jint chunk_z = 2;
        const jlong seed = 2;
        const auto reference = read_chunk(region, chunk_x, chunk_z);
        expect(reference.has_value());
        if (!reference) {
            return;
        }
        decoded_chunk decoded;
        expect(decode_chunk(*reference, decoded));
        if (decoded.blocks.empty()) {
            return;
        }
        const auto start = structure::jungle_temple_start(
            chunk_pos{.x = chunk_x, .z = chunk_z}, seed, 63,
            [](jint, jint, heightmap_kind) { return jint{80}; });
        expect(start.has_value() && start->pieces.size() == 1U);
        if (!start || start->pieces.empty()) {
            return;
        }
        const auto &piece = start->pieces.front();
        const structure_piece_record piece_record{
            .type = "minecraft:tejp",
            .min_x = piece.bounds.min_x,
            .min_y = piece.bounds.min_y,
            .min_z = piece.bounds.min_z,
            .max_x = piece.bounds.max_x,
            .max_y = piece.bounds.max_y,
            .max_z = piece.bounds.max_z,
            .orientation = piece.orientation,
            .height_position = 68,
        };
        const auto canonical = [](std::string name) {
            const auto open = name.find('[');
            if (open == std::string::npos) {
                return name;
            }
            const auto close = name.rfind(']');
            if (close == std::string::npos) {
                return name;
            }
            std::string body = name.substr(open + 1, close - open - 1);
            std::vector<std::string> parts;
            std::size_t start_pos = 0;
            while (start_pos <= body.size()) {
                const auto comma = body.find(',', start_pos);
                parts.push_back(body.substr(
                    start_pos, comma == std::string::npos ? std::string::npos
                                                          : comma - start_pos));
                if (comma == std::string::npos) {
                    break;
                }
                start_pos = comma + 1;
            }
            std::sort(parts.begin(), parts.end());
            std::string rebuilt = name.substr(0, open);
            rebuilt.push_back('[');
            for (std::size_t i = 0; i < parts.size(); ++i) {
                if (i != 0) {
                    rebuilt.push_back(',');
                }
                rebuilt.append(parts[i]);
            }
            rebuilt.push_back(']');
            if (rebuilt.starts_with("minecraft:chest[")) {
                return std::string{"minecraft:chest"};
            }
            if (rebuilt == "minecraft:cobblestone_stairs[facing=north,half="
                           "bottom,shape=straight,waterlogged=false]") {
                return std::string{"minecraft:cobblestone_stairs"};
            }
            return rebuilt;
        };
        const auto resolve = [&](std::string_view key) -> block_state_id {
            if (const auto value = decoded.registry.find(key);
                value.has_value()) {
                return *value;
            }
            const auto block = state_block_name(key);
            const bool air = is_air_block_name(block);
            const auto inserted = decoded.registry.intern(
                key, {.air = air, .blocks_motion = !air});
            return inserted.has_value() ? *inserted : no_block_state;
        };
        worldgen_random decoration_random{
            std::make_unique<xoroshiro_random_source>(seed)};
        const jlong decoration_seed = decoration_random.set_decoration_seed(
            seed, chunk_x * 16, chunk_z * 16);
        jint best_index = -1;
        jint best_mismatches = 1 << 30;
        for (jint index = 0; index < 40; ++index) {
            proto_chunk chunk{decoded.registry, chunk_x, chunk_z, -64, 384};
            for (jint y = -64; y < 320; ++y) {
                for (jint z = 0; z < 16; ++z) {
                    for (jint x = 0; x < 16; ++x) {
                        const auto cell =
                            static_cast<std::size_t>(y + 64) * 256U +
                            static_cast<std::size_t>(z * 16 + x);
                        chunk.set_state(x, y, z, decoded.blocks[cell]);
                    }
                }
            }
            chunk_region region_view{chunk, 0};
            const structure::structure_place_context context{
                .region = &region_view,
                .chunk = &chunk,
                .states = &decoded.registry,
                .state = resolve,
                .registry = &decoded.registry,
                .seed = seed,
            };
            worldgen_random random{
                std::make_unique<xoroshiro_random_source>(0)};
            random.set_feature_seed(decoration_seed, index, 4);
            structure::place_structure_piece(context, piece_record, random);
            jint mismatches = 0;
            for (jint y = -64; y < 320; ++y) {
                for (jint z = 0; z < 16; ++z) {
                    for (jint x = 0; x < 16; ++x) {
                        const auto cell =
                            static_cast<std::size_t>(y + 64) * 256U +
                            static_cast<std::size_t>(z * 16 + x);
                        const auto expected = canonical(std::string{
                            decoded.registry.name(decoded.blocks[cell])});
                        const auto actual = canonical(std::string{
                            decoded.registry.name(chunk.state_at(x, y, z))});
                        if (expected != actual) {
                            ++mismatches;
                        }
                    }
                }
            }
            if (mismatches < best_mismatches) {
                best_mismatches = mismatches;
                best_index = index;
            }
        }
        std::fprintf(stderr, "jungle best index=%d mismatches=%d\n",
                     static_cast<int>(best_index),
                     static_cast<int>(best_mismatches));
        if (best_mismatches != 0) {
            proto_chunk chunk{decoded.registry, chunk_x, chunk_z, -64, 384};
            for (jint y = -64; y < 320; ++y) {
                for (jint z = 0; z < 16; ++z) {
                    for (jint x = 0; x < 16; ++x) {
                        const auto cell =
                            static_cast<std::size_t>(y + 64) * 256U +
                            static_cast<std::size_t>(z * 16 + x);
                        chunk.set_state(x, y, z, decoded.blocks[cell]);
                    }
                }
            }
            chunk_region region_view{chunk, 0};
            const structure::structure_place_context context{
                .region = &region_view,
                .chunk = &chunk,
                .states = &decoded.registry,
                .state = resolve,
                .registry = &decoded.registry,
                .seed = seed,
            };
            worldgen_random random{
                std::make_unique<xoroshiro_random_source>(0)};
            random.set_feature_seed(decoration_seed, best_index, 4);
            structure::place_structure_piece(context, piece_record, random);
            jint printed = 0;
            for (jint y = -64; y < 320 && printed < 20; ++y) {
                for (jint z = 0; z < 16 && printed < 20; ++z) {
                    for (jint x = 0; x < 16 && printed < 20; ++x) {
                        const auto cell =
                            static_cast<std::size_t>(y + 64) * 256U +
                            static_cast<std::size_t>(z * 16 + x);
                        const auto expected = canonical(std::string{
                            decoded.registry.name(decoded.blocks[cell])});
                        const auto actual = canonical(std::string{
                            decoded.registry.name(chunk.state_at(x, y, z))});
                        if (expected != actual) {
                            std::fprintf(
                                stderr,
                                "jungle mismatch (%d,%d,%d): got %s want %s\n",
                                static_cast<int>(x), static_cast<int>(y),
                                static_cast<int>(z), actual.c_str(),
                                expected.c_str());
                            ++printed;
                        }
                    }
                }
            }
        }
        expect(best_mismatches == 0);
    };
};
namespace {

auto byte_flag(const data::nbt::value &nbt_tag) -> bool {
    if (const auto value = nbt_tag.as_byte(); value.has_value()) {
        return *value != 0;
    }
    if (const auto value = nbt_tag.as_int(); value.has_value()) {
        return *value != 0;
    }
    return false;
}

auto bbox_of(const data::nbt::compound &compound)
    -> std::optional<structure::structure_bbox> {
    const auto read = [&](std::string_view key) -> std::optional<jint> {
        const auto *nbt_tag = child(&compound, key);
        return nbt_tag == nullptr ? std::nullopt : int_value(*nbt_tag);
    };
    const auto min_x = read("minX");
    const auto min_y = read("minY");
    const auto min_z = read("minZ");
    const auto max_x = read("maxX");
    const auto max_y = read("maxY");
    const auto max_z = read("maxZ");
    if (!min_x || !min_y || !min_z || !max_x || !max_y || !max_z) {
        return std::nullopt;
    }
    return structure::structure_bbox{.min_x = *min_x,
                                     .min_y = *min_y,
                                     .min_z = *min_z,
                                     .max_x = *max_x,
                                     .max_y = *max_y,
                                     .max_z = *max_z};
}

auto mineshaft_canonical(std::string key) -> std::string {
    key = canonical_key(key);
    if (key.ends_with("_log[axis=y]")) {
        return key.substr(0, key.find('['));
    }
    if (key == "minecraft:rail[shape=north_south]") {
        return "minecraft:rail";
    }
    return key;
}

} // namespace

namespace {

auto generated_mineshaft_start(jint chunk_x, jint chunk_z, jlong seed)
    -> ::refrain::worldgen::structure_start {
    auto generated = structure::mineshaft_start(
        chunk_pos{.x = chunk_x, .z = chunk_z}, seed, -64, 63, false);
    ::refrain::worldgen::structure_start record{};
    record.structure = generated.structure;
    record.step = generated.step;
    record.chunk_x = chunk_x;
    record.chunk_z = chunk_z;
    record.min_x = generated.bounds.min_x;
    record.min_y = generated.bounds.min_y;
    record.min_z = generated.bounds.min_z;
    record.max_x = generated.bounds.max_x;
    record.max_y = generated.bounds.max_y;
    record.max_z = generated.bounds.max_z;
    for (const auto &piece : generated.pieces) {
        ::refrain::worldgen::structure_piece_record converted{};
        converted.type = piece.type;
        converted.min_x = piece.bounds.min_x;
        converted.min_y = piece.bounds.min_y;
        converted.min_z = piece.bounds.min_z;
        converted.max_x = piece.bounds.max_x;
        converted.max_y = piece.bounds.max_y;
        converted.max_z = piece.bounds.max_z;
        converted.orientation = piece.orientation;
        converted.template_id = piece.template_id;
        converted.template_x = piece.template_origin.x;
        converted.template_y = piece.template_origin.y;
        converted.template_z = piece.template_origin.z;
        converted.template_rotation = piece.template_rotation;
        converted.template_mirror = piece.template_mirror;
        converted.template_pivot_x = piece.template_pivot.x;
        converted.template_pivot_y = piece.template_pivot.y;
        converted.template_pivot_z = piece.template_pivot.z;
        converted.height_position = piece.height_position;
        converted.height_offset = piece.height_offset;
        converted.integrity = piece.integrity;
        converted.large = piece.large;
        converted.beached = piece.beached;
        converted.entrances = piece.entrances;
        converted.sections = piece.sections;
        converted.rails = piece.rails;
        converted.spider = piece.spider;
        converted.two_floored = piece.two_floored;
        converted.mesa = piece.mesa;
        record.pieces.push_back(std::move(converted));
    }
    return record;
}

} // namespace

suite<"worldgen/mineshaft-blocks"> mineshaft_block_tests = [] {
    "the mineshaft pieces match the reference chunk"_test = [&] {
        const char *region_path = std::getenv("REFRAIN_VANILLA_MINE_REGION");
        if (region_path == nullptr) {
            return;
        }
        constexpr jint center_x = -4;
        constexpr jint center_z = 0;
        constexpr jlong seed = 1;
        auto reference = read_chunk(region_path, center_x, center_z);
        expect(reference.has_value());
        if (!reference) {
            return;
        }
        decoded_chunk decoded;
        expect(decode_chunk(*reference, decoded));
        block_state_registry states;
        std::map<std::string, block_state_id, std::less<>> ids;
        const auto intern = [&](std::string_view name) -> block_state_id {
            const std::string key{name};
            if (const auto found = ids.find(key); found != ids.end()) {
                return found->second;
            }
            const auto block = state_block_name(key);
            const bool air = is_air_block_name(block);
            const bool fluid =
                block == "minecraft:water" || block == "minecraft:lava";
            const bool leaves = block.ends_with("_leaves");
            const auto id =
                states
                    .intern(key, {.air = air,
                                  .fluid = fluid,
                                  .water = block == "minecraft:water",
                                  .blocks_motion = !air && !fluid,
                                  .blocks_motion_no_leaves =
                                      !air && !fluid && !leaves})
                    .value_or(no_block_state);
            ids.emplace(key, id);
            return id;
        };
        for (std::size_t id = 0; id < decoded.registry.size(); ++id) {
            (void)intern(
                decoded.registry.name(static_cast<block_state_id>(id)));
        }
        const auto fill_chunk = [&](proto_chunk &chunk,
                                    const decoded_chunk &source) {
            for (jint y = -64; y < 320; ++y) {
                for (jint z = 0; z < 16; ++z) {
                    for (jint x = 0; x < 16; ++x) {
                        const auto index =
                            static_cast<std::size_t>(y + 64) * 256U +
                            static_cast<std::size_t>(z * 16 + x);
                        chunk.set_state(
                            x, y, z,
                            intern(source.registry.name(source.blocks[index])));
                    }
                }
            }
        };
        proto_chunk chunk{states, center_x, center_z, -64, 384};
        fill_chunk(chunk, decoded);
        chunk.set_status(chunk_status::terrain);

        const auto *structures = child(reference->as_compound(), "structures");
        const auto *starts = structures == nullptr
                                 ? nullptr
                                 : child(structures->as_compound(), "starts");
        if (starts == nullptr || starts->as_compound() == nullptr) {
            return;
        }
        const auto *start_compound =
            child(starts->as_compound(), "minecraft:mineshaft");
        const auto *children =
            start_compound == nullptr
                ? nullptr
                : child(start_compound->as_compound(), "Children");
        const auto *children_list =
            children == nullptr ? nullptr : children->as_list();
        expect(children_list != nullptr && !children_list->items.empty());
        if (children_list == nullptr || children_list->items.empty()) {
            return;
        }
        ::refrain::worldgen::structure_start start{};
        start.structure = "minecraft:mineshaft";
        start.step = "underground_structures";
        start.chunk_x = center_x;
        start.chunk_z = center_z;
        for (const auto &piece_value : children_list->items) {
            const auto *piece = piece_value.as_compound();
            const auto *type_tag = child(piece, "id");
            const auto type =
                type_tag == nullptr ? std::nullopt : type_tag->as_string();
            if (!type) {
                continue;
            }
            const auto *bb_tag = child(piece, "BB");
            const auto *bb =
                bb_tag == nullptr ? nullptr : bb_tag->as_compound();
            const auto bounds = bb == nullptr ? std::nullopt : bbox_of(*bb);
            if (!bounds) {
                continue;
            }
            auto *orientation_tag = child(piece, "O");
            const auto orientation = orientation_tag == nullptr
                                         ? std::optional<jint>{}
                                         : int_value(*orientation_tag);
            auto &record = start.pieces.emplace_back();
            record.type = std::string{type->data(), type->size()};
            record.min_x = bounds->min_x;
            record.min_y = bounds->min_y;
            record.min_z = bounds->min_z;
            record.max_x = bounds->max_x;
            record.max_y = bounds->max_y;
            record.max_z = bounds->max_z;
            record.orientation = orientation.value_or(-1);
            record.mesa = false;
            if (const auto *entrances = child(piece, "Entrances");
                entrances != nullptr && entrances->as_list() != nullptr) {
                for (const auto &entrance_value : entrances->as_list()->items) {
                    const auto *entrance = entrance_value.as_compound();
                    if (entrance == nullptr) {
                        continue;
                    }
                    if (const auto entrance_box = bbox_of(*entrance);
                        entrance_box) {
                        record.entrances.push_back(*entrance_box);
                    }
                }
            }
            if (const auto *sections = child(piece, "Sections");
                sections != nullptr) {
                record.sections = int_value(*sections).value_or(0);
            }
            if (const auto *rails = child(piece, "Rails"); rails != nullptr) {
                record.rails = byte_flag(*rails);
            }
            if (const auto *spider = child(piece, "Spider");
                spider != nullptr) {
                record.spider = byte_flag(*spider);
            }
            if (const auto *tf = child(piece, "tf"); tf != nullptr) {
                record.two_floored = byte_flag(*tf);
            }
            if (start.pieces.size() == 1) {
                start.min_x = record.min_x;
                start.min_y = record.min_y;
                start.min_z = record.min_z;
                start.max_x = record.max_x;
                start.max_y = record.max_y;
                start.max_z = record.max_z;
            }
            start.min_x = std::min(start.min_x, record.min_x);
            start.min_y = std::min(start.min_y, record.min_y);
            start.min_z = std::min(start.min_z, record.min_z);
            start.max_x = std::max(start.max_x, record.max_x);
            start.max_y = std::max(start.max_y, record.max_y);
            start.max_z = std::max(start.max_z, record.max_z);
        }
        if (std::getenv("REFRAIN_MINE_GENERATED_START") != nullptr) {
            chunk.add_structure_start(
                generated_mineshaft_start(center_x, center_z, seed));
        } else {
            chunk.add_structure_start(std::move(start));
        }

        std::vector<std::unique_ptr<proto_chunk>> neighbours;
        chunk_region region_view{chunk, 1};
        for (jint dz = -1; dz <= 1; ++dz) {
            for (jint dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dz == 0) {
                    continue;
                }
                auto payload =
                    read_chunk(region_path, center_x + dx, center_z + dz);
                if (!payload) {
                    continue;
                }
                decoded_chunk neighbour_decoded;
                if (!decode_chunk(*payload, neighbour_decoded)) {
                    continue;
                }
                auto neighbour = std::make_unique<proto_chunk>(
                    states, center_x + dx, center_z + dz, -64, 384);
                fill_chunk(*neighbour, neighbour_decoded);
                neighbour->set_status(chunk_status::terrain);
                region_view.attach(*neighbour);
                neighbours.push_back(std::move(neighbour));
            }
        }
        const auto resolve = [&](std::string_view key) -> block_state_id {
            return intern(key);
        };
        const structure::structure_place_context context{
            .region = &region_view,
            .chunk = &chunk,
            .states = &states,
            .state = resolve,
            .registry = &states,
            .seed = seed,
        };
        worldgen_random decoration{
            std::make_unique<xoroshiro_random_source>(seed)};
        const jlong decoration_seed =
            decoration.set_decoration_seed(seed, center_x * 16, center_z * 16);
        worldgen_random placement{std::make_unique<xoroshiro_random_source>(0)};
        placement.set_feature_seed(decoration_seed, 1, 3);
        structure::place_structures(chunk, context, "underground_structures",
                                    placement);

        std::size_t mismatches = 0;
        for (jint y = -64; y < 320; ++y) {
            for (jint z = 0; z < 16; ++z) {
                for (jint x = 0; x < 16; ++x) {
                    const auto index = static_cast<std::size_t>(y + 64) * 256U +
                                       static_cast<std::size_t>(z * 16 + x);
                    const auto expected = mineshaft_canonical(std::string{
                        decoded.registry.name(decoded.blocks[index])});
                    const auto actual = mineshaft_canonical(
                        std::string{states.name(chunk.state_at(x, y, z))});
                    if (expected == actual) {
                        continue;
                    }
                    if (mismatches < 10) {
                        std::fprintf(
                            stderr,
                            "mineshaft mismatch (%d,%d,%d): got %s want %s\n",
                            static_cast<int>(x), static_cast<int>(y),
                            static_cast<int>(z), actual.c_str(),
                            expected.c_str());
                    }
                    ++mismatches;
                }
            }
        }
        std::fprintf(stderr, "mineshaft block mismatches=%zu\n", mismatches);
        expect(mismatches == 0);
    };
};
