// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/structure/generator.hpp>

#include <refrain/worldgen/chunk/block_state_codec.hpp>
#include <refrain/worldgen/chunk/chunk_region.hpp>
#include <refrain/worldgen/structure/generation.hpp>
#include <refrain/worldgen/structure/placement_stage.hpp>
#include <refrain/worldgen/structure/stage.hpp>
#include <refrain/worldgen/structure/start.hpp>
#include <refrain/worldgen/terrain/aquifer.hpp>
#include <refrain/worldgen/terrain/terrain.hpp>

#include <algorithm>
#include <array>
#include <memory>
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

auto make_piece_record(const structure_piece &piece)
    -> ::refrain::worldgen::structure_piece_record {
    ::refrain::worldgen::structure_piece_record record{};
    record.type = piece.type;
    record.min_x = piece.bounds.min_x;
    record.min_y = piece.bounds.min_y;
    record.min_z = piece.bounds.min_z;
    record.max_x = piece.bounds.max_x;
    record.max_y = piece.bounds.max_y;
    record.max_z = piece.bounds.max_z;
    record.orientation = piece.orientation;
    record.template_id = piece.template_id;
    record.template_x = piece.template_origin.x;
    record.template_y = piece.template_origin.y;
    record.template_z = piece.template_origin.z;
    record.template_rotation = piece.template_rotation;
    record.template_mirror = piece.template_mirror;
    record.template_pivot_x = piece.template_pivot.x;
    record.template_pivot_y = piece.template_pivot.y;
    record.template_pivot_z = piece.template_pivot.z;
    record.height_position = piece.height_position;
    record.height_offset = piece.height_offset;
    record.integrity = piece.integrity;
    record.large = piece.large;
    record.beached = piece.beached;
    record.entrances = piece.entrances;
    record.sections = piece.sections;
    record.rails = piece.rails;
    record.spider = piece.spider;
    record.two_floored = piece.two_floored;
    record.mesa = piece.mesa;
    return record;
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
                                         density::noise_settings settings,
                                         structure_data_set data)
    : config_{std::move(config)}, settings_{std::move(settings)},
      data_{std::move(data)}, states_{make_registry()} {
    for (const auto &key : config_.known_blocks) {
        if (states_.find(key).has_value()) {
            continue;
        }
        const auto block = state_block_name(key);
        const bool air = is_air_block_name(block);
        const bool fluid =
            block == "minecraft:water" || block == "minecraft:lava";
        const bool leaves = block.ends_with("_leaves");
        (void)states_.intern(
            key, {.air = air,
                  .fluid = fluid,
                  .water = block == "minecraft:water",
                  .blocks_motion = !air && !fluid,
                  .blocks_motion_no_leaves = !air && !fluid && !leaves});
    }
    templates_ = std::make_unique<structure_template_manager>(
        config_.templates_root, states_);
}

auto structure_generator::create(const structure_generator_config &config)
    -> result<std::unique_ptr<structure_generator>> {
    auto settings =
        density::load_noise_settings(config.data_root, "minecraft/overworld",
                                     algorithm::xoroshiro, config.seed);
    if (!settings) {
        return std::unexpected{settings.error()};
    }
    auto data = load_structure_data(config.data_root);
    if (!data) {
        return std::unexpected{data.error()};
    }
    auto generator =
        std::unique_ptr<structure_generator>{new structure_generator{
            config, std::move(*settings), std::move(*data)}};
    return generator;
}

auto structure_generator::generate(const chunk_pos &pos)
    -> result<std::unique_ptr<proto_chunk>> {
    const auto ids = terrain::block_ids{
        .air = 0, .default_block = 1, .default_fluid = 2, .lava = 3};
    const auto picker = terrain::fluid_picker{.sea_level = config_.sea_level,
                                              .default_fluid = 2,
                                              .lava = 3,
                                              .air = 0};
    const auto options = terrain::proto_fill_options{
        .air = 0, .default_block = 1, .default_fluid = 2, .lava = 3};
    const density::sampler_context sampler_context;
    std::vector<std::unique_ptr<proto_chunk>> chunks;
    chunks.reserve(9);
    for (jint dz = -1; dz <= 1; ++dz) {
        for (jint dx = -1; dx <= 1; ++dx) {
            auto factory =
                new_positional_factory(algorithm::xoroshiro, config_.seed);
            auto aquifer_factory =
                factory->from_hash_of("minecraft:aquifer")->fork_positional();
            auto chunk = std::make_unique<proto_chunk>(
                states_, pos.x + dx, pos.z + dz, config_.min_y, config_.height);
            chunk->set_status(chunk_status::biomes);
            const volume chunk_volume{16,
                                      config_.height,
                                      16,
                                      (pos.x + dx) * 16,
                                      config_.min_y,
                                      (pos.z + dz) * 16};
            terrain::noise_aquifer aquifer{
                settings_.aquifer, *aquifer_factory, chunk_volume, picker, ids,
                sampler_context};
            terrain::fill_proto_chunk(*chunk, options,
                                      *settings_.router.final_density,
                                      sampler_context, aquifer);
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
        const auto block = state_block_name(key);
        const bool air = is_air_block_name(block);
        const bool leaves = block.ends_with("_leaves");
        return states_
            .intern(key, {.air = air,
                          .blocks_motion = !air,
                          .blocks_motion_no_leaves = !air && !leaves})
            .value_or(no_block_state);
    };
    const structure_start_context start_context{
        .seed = config_.seed,
        .data = &data_,
        .min_y = config_.min_y,
        .world_height = config_.height,
        .sea_level = config_.sea_level,
        .biome_at = [](jint, jint, jint) { return std::uint16_t{0}; },
        .biome_name = [this](std::uint16_t) -> std::string_view {
            return config_.biome;
        },
        .height_at =
            [&](jint x, jint z, heightmap_kind kind) {
                const jint local_x = x - pos.x * 16;
                const jint local_z = z - pos.z * 16;
                if (local_x < 0 || local_x > 15 || local_z < 0 ||
                    local_z > 15) {
                    return center->min_y();
                }
                return center->first_available(kind, local_x, local_z);
            },
        .templates = templates_.get(),
    };
    const auto starts = generate_structure_starts(pos, start_context);
    std::vector<std::vector<::refrain::worldgen::structure_piece_record>>
        records;
    for (const auto &start : starts) {
        auto record = make_start_record(start);
        records.push_back(record.pieces);
        center->add_structure_start(std::move(record));
    }

    const structure_place_context place_context{
        .region = &region,
        .chunk = center,
        .states = &states_,
        .state = resolve,
        .templates = templates_.get(),
        .registry = &states_,
        .seed = config_.seed,
    };
    worldgen_random decoration{
        std::make_unique<xoroshiro_random_source>(config_.seed)};
    const jlong decoration_seed =
        decoration.set_decoration_seed(config_.seed, pos.x * 16, pos.z * 16);
    for (const auto &start : starts) {
        const auto index = structure_step_index(data_, start.structure);
        if (!index) {
            continue;
        }
        jint step = 0;
        if (start.step == "surface_structures") {
            step = 4;
        } else if (start.step == "underground_structures") {
            step = 3;
        }
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
            };
            for (const auto &piece : records[start_index]) {
                worldgen_random random{
                    std::make_unique<xoroshiro_random_source>(config_.seed)};
                place_structure_piece(neighbour_context, piece, random);
            }
        }
    }
    auto result = std::move(chunks[4]);
    return result;
}

} // namespace refrain::worldgen::structure
