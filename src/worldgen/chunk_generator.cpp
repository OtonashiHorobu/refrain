// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/chunk_generator.hpp>

#include <refrain/worldgen/biome/biome_manager.hpp>
#include <refrain/worldgen/biome/biome_view.hpp>
#include <refrain/worldgen/carver/carver_data.hpp>
#include <refrain/worldgen/chunk/chunk_region.hpp>
#include <refrain/worldgen/chunk/status.hpp>
#include <refrain/worldgen/core/rng.hpp>
#include <refrain/worldgen/feature/feature_data.hpp>
#include <refrain/worldgen/feature/feature_plan.hpp>
#include <refrain/worldgen/feature/feature_stage.hpp>
#include <refrain/worldgen/provider.hpp>
#include <refrain/worldgen/structure/feature_placement.hpp>
#include <refrain/worldgen/structure/placement_stage.hpp>
#include <refrain/worldgen/structure/stage.hpp>
#include <refrain/worldgen/structure/start.hpp>
#include <refrain/worldgen/structure/structure_data.hpp>
#include <refrain/worldgen/terrain/beardifier.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace refrain::worldgen {

namespace {

auto fail(error_code code, std::string_view message) -> error {
    return make_error(code, message);
}

auto dimension_config_for(const chunk_generator_config &config)
    -> dimension_generator_config {
    return dimension_generator_config{
        .data_root = config.data_root,
        .kind = config.kind,
        .seed = config.seed,
        .fixed_biome = config.fixed_biome,
    };
}

auto target_status(generation_target target) -> chunk_status {
    switch (target) {
    case generation_target::terrain:
        return chunk_status::terrain;
    case generation_target::features:
        return chunk_status::features;
    case generation_target::full:
        return chunk_status::full;
    }
    return chunk_status::terrain;
}

auto make_start_record(const structure::structure_start &start)
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
    record.pieces.reserve(start.pieces.size());
    for (const auto &piece : start.pieces) {
        record.pieces.push_back(structure::to_piece_record(piece));
    }
    return record;
}

/// Bounds of the per-worker start-probe caches. The height cache holds a
/// little more than one radius-nine window; the block cache holds the vertical
/// columns the nether fossil scan revisits across the sliding window.
constexpr std::size_t start_height_capacity = 512;
constexpr std::size_t start_block_capacity = 2048;

constexpr std::array<chunk_pos, 9> neighbour_offsets{{
    {0, 0},
    {-1, -1},
    {0, -1},
    {1, -1},
    {-1, 0},
    {1, 0},
    {-1, 1},
    {0, 1},
    {1, 1},
}};

} // namespace

struct chunk_generator::shared_generator_data {
    /// Immutable dimension data: noise settings, material documents and rule,
    /// climates, noise functions, block tags and possible biomes.
    std::shared_ptr<const dimension_shared_data> dimension{};
    carver::loaded_carvers carvers{};
    feature::feature_data_set feature_data{};
    /// The per-step feature order and the flat placed-feature list used to
    /// build every worker's registry.
    std::vector<feature::step_features> feature_plan{};
    std::vector<std::string> placed_ids{};
    structure::structure_data_set structure_data{};
};

chunk_generator::chunk_generator(
    chunk_generator_config config,
    std::shared_ptr<const shared_generator_data> shared,
    std::unique_ptr<dimension_generator> dimension,
    std::unique_ptr<feature::feature_registry> features,
    std::unique_ptr<structure::structure_template_manager> templates)
    : config_{std::move(config)}, shared_{std::move(shared)},
      dimension_{std::move(dimension)}, features_{std::move(features)},
      templates_{std::move(templates)},
      has_structures_{config_.target != generation_target::terrain},
      has_features_{!config_.reference_world && has_structures_} {}

auto chunk_generator::create(chunk_generator_config config)
    -> result<std::unique_ptr<chunk_generator>> {
    if (config.data_root.empty()) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "chunk generator has no content data root")};
    }
    const bool needs_structures = config.target != generation_target::terrain;
    const bool needs_features = needs_structures && !config.reference_world;
    const bool needs_carvers =
        !config.reference_world && config.kind == dimension_kind::overworld;

    const auto dimension_config = dimension_config_for(config);
    auto dimension_shared = dimension_generator::load_shared(dimension_config);
    if (!dimension_shared) {
        return std::unexpected{dimension_shared.error()};
    }
    auto shared = std::make_shared<shared_generator_data>();
    shared->dimension = std::move(*dimension_shared);

    auto dimension =
        dimension_generator::create(dimension_config, shared->dimension);
    if (!dimension) {
        return std::unexpected{dimension.error()};
    }

    if (needs_carvers) {
        auto loaded = carver::load_carvers(config.data_root);
        if (!loaded) {
            return std::unexpected{loaded.error()};
        }
        shared->carvers = std::move(*loaded);
    }
    if (needs_structures || needs_features) {
        auto loaded = feature::load_feature_data(config.data_root);
        if (!loaded) {
            return std::unexpected{loaded.error()};
        }
        shared->feature_data = std::move(*loaded);
    }
    if (needs_features) {
        std::vector<std::string> possible_biomes;
        if (config.fixed_biome.empty()) {
            possible_biomes = (*dimension)->biome_source().biomes();
        } else {
            possible_biomes.push_back(config.fixed_biome);
        }
        auto sorted = feature::feature_sorter::build(
            possible_biomes, shared->feature_data.biomes);
        if (!sorted) {
            return std::unexpected{sorted.error()};
        }
        shared->feature_plan = std::move(*sorted);
        for (const auto &step : shared->feature_plan) {
            shared->placed_ids.insert(shared->placed_ids.end(),
                                      step.features.begin(),
                                      step.features.end());
        }
    }
    if (needs_structures) {
        auto loaded = structure::load_structure_data(config.data_root);
        if (!loaded) {
            return std::unexpected{loaded.error()};
        }
        shared->structure_data = std::move(*loaded);
    }

    std::unique_ptr<feature::feature_registry> features;
    if (needs_features) {
        features =
            std::make_unique<feature::feature_registry>((*dimension)->states());
        if (const auto loaded =
                features->load_placed(shared->feature_data, shared->placed_ids);
            !loaded) {
            return std::unexpected{loaded.error()};
        }
    }
    std::unique_ptr<structure::structure_template_manager> templates;
    if (needs_structures) {
        templates = std::make_unique<structure::structure_template_manager>(
            config.templates_root, (*dimension)->states());
    }

    const std::shared_ptr<const shared_generator_data> immutable = shared;
    return std::unique_ptr<chunk_generator>{
        new chunk_generator{std::move(config), immutable, std::move(*dimension),
                            std::move(features), std::move(templates)}};
}

auto chunk_generator::create_with_shared(
    chunk_generator_config config,
    std::shared_ptr<const shared_generator_data> shared)
    -> result<std::unique_ptr<chunk_generator>> {
    auto dimension = dimension_generator::create(dimension_config_for(config),
                                                 shared->dimension);
    if (!dimension) {
        return std::unexpected{dimension.error()};
    }
    std::unique_ptr<feature::feature_registry> features;
    if (config.target != generation_target::terrain &&
        !config.reference_world) {
        features =
            std::make_unique<feature::feature_registry>((*dimension)->states());
        if (const auto loaded =
                features->load_placed(shared->feature_data, shared->placed_ids);
            !loaded) {
            return std::unexpected{loaded.error()};
        }
    }
    std::unique_ptr<structure::structure_template_manager> templates;
    if (config.target != generation_target::terrain) {
        templates = std::make_unique<structure::structure_template_manager>(
            config.templates_root, (*dimension)->states());
    }
    return std::unique_ptr<chunk_generator>{new chunk_generator{
        std::move(config), std::move(shared), std::move(*dimension),
        std::move(features), std::move(templates)}};
}

auto chunk_generator::generate(const chunk_pos &pos)
    -> result<std::unique_ptr<proto_chunk>> {
    return generate_impl(pos);
}

auto chunk_generator::store_height_column(const chunk_pos &column,
                                          start_height_column value)
    -> const start_height_column * {
    if (const auto found = height_columns_.find(column);
        found != height_columns_.end()) {
        return &found->second;
    }
    while (height_column_order_.size() >= start_height_capacity) {
        const chunk_pos oldest = height_column_order_.front();
        height_column_order_.pop_front();
        height_columns_.erase(oldest);
    }
    height_column_order_.push_back(column);
    return &height_columns_.emplace(column, std::move(value)).first->second;
}

auto chunk_generator::store_block_column(block_column_key key,
                                         std::vector<block_state_id> states)
    -> const std::vector<block_state_id> * {
    if (const auto found = block_columns_.find(key);
        found != block_columns_.end()) {
        return &found->second;
    }
    while (block_column_order_.size() >= start_block_capacity) {
        const block_column_key oldest = block_column_order_.front();
        block_column_order_.pop_front();
        block_columns_.erase(oldest);
    }
    block_column_order_.push_back(key);
    return &block_columns_.emplace(std::move(key), std::move(states))
                .first->second;
}

auto chunk_generator::start_height_at(const chunk_pos &column)
    -> result<const start_height_column *> {
    if (const auto found = height_columns_.find(column);
        found != height_columns_.end()) {
        return &found->second;
    }
    auto generated = dimension_->generate_height_column(column);
    if (!generated) {
        return std::unexpected{generated.error()};
    }
    start_height_column snapshot;
    for (std::size_t kind = 0; kind < heightmap_kind_count; ++kind) {
        for (jint z = 0; z < 16; ++z) {
            for (jint x = 0; x < 16; ++x) {
                snapshot.heights[kind][static_cast<std::size_t>(z) * 16U +
                                       static_cast<std::size_t>(x)] =
                    (*generated)
                        ->first_available(static_cast<heightmap_kind>(kind), x,
                                          z);
            }
        }
    }
    return store_height_column(column, std::move(snapshot));
}

auto chunk_generator::start_block_at(const chunk_pos &column, jint x, jint y,
                                     jint z) -> result<block_state_id> {
    const auto &profile = dimension_->profile();
    if (y < profile.min_y || y >= profile.min_y + profile.height) {
        return dimension_->states().air();
    }
    const block_column_key key{.chunk = column, .x = x, .z = z};
    if (const auto found = block_columns_.find(key);
        found != block_columns_.end()) {
        return found->second[static_cast<std::size_t>(y - profile.min_y)];
    }
    auto generated = dimension_->generate_height_column(column);
    if (!generated) {
        return std::unexpected{generated.error()};
    }
    std::vector<block_state_id> states;
    states.reserve(static_cast<std::size_t>(profile.height));
    for (jint column_y = profile.min_y;
         column_y < profile.min_y + profile.height; ++column_y) {
        states.push_back((*generated)->state_at(x, column_y, z));
    }
    start_height_column snapshot;
    for (std::size_t kind = 0; kind < heightmap_kind_count; ++kind) {
        for (jint column_z = 0; column_z < 16; ++column_z) {
            for (jint column_x = 0; column_x < 16; ++column_x) {
                snapshot
                    .heights[kind][static_cast<std::size_t>(column_z) * 16U +
                                   static_cast<std::size_t>(column_x)] =
                    (*generated)
                        ->first_available(static_cast<heightmap_kind>(kind),
                                          column_x, column_z);
            }
        }
    }
    (void)store_height_column(column, std::move(snapshot));
    const auto *stored = store_block_column(key, std::move(states));
    return (*stored)[static_cast<std::size_t>(y - profile.min_y)];
}

auto chunk_generator::generate_impl(const chunk_pos &pos)
    -> result<std::unique_ptr<proto_chunk>> {
    auto &states = dimension_->states();
    const auto &profile = dimension_->profile();
    // Every chunk starts from the same multi-noise search state so its result
    // does not depend on which chunk this worker generated before.
    dimension_->reset_biome_search();

    // The starts stage feeds the beardifier, so it runs before the fill. Its
    // height probes read unbearded fill columns, not surface or carvers.
    std::optional<structure::structure_area> area;
    if (has_structures_) {
        std::optional<error> height_error;
        const auto height_at = [&](jint x, jint z,
                                   heightmap_kind kind) -> jint {
            if (height_error.has_value()) {
                return profile.min_y;
            }
            const chunk_pos column{.x = x >> 4, .z = z >> 4};
            const auto found = start_height_at(column);
            if (!found) {
                height_error = found.error();
                return profile.min_y;
            }
            return (*found)->heights[static_cast<std::size_t>(kind)]
                                    [static_cast<std::size_t>(z & 15) * 16U +
                                     static_cast<std::size_t>(x & 15)];
        };
        const auto block_at = [&](jint x, jint y, jint z) -> block_state_id {
            if (height_error.has_value()) {
                return states.air();
            }
            const chunk_pos column{.x = x >> 4, .z = z >> 4};
            const auto found = start_block_at(column, x & 15, y, z & 15);
            if (!found) {
                height_error = found.error();
                return states.air();
            }
            return *found;
        };

        structure::structure_start_context start_context{
            .seed = config_.seed,
            .data = &shared_->structure_data,
            .min_y = profile.min_y,
            .world_height = profile.height,
            .sea_level = profile.sea_level,
            .generation_min_y = dimension_->generation_min_y(),
            .generation_height = dimension_->generation_height(),
            .fixed_biome = !config_.fixed_biome.empty(),
        };
        if (config_.fixed_biome.empty()) {
            start_context.biome_at = [this](jint quart_x, jint quart_y,
                                            jint quart_z) {
                return dimension_->biome_at_quart(quart_x, quart_y, quart_z);
            };
            start_context.biome_name = [this](std::uint16_t id) {
                return dimension_->biome_name(id);
            };
        } else {
            start_context.biome_at = [](jint, jint, jint) {
                return std::uint16_t{0};
            };
            start_context.biome_name = [this](std::uint16_t) {
                return std::string_view{config_.fixed_biome};
            };
        }
        start_context.height_at = height_at;
        start_context.block_at = block_at;
        start_context.states = &states;
        start_context.templates = templates_.get();
        if (!config_.structure_sets.empty()) {
            const auto &allowed = config_.structure_sets;
            start_context.set_filter = [&allowed](std::string_view id) {
                return std::find(allowed.begin(), allowed.end(), id) !=
                       allowed.end();
            };
        }
        area = structure::generate_structure_area(pos, start_context, 9);
        if (height_error) {
            return std::unexpected{*height_error};
        }
    }

    const bool needs_neighbours = has_structures_ || has_features_;
    const std::size_t chunk_count = needs_neighbours ? 9U : 1U;
    std::array<std::unique_ptr<proto_chunk>, 9> chunks{};
    for (std::size_t index = 0; index < chunk_count; ++index) {
        const chunk_pos target{
            .x = pos.x + neighbour_offsets[index].x,
            .z = pos.z + neighbour_offsets[index].z,
        };
        density::sampler_context fill_context;
        std::optional<beardifier> beard;
        if (area.has_value()) {
            const auto collected = structure::collect_beard(
                area->starts, shared_->structure_data, target);
            if (!collected.pieces.empty() || !collected.junctions.empty() ||
                collected.affected_box.has_value()) {
                beard.emplace(collected.pieces, collected.junctions,
                              collected.affected_box);
                fill_context.set_field(density::context_key::beardifier,
                                       &*beard);
            }
        }
        const terrain_stages stages{
            .fill_context = area.has_value() ? &fill_context : nullptr,
            .carvers = (!config_.reference_world &&
                        config_.kind == dimension_kind::overworld)
                           ? &shared_->carvers.biomes
                           : nullptr,
            .surface = !config_.reference_world,
        };
        auto generated = dimension_->generate_terrain(target, stages);
        if (!generated) {
            return std::unexpected{generated.error()};
        }
        chunks[index] = std::move(*generated);
    }
    auto *center = chunks[0].get();
    if (config_.target == generation_target::terrain) {
        return std::move(chunks[0]);
    }

    std::vector<std::unique_ptr<proto_chunk>> neighbours;
    neighbours.reserve(chunk_count - 1);
    chunk_region region{*center, 1};
    for (std::size_t index = 1; index < chunk_count; ++index) {
        region.attach(*chunks[index]);
        neighbours.push_back(std::move(chunks[index]));
    }

    std::vector<const structure::structure_start *> owned;
    if (area.has_value()) {
        for (const auto &start : area->starts) {
            if (start.chunk.x == pos.x && start.chunk.z == pos.z) {
                owned.push_back(&start);
            }
        }
    }
    for (const auto *start : owned) {
        center->add_structure_start(make_start_record(*start));
    }
    if (area.has_value()) {
        for (const auto &reference : area->references) {
            center->add_structure_reference(reference.structure,
                                            reference.chunk_x,
                                            reference.chunk_z, reference.count);
        }
    }
    // References name starts owned by other chunks whose pieces can reach
    // this one. Add them to the placement set; the owning chunk's records are
    // already in `owned`.
    std::vector<const structure::structure_start *> active = owned;
    if (area.has_value()) {
        for (const auto &reference : area->references) {
            if (reference.chunk_x == pos.x && reference.chunk_z == pos.z) {
                continue;
            }
            for (const auto &start : area->starts) {
                if (start.structure == reference.structure &&
                    start.chunk.x == reference.chunk_x &&
                    start.chunk.z == reference.chunk_z) {
                    active.push_back(&start);
                    break;
                }
            }
        }
    }

    const auto resolve_state =
        [&states](std::string_view key) -> block_state_id {
        if (const auto found = states.find(key); found.has_value()) {
            return *found;
        }
        const auto interned = states.intern(key, states.infer_traits(key));
        return interned.has_value() ? *interned : no_block_state;
    };

    std::unordered_map<std::string, jint> adjusted_heights;
    structure::structure_place_context place_context{
        .region = &region,
        .chunk = center,
        .states = &states,
        .state = resolve_state,
        .templates = templates_.get(),
        .registry = &states,
        .seed = config_.seed,
        .sea_level = profile.sea_level,
        .level_random = nullptr,
        .adjusted_heights = &adjusted_heights,
        .data = &shared_->structure_data,
        .place_feature = {},
    };

    std::vector<std::string> feature_ids;
    for (const auto *start : active) {
        for (const auto &piece : start->pieces) {
            structure::collect_feature_ids(structure::to_piece_record(piece),
                                           feature_ids);
        }
    }
    std::optional<structure::structure_feature_placer> structure_features;
    if (!feature_ids.empty()) {
        structure_features.emplace(place_context, shared_->feature_data);
        if (const auto loaded = structure_features->load(feature_ids);
            !loaded) {
            return std::unexpected{loaded.error()};
        }
        place_context.place_feature =
            [&structure_features](std::string_view id, worldgen_random &random,
                                  const block_pos &position) {
                return structure_features->place(id, random, position);
            };
    }

    worldgen_random decoration{
        std::make_unique<xoroshiro_random_source>(config_.seed)};
    const jlong decoration_seed =
        decoration.set_decoration_seed(config_.seed, pos.x * 16, pos.z * 16);
    for (const auto *start : active) {
        const auto index = structure::structure_step_index(
            shared_->structure_data, start->structure);
        if (!index.has_value()) {
            continue;
        }
        const jint step = structure::generation_step_ordinal(start->step);
        worldgen_random random{std::make_unique<xoroshiro_random_source>(0)};
        random.set_feature_seed(decoration_seed, *index, step);
        for (const auto &piece : start->pieces) {
            if (piece.bounds.max_x < pos.x * 16 ||
                piece.bounds.min_x > pos.x * 16 + 15 ||
                piece.bounds.max_z < pos.z * 16 ||
                piece.bounds.min_z > pos.z * 16 + 15) {
                continue;
            }
            structure::place_structure_piece(
                place_context, structure::to_piece_record(piece), random);
        }
    }

    // Pieces a start owns are also clipped into the eight neighbours, so the
    // shape repair of the target chunk sees the whole building.
    for (const auto *start : active) {
        const auto index = structure::structure_step_index(
            shared_->structure_data, start->structure);
        if (!index.has_value()) {
            continue;
        }
        const jint step = structure::generation_step_ordinal(start->step);
        for (auto &neighbour : neighbours) {
            bool intersects = false;
            for (const auto &piece : start->pieces) {
                if (piece.bounds.max_x < neighbour->chunk_x() * 16 ||
                    piece.bounds.min_x > neighbour->chunk_x() * 16 + 15 ||
                    piece.bounds.max_z < neighbour->chunk_z() * 16 ||
                    piece.bounds.min_z > neighbour->chunk_z() * 16 + 15) {
                    continue;
                }
                intersects = true;
                break;
            }
            if (!intersects) {
                continue;
            }
            auto neighbour_context = place_context;
            neighbour_context.chunk = neighbour.get();
            worldgen_random random{
                std::make_unique<xoroshiro_random_source>(0)};
            const jlong neighbour_seed = random.set_decoration_seed(
                config_.seed, neighbour->chunk_x() * 16,
                neighbour->chunk_z() * 16);
            random.set_feature_seed(neighbour_seed, *index, step);
            for (const auto &piece : start->pieces) {
                if (piece.bounds.max_x < neighbour->chunk_x() * 16 ||
                    piece.bounds.min_x > neighbour->chunk_x() * 16 + 15 ||
                    piece.bounds.max_z < neighbour->chunk_z() * 16 ||
                    piece.bounds.min_z > neighbour->chunk_z() * 16 + 15) {
                    continue;
                }
                structure::place_structure_piece(
                    neighbour_context, structure::to_piece_record(piece),
                    random);
            }
        }
    }
    structure::refresh_chunk_shapes(*center, region, states);

    if (has_features_) {
        dimension_biome_resolver uncached{dimension_->biome_source(),
                                          dimension_->sampler()};
        chunk_biome_resolver biome_view{
            [&region](jint chunk_x, jint chunk_z) -> const proto_chunk * {
                return region.find(chunk_x, chunk_z);
            },
            uncached};
        biome_manager manager{biome_view,
                              biome_manager::obfuscate_seed(config_.seed)};
        const auto biome_of =
            [this, &manager](const block_pos &block) -> std::string_view {
            if (!config_.fixed_biome.empty()) {
                return config_.fixed_biome;
            }
            return dimension_->biome_name(
                manager.get_biome(block.x, block.y, block.z));
        };
        const auto temperature =
            [this, &biome_of](const block_pos &block) -> std::optional<float> {
            return dimension_->biome_temperature(biome_of(block), block.x,
                                                 block.y, block.z);
        };
        const auto world_state = [&region](const block_pos &block) {
            return region.state_at(block.x, block.y, block.z);
        };

        feature::feature_context feature_context{
            .placement =
                feature::placement_context{
                    .region = &region,
                    .chunk = center,
                    .states = &states,
                    .generation =
                        provider::generation_context{
                            .min_y = center->min_y(),
                            .height = center->height(),
                            .sea_level = profile.sea_level,
                            .world_seed = config_.seed,
                        },
                },
            .place_by_id = {},
            .templates = templates_.get(),
            .registry = &states,
            .processor_list = {},
        };
        feature_context.placement.height = [&region](heightmap_kind kind,
                                                     jint x, jint z) {
            return region.height_at(kind, x, z);
        };
        feature_context.placement.biome = biome_of;
        feature_context.placement.resolve_state = resolve_state;
        feature_context.placement.should_freeze =
            [&temperature, &world_state, &states](const block_pos &block) {
                const auto value = temperature(
                    block_pos{.x = block.x, .y = block.y + 1, .z = block.z});
                if (!value.has_value() || *value >= 0.15F) {
                    return false;
                }
                return states.traits(world_state(block)).water;
            };
        feature_context.placement.should_snow = [&temperature, &world_state,
                                                 &states](
                                                    const block_pos &block) {
            const auto value = temperature(block);
            if (!value.has_value() || *value >= 0.15F) {
                return false;
            }
            const auto state = world_state(block);
            const auto name = states.name(state);
            if (!states.traits(state).air &&
                !name.starts_with("minecraft:snow")) {
                return false;
            }
            const auto below = world_state(
                block_pos{.x = block.x, .y = block.y - 1, .z = block.z});
            if (states.in_tag(below, "minecraft:cannot_support_snow_layer")) {
                return false;
            }
            if (states.in_tag(below, "minecraft:support_override_snow_layer")) {
                return true;
            }
            const auto below_name = states.name(below);
            if (below_name.ends_with("_leaves") ||
                below_name.starts_with("minecraft:bamboo") ||
                below_name.starts_with("minecraft:vine") ||
                below_name.starts_with("minecraft:snow")) {
                return false;
            }
            return states.traits(below).blocks_motion;
        };
        if (templates_ != nullptr) {
            feature_context.processor_list = [this](std::string_view id)
                -> const structure::structure_processor_list * {
                const auto found = shared_->structure_data.processor_lists.find(
                    std::string{id});
                if (found == shared_->structure_data.processor_lists.end()) {
                    return nullptr;
                }
                return &templates_->processor_list(id, found->second);
            };
        }

        std::vector<std::string> candidate_biomes;
        if (!config_.fixed_biome.empty()) {
            candidate_biomes.push_back(config_.fixed_biome);
        } else {
            std::set<std::string> names;
            const jint min_quart_x = (pos.x - 1) * 4;
            const jint max_quart_x = (pos.x + 2) * 4;
            const jint min_quart_z = (pos.z - 1) * 4;
            const jint max_quart_z = (pos.z + 2) * 4;
            const jint min_quart_y = profile.min_y >> 2;
            const jint max_quart_y = (profile.min_y + profile.height) >> 2;
            for (jint quart_x = min_quart_x; quart_x < max_quart_x; ++quart_x) {
                for (jint quart_y = min_quart_y; quart_y < max_quart_y;
                     ++quart_y) {
                    for (jint quart_z = min_quart_z; quart_z < max_quart_z;
                         ++quart_z) {
                        names.emplace(
                            dimension_->biome_name(dimension_->biome_at_quart(
                                quart_x, quart_y, quart_z)));
                    }
                }
            }
            candidate_biomes.assign(names.begin(), names.end());
        }
        feature::decorate(feature_context, shared_->feature_plan,
                          features_->placed(), candidate_biomes,
                          shared_->feature_data.biomes, config_.seed);
    }

    center->set_status(target_status(config_.target));
    return std::move(chunks[0]);
}

auto chunk_generator::generate_batch(std::span<const chunk_pos> positions,
                                     std::size_t workers)
    -> result<std::vector<std::unique_ptr<proto_chunk>>> {
    std::vector<std::unique_ptr<proto_chunk>> chunks(positions.size());
    if (positions.empty()) {
        return chunks;
    }
    std::size_t worker_count = workers;
    if (worker_count == 0) {
        worker_count = std::thread::hardware_concurrency();
        if (worker_count == 0) {
            worker_count = 1;
        }
    }
    worker_count = std::min(worker_count, positions.size());
    if (worker_count <= 1) {
        for (std::size_t index = 0; index < positions.size(); ++index) {
            auto generated = generate_impl(positions[index]);
            if (!generated) {
                return std::unexpected{generated.error()};
            }
            chunks[index] = std::move(*generated);
        }
        return chunks;
    }

    while (workers_.size() + 1 < worker_count) {
        auto created = create_with_shared(config_, shared_);
        if (!created) {
            return std::unexpected{created.error()};
        }
        workers_.push_back(std::move(*created));
    }
    std::vector<chunk_generator *> worker_instances;
    worker_instances.reserve(worker_count);
    worker_instances.push_back(this);
    for (std::size_t index = 1; index < worker_count; ++index) {
        worker_instances.push_back(workers_[index - 1].get());
    }

    const std::size_t claim_size = std::clamp<std::size_t>(
        (positions.size() + worker_count * 2U - 1) / (worker_count * 2U), 1U,
        16U);
    std::atomic<std::size_t> next{0};
    std::vector<std::optional<error>> errors(positions.size());
    std::vector<std::jthread> threads;
    threads.reserve(worker_count);
    for (std::size_t worker = 0; worker < worker_count; ++worker) {
        threads.emplace_back([&, worker] {
            auto *generator = worker_instances[worker];
            for (;;) {
                const std::size_t begin = next.fetch_add(claim_size);
                if (begin >= positions.size()) {
                    return;
                }
                const std::size_t end =
                    std::min(begin + claim_size, positions.size());
                for (std::size_t index = begin; index < end; ++index) {
                    try {
                        auto generated =
                            generator->generate_impl(positions[index]);
                        if (!generated) {
                            errors[index] = generated.error();
                        } else {
                            chunks[index] = std::move(*generated);
                        }
                    } catch (const std::exception &exception) {
                        errors[index] =
                            fail(error_code::unknown, exception.what());
                    } catch (...) {
                        errors[index] = fail(error_code::unknown,
                                             "chunk generation failed");
                    }
                }
            }
        });
    }
    for (auto &thread : threads) {
        thread.join();
    }
    for (const auto &error : errors) {
        if (error.has_value()) {
            return std::unexpected{*error};
        }
    }
    return chunks;
}

auto chunk_generator::generate_area(chunk_pos origin, jint width, jint depth,
                                    std::size_t workers)
    -> result<std::vector<std::unique_ptr<proto_chunk>>> {
    if (width <= 0 || depth <= 0) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "generation area needs a positive width and depth")};
    }
    std::vector<chunk_pos> positions;
    positions.reserve(static_cast<std::size_t>(width) *
                      static_cast<std::size_t>(depth));
    for (jint x = 0; x < width; ++x) {
        for (jint z = 0; z < depth; ++z) {
            positions.push_back(
                chunk_pos{.x = origin.x + x, .z = origin.z + z});
        }
    }
    return generate_batch(positions, workers);
}

} // namespace refrain::worldgen
