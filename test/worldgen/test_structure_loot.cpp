// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Structure containers against reference worlds: the loot table id and the
// loot seed a piece writes must be the ones the reference chunk stores. Each
// case is opt-in and reads a region file from the environment, so a checkout
// without reference worlds skips the cases instead of inventing expectations.

#include <refrain/data/nbt.hpp>
#include <refrain/worldgen/chunk/block_state_codec.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/chunk_generator.hpp>
#include <refrain/worldgen/core/rng.hpp>
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
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace refrain;
using namespace refrain::worldgen;
using namespace refrain::worldgen::structure;

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

/// The feature random the reference's features stage hands to one structure of
/// `chunk`: a xoroshiro stream seeded from the decoration seed and the
/// structure index within its generation step.
auto feature_random(jlong seed, jint chunk_x, jint chunk_z, jint index,
                    jint step) -> worldgen_random {
    worldgen_random decoration{std::make_unique<xoroshiro_random_source>(seed)};
    const jlong decoration_seed =
        decoration.set_decoration_seed(seed, chunk_x * 16, chunk_z * 16);
    worldgen_random random{std::make_unique<xoroshiro_random_source>(0)};
    random.set_feature_seed(decoration_seed, index, step);
    return random;
}

/// The position of `structure_id` among the structures of its generation step
/// and the step ordinal, both read from the datapack the reference world used.
auto feature_index(std::string_view root, std::string_view structure_id)
    -> std::optional<std::pair<jint, jint>> {
    auto data = structure::load_structure_data(root);
    if (!data) {
        return std::nullopt;
    }
    const auto found = data->structures.find(std::string{structure_id});
    const auto index = structure::structure_step_index(*data, structure_id);
    if (found == data->structures.end() || !index) {
        return std::nullopt;
    }
    return std::make_pair(
        *index, structure::generation_step_ordinal(found->second.step));
}

/// Compares every loot container the reference chunk stores against the placed
/// chunk. Returns [checked, mismatches] and prints the first mismatches.
auto compare_loot_containers(const data::nbt::value &reference_root,
                             const proto_chunk &chunk, std::string_view label,
                             std::string_view table_prefix = {},
                             bool compare_seed = true)
    -> std::pair<std::size_t, std::size_t> {
    std::size_t checked = 0;
    std::size_t mismatches = 0;
    const auto *block_entities =
        child(reference_root.as_compound(), "block_entities");
    if (block_entities == nullptr || block_entities->as_list() == nullptr) {
        return {0, 0};
    }
    for (const auto &entry : block_entities->as_list()->items) {
        const auto *entity = entry.as_compound();
        const auto *table_tag = child(entity, "LootTable");
        if (table_tag == nullptr) {
            continue;
        }
        const auto table = table_tag->as_string();
        if (!table_prefix.empty() && !table->starts_with(table_prefix)) {
            continue;
        }
        const auto x = child(entity, "x");
        const auto y = child(entity, "y");
        const auto z = child(entity, "z");
        if (!table || x == nullptr || y == nullptr || z == nullptr) {
            continue;
        }
        const auto world_x = int_value(*x);
        const auto world_y = int_value(*y);
        const auto world_z = int_value(*z);
        if (!world_x || !world_y || !world_z) {
            continue;
        }
        const block_pos position{
            .x = *world_x & 15, .y = *world_y, .z = *world_z & 15};
        if (position.y < chunk.min_y() ||
            position.y >= chunk.min_y() + chunk.height()) {
            continue;
        }
        ++checked;
        const auto *placed = chunk.block_entity_at(position);
        if (placed == nullptr) {
            ++mismatches;
            if (mismatches <= 8) {
                std::fprintf(
                    stderr,
                    "%.*s loot (%d,%d,%d): missing container, want %s\n",
                    static_cast<int>(label.size()), label.data(),
                    static_cast<int>(*world_x), static_cast<int>(*world_y),
                    static_cast<int>(*world_z), std::string{*table}.c_str());
            }
            continue;
        }
        const auto *placed_table = child(placed, "LootTable");
        const auto *placed_seed = child(placed, "LootTableSeed");
        const auto *reference_seed = child(entity, "LootTableSeed");
        const auto expected_seed = reference_seed == nullptr
                                       ? std::nullopt
                                       : reference_seed->as_long();
        const auto actual_seed =
            placed_seed == nullptr ? std::nullopt : placed_seed->as_long();
        const bool table_matches =
            placed_table != nullptr && placed_table->as_string() == table;
        const bool seed_matches =
            !compare_seed ||
            (expected_seed.has_value()
                 ? actual_seed.has_value() && *actual_seed == *expected_seed
                 : !actual_seed.has_value());
        if (!table_matches || !seed_matches) {
            ++mismatches;
            if (mismatches <= 8) {
                std::fprintf(
                    stderr, "%.*s loot (%d,%d,%d): got %s/%lld want %s/%lld\n",
                    static_cast<int>(label.size()), label.data(),
                    static_cast<int>(*world_x), static_cast<int>(*world_y),
                    static_cast<int>(*world_z),
                    placed_table == nullptr
                        ? "<none>"
                        : std::string{placed_table->as_string().value_or(
                                          std::string_view{"<none>"})}
                              .c_str(),
                    static_cast<long long>(actual_seed.value_or(0)),
                    std::string{*table}.c_str(),
                    static_cast<long long>(expected_seed.value_or(0)));
            }
        }
    }
    return {checked, mismatches};
}

} // namespace

using namespace boost::ut;

namespace {

/// Reference chunk palettes only hold blocks the world stored, so a template
/// block entity marker (the structure block) is missing from the registry the
/// template loader resolves against. Register the template-only names first;
/// the loader interns their property combinations from the template.
void register_template_blocks(block_state_registry &registry) {
    for (const auto *name : {"minecraft:structure_block", "minecraft:jigsaw",
                             "minecraft:barrier", "minecraft:structure_void"}) {
        if (!registry.find(name).has_value()) {
            (void)registry.intern(name, {.air = false, .blocks_motion = true});
        }
    }
}

auto resolve_state(block_state_registry &registry) {
    return [&registry](std::string_view key) -> block_state_id {
        if (const auto value = registry.find(key); value.has_value()) {
            return *value;
        }
        const auto block = state_block_name(key);
        const bool air = is_air_block_name(block);
        const auto inserted =
            registry.intern(key, {.air = air, .blocks_motion = !air});
        return inserted.has_value() ? *inserted : no_block_state;
    };
}

auto filler(decoded_chunk &decoded, proto_chunk &chunk,
            const data::nbt::value &reference) -> bool {
    for (jint y = -64; y < 320; ++y) {
        for (jint z = 0; z < 16; ++z) {
            for (jint x = 0; x < 16; ++x) {
                const auto index = static_cast<std::size_t>(y + 64) * 256U +
                                   static_cast<std::size_t>(z * 16 + x);
                chunk.set_state(x, y, z, decoded.blocks[index]);
            }
        }
    }
    // The reference already stores the containers the placement is about to
    // write; clearing their blocks lets the placement create them again.
    const auto *block_entities =
        child(reference.as_compound(), "block_entities");
    if (block_entities == nullptr || block_entities->as_list() == nullptr) {
        return true;
    }
    const auto air = decoded.registry.air();
    for (const auto &entry : block_entities->as_list()->items) {
        const auto *entity = entry.as_compound();
        const auto x = child(entity, "x");
        const auto y = child(entity, "y");
        const auto z = child(entity, "z");
        if (x == nullptr || y == nullptr || z == nullptr) {
            continue;
        }
        const auto world_x = int_value(*x);
        const auto world_y = int_value(*y);
        const auto world_z = int_value(*z);
        if (!world_x || !world_y || !world_z) {
            continue;
        }
        if ((*world_x >> 4) != chunk.chunk_x() ||
            (*world_z >> 4) != chunk.chunk_z()) {
            continue;
        }
        (void)chunk.set_state(*world_x & 15, *world_y, *world_z & 15, air);
    }
    // The piece placement reads heightmaps the reference chunk stores rather
    // than recomputing them from the copied blocks.
    for (jint z = 0; z < 16; ++z) {
        for (jint x = 0; x < 16; ++x) {
            jint surface = chunk.min_y();
            jint floor = chunk.min_y();
            for (jint y = chunk.min_y() + chunk.height() - 1;
                 y >= chunk.min_y(); --y) {
                const auto name =
                    decoded.registry.name(chunk.state_at(x, y, z));
                if (surface == chunk.min_y() &&
                    !is_air_block_name(state_block_name(name))) {
                    surface = y + 1;
                }
                const bool fluid = name.starts_with("minecraft:water") ||
                                   name.starts_with("minecraft:lava");
                if (floor == chunk.min_y() &&
                    !is_air_block_name(state_block_name(name)) && !fluid) {
                    floor = y + 1;
                }
            }
            chunk.heightmap_at(heightmap_kind::world_surface_wg)
                .set_first_available(x, z, surface);
            chunk.heightmap_at(heightmap_kind::ocean_floor_wg)
                .set_first_available(x, z, floor);
            chunk.heightmap_at(heightmap_kind::motion_blocking_no_leaves)
                .set_first_available(x, z, floor);
        }
    }
    return true;
}

} // namespace

suite<"worldgen/structure-loot"> structure_loot_tests = [] {
    "the buried treasure container keeps the reference table and seed"_test =
        [] {
            if (std::getenv("REFRAIN_VANILLA_STRUCTURE_REAL") == nullptr) {
                return;
            }
            const char *region =
                std::getenv("REFRAIN_VANILLA_STRUCTURE_REGION");
            const char *root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
            if (region == nullptr || root == nullptr) {
                return;
            }
            jint chunk_x = 0;
            jint chunk_z = 0;
            if (const char *value =
                    std::getenv("REFRAIN_VANILLA_BURIED_CHUNK_X");
                value != nullptr) {
                chunk_x = std::atoi(value);
            }
            if (const char *value =
                    std::getenv("REFRAIN_VANILLA_BURIED_CHUNK_Z");
                value != nullptr) {
                chunk_z = std::atoi(value);
            }
            jlong seed = 12345;
            if (const char *value = std::getenv("REFRAIN_VANILLA_ORACLE_SEED");
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
            register_template_blocks(decoded.registry);
            const auto feature =
                feature_index(root, "minecraft:buried_treasure");
            expect(feature.has_value());
            if (!feature) {
                return;
            }
            proto_chunk chunk{decoded.registry, chunk_x, chunk_z, -64, 384};
            expect(filler(decoded, chunk, *reference));
            const auto resolve = resolve_state(decoded.registry);
            chunk_region region_view{chunk, 0};
            const structure_place_context context{
                .region = &region_view,
                .chunk = &chunk,
                .states = &decoded.registry,
                .state = resolve,
            };
            const auto start = structure::buried_treasure_start(
                chunk_pos{.x = chunk_x, .z = chunk_z});
            auto random = feature_random(seed, chunk_x, chunk_z, feature->first,
                                         feature->second);
            structure::place_structure_piece(context,
                                             structure_piece_record{
                                                 .type = "minecraft:btp",
                                                 .min_x = start.bounds.min_x,
                                                 .min_y = start.bounds.min_y,
                                                 .min_z = start.bounds.min_z,
                                                 .max_x = start.bounds.max_x,
                                                 .max_y = start.bounds.max_y,
                                                 .max_z = start.bounds.max_z,
                                             },
                                             random);
            const auto [checked, mismatches] =
                compare_loot_containers(*reference, chunk, "buried treasure");
            expect(checked > 0)
                << "no buried treasure container in the reference chunk";
            expect(mismatches == 0);
        };

    "the jungle temple containers keep the reference tables and seeds"_test =
        [] {
            if (std::getenv("REFRAIN_VANILLA_JUNGLE_BLOCKS_REAL") == nullptr) {
                return;
            }
            const char *region = std::getenv("REFRAIN_VANILLA_JUNGLE_REGION");
            const char *root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
            if (region == nullptr || root == nullptr) {
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
            register_template_blocks(decoded.registry);
            const auto feature =
                feature_index(root, "minecraft:jungle_pyramid");
            expect(feature.has_value());
            if (!feature) {
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
            proto_chunk chunk{decoded.registry, chunk_x, chunk_z, -64, 384};
            const auto resolve = resolve_state(decoded.registry);
            chunk_region region_view{chunk, 0};
            const structure_place_context context{
                .region = &region_view,
                .chunk = &chunk,
                .states = &decoded.registry,
                .state = resolve,
                .seed = seed,
            };
            auto random = feature_random(seed, chunk_x, chunk_z, feature->first,
                                         feature->second);
            structure::place_structure_piece(context, piece_record, random);
            const auto [checked, mismatches] =
                compare_loot_containers(*reference, chunk, "jungle temple");
            expect(checked >= 4U) << "expected four jungle temple containers";
            expect(mismatches == 0);
        };

    "the desert pyramid containers keep the reference table and seeds"_test =
        [] {
            if (std::getenv("REFRAIN_VANILLA_PYRAMID_REAL") == nullptr) {
                return;
            }
            const char *region = std::getenv("REFRAIN_VANILLA_PYRAMID_REGION");
            const char *root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
            if (region == nullptr || root == nullptr) {
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
            register_template_blocks(decoded.registry);
            const auto feature =
                feature_index(root, "minecraft:desert_pyramid");
            expect(feature.has_value());
            if (!feature) {
                return;
            }
            const auto start = structure::desert_pyramid_start(
                chunk_pos{.x = chunk_x, .z = chunk_z}, seed);
            expect(start.pieces.size() == 1U);
            if (start.pieces.empty()) {
                return;
            }
            const auto &source = start.pieces.front();
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
            proto_chunk chunk{decoded.registry, chunk_x, chunk_z, -64, 384};
            const auto resolve = resolve_state(decoded.registry);
            chunk_region region_view{chunk, 0};
            const structure_place_context context{
                .region = &region_view,
                .chunk = &chunk,
                .states = &decoded.registry,
                .state = resolve,
            };
            auto random = feature_random(seed, chunk_x, chunk_z, feature->first,
                                         feature->second);
            structure::place_structure_piece(context, piece_record, random);
            const auto [checked, mismatches] = compare_loot_containers(
                *reference, chunk, "desert pyramid", "minecraft:chests/");
            expect(checked >= 4U) << "expected four desert pyramid containers";
            expect(mismatches == 0);
        };

    "the shipwreck containers keep the reference tables and seeds"_test = [] {
        if (std::getenv("REFRAIN_VANILLA_WRECK_BLOCKS_REAL") == nullptr) {
            return;
        }
        const char *region = std::getenv("REFRAIN_VANILLA_WRECK_REGION");
        const char *root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
        const char *templates_root = std::getenv("REFRAIN_VANILLA_STRUCTURES");
        if (region == nullptr || root == nullptr || templates_root == nullptr) {
            return;
        }
        jint chunk_x = 4;
        jint chunk_z = 8;
        jlong seed = 1;
        jint template_y = 57;
        bool beached = true;
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
        if (const char *value = std::getenv("REFRAIN_VANILLA_WRECK_BEACHED");
            value != nullptr) {
            beached = std::atoi(value) != 0;
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
        register_template_blocks(decoded.registry);
        const auto feature =
            feature_index(root, beached ? "minecraft:shipwreck_beached"
                                        : "minecraft:shipwreck");
        expect(feature.has_value());
        if (!feature) {
            return;
        }
        structure_template_manager manager{templates_root, decoded.registry};
        auto start = shipwreck_start(
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
        proto_chunk chunk{decoded.registry, chunk_x, chunk_z, -64, 384};
        const auto resolve = resolve_state(decoded.registry);
        chunk_region region_view{chunk, 0};
        const structure_place_context context{
            .region = &region_view,
            .chunk = &chunk,
            .states = &decoded.registry,
            .state = resolve,
            .templates = &manager,
            .registry = &decoded.registry,
            .seed = seed,
        };
        auto random = feature_random(seed, chunk_x, chunk_z, feature->first,
                                     feature->second);
        place_structure_piece(context, piece_record, random);
        const auto [checked, mismatches] = compare_loot_containers(
            *reference, chunk, "shipwreck", "minecraft:chests/shipwreck");
        expect(checked > 0) << "no shipwreck container in the reference chunk";
        expect(mismatches == 0);
    };

    "the ocean ruin containers keep the reference tables and seeds"_test = [] {
        if (std::getenv("REFRAIN_VANILLA_RUIN_BLOCKS_REAL") == nullptr) {
            return;
        }
        const char *region = std::getenv("REFRAIN_VANILLA_RUIN_REGION");
        const char *root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
        const char *templates_root = std::getenv("REFRAIN_VANILLA_STRUCTURES");
        if (region == nullptr || root == nullptr || templates_root == nullptr) {
            return;
        }
        // The warm ruin's start piece sits at chunk (5, 10); its chest marker
        // and brushable positions fall into chunk (5, 9), which is the chunk
        // that stores them.
        const jint start_x = 5;
        const jint start_z = 10;
        const jint chunk_x = 5;
        const jint chunk_z = 9;
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
        register_template_blocks(decoded.registry);
        const auto feature = feature_index(root, "minecraft:ocean_ruin_warm");
        expect(feature.has_value());
        if (!feature) {
            return;
        }
        structure_template_manager manager{templates_root, decoded.registry};
        const auto start =
            ocean_ruin_start(chunk_pos{.x = start_x, .z = start_z}, seed,
                             manager, true, 0.3F, 0.9F);
        expect(start.pieces.size() == 1U);
        if (start.pieces.empty()) {
            return;
        }
        const auto &piece = start.pieces.front();
        const structure_piece_record piece_record{
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
            .template_y = 69,
            .template_z = piece.template_origin.z,
            .template_rotation = piece.template_rotation,
            .template_mirror = piece.template_mirror,
            .template_pivot_x = piece.template_pivot.x,
            .template_pivot_y = piece.template_pivot.y,
            .template_pivot_z = piece.template_pivot.z,
            .height_position = 69,
            .integrity = piece.integrity,
            .large = piece.large,
        };
        proto_chunk chunk{decoded.registry, chunk_x, chunk_z, -64, 384};
        const auto resolve = resolve_state(decoded.registry);
        chunk_region region_view{chunk, 0};
        const structure_place_context context{
            .region = &region_view,
            .chunk = &chunk,
            .states = &decoded.registry,
            .state = resolve,
            .templates = &manager,
            .registry = &decoded.registry,
            .seed = seed,
        };
        auto random = feature_random(seed, chunk_x, chunk_z, feature->first,
                                     feature->second);
        place_structure_piece(context, piece_record, random);
        const auto [chests, chest_mismatches] =
            compare_loot_containers(*reference, chunk, "ocean ruin chest",
                                    "minecraft:chests/underwater_ruin");
        const auto [brushes, brush_mismatches] =
            compare_loot_containers(*reference, chunk, "ocean ruin brush",
                                    "minecraft:archaeology/ocean_ruin");
        expect(chests > 0) << "no ocean ruin chest in the reference chunk";
        expect(brushes > 0) << "no ocean ruin brushable in the reference chunk";
        expect(chest_mismatches == 0);
        expect(brush_mismatches == 0);
    };

    "the stronghold containers keep the reference tables and seeds"_test = [] {
        const char *region = std::getenv("REFRAIN_VANILLA_STRONGHOLD_REGION");
        const char *root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
        if (region == nullptr || root == nullptr) {
            return;
        }
        // The stronghold-only oracle world keeps one stronghold container per
        // library and corridor piece: chunk (105, 51) holds a library chest
        // and chunk (107, 51) a corridor chest. The saved container seed is
        // part of every comparison.
        std::vector<chunk_pos> chunks{{.x = 105, .z = 51}, {.x = 107, .z = 51}};
        const char *chunk_x_env =
            std::getenv("REFRAIN_VANILLA_STRONGHOLD_LOOT_CHUNK_X");
        const char *chunk_z_env =
            std::getenv("REFRAIN_VANILLA_STRONGHOLD_LOOT_CHUNK_Z");
        if (chunk_x_env != nullptr && chunk_z_env != nullptr) {
            chunks = {chunk_pos{.x = std::atoi(chunk_x_env),
                                .z = std::atoi(chunk_z_env)}};
        }
        const jlong seed = 1;
        const char *biome_override =
            std::getenv("REFRAIN_VANILLA_STRONGHOLD_LOOT_BIOME");
        const std::string biome =
            biome_override == nullptr ? "minecraft:plains" : biome_override;
        const auto feature = feature_index(root, "minecraft:stronghold");
        expect(feature.has_value());
        if (!feature) {
            return;
        }
        // The smooth-stone selector and the interior boxes read the block a
        // position holds before they draw for it, so the seed a container
        // receives depends on the terrain the reference placement saw. The
        // saved chunk already holds the finished stronghold, so replaying onto
        // it would read the wrong states. Generate the pre-structure terrain
        // instead: the oracle world is a reference-mode fill with a fixed
        // biome, no surface pass and no carvers.
        auto generator = chunk_generator::create(chunk_generator_config{
            .data_root = root,
            .templates_root = "",
            .seed = seed,
            .kind = dimension_kind::overworld,
            .fixed_biome = biome,
            .target = generation_target::terrain,
            .structure_sets = {},
            .reference_world = true,
        });
        expect(generator.has_value()) << "the reference terrain generator";
        if (!generator) {
            return;
        }
        for (const auto &target : chunks) {
            const auto reference = read_chunk(region, target.x, target.z);
            expect(reference.has_value());
            if (!reference) {
                continue;
            }
            // The chunk stores a reference to the start piece's chunk; the
            // start itself is owned by that chunk and its pieces reach this
            // one.
            const auto *structures =
                child(reference->as_compound(), "structures");
            const auto *references =
                structures == nullptr
                    ? nullptr
                    : child(structures->as_compound(), "References");
            const auto *stronghold_reference =
                references == nullptr
                    ? nullptr
                    : child(references->as_compound(), "minecraft:stronghold");
            const auto packed = stronghold_reference == nullptr
                                    ? std::nullopt
                                    : stronghold_reference->as_long_array();
            expect(packed.has_value() && !packed->empty());
            if (!packed || packed->empty()) {
                continue;
            }
            const jint start_x = static_cast<jint>((*packed)[0] & 0xFFFFFFFFLL);
            const jint start_z = static_cast<jint>((*packed)[0] >> 32);
            const auto start = stronghold_start(
                chunk_pos{.x = start_x, .z = start_z}, seed, -64, 63);
            expect(!start.pieces.empty());
            if (start.pieces.empty()) {
                continue;
            }
            const auto base = (*generator)->generate(target);
            expect(base.has_value());
            if (!base) {
                continue;
            }
            auto &chunk = **base;
            auto &states = (*generator)->states();
            const auto resolve = resolve_state(states);
            chunk_region region_view{chunk, 0};
            const structure_place_context context{
                .region = &region_view,
                .chunk = &chunk,
                .states = &states,
                .state = resolve,
                .registry = &states,
                .seed = seed,
            };
            auto random = feature_random(seed, target.x, target.z,
                                         feature->first, feature->second);
            for (const auto &piece : start.pieces) {
                if (piece.bounds.max_x < target.x * 16 ||
                    piece.bounds.min_x > target.x * 16 + 15 ||
                    piece.bounds.max_z < target.z * 16 ||
                    piece.bounds.min_z > target.z * 16 + 15) {
                    continue;
                }
                place_structure_piece(context, to_piece_record(piece), random);
            }
            const auto [checked, mismatches] = compare_loot_containers(
                *reference, chunk, "stronghold", "minecraft:chests/stronghold");
            expect(checked > 0)
                << "no stronghold container in the reference chunk";
            expect(mismatches == 0);
        }
    };

    "the nether fortress containers keep the reference tables and seeds"_test =
        [] {
            const char *region = std::getenv("REFRAIN_VANILLA_FORTRESS_REGION");
            const char *root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
            if (region == nullptr || root == nullptr) {
                return;
            }
            // The nether-complexes oracle world stores a fortress chest in
            // chunk (17, -8); that piece is owned by the start at (21, -8).
            jint chunk_x = 17;
            jint chunk_z = -8;
            if (const char *value =
                    std::getenv("REFRAIN_VANILLA_FORTRESS_LOOT_CHUNK_X");
                value != nullptr) {
                chunk_x = std::atoi(value);
            }
            if (const char *value =
                    std::getenv("REFRAIN_VANILLA_FORTRESS_LOOT_CHUNK_Z");
                value != nullptr) {
                chunk_z = std::atoi(value);
            }
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
            const auto *structures =
                child(reference->as_compound(), "structures");
            const auto *references =
                structures == nullptr
                    ? nullptr
                    : child(structures->as_compound(), "References");
            const auto *fortress_reference =
                references == nullptr
                    ? nullptr
                    : child(references->as_compound(), "minecraft:fortress");
            const auto packed = fortress_reference == nullptr
                                    ? std::nullopt
                                    : fortress_reference->as_long_array();
            expect(packed.has_value() && !packed->empty());
            if (!packed || packed->empty()) {
                return;
            }
            const jint start_x = static_cast<jint>((*packed)[0] & 0xFFFFFFFFLL);
            const jint start_z = static_cast<jint>((*packed)[0] >> 32);
            const auto feature = feature_index(root, "minecraft:fortress");
            expect(feature.has_value());
            if (!feature) {
                return;
            }
            const auto start =
                fortress_start(chunk_pos{.x = start_x, .z = start_z}, seed);
            expect(!start.pieces.empty());
            if (start.pieces.empty()) {
                return;
            }
            // A fortress piece draws randomness only for its container, after
            // state-independent boxes, so replaying onto the saved block grid
            // reproduces the reference placement stream.
            proto_chunk chunk{decoded.registry, chunk_x, chunk_z, 0, 256};
            expect(filler(decoded, chunk, *reference));
            const auto resolve = resolve_state(decoded.registry);
            chunk_region region_view{chunk, 0};
            const structure_place_context context{
                .region = &region_view,
                .chunk = &chunk,
                .states = &decoded.registry,
                .state = resolve,
                .registry = &decoded.registry,
                .seed = seed,
            };
            auto random = feature_random(seed, chunk_x, chunk_z, feature->first,
                                         feature->second);
            for (const auto &piece : start.pieces) {
                if (piece.bounds.max_x < chunk_x * 16 ||
                    piece.bounds.min_x > chunk_x * 16 + 15 ||
                    piece.bounds.max_z < chunk_z * 16 ||
                    piece.bounds.min_z > chunk_z * 16 + 15) {
                    continue;
                }
                place_structure_piece(context, to_piece_record(piece), random);
            }
            const auto [checked, mismatches] =
                compare_loot_containers(*reference, chunk, "fortress",
                                        "minecraft:chests/nether_bridge");
            expect(checked > 0)
                << "no nether fortress container in the reference chunk";
            expect(mismatches == 0);
        };

    "the igloo basement chest keeps the reference table and seed"_test = [] {
        if (std::getenv("REFRAIN_VANILLA_IGLOO_BASEMENT_REAL") == nullptr) {
            return;
        }
        const char *region =
            std::getenv("REFRAIN_VANILLA_IGLOO_BASEMENT_REGION");
        if (region == nullptr) {
            region = std::getenv("REFRAIN_VANILLA_IGLOO_REGION");
        }
        const char *root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
        const char *templates_root = std::getenv("REFRAIN_VANILLA_STRUCTURES");
        if (region == nullptr || root == nullptr || templates_root == nullptr) {
            return;
        }
        const jint chunk_x = 9;
        const jint chunk_z = 2;
        const jlong seed = 3;
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
        register_template_blocks(decoded.registry);
        const auto feature = feature_index(root, "minecraft:igloo");
        expect(feature.has_value());
        if (!feature) {
            return;
        }
        structure_template_manager manager{templates_root, decoded.registry};
        const auto start =
            igloo_start(chunk_pos{.x = chunk_x, .z = chunk_z}, seed, manager);
        expect(!start.pieces.empty());
        if (start.pieces.empty()) {
            return;
        }
        const auto *structures = child(reference->as_compound(), "structures");
        const auto *starts = structures == nullptr
                                 ? nullptr
                                 : child(structures->as_compound(), "starts");
        const auto *igloo_start =
            starts == nullptr ? nullptr
                              : child(starts->as_compound(), "minecraft:igloo");
        const auto *saved_children =
            igloo_start == nullptr
                ? nullptr
                : child(igloo_start->as_compound(), "Children");
        const auto *saved =
            saved_children == nullptr ? nullptr : saved_children->as_list();
        expect(saved != nullptr && !saved->items.empty());
        if (saved == nullptr || saved->items.empty()) {
            return;
        }
        const auto *saved_bb = child(saved->items.front().as_compound(), "BB");
        const auto saved_values =
            saved_bb == nullptr ? std::nullopt : saved_bb->as_int_array();
        expect(saved_values.has_value() && saved_values->size() == 6U);
        if (!saved_values || saved_values->size() != 6U) {
            return;
        }
        // The basement chest sits in the bottom template, which is placed
        // first. Its world y is the generated box shifted by the reference's
        // postProcess height query; that query returned a surface height the
        // saved chunk no longer shows, so pin the query result the saved box
        // implies.
        const auto &bottom = start.pieces.front();
        expect(bottom.template_id == "minecraft:igloo/bottom");
        const jint shift = (*saved_values)[1] - bottom.bounds.min_y;
        // The basement template's offset from the start position.
        const block_pos offset{.x = 0, .y = -3, .z = -2};
        const auto transform = template_transform{
            .mirror = static_cast<template_mirror>(bottom.template_mirror),
            .rotation =
                static_cast<template_rotation>(bottom.template_rotation),
            .pivot = bottom.template_pivot,
        };
        const auto entrance_local = transform_position(
            block_pos{.x = 3 - offset.x, .y = 0, .z = -offset.z}, transform);
        const jint entrance_x = bottom.template_origin.x + entrance_local.x;
        const jint entrance_z = bottom.template_origin.z + entrance_local.z;
        proto_chunk chunk{decoded.registry, chunk_x, chunk_z, -64, 384};
        expect(filler(decoded, chunk, *reference));
        chunk_region region_view{chunk, 1};
        auto *entrance_chunk =
            region_view.find(entrance_x >> 4, entrance_z >> 4);
        expect(entrance_chunk != nullptr);
        if (entrance_chunk == nullptr) {
            return;
        }
        entrance_chunk->heightmap_at(heightmap_kind::world_surface_wg)
            .set_first_available(entrance_x & 15, entrance_z & 15, 91 + shift);
        const auto resolve = resolve_state(decoded.registry);
        const structure_place_context context{
            .region = &region_view,
            .chunk = &chunk,
            .states = &decoded.registry,
            .state = resolve,
            .templates = &manager,
            .registry = &decoded.registry,
            .seed = seed,
        };
        auto random = feature_random(seed, chunk_x, chunk_z, feature->first,
                                     feature->second);
        const structure_piece_record piece_record{
            .type = bottom.type,
            .min_x = bottom.bounds.min_x,
            .min_y = bottom.bounds.min_y,
            .min_z = bottom.bounds.min_z,
            .max_x = bottom.bounds.max_x,
            .max_y = bottom.bounds.max_y,
            .max_z = bottom.bounds.max_z,
            .orientation = bottom.orientation,
            .template_id = bottom.template_id,
            .template_x = bottom.template_origin.x,
            .template_y = bottom.template_origin.y,
            .template_z = bottom.template_origin.z,
            .template_rotation = bottom.template_rotation,
            .template_mirror = bottom.template_mirror,
            .template_pivot_x = bottom.template_pivot.x,
            .template_pivot_y = bottom.template_pivot.y,
            .template_pivot_z = bottom.template_pivot.z,
        };
        place_structure_piece(context, piece_record, random);
        const auto [checked, mismatches] = compare_loot_containers(
            *reference, chunk, "igloo", "minecraft:chests/igloo");
        expect(checked > 0) << "no igloo chest in the reference chunk";
        expect(mismatches == 0);
    };

    "a wrong or missing container id is reported as a mismatch"_test = [] {
        // A reference container and a placed chunk that disagree on the loot
        // table id must fail the comparison; a family that silently names the
        // wrong, or no, id therefore cannot pass its reference case.
        block_state_registry registry;
        const auto chest =
            registry.intern("minecraft:chest[facing=north]",
                            {.air = false, .blocks_motion = true});
        expect(chest.has_value());
        if (!chest) {
            return;
        }
        const block_pos position{.x = 3, .y = 12, .z = 4};
        data::nbt::compound expected_entity;
        expected_entity["id"] =
            data::nbt::value{std::string{"minecraft:chest"}};
        expected_entity["x"] = data::nbt::value{static_cast<jint>(3)};
        expected_entity["y"] = data::nbt::value{static_cast<jint>(12)};
        expected_entity["z"] = data::nbt::value{static_cast<jint>(4)};
        expected_entity["LootTable"] = data::nbt::value{
            std::string{"minecraft:chests/stronghold_corridor"}};
        expected_entity["LootTableSeed"] =
            data::nbt::value{static_cast<jlong>(42)};
        auto entities = data::nbt::make_list(data::nbt::tag_type::compound);
        (void)entities.push(data::nbt::value{std::move(expected_entity)});
        data::nbt::compound reference_root;
        reference_root["block_entities"] =
            data::nbt::value{std::move(entities)};
        const data::nbt::value reference{std::move(reference_root)};
        proto_chunk chunk{registry, 0, 0, -64, 384};
        const auto place = [&](std::optional<std::string_view> table) {
            data::nbt::compound entity;
            entity["id"] = data::nbt::value{std::string{"minecraft:chest"}};
            entity["LootTableSeed"] = data::nbt::value{static_cast<jlong>(42)};
            if (table.has_value()) {
                entity["LootTable"] = data::nbt::value{std::string{*table}};
            }
            chunk.set_block_entity(position, entity);
        };
        place(std::string_view{"minecraft:chests/stronghold_corridor"});
        expect(compare_loot_containers(reference, chunk, "matching").second ==
               0U);
        place(std::string_view{"minecraft:chests/simple_dungeon"});
        expect(compare_loot_containers(reference, chunk, "wrong id").second ==
               1U);
        place(std::nullopt);
        expect(compare_loot_containers(reference, chunk, "missing id").second ==
               1U);
        (void)chest;
    };
};
