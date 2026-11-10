// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Full-stage oracle for a reference world generated with an empty material
// rule and no carvers or features. Inputs: REFRAIN_FULL_STAGE_REGION,
// REFRAIN_VANILLA_WORLDGEN and REFRAIN_VANILLA_STRUCTURES.

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

#include "worldgen/full_stage_support.hpp"
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
using namespace refrain::worldgen::test_support;

} // namespace

namespace {

/// Runs one full-stage reference comparison. The environment supplies the
/// region, candidate chunk, start chunk, seed, biome and structure switch; the
/// structure-independent defaults let the legacy synthetic case run without
/// them, while the reference cases require every candidate value.
void run_full_stage_case(std::string_view prefix,
                         std::optional<jint> default_chunk_x,
                         std::optional<jint> default_chunk_z,
                         std::optional<jlong> default_seed,
                         std::string_view default_biome,
                         std::string_view default_template_dirs,
                         bool compare_block_entities = false) {
    const std::string base{"REFRAIN_" + std::string{prefix}};
    const auto env = [&base](std::string_view suffix) -> std::string {
        const std::string key = base + "_" + std::string{suffix};
        const char *value = std::getenv(key.c_str());
        return value == nullptr ? std::string{} : std::string{value};
    };
    const char *data_root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
    const char *templates_root = std::getenv("REFRAIN_VANILLA_STRUCTURES");
    const std::string region_path = env("REGION");
    const std::string chunk_x_raw = env("CHUNK_X");
    const std::string chunk_z_raw = env("CHUNK_Z");
    const std::string seed_raw = env("SEED");
    std::optional<jint> chunk_x_opt;
    if (!chunk_x_raw.empty()) {
        chunk_x_opt = std::atoi(chunk_x_raw.c_str());
    } else if (default_chunk_x.has_value()) {
        chunk_x_opt = *default_chunk_x;
    }
    std::optional<jint> chunk_z_opt;
    if (!chunk_z_raw.empty()) {
        chunk_z_opt = std::atoi(chunk_z_raw.c_str());
    } else if (default_chunk_z.has_value()) {
        chunk_z_opt = *default_chunk_z;
    }
    std::optional<jlong> seed_opt;
    if (!seed_raw.empty()) {
        seed_opt = std::strtoll(seed_raw.c_str(), nullptr, 10);
    } else if (default_seed.has_value()) {
        seed_opt = *default_seed;
    }
    if (data_root == nullptr || templates_root == nullptr ||
        region_path.empty() || !chunk_x_opt.has_value() ||
        !chunk_z_opt.has_value() || !seed_opt.has_value() ||
        env("STRUCTURES").empty()) {
        return;
    }
    const jint chunk_x = *chunk_x_opt;
    const jint chunk_z = *chunk_z_opt;
    const jlong seed = *seed_opt;
    std::string biome{default_biome};
    if (const auto value = env("BIOME"); !value.empty()) {
        biome = value;
    }
    const std::string start_x_raw = env("START_CHUNK_X");
    const std::string start_z_raw = env("START_CHUNK_Z");
    const jint start_x =
        start_x_raw.empty() ? chunk_x : std::atoi(start_x_raw.c_str());
    const jint start_z =
        start_z_raw.empty() ? chunk_z : std::atoi(start_z_raw.c_str());
    const bool own_start = start_x == chunk_x && start_z == chunk_z;
    dimension_kind dimension = dimension_kind::overworld;
    if (const auto value = env("DIMENSION"); !value.empty()) {
        if (value == "the_nether") {
            dimension = dimension_kind::nether;
        } else if (value == "the_end") {
            dimension = dimension_kind::end;
        }
    }
    std::string template_dirs{default_template_dirs};
    if (const auto value = env("TEMPLATE_DIRS"); !value.empty()) {
        template_dirs = value;
    }
    const auto &profile = dimension_profile_for(dimension);
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
    const auto reference = decode(*document, motion, motion_no_leaves,
                                  profile.min_y, profile.height);
    expect(reference.has_value());
    if (!reference) {
        return;
    }
    auto settings = density::load_noise_settings(
        data_root, profile.noise_settings, algorithm::xoroshiro, seed);
    expect(settings.has_value()) << settings.error().message;
    if (!settings) {
        return;
    }
    const jint generation_min_y = std::max(profile.min_y, settings->min_y);
    const jint generation_max_y = std::min(profile.min_y + profile.height,
                                           settings->min_y + settings->height);
    const jint generation_height = generation_max_y - generation_min_y;
    block_state_registry states;
    expect(states.intern("minecraft:air", {.air = true})
               .value_or(no_block_state) == 0);
    const auto default_block =
        states
            .intern(settings->default_block, {.air = false,
                                              .blocks_motion = true,
                                              .blocks_motion_no_leaves = true})
            .value_or(no_block_state);
    block_state_id default_fluid = states.air();
    if (settings->default_fluid != "minecraft:air") {
        const bool water = settings->default_fluid == "minecraft:water";
        default_fluid =
            states
                .intern(settings->default_fluid,
                        {.air = false, .fluid = true, .water = water})
                .value_or(no_block_state);
    }
    const auto lava =
        states.intern("minecraft:lava", {.air = false, .fluid = true})
            .value_or(no_block_state);
    const terrain::block_ids ids{.air = states.air(),
                                 .default_block = default_block,
                                 .default_fluid = default_fluid,
                                 .lava = lava};
    const terrain::fluid_picker picker{.sea_level = settings->sea_level,
                                       .default_fluid = default_fluid,
                                       .lava = lava,
                                       .air = states.air()};
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
    std::vector<std::string> primed_directories;
    std::size_t directory_start = 0;
    while (directory_start <= template_dirs.size()) {
        const auto comma = template_dirs.find(',', directory_start);
        const auto directory = template_dirs.substr(
            directory_start, comma == std::string::npos
                                 ? std::string::npos
                                 : comma - directory_start);
        if (!directory.empty()) {
            primed_directories.push_back(directory);
        }
        if (comma == std::string::npos) {
            break;
        }
        directory_start = comma + 1;
    }
    if (dimension == dimension_kind::nether) {
        primed_directories.emplace_back("minecraft/structure/bastion");
        primed_directories.emplace_back("minecraft/structure/nether_fossils");
    }
    for (const auto &directory : primed_directories) {
        test_support::prime_template_states(states, templates_root, directory);
    }
    proto_chunk chunk{states, chunk_x, chunk_z, profile.min_y, profile.height};
    chunk.set_status(chunk_status::biomes);
    const terrain::proto_fill_options fill_options{
        .air = states.air(),
        .default_block = default_block,
        .default_fluid = default_fluid,
        .lava = lava};
    const terrain::proto_fill_volume fill_volume{.min_y = generation_min_y,
                                                 .height = generation_height};

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
            aquifer_holder holder;
            if (settings->aquifers) {
                auto random =
                    new_positional_factory(algorithm::xoroshiro, seed);
                auto aquifer_random = random->from_hash_of("minecraft:aquifer");
                holder.factory = aquifer_random->fork_positional();
                const volume chunk_volume{16,
                                          generation_height,
                                          16,
                                          target_pos.x * 16,
                                          generation_min_y,
                                          target_pos.z * 16};
                holder.aquifer = std::make_unique<terrain::noise_aquifer>(
                    settings->aquifer, *holder.factory, chunk_volume, picker,
                    ids, context);
            } else {
                holder.aquifer =
                    std::make_unique<terrain::disabled_aquifer>(picker);
            }
            terrain::fill_proto_chunk(target, fill_options, fill_volume,
                                      *settings->router.final_density, context,
                                      *holder.aquifer);
        };

    // Unbearded fill heights: the source `getFirstFreeHeight` reads while
    // the starts stage expands a jigsaw.
    std::map<chunk_pos, std::unique_ptr<proto_chunk>> height_cache;
    const auto height_at = [&](jint x, jint z, heightmap_kind kind) -> jint {
        const chunk_pos pos{.x = x >> 4, .z = z >> 4};
        auto found = height_cache.find(pos);
        if (found == height_cache.end()) {
            auto height_chunk = std::make_unique<proto_chunk>(
                states, pos.x, pos.z, profile.min_y, profile.height);
            height_chunk->set_status(chunk_status::biomes);
            aquifer_holder holder;
            const density::sampler_context context;
            if (settings->aquifers) {
                auto random =
                    new_positional_factory(algorithm::xoroshiro, seed);
                auto aquifer_random = random->from_hash_of("minecraft:aquifer");
                holder.factory = aquifer_random->fork_positional();
                const volume chunk_volume{
                    16,         generation_height, 16,
                    pos.x * 16, generation_min_y,  pos.z * 16};
                holder.aquifer = std::make_unique<terrain::noise_aquifer>(
                    settings->aquifer, *holder.factory, chunk_volume, picker,
                    ids, context);
            } else {
                holder.aquifer =
                    std::make_unique<terrain::disabled_aquifer>(picker);
            }
            terrain::fill_proto_chunk(*height_chunk, fill_options, fill_volume,
                                      *settings->router.final_density, context,
                                      *holder.aquifer);
            found = height_cache.emplace(pos, std::move(height_chunk)).first;
        }
        return found->second->first_available(kind, x & 15, z & 15);
    };
    const auto block_at = [&](jint x, jint y, jint z) -> block_state_id {
        const chunk_pos pos{.x = x >> 4, .z = z >> 4};
        (void)height_at(x, z, heightmap_kind::world_surface_wg);
        const auto found = height_cache.find(pos);
        if (found == height_cache.end()) {
            return states.air();
        }
        return found->second->state_at(x & 15, y, z & 15);
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
        structure::structure_template_manager manager{templates_root, states};
        const structure::structure_start_context start_context{
            .seed = seed,
            .data = &*data,
            .min_y = profile.min_y,
            .world_height = profile.height,
            .sea_level = settings->sea_level,
            .generation_min_y = generation_min_y,
            .generation_height = generation_height,
            .fixed_biome = true,
            .biome_at = [](jint, jint, jint) { return std::uint16_t{0}; },
            .biome_name = [&biome](std::uint16_t) -> std::string_view {
                return biome;
            },
            .height_at = height_at,
            .block_at = block_at,
            .states = &states,
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
                    states, chunk_x + dx, chunk_z + dz, profile.min_y,
                    profile.height);
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
            const jint step = structure::generation_step_ordinal(start.step);
            auto placement_random =
                worldgen_random{std::make_unique<xoroshiro_random_source>(0)};
            placement_random.set_feature_seed(decoration_seed, *index, step);
            auto start_placement = place_context;
            if (!start.pieces.empty()) {
                start_placement.reference = structure::start_reference_position(
                    start.pieces.front().bounds);
            }
            if (own_start) {
                structure::place_structures(chunk, start_placement, start.step,
                                            placement_random);
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
                    start_placement, structure::to_piece_record(piece),
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
                auto neighbour_context = place_context;
                neighbour_context.region = &region_view;
                neighbour_context.chunk = neighbour.get();
                neighbour_context.templates = &manager;
                neighbour_context.adjusted_heights = &adjusted_heights;
                neighbour_context.place_feature = place_feature;
                worldgen_random neighbour_random{
                    std::make_unique<xoroshiro_random_source>(0)};
                const jlong neighbour_decoration_seed =
                    neighbour_random.set_decoration_seed(
                        seed, neighbour->chunk_x() * 16,
                        neighbour->chunk_z() * 16);
                const auto start_index_value = structure::structure_step_index(
                    *data, starts[start_index].structure);
                if (!start_index_value) {
                    continue;
                }
                const auto &start = starts[start_index];
                if (!start.pieces.empty()) {
                    neighbour_context.reference =
                        structure::start_reference_position(
                            start.pieces.front().bounds);
                }
                const jint step =
                    structure::generation_step_ordinal(start.step);
                neighbour_random.set_feature_seed(neighbour_decoration_seed,
                                                  *start_index_value, step);
                for (const auto &piece : all_records[start_index]) {
                    if (piece.max_x < neighbour->chunk_x() * 16 ||
                        piece.min_x > neighbour->chunk_x() * 16 + 15 ||
                        piece.max_z < neighbour->chunk_z() * 16 ||
                        piece.min_z > neighbour->chunk_z() * 16 + 15) {
                        continue;
                    }
                    structure::place_structure_piece(neighbour_context, piece,
                                                     neighbour_random);
                }
            }
        }
        if (std::getenv("REFRAIN_MANSION_DEBUG") != nullptr) {
            for (const auto &neighbour : neighbours) {
                if (neighbour->chunk_x() == 47 && neighbour->chunk_z() == 5) {
                    std::fprintf(
                        stderr, "mansion east %s\n",
                        std::string{states.name(neighbour->state_at(0, 85, 2))}
                            .c_str());
                }
            }
        }
        structure::refresh_chunk_shapes(chunk, region_view, states);
    }
    const auto comparison = compare_chunks(chunk, *reference, *document,
                                           profile.min_y, profile.height);
    std::fprintf(stderr,
                 "full-stage: %zu block mismatches, %d heightmap "
                 "mismatches\n",
                 comparison.block_mismatches, comparison.heightmap_mismatches);
    expect(comparison.heightmap_mismatches == 0);
    expect(comparison.block_mismatches == 0);

    if (compare_block_entities || !env("BLOCK_ENTITIES").empty()) {
        // Loot-bearing block entities are the strongest observation of the
        // piece random stream: both the table and the seed it stored must
        // match. The chunk serialiser adds coordinates and the block entity
        // id, so only the two loot fields are compared.
        std::map<block_pos, std::pair<std::string, jlong>> expected_loot;
        const auto *block_entities =
            child(document->as_compound(), "block_entities");
        if (block_entities != nullptr && block_entities->as_list() != nullptr) {
            for (const auto &entry : block_entities->as_list()->items) {
                const auto *tag = entry.as_compound();
                if (tag == nullptr) {
                    continue;
                }
                const auto *table = child(tag, "LootTable");
                const auto *seed_tag = child(tag, "LootTableSeed");
                const auto *x = child(tag, "x");
                const auto *y = child(tag, "y");
                const auto *z = child(tag, "z");
                if (table == nullptr || seed_tag == nullptr || x == nullptr ||
                    y == nullptr || z == nullptr) {
                    continue;
                }
                const auto table_name = table->as_string();
                const auto seed_value = seed_tag->as_long();
                if (!table_name || !seed_value) {
                    continue;
                }
                expected_loot[block_pos{.x = *x->as_int() & 15,
                                        .y = *y->as_int(),
                                        .z = *z->as_int() & 15}] =
                    std::make_pair(std::string(*table_name), *seed_value);
            }
        }
        std::size_t loot_mismatches = 0;
        for (const auto &[pos, expected] : expected_loot) {
            const auto *entity = chunk.block_entity_at(pos);
            if (entity == nullptr) {
                std::fprintf(stderr,
                             "loot mismatch (%d,%d,%d): missing entity, want "
                             "%s/%lld\n",
                             static_cast<int>(pos.x), static_cast<int>(pos.y),
                             static_cast<int>(pos.z), expected.first.c_str(),
                             static_cast<long long>(expected.second));
                ++loot_mismatches;
                continue;
            }
            const auto table = entity->find("LootTable");
            const auto seed_tag = entity->find("LootTableSeed");
            if (table == entity->end() || seed_tag == entity->end() ||
                table->second.as_string().value_or("") != expected.first ||
                seed_tag->second.as_long().value_or(0) != expected.second) {
                std::fprintf(
                    stderr,
                    "loot mismatch (%d,%d,%d): got %s/%lld want %s/%lld\n",
                    static_cast<int>(pos.x), static_cast<int>(pos.y),
                    static_cast<int>(pos.z),
                    table == entity->end()
                        ? "<none>"
                        : std::string{table->second.as_string().value_or("")}
                              .c_str(),
                    static_cast<long long>(
                        seed_tag == entity->end()
                            ? 0
                            : seed_tag->second.as_long().value_or(0)),
                    expected.first.c_str(),
                    static_cast<long long>(expected.second));
                ++loot_mismatches;
            }
        }
        for (const auto &[pos, entity] : chunk.block_entities()) {
            if (entity.find("LootTable") != entity.end() &&
                !expected_loot.contains(pos)) {
                const auto table = entity.find("LootTable");
                std::fprintf(stderr,
                             "loot mismatch (%d,%d,%d): unexpected entity %s\n",
                             static_cast<int>(pos.x), static_cast<int>(pos.y),
                             static_cast<int>(pos.z),
                             std::string{table->second.as_string().value_or("")}
                                 .c_str());
                ++loot_mismatches;
            }
        }
        std::fprintf(stderr,
                     "full-stage: %zu loot block entity mismatches over %zu "
                     "reference entities\n",
                     loot_mismatches, expected_loot.size());
        expect(loot_mismatches == 0);
    }

    if (!own_start) {
        return;
    }
    auto generator = structure::structure_generator::create(
        structure::structure_generator_config{
            .data_root = data_root,
            .templates_root = templates_root,
            .biome = biome,
            .seed = seed,
            .kind = dimension,
            .min_y = profile.min_y,
            .height = profile.height,
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
            for (jint y = profile.min_y; y < profile.min_y + profile.height;
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
            std::fprintf(stderr, "full-stage: %zu service mismatches\n",
                         service_mismatches);
            expect(service_mismatches == 0);
            for (const auto kind :
                 {heightmap_kind::world_surface, heightmap_kind::ocean_floor,
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
}
} // namespace

suite<"worldgen/full-stage"> full_stage_tests = [] {
    "fill and structure placement reproduce the reference chunk"_test = [] {
        run_full_stage_case(
            "FULL_STAGE", 9, 2, 2, "minecraft:jungle",
            "minecraft/structure/village,minecraft/structure/end_city");
    };

    "a pillager outpost chunk matches the reference full-stage world"_test =
        [] {
            run_full_stage_case("JIGSAW_OUTPOST_FULL_STAGE", std::nullopt,
                                std::nullopt, std::nullopt, "minecraft:plains",
                                "minecraft/structure/pillager_outpost");
        };

    "a taiga village chunk matches the reference full-stage world"_test = [] {
        run_full_stage_case("JIGSAW_VILLAGE_FULL_STAGE", std::nullopt,
                            std::nullopt, std::nullopt, "minecraft:taiga",
                            "minecraft/structure/village");
    };

    "an ancient city chunk matches the reference full-stage world"_test = [] {
        run_full_stage_case("JIGSAW_ANCIENT_FULL_STAGE", std::nullopt,
                            std::nullopt, std::nullopt, "minecraft:deep_dark",
                            "minecraft/structure/ancient_city");
    };

    "a trial chambers chunk matches the reference full-stage world"_test = [] {
        run_full_stage_case("JIGSAW_TRIAL_FULL_STAGE", std::nullopt,
                            std::nullopt, std::nullopt, "minecraft:plains",
                            "minecraft/structure/trial_chambers");
    };

    "a trail ruins chunk matches the reference full-stage world"_test = [] {
        run_full_stage_case("TRAIL_RUINS_FULL_STAGE", std::nullopt,
                            std::nullopt, std::nullopt, "minecraft:taiga",
                            "minecraft/structure/trail_ruins", true);
    };

    "a trial chambers chest chunk matches the reference loot block entities"_test =
        [] {
            run_full_stage_case("JIGSAW_TRIAL_LOOT_FULL_STAGE", std::nullopt,
                                std::nullopt, std::nullopt, "minecraft:plains",
                                "minecraft/structure/trial_chambers", true);
        };

    "a bastion high rampart chunk matches the reference full-stage world"_test =
        [] {
            run_full_stage_case("BASTION_RAMPART_FULL_STAGE", std::nullopt,
                                std::nullopt, std::nullopt,
                                "minecraft:nether_wastes",
                                "minecraft/structure/bastion", true);
        };
};
