// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Fill-stage oracle for a world generated with an empty material rule and no
// carvers or features. Inputs: REFRAIN_VANILLA_JUNGLE_REGION and
// REFRAIN_VANILLA_WORLDGEN.

#include <refrain/core/io.hpp>
#include <refrain/data/nbt.hpp>
#include <refrain/worldgen/chunk/block_state_codec.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/chunk/chunk_region.hpp>
#include <refrain/worldgen/chunk/proto_chunk.hpp>
#include <refrain/worldgen/density/density.hpp>
#include <refrain/worldgen/density/density_router.hpp>
#include <refrain/worldgen/feature/feature_data.hpp>
#include <refrain/worldgen/structure/feature_placement.hpp>
#include <refrain/worldgen/structure/generation.hpp>
#include <refrain/worldgen/structure/generator.hpp>
#include <refrain/worldgen/structure/placement_stage.hpp>
#include <refrain/worldgen/structure/stage.hpp>
#include <refrain/worldgen/structure/start.hpp>
#include <refrain/worldgen/structure/structure_data.hpp>
#include <refrain/worldgen/structure/template.hpp>
#include <refrain/worldgen/terrain/aquifer.hpp>
#include <refrain/worldgen/terrain/beardifier.hpp>
#include <refrain/worldgen/terrain/terrain.hpp>

#include "worldgen/jigsaw_support.hpp"

#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace boost::ut;
using namespace refrain;
using namespace refrain::worldgen;

constexpr jint oracle_min_y = -64;
constexpr jint oracle_height = 384;

auto child(const data::nbt::compound *root, std::string_view key)
    -> const data::nbt::value * {
    if (root == nullptr) {
        return nullptr;
    }
    const auto found = root->find(key);
    return found == root->end() ? nullptr : &found->second;
}

auto chunk_payload(std::span<const std::byte> region, jint chunk_x,
                   jint chunk_z) -> std::optional<std::span<const std::byte>> {
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

auto canonical(std::string name) -> std::string {
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
    std::vector<std::string> parts;
    std::string_view body{name.data() + open + 1, close - open - 1};
    while (true) {
        const auto comma = body.find(',');
        const std::string part{body.substr(0, comma)};
        const bool default_property =
            part == "waterlogged=false" || (fluid && part == "level=0") ||
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
                            part == "open=false" || part == "powered=false"));
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
    if (rebuilt.ends_with("_log[axis=y]") ||
        rebuilt.find("_door[") != std::string::npos) {
        return rebuilt.substr(0, rebuilt.find('['));
    }
    return rebuilt;
}

auto decode(const data::nbt::value &document,
            const std::set<std::string> &motion,
            const std::set<std::string> &motion_no_leaves)
    -> std::optional<decoded> {
    const auto *sections = child(document.as_compound(), "sections");
    if (sections == nullptr || sections->as_list() == nullptr) {
        return std::nullopt;
    }
    decoded out;
    out.blocks.assign(static_cast<std::size_t>(oracle_height) * 256U,
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
            const auto local_y = block_y - oracle_min_y;
            if (local_y < 0 || local_y >= oracle_height) {
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

auto reference_heightmap(const data::nbt::value &document, std::string_view key,
                         std::size_t index) -> std::optional<jint> {
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
    constexpr std::size_t per_long = 64U / 9U;
    const std::size_t word = index / per_long;
    const std::size_t offset = (index % per_long) * 9U;
    if (word >= data->size()) {
        return std::nullopt;
    }
    const auto value =
        (static_cast<std::uint64_t>((*data)[word]) >> offset) & 0x1FFU;
    return static_cast<jint>(value) + oracle_min_y;
}

} // namespace

suite<"worldgen/full-stage"> full_stage_tests = [] {
    "fill and structure placement reproduce the reference chunk"_test = [] {
        const char *data_root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
        const char *region_path = std::getenv("REFRAIN_FULL_STAGE_REGION");
        const char *templates_root = std::getenv("REFRAIN_VANILLA_STRUCTURES");
        if (std::getenv("REFRAIN_FULL_STAGE_STRUCTURES") == nullptr ||
            data_root == nullptr || region_path == nullptr ||
            templates_root == nullptr) {
            return;
        }
        jint chunk_x = 9;
        jint chunk_z = 2;
        jlong seed = 2;
        std::string biome = "minecraft:jungle";
        if (const char *value = std::getenv("REFRAIN_FULL_STAGE_CHUNK_X");
            value != nullptr) {
            chunk_x = std::atoi(value);
        }
        if (const char *value = std::getenv("REFRAIN_FULL_STAGE_CHUNK_Z");
            value != nullptr) {
            chunk_z = std::atoi(value);
        }
        if (const char *value = std::getenv("REFRAIN_FULL_STAGE_SEED");
            value != nullptr) {
            seed = std::strtoll(value, nullptr, 10);
        }
        if (const char *value = std::getenv("REFRAIN_FULL_STAGE_BIOME");
            value != nullptr) {
            biome = value;
        }
        jint start_x = chunk_x;
        jint start_z = chunk_z;
        if (const char *value = std::getenv("REFRAIN_FULL_STAGE_START_CHUNK_X");
            value != nullptr) {
            start_x = std::atoi(value);
        }
        if (const char *value = std::getenv("REFRAIN_FULL_STAGE_START_CHUNK_Z");
            value != nullptr) {
            start_z = std::atoi(value);
        }
        const bool own_start = start_x == chunk_x && start_z == chunk_z;
        const auto region = core::io::read_file(region_path);
        expect(region.has_value()) << region.error().message;
        if (!region) {
            return;
        }
        const auto payload = chunk_payload(*region, chunk_x, chunk_z);
        expect(payload.has_value());
        if (!payload) {
            return;
        }
        const auto document =
            data::nbt::read(*payload, data::nbt::compression::zlib);
        expect(document.has_value()) << document.error().message;
        if (!document) {
            return;
        }
        const auto block_tags = load_block_tags(data_root);
        expect(block_tags.has_value()) << block_tags.error().message;
        if (!block_tags) {
            return;
        }
        const auto resolve_tag_names = [&](std::string_view tag) {
            std::set<std::string> names;
            if (!block_tags->has(tag)) {
                return names;
            }
            auto resolved = block_tags->resolve(tag);
            expect(resolved.has_value());
            if (resolved) {
                names.insert(resolved->begin(), resolved->end());
            }
            return names;
        };
        const auto motion =
            resolve_tag_names("minecraft:blocks_motion_in_heightmap");
        const auto motion_no_leaves =
            resolve_tag_names("minecraft:blocks_motion_in_heightmap_no_leaves");
        const auto reference = decode(*document, motion, motion_no_leaves);
        expect(reference.has_value());
        if (!reference) {
            return;
        }
        auto settings = density::load_noise_settings(
            data_root, "minecraft/overworld", algorithm::xoroshiro, seed);
        expect(settings.has_value()) << settings.error().message;
        if (!settings) {
            return;
        }
        block_state_registry states;
        expect(states.intern("minecraft:air", {.air = true})
                   .value_or(no_block_state) == 0);
        expect(states
                   .intern("minecraft:stone", {.air = false,
                                               .blocks_motion = true,
                                               .blocks_motion_no_leaves = true})
                   .value_or(no_block_state) == 1);
        expect(states
                   .intern("minecraft:water",
                           {.air = false, .fluid = true, .water = true})
                   .value_or(no_block_state) == 2);
        expect(states.intern("minecraft:lava", {.air = false, .fluid = true})
                   .value_or(no_block_state) == 3);
        const terrain::block_ids ids{
            .air = 0, .default_block = 1, .default_fluid = 2, .lava = 3};
        const terrain::fluid_picker picker{.sea_level = settings->sea_level,
                                           .default_fluid = 2,
                                           .lava = 3,
                                           .air = 0};
        // Reference states colour the registry before the templates intern
        // their palettes: template state inference is coarser, so a flower or
        // grass block first seen in a palette would keep wrong heightmap
        // traits.
        for (std::size_t id = 0; id < reference->registry.size(); ++id) {
            const auto state = static_cast<block_state_id>(id);
            const std::string name{reference->registry.name(state)};
            if (!states.find(name).has_value()) {
                (void)states.intern(name, reference->registry.traits(state));
            }
        }
        test_support::prime_template_states(states, templates_root,
                                            "minecraft/structure/village");
        proto_chunk chunk{states, chunk_x, chunk_z, oracle_min_y,
                          oracle_height};
        chunk.set_status(chunk_status::biomes);
        const terrain::proto_fill_options fill_options{
            .air = 0, .default_block = 1, .default_fluid = 2, .lava = 3};

        // Fills one chunk with the beardifier its adapted starts produce.
        const auto fill_chunk =
            [&](proto_chunk &target, const chunk_pos &target_pos,
                const structure::structure_data_set &data,
                const std::vector<structure::structure_start> &starts) {
                const auto beard =
                    structure::collect_beard(starts, data, target_pos);
                density::sampler_context context;
                std::optional<beardifier> beardifier;
                if (!beard.pieces.empty() || !beard.junctions.empty() ||
                    beard.affected_box.has_value()) {
                    beardifier.emplace(beard.pieces, beard.junctions,
                                       beard.affected_box);
                    context.set_field(density::context_key::beardifier,
                                      &*beardifier);
                }
                auto random =
                    new_positional_factory(algorithm::xoroshiro, seed);
                auto aquifer_random = random->from_hash_of("minecraft:aquifer");
                auto factory = aquifer_random->fork_positional();
                const volume chunk_volume{16,           oracle_height,
                                          16,           target_pos.x * 16,
                                          oracle_min_y, target_pos.z * 16};
                terrain::noise_aquifer aquifer{settings->aquifer,
                                               *factory,
                                               chunk_volume,
                                               picker,
                                               ids,
                                               context};
                terrain::fill_proto_chunk(target, fill_options,
                                          *settings->router.final_density,
                                          context, aquifer);
            };

        // Unbearded fill heights: the source `getFirstFreeHeight` reads while
        // the starts stage expands a jigsaw.
        std::map<chunk_pos, std::unique_ptr<proto_chunk>> height_cache;
        const auto height_at = [&](jint x, jint z,
                                   heightmap_kind kind) -> jint {
            const chunk_pos pos{.x = x >> 4, .z = z >> 4};
            auto found = height_cache.find(pos);
            if (found == height_cache.end()) {
                auto height_chunk = std::make_unique<proto_chunk>(
                    states, pos.x, pos.z, oracle_min_y, oracle_height);
                height_chunk->set_status(chunk_status::biomes);
                auto random =
                    new_positional_factory(algorithm::xoroshiro, seed);
                auto aquifer_random = random->from_hash_of("minecraft:aquifer");
                auto factory = aquifer_random->fork_positional();
                const density::sampler_context context;
                const volume chunk_volume{16,           oracle_height,
                                          16,           pos.x * 16,
                                          oracle_min_y, pos.z * 16};
                terrain::noise_aquifer aquifer{settings->aquifer,
                                               *factory,
                                               chunk_volume,
                                               picker,
                                               ids,
                                               context};
                terrain::fill_proto_chunk(*height_chunk, fill_options,
                                          *settings->router.final_density,
                                          context, aquifer);
                found =
                    height_cache.emplace(pos, std::move(height_chunk)).first;
            }
            return found->second->first_available(kind, x & 15, z & 15);
        };

        {
            const auto data = structure::load_structure_data(data_root);
            expect(data.has_value());
            if (!data) {
                return;
            }
            const auto tagged = apply_block_tags(states, *block_tags);
            expect(tagged.has_value()) << tagged.error().message;
            if (!tagged) {
                return;
            }
            structure::structure_template_manager manager{templates_root,
                                                          states};
            const structure::structure_start_context start_context{
                .seed = seed,
                .data = &*data,
                .min_y = oracle_min_y,
                .world_height = oracle_height,
                .sea_level = settings->sea_level,
                .fixed_biome = true,
                .biome_at = [](jint, jint, jint) { return std::uint16_t{0}; },
                .biome_name = [&biome](std::uint16_t) -> std::string_view {
                    return biome;
                },
                .height_at = height_at,
                .templates = &manager,
            };
            // The beardifier sees every start whose adapted bounding box
            // reaches a compared chunk, i.e. starts up to eight chunks away.
            constexpr jint scan_radius = 9;
            std::vector<structure::structure_start> area_starts;
            for (jint dx = -scan_radius; dx <= scan_radius; ++dx) {
                for (jint dz = -scan_radius; dz <= scan_radius; ++dz) {
                    auto found_starts = structure::generate_structure_starts(
                        chunk_pos{.x = chunk_x + dx, .z = chunk_z + dz},
                        start_context);
                    for (auto &start : found_starts) {
                        area_starts.push_back(std::move(start));
                    }
                }
            }
            std::vector<structure::structure_start> starts;
            for (const auto &start : area_starts) {
                if (start.chunk.x == start_x && start.chunk.z == start_z) {
                    starts.push_back(start);
                }
            }
            expect(!starts.empty());
            if (starts.empty()) {
                return;
            }
            fill_chunk(chunk, chunk_pos{.x = chunk_x, .z = chunk_z}, *data,
                       area_starts);
            chunk.set_status(chunk_status::terrain);

            std::vector<std::unique_ptr<proto_chunk>> neighbours;
            chunk_region region_view{chunk, 1};
            for (jint dz = -1; dz <= 1; ++dz) {
                for (jint dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dz == 0) {
                        continue;
                    }
                    auto neighbour = std::make_unique<proto_chunk>(
                        states, chunk_x + dx, chunk_z + dz, oracle_min_y,
                        oracle_height);
                    neighbour->set_status(chunk_status::biomes);
                    fill_chunk(*neighbour,
                               chunk_pos{.x = chunk_x + dx, .z = chunk_z + dz},
                               *data, area_starts);
                    neighbour->set_status(chunk_status::terrain);
                    region_view.attach(*neighbour);
                    neighbours.push_back(std::move(neighbour));
                }
            }

            std::vector<std::vector<structure_piece_record>> all_records;
            for (const auto &start : starts) {
                std::vector<structure_piece_record> piece_records;
                ::refrain::worldgen::structure_start record{
                    .structure = start.structure,
                    .step = start.step,
                    .chunk_x = start.chunk.x,
                    .chunk_z = start.chunk.z,
                    .min_x = start.bounds.min_x,
                    .min_y = start.bounds.min_y,
                    .min_z = start.bounds.min_z,
                    .max_x = start.bounds.max_x,
                    .max_y = start.bounds.max_y,
                    .max_z = start.bounds.max_z,
                };
                for (const auto &piece : start.pieces) {
                    piece_records.push_back(structure::to_piece_record(piece));
                }
                record.pieces = piece_records;
                if (own_start) {
                    chunk.add_structure_start(std::move(record));
                }
                all_records.push_back(std::move(piece_records));
            }
            std::vector<std::string> feature_ids;
            for (const auto &piece_records : all_records) {
                for (const auto &piece : piece_records) {
                    structure::collect_feature_ids(piece, feature_ids);
                }
            }
            std::optional<feature::feature_data_set> feature_data;
            std::optional<structure::structure_feature_placer> feature_placer;
            std::function<bool(std::string_view, worldgen_random &,
                               const block_pos &)>
                place_feature;
            if (!feature_ids.empty()) {
                auto loaded_features = feature::load_feature_data(data_root);
                expect(loaded_features.has_value())
                    << loaded_features.error().message;
                if (!loaded_features) {
                    return;
                }
                feature_data = std::move(*loaded_features);
            }
            const auto resolve = [&](std::string_view key) -> block_state_id {
                if (const auto value = states.find(key); value) {
                    return *value;
                }
                const auto block = state_block_name(key);
                const bool air = is_air_block_name(block);
                const bool leaves = block.ends_with("_leaves");
                const auto interned = states.intern(
                    key, {.air = air,
                          .blocks_motion = !air,
                          .blocks_motion_no_leaves = !air && !leaves});
                return interned.has_value() ? *interned : no_block_state;
            };
            std::unordered_map<std::string, jint> adjusted_heights;
            structure::structure_place_context place_context{
                .region = &region_view,
                .chunk = &chunk,
                .states = &states,
                .state = resolve,
                .templates = &manager,
                .registry = &states,
                .seed = seed,
                .sea_level = settings->sea_level,
                .adjusted_heights = &adjusted_heights,
                .data = &*data,
            };
            if (feature_data) {
                feature_placer.emplace(place_context, *feature_data);
                const auto loaded_features = feature_placer->load(feature_ids);
                expect(loaded_features.has_value())
                    << loaded_features.error().message;
                if (!loaded_features) {
                    return;
                }
                place_feature = [&feature_placer](std::string_view id,
                                                  worldgen_random &random,
                                                  const block_pos &position) {
                    return feature_placer->place(id, random, position);
                };
                place_context.place_feature = place_feature;
            }
            worldgen_random decoration_random{
                std::make_unique<xoroshiro_random_source>(seed)};
            const jlong decoration_seed = decoration_random.set_decoration_seed(
                seed, chunk_x * 16, chunk_z * 16);
            for (const auto &start : starts) {
                const auto index =
                    structure::structure_step_index(*data, start.structure);
                expect(index.has_value());
                if (!index) {
                    continue;
                }
                jint step = 0;
                if (start.step == "surface_structures") {
                    step = 4;
                } else if (start.step == "underground_structures") {
                    step = 3;
                }
                auto placement_random = worldgen_random{
                    std::make_unique<xoroshiro_random_source>(0)};
                placement_random.set_feature_seed(decoration_seed, *index,
                                                  step);
                if (own_start) {
                    structure::place_structures(chunk, place_context,
                                                start.step, placement_random);
                    continue;
                }
                for (const auto &piece : start.pieces) {
                    if (piece.bounds.max_x < chunk_x * 16 ||
                        piece.bounds.min_x > chunk_x * 16 + 15 ||
                        piece.bounds.max_z < chunk_z * 16 ||
                        piece.bounds.min_z > chunk_z * 16 + 15) {
                        continue;
                    }
                    structure::place_structure_piece(
                        place_context, structure::to_piece_record(piece),
                        placement_random);
                }
            }
            for (std::size_t start_index = 0; start_index < starts.size();
                 ++start_index) {
                for (const auto &neighbour : neighbours) {
                    if (neighbour->chunk_x() == chunk_x &&
                        neighbour->chunk_z() == chunk_z) {
                        continue;
                    }
                    bool intersects = false;
                    for (const auto &piece : all_records[start_index]) {
                        if (piece.min_x <= neighbour->chunk_x() * 16 + 15 &&
                            piece.max_x >= neighbour->chunk_x() * 16 &&
                            piece.min_z <= neighbour->chunk_z() * 16 + 15 &&
                            piece.max_z >= neighbour->chunk_z() * 16) {
                            intersects = true;
                        }
                    }
                    if (!intersects) {
                        continue;
                    }
                    const structure::structure_place_context neighbour_context{
                        .region = &region_view,
                        .chunk = neighbour.get(),
                        .states = &states,
                        .state = resolve,
                        .templates = &manager,
                        .registry = &states,
                        .seed = seed,
                        .sea_level = settings->sea_level,
                        .adjusted_heights = &adjusted_heights,
                        .data = &*data,
                        .place_feature = place_feature,
                    };
                    worldgen_random neighbour_random{
                        std::make_unique<xoroshiro_random_source>(0)};
                    const jlong neighbour_decoration_seed =
                        neighbour_random.set_decoration_seed(
                            seed, neighbour->chunk_x() * 16,
                            neighbour->chunk_z() * 16);
                    const auto start_index_value =
                        structure::structure_step_index(
                            *data, starts[start_index].structure);
                    if (!start_index_value) {
                        continue;
                    }
                    const auto &start = starts[start_index];
                    jint step = 0;
                    if (start.step == "surface_structures") {
                        step = 4;
                    } else if (start.step == "underground_structures") {
                        step = 3;
                    }
                    neighbour_random.set_feature_seed(neighbour_decoration_seed,
                                                      *start_index_value, step);
                    for (const auto &piece : all_records[start_index]) {
                        if (piece.max_x < neighbour->chunk_x() * 16 ||
                            piece.min_x > neighbour->chunk_x() * 16 + 15 ||
                            piece.max_z < neighbour->chunk_z() * 16 ||
                            piece.min_z > neighbour->chunk_z() * 16 + 15) {
                            continue;
                        }
                        structure::place_structure_piece(
                            neighbour_context, piece, neighbour_random);
                    }
                }
            }
            if (std::getenv("REFRAIN_MANSION_DEBUG") != nullptr) {
                for (const auto &neighbour : neighbours) {
                    if (neighbour->chunk_x() == 47 &&
                        neighbour->chunk_z() == 5) {
                        std::fprintf(
                            stderr, "mansion east %s\n",
                            std::string{
                                states.name(neighbour->state_at(0, 85, 2))}
                                .c_str());
                    }
                }
            }
            structure::refresh_chunk_shapes(chunk, region_view, states);
        }
        std::size_t mismatches = 0;
        for (jint y = oracle_min_y; y < oracle_min_y + oracle_height; ++y) {
            for (jint z = 0; z < 16; ++z) {
                for (jint x = 0; x < 16; ++x) {
                    const auto index =
                        static_cast<std::size_t>(y - oracle_min_y) * 256U +
                        static_cast<std::size_t>(z) * 16U +
                        static_cast<std::size_t>(x);
                    const auto expected = canonical(std::string{
                        reference->registry.name(reference->blocks[index])});
                    const auto actual = canonical(
                        std::string{states.name(chunk.state_at(x, y, z))});
                    if (expected == actual) {
                        continue;
                    }
                    if (mismatches < 10) {
                        std::fprintf(
                            stderr,
                            "full-stage mismatch (%d,%d,%d): got %s want %s\n",
                            static_cast<int>(x), static_cast<int>(y),
                            static_cast<int>(z), actual.c_str(),
                            expected.c_str());
                    }
                    ++mismatches;
                }
            }
        }
        jint heightmap_mismatches = 0;
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
                const auto expected =
                    reference_heightmap(*document, kind_name, index);
                if (!expected) {
                    continue;
                }
                const auto x = static_cast<jint>(index % 16U);
                const auto z = static_cast<jint>(index / 16U);
                if (chunk.first_available(kind, x, z) != *expected) {
                    if (heightmap_mismatches < 8) {
                        std::fprintf(
                            stderr, "heightmap %s (%d,%d): got %d want %d\n",
                            kind_name, static_cast<int>(x), static_cast<int>(z),
                            static_cast<int>(chunk.first_available(kind, x, z)),
                            static_cast<int>(*expected));
                    }
                    ++heightmap_mismatches;
                }
            }
        }
        std::fprintf(stderr,
                     "full-stage: %zu block mismatches, %d heightmap "
                     "mismatches\n",
                     mismatches, heightmap_mismatches);
        expect(heightmap_mismatches == 0);
        expect(mismatches == 0);

        if (!own_start) {
            return;
        }
        auto generator = structure::structure_generator::create(
            structure::structure_generator_config{
                .data_root = data_root,
                .templates_root = templates_root,
                .biome = biome,
                .seed = seed,
                .min_y = oracle_min_y,
                .height = oracle_height,
                .sea_level = settings->sea_level,
                .known_blocks =
                    [&states] {
                        std::vector<std::string> names;
                        names.reserve(states.size());
                        for (std::size_t id = 0; id < states.size(); ++id) {
                            names.emplace_back(
                                states.name(static_cast<block_state_id>(id)));
                        }
                        return names;
                    }(),
            });
        expect(generator.has_value());
        if (generator) {
            const auto service =
                (*generator)->generate(chunk_pos{.x = chunk_x, .z = chunk_z});
            expect(service.has_value());
            if (service) {
                std::size_t service_mismatches = 0;
                for (jint y = oracle_min_y; y < oracle_min_y + oracle_height;
                     ++y) {
                    for (jint z = 0; z < 16; ++z) {
                        for (jint x = 0; x < 16; ++x) {
                            if (canonical(std::string{
                                    states.name(chunk.state_at(x, y, z))}) !=
                                canonical(std::string{(*service)->table().name(
                                    (*service)->state_at(x, y, z))})) {
                                ++service_mismatches;
                            }
                        }
                    }
                }
                expect(service_mismatches == 0);
                for (const auto kind :
                     {heightmap_kind::world_surface,
                      heightmap_kind::ocean_floor,
                      heightmap_kind::motion_blocking,
                      heightmap_kind::motion_blocking_no_leaves}) {
                    for (jint z = 0; z < 16; ++z) {
                        for (jint x = 0; x < 16; ++x) {
                            expect(chunk.first_available(kind, x, z) ==
                                   (*service)->first_available(kind, x, z));
                        }
                    }
                }
            }
        }
    };
};
