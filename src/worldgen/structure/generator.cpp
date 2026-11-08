// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/structure/generator.hpp>

#include <refrain/worldgen/chunk/block_state_codec.hpp>
#include <refrain/worldgen/chunk/chunk_region.hpp>
#include <refrain/worldgen/feature/feature_data.hpp>
#include <refrain/worldgen/structure/feature_placement.hpp>
#include <refrain/worldgen/structure/generation.hpp>
#include <refrain/worldgen/structure/placement_stage.hpp>
#include <refrain/worldgen/structure/stage.hpp>
#include <refrain/worldgen/structure/start.hpp>
#include <refrain/worldgen/terrain/aquifer.hpp>
#include <refrain/worldgen/terrain/terrain.hpp>

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <set>
#include <vector>

namespace refrain::worldgen::structure {

namespace {

auto make_registry() -> block_state_registry {
    block_state_registry states;
    (void)states.intern("minecraft:air", {.air = true});
    (void)states.intern(
        "minecraft:stone",
        {.air = false, .blocks_motion = true, .blocks_motion_no_leaves = true});
    (void)states.intern("minecraft:water",
                        {.air = false, .fluid = true, .water = true});
    (void)states.intern("minecraft:lava", {.air = false, .fluid = true});
    return states;
}

auto solid_traits() -> state_traits {
    return state_traits{
        .air = false, .blocks_motion = true, .blocks_motion_no_leaves = true};
}

auto fluid_traits(bool water) -> state_traits {
    return state_traits{.air = false, .fluid = true, .water = water};
}

auto make_piece_record(const structure_piece &piece)
    -> ::refrain::worldgen::structure_piece_record {
    return to_piece_record(piece);
}

auto tag_names(const block_tag_set &tags, std::string_view tag)
    -> std::set<std::string, std::less<>> {
    std::set<std::string, std::less<>> names;
    if (!tags.has(tag)) {
        return names;
    }
    auto resolved = tags.resolve(tag);
    if (resolved) {
        names.insert(resolved->begin(), resolved->end());
    }
    return names;
}

auto make_start_record(const structure_start &start)
    -> ::refrain::worldgen::structure_start {
    ::refrain::worldgen::structure_start record{};
    record.structure = start.structure;
    record.step = start.step;
    record.chunk_x = start.chunk.x;
    record.chunk_z = start.chunk.z;
    record.min_x = start.bounds.min_x;
    record.min_y = start.bounds.min_y;
    record.min_z = start.bounds.min_z;
    record.max_x = start.bounds.max_x;
    record.max_y = start.bounds.max_y;
    record.max_z = start.bounds.max_z;
    for (const auto &piece : start.pieces) {
        record.pieces.push_back(make_piece_record(piece));
    }
    return record;
}

} // namespace

structure_generator::structure_generator(structure_generator_config config,
                                         const dimension_profile &profile,
                                         density::noise_settings settings,
                                         structure_data_set data,
                                         block_tag_set tags)
    : config_{std::move(config)}, profile_{&profile},
      settings_{std::move(settings)}, data_{std::move(data)},
      tags_{std::move(tags)}, states_{make_registry()} {
    // The profile owns the level bounds; the noise settings own the range the
    // terrain stages write, which is shorter for the nether and the end.
    min_y_ = config_.min_y.value_or(profile_->min_y);
    height_ = config_.height.value_or(profile_->height);
    sea_level_ = config_.sea_level.value_or(profile_->sea_level);
    generation_min_y_ = std::max(min_y_, settings_.min_y);
    const jint generation_max_y =
        std::min(min_y_ + height_, settings_.min_y + settings_.height);
    generation_height_ = generation_max_y - generation_min_y_;
    if (generation_height_ <= 0) {
        generation_height_ = height_;
        generation_min_y_ = min_y_;
    }
    motion_ = tag_names(tags_, "minecraft:blocks_motion_in_heightmap");
    motion_no_leaves_ =
        tag_names(tags_, "minecraft:blocks_motion_in_heightmap_no_leaves");
    for (const auto &key : config_.known_blocks) {
        if (states_.find(key).has_value()) {
            continue;
        }
        (void)states_.intern(key, inferred_traits(key));
    }
    const auto block = states_.intern(settings_.default_block, solid_traits());
    default_block_state_ = block.value_or(no_block_state);
    if (settings_.default_fluid == "minecraft:air") {
        default_fluid_state_ = states_.air();
    } else {
        const bool water = settings_.default_fluid == "minecraft:water";
        default_fluid_state_ =
            states_.intern(settings_.default_fluid, fluid_traits(water))
                .value_or(no_block_state);
    }
    lava_state_ = states_.intern("minecraft:lava", fluid_traits(false))
                      .value_or(no_block_state);
    air_state_ = states_.air();
    templates_ = std::make_unique<structure_template_manager>(
        config_.templates_root, states_);
}

auto structure_generator::inferred_traits(std::string_view key) const
    -> state_traits {
    const auto block = state_block_name(key);
    const bool air = is_air_block_name(block);
    const bool water = block == "minecraft:water" ||
                       key.find("waterlogged=true") != std::string_view::npos;
    const bool fluid = water || block == "minecraft:lava" ||
                       block == "minecraft:bubble_column";
    return state_traits{
        .air = air,
        .fluid = fluid,
        .water = water,
        .blocks_motion = motion_.contains(std::string{block}),
        .blocks_motion_no_leaves =
            motion_no_leaves_.contains(std::string{block}),
    };
}

auto structure_generator::create(const structure_generator_config &config)
    -> result<std::unique_ptr<structure_generator>> {
    const auto &profile = dimension_profile_for(config.kind);
    auto settings =
        density::load_noise_settings(config.data_root, profile.noise_settings,
                                     algorithm::xoroshiro, config.seed);
    if (!settings) {
        return std::unexpected{settings.error()};
    }
    auto data = load_structure_data(config.data_root);
    if (!data) {
        return std::unexpected{data.error()};
    }
    auto tags = load_block_tags(config.data_root);
    if (!tags) {
        return std::unexpected{tags.error()};
    }
    auto generator = std::unique_ptr<structure_generator>{
        new structure_generator{config, profile, std::move(*settings),
                                std::move(*data), std::move(*tags)}};
    return generator;
}

auto structure_generator::generate(const chunk_pos &pos)
    -> result<std::unique_ptr<proto_chunk>> {
    const auto ids = terrain::block_ids{.air = air_state_,
                                        .default_block = default_block_state_,
                                        .default_fluid = default_fluid_state_,
                                        .lava = lava_state_};
    const auto picker =
        terrain::fluid_picker{.sea_level = sea_level_,
                              .default_fluid = default_fluid_state_,
                              .lava = lava_state_,
                              .air = air_state_};
    const auto options =
        terrain::proto_fill_options{.air = air_state_,
                                    .default_block = default_block_state_,
                                    .default_fluid = default_fluid_state_,
                                    .lava = lava_state_};
    const auto fill_volume = terrain::proto_fill_volume{
        .min_y = generation_min_y_, .height = generation_height_};
    struct aquifer_holder {
        std::unique_ptr<positional_random_factory> factory{};
        std::unique_ptr<terrain::aquifer> aquifer{};
    };
    const auto create_aquifer =
        [&](jint chunk_x, jint chunk_z,
            const density::sampler_context &context) -> aquifer_holder {
        aquifer_holder holder;
        if (!settings_.aquifers) {
            holder.aquifer =
                std::make_unique<terrain::disabled_aquifer>(picker);
            return holder;
        }
        auto random =
            new_positional_factory(algorithm::xoroshiro, config_.seed);
        auto aquifer_random = random->from_hash_of("minecraft:aquifer");
        holder.factory = aquifer_random->fork_positional();
        const volume chunk_volume{
            16,           generation_height_, 16,
            chunk_x * 16, generation_min_y_,  chunk_z * 16};
        holder.aquifer = std::make_unique<terrain::noise_aquifer>(
            settings_.aquifer, *holder.factory, chunk_volume, picker, ids,
            context);
        return holder;
    };

    // The starts stage reads `getFirstFreeHeight` from the unbearded noise.
    std::map<chunk_pos, std::unique_ptr<proto_chunk>> height_cache;
    const auto height_at = [&](jint x, jint z, heightmap_kind kind) -> jint {
        const chunk_pos target{.x = x >> 4, .z = z >> 4};
        auto found = height_cache.find(target);
        if (found == height_cache.end()) {
            auto column = std::make_unique<proto_chunk>(
                states_, target.x, target.z, min_y_, height_);
            column->set_status(chunk_status::biomes);
            const density::sampler_context context;
            auto holder = create_aquifer(target.x, target.z, context);
            terrain::fill_proto_chunk(*column, options, fill_volume,
                                      *settings_.router.final_density, context,
                                      *holder.aquifer);
            found = height_cache.emplace(target, std::move(column)).first;
        }
        return found->second->first_available(kind, x & 15, z & 15);
    };

    const auto block_at = [&](jint x, jint y, jint z) -> block_state_id {
        const chunk_pos target{.x = x >> 4, .z = z >> 4};
        auto found = height_cache.find(target);
        if (found == height_cache.end()) {
            (void)height_at(x, z, heightmap_kind::world_surface_wg);
            found = height_cache.find(target);
            if (found == height_cache.end()) {
                return air_state_;
            }
        }
        return found->second->state_at(x & 15, y, z & 15);
    };
    const structure_start_context start_context{
        .seed = config_.seed,
        .data = &data_,
        .min_y = min_y_,
        .world_height = height_,
        .sea_level = sea_level_,
        .generation_min_y = generation_min_y_,
        .generation_height = generation_height_,
        .fixed_biome = true,
        .biome_at = [](jint, jint, jint) { return std::uint16_t{0}; },
        .biome_name = [this](std::uint16_t) -> std::string_view {
            return config_.biome;
        },
        .height_at = height_at,
        .block_at = block_at,
        .states = &states_,
        .templates = templates_.get(),
    };
    // The beardifier of the target chunk includes starts up to eight chunks
    // away; scan a little wider so every compared neighbour sees its own set.
    constexpr jint scan_radius = 9;
    std::vector<structure_start> area_starts;
    for (jint dx = -scan_radius; dx <= scan_radius; ++dx) {
        for (jint dz = -scan_radius; dz <= scan_radius; ++dz) {
            auto generated = generate_structure_starts(
                chunk_pos{.x = pos.x + dx, .z = pos.z + dz}, start_context);
            for (auto &entry : generated) {
                area_starts.push_back(std::move(entry));
            }
        }
    }
    std::vector<structure_start> starts;
    for (const auto &start : area_starts) {
        if (start.chunk.x == pos.x && start.chunk.z == pos.z) {
            starts.push_back(start);
        }
    }

    std::vector<std::unique_ptr<proto_chunk>> chunks;
    chunks.reserve(9);
    for (jint dz = -1; dz <= 1; ++dz) {
        for (jint dx = -1; dx <= 1; ++dx) {
            const chunk_pos target{.x = pos.x + dx, .z = pos.z + dz};
            auto chunk = std::make_unique<proto_chunk>(
                states_, target.x, target.z, min_y_, height_);
            chunk->set_status(chunk_status::biomes);
            const auto beard = collect_beard(area_starts, data_, target);
            density::sampler_context context;
            std::optional<beardifier> beardifier;
            if (!beard.pieces.empty() || !beard.junctions.empty() ||
                beard.affected_box.has_value()) {
                beardifier.emplace(beard.pieces, beard.junctions,
                                   beard.affected_box);
                context.set_field(density::context_key::beardifier,
                                  &*beardifier);
            }
            auto holder = create_aquifer(target.x, target.z, context);
            terrain::fill_proto_chunk(*chunk, options, fill_volume,
                                      *settings_.router.final_density, context,
                                      *holder.aquifer);
            chunk->set_status(chunk_status::terrain);
            chunks.push_back(std::move(chunk));
        }
    }
    auto *center = chunks[4].get();
    chunk_region region{*center, 1};
    for (std::size_t index = 0; index < chunks.size(); ++index) {
        if (chunks[index].get() != center) {
            region.attach(*chunks[index]);
        }
    }

    const auto resolve = [&](std::string_view key) -> block_state_id {
        if (const auto found = states_.find(key); found) {
            return *found;
        }
        return states_.intern(key, inferred_traits(key))
            .value_or(no_block_state);
    };
    std::vector<std::vector<::refrain::worldgen::structure_piece_record>>
        records;
    for (const auto &start : starts) {
        auto record = make_start_record(start);
        records.push_back(record.pieces);
        center->add_structure_start(std::move(record));
    }

    std::vector<std::string> feature_ids;
    for (const auto &piece_records : records) {
        for (const auto &piece : piece_records) {
            collect_feature_ids(piece, feature_ids);
        }
    }
    std::optional<feature::feature_data_set> feature_data;
    if (!feature_ids.empty()) {
        auto loaded_features = feature::load_feature_data(config_.data_root);
        if (!loaded_features) {
            return std::unexpected{loaded_features.error()};
        }
        feature_data = std::move(*loaded_features);
        if (const auto tagged = apply_block_tags(states_, tags_); !tagged) {
            return std::unexpected{tagged.error()};
        }
    }

    std::unordered_map<std::string, jint> adjusted_heights;
    structure_place_context place_context{
        .region = &region,
        .chunk = center,
        .states = &states_,
        .state = resolve,
        .templates = templates_.get(),
        .registry = &states_,
        .seed = config_.seed,
        .sea_level = sea_level_,
        .adjusted_heights = &adjusted_heights,
        .data = &data_,
    };
    std::optional<structure_feature_placer> feature_placer;
    std::function<bool(std::string_view, worldgen_random &, const block_pos &)>
        place_feature;
    if (feature_data) {
        feature_placer.emplace(place_context, *feature_data);
        if (const auto loaded = feature_placer->load(feature_ids); !loaded) {
            return std::unexpected{loaded.error()};
        }
        place_feature = [&feature_placer](std::string_view id,
                                          worldgen_random &random,
                                          const block_pos &position) {
            return feature_placer->place(id, random, position);
        };
        place_context.place_feature = place_feature;
    }
    worldgen_random decoration{
        std::make_unique<xoroshiro_random_source>(config_.seed)};
    const jlong decoration_seed =
        decoration.set_decoration_seed(config_.seed, pos.x * 16, pos.z * 16);
    for (const auto &start : starts) {
        const auto index = structure_step_index(data_, start.structure);
        if (!index) {
            continue;
        }
        const jint step = generation_step_ordinal(start.step);
        worldgen_random random{std::make_unique<xoroshiro_random_source>(0)};
        random.set_feature_seed(decoration_seed, *index, step);
        place_structures(*center, place_context, start.step, random);
    }
    for (std::size_t start_index = 0; start_index < starts.size();
         ++start_index) {
        for (std::size_t chunk_index = 0; chunk_index < chunks.size();
             ++chunk_index) {
            if (chunks[chunk_index].get() == center) {
                continue;
            }
            bool intersects = false;
            for (const auto &piece : records[start_index]) {
                if (piece.min_x <= chunks[chunk_index]->chunk_x() * 16 + 15 &&
                    piece.max_x >= chunks[chunk_index]->chunk_x() * 16 &&
                    piece.min_z <= chunks[chunk_index]->chunk_z() * 16 + 15 &&
                    piece.max_z >= chunks[chunk_index]->chunk_z() * 16) {
                    intersects = true;
                }
            }
            if (!intersects) {
                continue;
            }
            const structure_place_context neighbour_context{
                .region = &region,
                .chunk = chunks[chunk_index].get(),
                .states = &states_,
                .state = resolve,
                .templates = templates_.get(),
                .registry = &states_,
                .seed = config_.seed,
                .sea_level = sea_level_,
                .adjusted_heights = &adjusted_heights,
                .data = &data_,
                .place_feature = place_feature,
            };
            const auto index =
                structure_step_index(data_, starts[start_index].structure);
            if (!index) {
                continue;
            }
            const jint step = generation_step_ordinal(starts[start_index].step);
            worldgen_random random{
                std::make_unique<xoroshiro_random_source>(0)};
            const jlong neighbour_decoration_seed = random.set_decoration_seed(
                config_.seed, chunks[chunk_index]->chunk_x() * 16,
                chunks[chunk_index]->chunk_z() * 16);
            random.set_feature_seed(neighbour_decoration_seed, *index, step);
            for (const auto &piece : records[start_index]) {
                if (piece.max_x < chunks[chunk_index]->chunk_x() * 16 ||
                    piece.min_x > chunks[chunk_index]->chunk_x() * 16 + 15 ||
                    piece.max_z < chunks[chunk_index]->chunk_z() * 16 ||
                    piece.min_z > chunks[chunk_index]->chunk_z() * 16 + 15) {
                    continue;
                }
                place_structure_piece(neighbour_context, piece, random);
            }
        }
    }
    refresh_chunk_shapes(*center, region, states_);
    auto result = std::move(chunks[4]);
    return result;
}

} // namespace refrain::worldgen::structure
