// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Opt-in surface oracle: fills a chunk, runs the real overworld material rule
// tree over it and compares every block with a plain reference world. The
// reference world uses the normal multi-noise biome source, so the biome
// manager, climate sampler and surface depth/secondary/band all take part.
//
// `REFRAIN_VANILLA_WORLDGEN` points at the datapack `data` root,
// `REFRAIN_VANILLA_SURFACE_REGION` at the overworld region file of that world
// (chunks (0,0) to (2,2) force-loaded), and `REFRAIN_VANILLA_ORACLE_SEED` at
// the world seed (default 12345).

#include <refrain/core/io.hpp>
#include <refrain/data/nbt.hpp>
#include <refrain/worldgen/biome/biome_climate.hpp>
#include <refrain/worldgen/biome/biome_manager.hpp>
#include <refrain/worldgen/biome/biome_source.hpp>
#include <refrain/worldgen/biome/overworld_biome_builder.hpp>
#include <refrain/worldgen/carver/carver_data.hpp>
#include <refrain/worldgen/carver/carver_stage.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/chunk/proto_chunk.hpp>
#include <refrain/worldgen/density/density_router.hpp>
#include <refrain/worldgen/feature/feature_data.hpp>
#include <refrain/worldgen/feature/feature_registry.hpp>
#include <refrain/worldgen/feature/feature_stage.hpp>
#include <refrain/worldgen/terrain/surface.hpp>
#include <refrain/worldgen/terrain/terrain.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

using namespace refrain;
using namespace refrain::worldgen;
using namespace refrain::worldgen::surface;

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

auto canonical_state_name(std::string_view name) -> std::string_view {
    if (name.size() > 8 && name.ends_with("[axis=y]")) {
        const auto base = name.substr(0, name.size() - 8);
        if (base.ends_with("_log") || base.ends_with("_stem") ||
            base.ends_with("_wood")) {
            return base;
        }
    }
    constexpr std::string_view leaf_defaults =
        "[distance=7,persistent=false,waterlogged=false]";
    if (name.size() > leaf_defaults.size() && name.ends_with(leaf_defaults)) {
        return name.substr(0, name.size() - leaf_defaults.size());
    }
    return name;
}

auto decode_expected(const data::nbt::value &root, block_states &states)
    -> std::optional<std::vector<state_id>> {
    const auto *sections = child(root.as_compound(), "sections");
    if (sections == nullptr) {
        return std::nullopt;
    }
    std::vector<state_id> expected(
        static_cast<std::size_t>(oracle_height) * 256U, id_air);
    for (const auto &section_value : sections->as_list()->items) {
        const auto *section = section_value.as_compound();
        const auto *y_value = child(section, "Y");
        const auto *states_value = child(section, "block_states");
        if (y_value == nullptr || states_value == nullptr) {
            return std::nullopt;
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
            if (block_y < oracle_min_y ||
                block_y >= oracle_min_y + oracle_height) {
                return std::nullopt;
            }
            const auto local_x = static_cast<jint>(i % 16U);
            const auto local_z = static_cast<jint>((i % 256U) / 16U);
            expected[static_cast<std::size_t>(block_y - oracle_min_y) * 256U +
                     static_cast<std::size_t>(local_z) * 16U +
                     static_cast<std::size_t>(local_x)] = ids[i];
        }
    }
    return expected;
}

struct factory_provider {
    positional_random_factory &root;
    std::unordered_map<std::string, std::unique_ptr<positional_random_factory>>
        cache{};

    auto get(std::string_view name) -> positional_random_factory & {
        std::string key{name};
        if (const auto found = cache.find(key); found != cache.end()) {
            return *found->second;
        }
        auto source = root.from_hash_of(key);
        return *cache.emplace(key, source->fork_positional()).first->second;
    }
};

} // namespace

using namespace boost::ut;

suite<"worldgen/surface-oracle"> surface_oracle_tests = [] {
    "generated chunks match the reference surface blocks"_test = [] {
        const char *data_root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
        const char *carver_region =
            std::getenv("REFRAIN_VANILLA_CARVER_REGION");
        const char *feature_region =
            std::getenv("REFRAIN_VANILLA_FEATURE_REGION");
        const char *feature_real_region =
            std::getenv("REFRAIN_VANILLA_FEATURE_REAL_REGION");
        const bool carver_mode = carver_region != nullptr;
        const bool feature_real_mode = feature_real_region != nullptr;
        const bool feature_mode =
            feature_region != nullptr || feature_real_mode;
        const char *region_path =
            feature_real_mode
                ? feature_real_region
                : (feature_mode
                       ? feature_region
                       : (carver_mode
                              ? carver_region
                              : std::getenv("REFRAIN_VANILLA_SURFACE_REGION")));
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
        const auto documents_result = load_material_documents(data_root);
        expect(documents_result.has_value())
            << documents_result.error().message;
        if (!documents_result) {
            return;
        }
        const material_documents &documents = *documents_result;
        biome_climates climates;
        expect(climates.load(data_root).has_value());
        const biome_temperatures temperatures;
        const std::string *overworld_text = nullptr;
        for (const auto &[id, text] : documents.rules) {
            if (id == "minecraft:overworld") {
                overworld_text = &text;
            }
        }
        expect(overworld_text != nullptr);
        if (overworld_text == nullptr) {
            return;
        }

        noise_registry registry{algorithm::xoroshiro, seed};
        expect(registry.load(data_root).has_value());
        auto root_factory = new_positional_factory(algorithm::xoroshiro, seed);
        multi_noise_biome_source source{overworld_parameters()};
        const bool real_biomes =
            std::getenv("REFRAIN_VANILLA_SURFACE_BIOMES") != nullptr ||
            carver_mode || feature_real_mode;
        const char *fixed_biome_raw =
            std::getenv("REFRAIN_VANILLA_SURFACE_FIXED_BIOME");
        const std::string fixed_biome =
            fixed_biome_raw == nullptr ? "oracle:terrain" : fixed_biome_raw;
        std::optional<feature::feature_data_set> feature_data;
        if (feature_mode) {
            const char *feature_root =
                std::getenv("REFRAIN_VANILLA_FEATURE_DATA");
            if (feature_root == nullptr) {
                return;
            }
            auto loaded = feature::load_feature_data(feature_root);
            expect(loaded.has_value()) << loaded.error().message;
            if (!loaded) {
                return;
            }
            feature_data = std::move(*loaded);
        }
        std::optional<carver::loaded_carvers> carvers;
        if (carver_mode) {
            auto loaded = carver::load_carvers(data_root);
            expect(loaded.has_value()) << loaded.error().message;
            if (!loaded) {
                return;
            }
            carvers = std::move(*loaded);
        }
        std::set<std::string> possible_biomes;
        if (real_biomes) {
            for (const auto &parameter : overworld_parameters()) {
                possible_biomes.insert(parameter.biome);
            }
        } else {
            possible_biomes.insert(fixed_biome);
        }

        constexpr std::array<std::array<jint, 2>, 5> chunks{{
            {0, 0},
            {1, 0},
            {0, 1},
            {1, 1},
            {2, 2},
        }};
        std::size_t mismatches = 0;
        std::size_t compared = 0;
        for (const auto &coordinates : chunks) {
            const jint chunk_x = coordinates[0];
            const jint chunk_z = coordinates[1];
            const auto payload = chunk_payload(*region, chunk_x, chunk_z);
            expect(payload.has_value()) << "missing chunk";
            if (!payload) {
                continue;
            }
            const auto document =
                data::nbt::read(*payload, data::nbt::compression::zlib);
            expect(document.has_value()) << document.error().message;
            if (!document) {
                continue;
            }

            auto settings = density::load_noise_settings(
                data_root, "minecraft/overworld", algorithm::xoroshiro, seed);
            expect(settings.has_value()) << settings.error().message;
            if (!settings) {
                return;
            }

            block_state_registry block_registry;
            auto air_state =
                block_registry.intern("minecraft:air", {.air = true});
            auto stone_state = block_registry.intern(
                settings->default_block, {.air = false,
                                          .blocks_motion = true,
                                          .blocks_motion_no_leaves = true});
            auto water_state = block_registry.intern(
                "minecraft:water",
                {.air = false, .fluid = true, .water = true});
            auto lava_state = block_registry.intern(
                "minecraft:lava", {.air = false, .fluid = true});
            auto bedrock_state = block_registry.intern(
                "minecraft:bedrock", {.air = false,
                                      .blocks_motion = true,
                                      .blocks_motion_no_leaves = true});
            auto coal_ore_state = block_registry.intern(
                "minecraft:coal_ore", {.air = false,
                                       .blocks_motion = true,
                                       .blocks_motion_no_leaves = true});
            auto sand_state = block_registry.intern(
                "minecraft:sand", {.air = false,
                                   .blocks_motion = true,
                                   .blocks_motion_no_leaves = true});
            auto dirt_state = block_registry.intern(
                "minecraft:dirt", {.air = false,
                                   .blocks_motion = true,
                                   .blocks_motion_no_leaves = true});
            auto moss_state =
                block_registry.intern("minecraft:mossy_cobblestone",
                                      {.air = false,
                                       .blocks_motion = true,
                                       .blocks_motion_no_leaves = true});
            expect(air_state.has_value());
            expect(stone_state.has_value());
            expect(water_state.has_value());
            expect(lava_state.has_value());
            expect(bedrock_state.has_value());
            expect(coal_ore_state.has_value());
            expect(sand_state.has_value());
            expect(dirt_state.has_value());
            if (!air_state || !stone_state || !water_state || !lava_state ||
                !bedrock_state || !coal_ore_state || !sand_state) {
                return;
            }
            const std::array<block_state_id, 1> uncarvable{*bedrock_state};
            block_registry.add_tag("minecraft:uncarvable", uncarvable);
            if (dirt_state) {
                const std::array<block_state_id, 1> podzol_ground{*dirt_state};
                block_registry.add_tag(
                    "minecraft:beneath_tree_podzol_replaceable", podzol_ground);
            }
            block_states states{block_registry};

            const auto expected = decode_expected(*document, states);
            expect(expected.has_value()) << "cannot decode reference blocks";
            if (!expected) {
                continue;
            }

            const terrain::block_ids blocks{.air = *air_state,
                                            .default_block = *stone_state,
                                            .default_fluid = *water_state,
                                            .lava = *lava_state};
            const terrain::fluid_picker picker{.sea_level = settings->sea_level,
                                               .default_fluid = *water_state,
                                               .lava = *lava_state,
                                               .air = *air_state};
            const volume chunk_volume{16,           oracle_height,
                                      16,           chunk_x * 16,
                                      oracle_min_y, chunk_z * 16};
            auto aquifer_factory =
                new_positional_factory(algorithm::xoroshiro, seed);
            auto aquifer_random =
                aquifer_factory->from_hash_of("minecraft:aquifer");
            auto aquifer_positional = aquifer_random->fork_positional();
            const density::sampler_context context;
            terrain::noise_aquifer aquifer{settings->aquifer,
                                           *aquifer_positional,
                                           chunk_volume,
                                           picker,
                                           blocks,
                                           context};
            proto_chunk chunk{block_registry, chunk_x, chunk_z, oracle_min_y,
                              oracle_height};
            chunk.set_status(chunk_status::biomes);
            const terrain::proto_fill_options fill_options{
                .air = *air_state,
                .default_block = *stone_state,
                .default_fluid = *water_state,
                .lava = *lava_state,
            };
            terrain::fill_proto_chunk(chunk, fill_options,
                                      *settings->router.final_density, context,
                                      aquifer);

            jint max_block_y = oracle_min_y - 1;
            for (jint y = oracle_min_y + oracle_height - 1; y >= oracle_min_y;
                 --y) {
                bool found = false;
                for (jint z = 0; z < 16 && !found; ++z) {
                    for (jint x = 0; x < 16; ++x) {
                        if (chunk.state_at(x, y, z) != *air_state) {
                            found = true;
                            break;
                        }
                    }
                }
                if (found) {
                    const jint local = y - oracle_min_y;
                    max_block_y = oracle_min_y + (local / 16) * 16 + 15;
                    break;
                }
            }
            const generation_bounds bounds{
                .min_gen_y = oracle_min_y,
                .gen_depth = oracle_height,
                .sea_level = settings->sea_level,
                .min_y = oracle_min_y,
                .max_block_y = max_block_y,
                .chunk_min_x = chunk_x * 16,
                .chunk_min_z = chunk_z * 16,
            };
            surface_system system{states,
                                  bounds,
                                  registry,
                                  *root_factory,
                                  static_cast<state_id>(*stone_state),
                                  static_cast<state_id>(*air_state)};
            factory_provider factories{*root_factory};
            const climate_sampler sampler{*settings->router.temperature,
                                          *settings->router.vegetation,
                                          *settings->router.continents,
                                          *settings->router.erosion,
                                          *settings->router.depth,
                                          *settings->router.ridges,
                                          context};
            climate_biome_resolver resolver{source, sampler};
            biome_manager manager{resolver,
                                  biome_manager::obfuscate_seed(seed)};

            material_providers providers;
            if (real_biomes) {
                providers.biome = [&manager,
                                   &source](jint x, jint y,
                                            jint z) -> std::string_view {
                    return source.biome(manager.get_biome(x, y, z));
                };
            } else {
                providers.biome = [fixed_biome](jint, jint,
                                                jint) -> std::string_view {
                    return fixed_biome;
                };
            }
            providers.preliminary_surface = [&settings, &context](jint x,
                                                                  jint z) {
                return settings->router.chunk_surface_level->sample_value(
                    context, x, 0, z);
            };
            providers.surface_depth = [&system](jint x, jint z) {
                return system.surface_depth(x, z);
            };
            providers.surface_secondary = [&system](jint x, jint z) {
                return system.surface_secondary(x, z);
            };
            providers.band = [&system](jint x, jint y, jint z) {
                return system.band(x, y, z);
            };
            providers.noise = [&registry](std::string_view id) {
                return registry.get(id);
            };
            providers.density =
                [&settings](
                    std::string_view id) -> result<density::sampler_ptr> {
                return density::decode_density("\"" + std::string{id} + "\"",
                                               settings->documents);
            };
            providers.random_factory = [&factories](std::string_view name)
                -> positional_random_factory & { return factories.get(name); };
            providers.cold_enough_to_snow =
                [&climates, &temperatures, sea_level = settings->sea_level](
                    std::string_view biome, jint x, jint y, jint z) {
                    const auto *climate = climates.get(biome);
                    if (climate == nullptr) {
                        return false;
                    }
                    return temperatures.temperature_at(*climate, x, y, z,
                                                       sea_level) < 0.15F;
                };
            providers.possible_biomes = &possible_biomes;

            material_context rule_context{states, bounds, providers, context};
            const auto decoded = decode_rule(*overworld_text);
            expect(decoded.has_value());
            if (!decoded) {
                continue;
            }
            rule_compiler compiler{documents, rule_context};
            const auto compiled = compiler.compile(*decoded);
            if (!compiled) {
                std::fprintf(stderr, "surface compile: %s\n",
                             compiled.error().message.c_str());
            }
            expect(compiled.has_value());
            if (!compiled) {
                continue;
            }

            const auto built =
                system.build_surface(chunk, rule_context, *compiled, climates,
                                     temperatures, providers.biome);
            if (!built) {
                std::fprintf(
                    stderr, "surface (%d,%d): %s\n", static_cast<int>(chunk_x),
                    static_cast<int>(chunk_z), built.error().message.c_str());
            }
            expect(built.has_value());
            if (!built) {
                continue;
            }

            if (carver_mode) {
                carver::carver_generator generator{
                    [&source, &sampler](jint quart_x,
                                        jint quart_z) -> std::string_view {
                        return source.biome(
                            source.noise_biome(sampler, quart_x, 0, quart_z));
                    },
                    carvers->biomes, seed,
                    provider::generation_context{.min_y = oracle_min_y,
                                                 .height = oracle_height,
                                                 .sea_level =
                                                     settings->sea_level}};
                const auto mask =
                    generator.generate(chunk_pos{.x = chunk_x, .z = chunk_z});
                carver::carve_apply_context apply{
                    .aquifer = &aquifer,
                    .states = &block_registry,
                };
                apply.top_material =
                    [&system, &rule_context, &compiled, &chunk](
                        jint block_x, jint block_y, jint block_z,
                        bool under_fluid) -> std::optional<surface::state_id> {
                    return system.top_material(
                        rule_context, *compiled, block_x, block_y, block_z,
                        under_fluid, [&chunk](jint x, jint z) {
                            return chunk.first_available(
                                       heightmap_kind::world_surface_wg, x, z) -
                                   1;
                        });
                };
                carver::apply_carving_mask(chunk, mask, apply);
            }

            chunk.set_status(chunk_status::terrain);
            if (feature_mode) {
                feature::feature_registry feature_registry{block_registry};
                const auto loaded = feature_registry.load(*feature_data);
                expect(loaded.has_value()) << loaded.error().message;
                if (!loaded) {
                    continue;
                }
                std::vector<std::string> plan_biomes;
                std::vector<std::string> candidate_biomes;
                if (feature_real_mode) {
                    std::set<std::string> seen;
                    for (const auto &parameter : overworld_parameters()) {
                        if (seen.insert(parameter.biome).second) {
                            plan_biomes.push_back(parameter.biome);
                        }
                    }
                    biome_grid grid{source,
                                    sampler,
                                    (chunk_x - 1) * 4,
                                    -16,
                                    (chunk_z - 1) * 4,
                                    12,
                                    96,
                                    12};
                    std::set<std::string> candidates;
                    for (jint qx = (chunk_x - 1) * 4; qx < (chunk_x + 2) * 4;
                         ++qx) {
                        for (jint qy = -16; qy < 80; ++qy) {
                            for (jint qz = (chunk_z - 1) * 4;
                                 qz < (chunk_z + 2) * 4; ++qz) {
                                candidates.insert(std::string{
                                    source.biome(grid.at(qx, qy, qz))});
                            }
                        }
                    }
                    candidate_biomes.assign(candidates.begin(),
                                            candidates.end());
                } else {
                    plan_biomes = {fixed_biome};
                    candidate_biomes = {fixed_biome};
                }
                const auto plan = feature::feature_sorter::build(
                    plan_biomes, feature_data->biomes);
                expect(plan.has_value()) << "cannot order features";
                if (!plan) {
                    continue;
                }
                feature::placement_context feature_placement{
                    .region = nullptr,
                    .chunk = &chunk,
                    .states = &block_registry,
                    .generation =
                        provider::generation_context{
                            .min_y = oracle_min_y,
                            .height = oracle_height,
                            .sea_level = settings->sea_level,
                        },
                };
                feature_placement.region = nullptr;
                feature::feature_context feature_ctx{.placement =
                                                         feature_placement};
                // Writes must go through a region so the write radius is
                // enforced even though this oracle only has one chunk.
                chunk_region feature_region_handle{chunk, 1};
                feature_ctx.placement.region = &feature_region_handle;
                feature::decorate(feature_ctx, *plan, feature_registry.placed(),
                                  candidate_biomes, feature_data->biomes, seed);
            }

            for (jint y = oracle_min_y; y < oracle_min_y + oracle_height; ++y) {
                for (jint z = 0; z < 16; ++z) {
                    for (jint x = 0; x < 16; ++x) {
                        const auto index = static_cast<std::size_t>(
                            (y - oracle_min_y) * 256 + z * 16 + x);
                        ++compared;
                        const auto actual = chunk.state_at(x, y, z);
                        if (canonical_state_name(states.name(actual)) ==
                            canonical_state_name(
                                states.name((*expected)[index]))) {
                            continue;
                        }
                        if (mismatches < 8) {
                            std::fprintf(
                                stderr,
                                "chunk (%d,%d) block (%d,%d,%d): got %s want "
                                "%s\n",
                                static_cast<int>(chunk_x),
                                static_cast<int>(chunk_z), static_cast<int>(x),
                                static_cast<int>(y), static_cast<int>(z),
                                std::string{states.name(actual)}.c_str(),
                                std::string{states.name((*expected)[index])}
                                    .c_str());
                        }
                        ++mismatches;
                    }
                }
            }
        }
        expect(compared == 5U * 384U * 256U);
        expect(mismatches == 0) << "surface mismatches: " << mismatches;
    };
};
