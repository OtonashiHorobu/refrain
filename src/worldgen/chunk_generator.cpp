// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/chunk_generator.hpp>

#include <refrain/worldgen/biome/biome_manager.hpp>
#include <refrain/worldgen/biome/biome_view.hpp>
#include <refrain/worldgen/chunk/chunk_region.hpp>
#include <refrain/worldgen/chunk/status.hpp>
#include <refrain/worldgen/core/rng.hpp>
#include <refrain/worldgen/feature/feature_data.hpp>
#include <refrain/worldgen/feature/feature_plan.hpp>
#include <refrain/worldgen/feature/feature_stage.hpp>
#include <refrain/worldgen/provider.hpp>
#include <refrain/worldgen/stage_profile.hpp>
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

/// Every chunk offset in a square of the given radius, ordered by ascending x
/// and then ascending z. A force-load adds one ticket per chunk in that order,
/// so a reference world built that way decorated the chunks in this sequence.
auto square_offsets(jint radius) -> std::vector<chunk_pos> {
    std::vector<chunk_pos> offsets;
    offsets.reserve(
        static_cast<std::size_t>((2 * radius + 1) * (2 * radius + 1)));
    for (jint x = -radius; x <= radius; ++x) {
        for (jint z = -radius; z <= radius; ++z) {
            offsets.push_back(chunk_pos{.x = x, .z = z});
        }
    }
    return offsets;
}

/// Moves `center` to the front of `positions`. The chunk generation loop
/// keeps the target first so the returned chunk is `chunks[0]`.
auto center_first_positions(std::vector<chunk_pos> positions,
                            const chunk_pos &center) -> std::vector<chunk_pos> {
    const auto found = std::find(positions.begin(), positions.end(), center);
    if (found != positions.end()) {
        positions.erase(found);
        positions.insert(positions.begin(), center);
    }
    return positions;
}

} // namespace

auto force_load_decoration_order(const chunk_rectangle &box)
    -> std::vector<chunk_pos> {
    std::vector<chunk_pos> order;
    if (box.empty()) {
        return order;
    }
    std::set<chunk_pos> decorated;
    const auto push_square = [&](jint center_x, jint center_z) {
        for (jint x = center_x - 1; x <= center_x + 1; ++x) {
            for (jint z = center_z - 1; z <= center_z + 1; ++z) {
                const chunk_pos position{.x = x, .z = z};
                if (decorated.insert(position).second) {
                    order.push_back(position);
                }
            }
        }
    };
    // A force-loaded chunk's ticket keeps its own two rings at the full
    // status, so the ticket's generation task runs the feature layer over the
    // three-by-three around it. Tickets run in the order the command adds
    // them: ascending x, then ascending z.
    for (jint x = box.min_x; x <= box.max_x; ++x) {
        for (jint z = box.min_z; z <= box.max_z; ++z) {
            push_square(x, z);
        }
    }
    // The propagated tickets two rings out are full chunks themselves, so
    // their own tasks run one more feature layer each, in the order the
    // tickets reached them.
    for (jint x = box.min_x - 2; x <= box.max_x + 2; ++x) {
        for (jint z = box.min_z - 2; z <= box.max_z + 2; ++z) {
            const bool propagated_ring = x < box.min_x - 1 ||
                                         x > box.max_x + 1 ||
                                         z < box.min_z - 1 || z > box.max_z + 1;
            if (propagated_ring) {
                push_square(x, z);
            }
        }
    }
    return order;
}

struct chunk_generator::shared_generator_data {
    /// Immutable dimension data: noise settings, material documents and rule,
    /// climates, noise functions, block tags, possible biomes and the
    /// dimension's carvers.
    std::shared_ptr<const dimension_shared_data> dimension{};
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
    if (config.decorate_neighbours && config.fixed_biome.empty()) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "neighbour feature decoration requires a fixed biome")};
    }
    if (config.neighbour_radius < 0 || config.neighbour_radius > 8) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "neighbour feature radius is outside its supported range")};
    }
    if (config.neighbours_ticket_sweep && config.neighbour_box.empty()) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "the ticket sweep needs a reference neighbour box")};
    }
    const bool needs_structures = config.target != generation_target::terrain;
    const bool needs_features = needs_structures && !config.reference_world;

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

auto chunk_generator::cached_terrain(const chunk_pos &pos) const
    -> const proto_chunk * {
    const auto found = terrain_cache_.find(pos);
    return found == terrain_cache_.end() ? nullptr : found->second.get();
}

void chunk_generator::store_terrain(const chunk_pos &pos,
                                    std::unique_ptr<proto_chunk> chunk) {
    if (terrain_cache_.contains(pos)) {
        return;
    }
    while (terrain_cache_order_.size() >= terrain_cache_capacity) {
        terrain_cache_.erase(terrain_cache_order_.front());
        terrain_cache_order_.pop_front();
    }
    terrain_cache_order_.push_back(pos);
    terrain_cache_.emplace(pos, std::move(chunk));
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
        start_context.profile = profile_;
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
    // A feature reference run may decorate a wider neighbourhood than the
    // eight surrounding chunks, or the reference world's whole generated
    // rectangle. Decorated chunks read their own generation region, so the
    // attached terrain covers one more ring than the decorated set; that ring
    // holds terrain only, exactly as the reference generated it as a
    // dependency of the decorated chunks.
    const bool boxed =
        config_.decorate_neighbours && !config_.neighbour_box.empty();
    if (boxed && (pos.x < config_.neighbour_box.min_x ||
                  pos.x > config_.neighbour_box.max_x ||
                  pos.z < config_.neighbour_box.min_z ||
                  pos.z > config_.neighbour_box.max_z)) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "feature neighbour box does not contain the target chunk")};
    }
    const jint decorated_radius =
        config_.decorate_neighbours
            ? std::max<jint>(0, config_.neighbour_radius)
            : 0;
    std::vector<chunk_pos> decorated_positions;
    std::vector<chunk_pos> region_positions;
    if (!needs_neighbours) {
        region_positions.push_back(pos);
    } else if (boxed) {
        const auto &box = config_.neighbour_box;
        if (config_.neighbours_ticket_sweep) {
            decorated_positions = force_load_decoration_order(box);
            // The sweep covers three rings around the box; every decorated
            // chunk reads the chunks its own generation region covers, which
            // adds one more terrain ring.
            for (jint x = box.min_x - 4; x <= box.max_x + 4; ++x) {
                for (jint z = box.min_z - 4; z <= box.max_z + 4; ++z) {
                    region_positions.push_back(chunk_pos{.x = x, .z = z});
                }
            }
        } else {
            for (jint x = box.min_x; x <= box.max_x; ++x) {
                for (jint z = box.min_z; z <= box.max_z; ++z) {
                    decorated_positions.push_back(chunk_pos{.x = x, .z = z});
                }
            }
            for (jint x = box.min_x - 1; x <= box.max_x + 1; ++x) {
                for (jint z = box.min_z - 1; z <= box.max_z + 1; ++z) {
                    region_positions.push_back(chunk_pos{.x = x, .z = z});
                }
            }
        }
    } else {
        for (const auto &offset : square_offsets(decorated_radius)) {
            decorated_positions.push_back(
                chunk_pos{.x = pos.x + offset.x, .z = pos.z + offset.z});
        }
        const jint region_radius = std::max<jint>(1, decorated_radius + 1);
        for (const auto &offset : square_offsets(region_radius)) {
            region_positions.push_back(
                chunk_pos{.x = pos.x + offset.x, .z = pos.z + offset.z});
        }
    }
    if (decorated_positions.empty()) {
        decorated_positions.push_back(pos);
    }
    region_positions = center_first_positions(std::move(region_positions), pos);
    const std::size_t chunk_count = region_positions.size();
    std::vector<std::unique_ptr<proto_chunk>> chunks(chunk_count);
    for (std::size_t index = 0; index < chunk_count; ++index) {
        const chunk_pos target = region_positions[index];
        density::sampler_context fill_context;
        std::optional<beardifier> beard;
        bool bearded = false;
        if (area.has_value()) {
            const stage_timer timer{profile_, "structure.beard"};
            const auto collected = structure::collect_beard(
                area->starts, shared_->structure_data, target);
            if (!collected.pieces.empty() || !collected.junctions.empty() ||
                collected.affected_box.has_value()) {
                beard.emplace(collected.pieces, collected.junctions,
                              collected.affected_box);
                fill_context.set_field(density::context_key::beardifier,
                                       &*beard);
                bearded = true;
            }
        }
        // Terrain is a pure function of the seed, the data and the position
        // once the biome search starts from a fresh candidate, so the eight
        // neighbour fills repeat neither across the batch nor within a
        // target. A cached chunk is cloned because the structure and feature
        // stages write into the generation region, and a bearded fill is
        // never reused or stored: its result depends on the structure pieces
        // around the target, not on the position alone.
        if (needs_neighbours && !bearded) {
            if (const auto *cached = cached_terrain(target);
                cached != nullptr) {
                const stage_timer timer{profile_, "terrain.cached_clone"};
                chunks[index] = cached->clone();
                continue;
            }
        }
        // A reference world strips the surface pass and carvers unless the
        // carver reference mode asks for the terrain stages of the dimension.
        const bool reference_terrain =
            !config_.reference_world || config_.reference_carvers;
        const terrain_stages stages{
            .fill_context = area.has_value() ? &fill_context : nullptr,
            .carvers = reference_terrain ? dimension_->carvers() : nullptr,
            .surface = reference_terrain,
            .profile = profile_,
        };
        // A bearded fill is regenerated whenever a target needs it, so its row
        // overlaps the terrain rows and only reports how much of the fill work
        // the beardifier costs.
        std::optional<stage_timer> bearded_fill;
        if (bearded) {
            bearded_fill.emplace(profile_, "terrain.bearded_fill");
        }
        dimension_->reset_biome_search();
        auto generated = dimension_->generate_terrain(target, stages);
        if (!generated) {
            return std::unexpected{generated.error()};
        }
        if (needs_neighbours && !bearded) {
            const stage_timer timer{profile_, "terrain.cache_store"};
            store_terrain(target, (*generated)->clone());
        }
        chunks[index] = std::move(*generated);
    }
    auto *center = chunks[0].get();
    if (config_.target == generation_target::terrain) {
        return std::move(chunks[0]);
    }

    chunk_region region{*center, 1};
    for (std::size_t index = 1; index < chunk_count; ++index) {
        region.attach(*chunks[index]);
    }
    // Pieces are clipped into the eight direct neighbours so the shape repair
    // of the compared chunk sees the complete building.
    std::vector<proto_chunk *> neighbours;
    if (chunk_count > 1) {
        for (const auto &offset : square_offsets(1)) {
            if (offset.x == 0 && offset.z == 0) {
                continue;
            }
            if (auto *chunk = region.find(pos.x + offset.x, pos.z + offset.z);
                chunk != nullptr) {
                neighbours.push_back(chunk);
            }
        }
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

    std::optional<structure::structure_feature_placer> structure_features;
    {
        const stage_timer timer{profile_, "structure.features"};
        std::vector<std::string> feature_ids;
        for (const auto *start : active) {
            for (const auto &piece : start->pieces) {
                structure::collect_feature_ids(
                    structure::to_piece_record(piece), feature_ids);
            }
        }
        if (!feature_ids.empty()) {
            structure_features.emplace(place_context, shared_->feature_data);
            if (const auto loaded = structure_features->load(feature_ids);
                !loaded) {
                return std::unexpected{loaded.error()};
            }
            place_context.place_feature =
                [&structure_features](std::string_view id,
                                      worldgen_random &random,
                                      const block_pos &position) {
                    return structure_features->place(id, random, position);
                };
        }
    }

    stage_timer pieces{profile_, "structure.pieces"};
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
            neighbour_context.chunk = neighbour;
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
    pieces.stop();
    {
        const stage_timer timer{profile_, "structure.shapes"};
        structure::refresh_chunk_shapes(*center, region, states);
    }

    if (has_features_) {
        // Feature placement reads biomes through the chunks of the region and
        // falls back to the noise source only outside it. Starting that
        // fallback from a fresh candidate keeps the decoration independent of
        // how many fills the target needed.
        stage_timer prepare{profile_, "feature.prepare"};
        dimension_->reset_biome_search();
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
                            .min_y = dimension_->generation_min_y(),
                            .height = dimension_->generation_height(),
                            .sea_level = profile.sea_level,
                            .world_seed = config_.seed,
                        },
                },
            .place_by_id = {},
            .templates = templates_.get(),
            .registry = &states,
            .processor_list = {},
            .profile = profile_,
        };
        feature_context.placement.height = [&region](heightmap_kind kind,
                                                     jint x, jint z) {
            return region.height_at(kind, x, z);
        };
        feature_context.place_configured = [&feature_context,
                                            this](std::string_view id,
                                                  worldgen_random &random,
                                                  const block_pos &position) {
            const auto *configured =
                features_ == nullptr ? nullptr : features_->configured(id);
            return configured != nullptr &&
                   configured->place(feature_context, random, position);
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
        prepare.stop();
        const auto decorate_chunk = [&](proto_chunk &target) {
            feature_context.placement.chunk = &target;
            feature_context.placement.generation.min_y =
                dimension_->generation_min_y();
            feature_context.placement.generation.height =
                dimension_->generation_height();
            const stage_timer timer{profile_, "feature.decorate"};
            feature::decorate(feature_context, shared_->feature_plan,
                              features_->placed(), candidate_biomes,
                              shared_->feature_data.biomes, config_.seed);
        };
        // A reference world saves every chunk of the generated area, so its
        // chunks contain the feature writes of their neighbours. Decorating
        // the target chunk first and then its neighbours reproduces those
        // writes for a target chunk that the reference generated before its
        // neighbours. The generation-order mode follows the ticket order of a
        // force-load instead: every chunk that precedes the target in
        // ascending x and z decorates first, then the target, then the rest.
        const auto decorate_position = [&](const chunk_pos &position) {
            if (position == pos) {
                // The target writes through its own generation region too,
                // so restore its write zone after a neighbour decoration.
                region.set_write_focus(pos, 1);
                decorate_chunk(*center);
                return;
            }
            if (auto *target = region.find(position); target != nullptr) {
                // The reference generation region of this chunk allows writes
                // in its eight neighbours, not around the compared chunk.
                region.set_write_focus(position, 1);
                decorate_chunk(*target);
            }
        };
        if (!config_.decorate_neighbours) {
            decorate_chunk(*center);
        } else if (config_.neighbours_in_generation_order ||
                   config_.neighbours_ticket_sweep) {
            for (const auto &position : decorated_positions) {
                decorate_position(position);
            }
        } else if (config_.neighbours_before_target) {
            for (const auto &position : decorated_positions) {
                if (position != pos) {
                    decorate_position(position);
                }
            }
            decorate_position(pos);
        } else {
            decorate_position(pos);
            for (const auto &position : decorated_positions) {
                if (position != pos) {
                    decorate_position(position);
                }
            }
        }
    }

    center->set_status(target_status(config_.target));
    return std::move(chunks[0]);
}

auto chunk_generator::worker_instances(std::size_t worker_count)
    -> result<std::vector<chunk_generator *>> {
    while (workers_.size() + 1 < worker_count) {
        auto created = create_with_shared(config_, shared_);
        if (!created) {
            return std::unexpected{created.error()};
        }
        workers_.push_back(std::move(*created));
    }
    std::vector<chunk_generator *> instances;
    instances.reserve(worker_count);
    instances.push_back(this);
    for (std::size_t index = 1; index < worker_count; ++index) {
        instances.push_back(workers_[index - 1].get());
    }
    return instances;
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

    auto instances = worker_instances(worker_count);
    if (!instances) {
        return std::unexpected{instances.error()};
    }
    auto &instances_by_worker = *instances;

    const std::size_t claim_size = std::clamp<std::size_t>(
        (positions.size() + worker_count * 2U - 1) / (worker_count * 2U), 1U,
        16U);
    std::atomic<std::size_t> next{0};
    std::vector<std::optional<error>> errors(positions.size());
    std::vector<std::jthread> threads;
    threads.reserve(worker_count);
    for (std::size_t worker = 0; worker < worker_count; ++worker) {
        threads.emplace_back([&, worker] {
            auto *generator = instances_by_worker[worker];
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
    std::size_t worker_count = workers;
    if (worker_count == 0) {
        worker_count = std::thread::hardware_concurrency();
        if (worker_count == 0) {
            worker_count = 1;
        }
    }
    worker_count = std::min(worker_count, positions.size());
    if (worker_count <= 1) {
        return generate_batch(positions, 1);
    }
    auto instances = worker_instances(worker_count);
    if (!instances) {
        return std::unexpected{instances.error()};
    }
    // A worker takes one contiguous run of the rectangle. Consecutive chunks
    // share most of their neighbour fills, so a run keeps each worker's
    // terrain cache hot; work stealing would hand every worker a scattered
    // set and repeat those fills once per worker.
    std::vector<std::unique_ptr<proto_chunk>> chunks(positions.size());
    std::vector<std::optional<error>> errors(positions.size());
    const std::size_t base = positions.size() / worker_count;
    const std::size_t extra = positions.size() % worker_count;
    std::vector<std::jthread> threads;
    threads.reserve(worker_count);
    std::size_t begin = 0;
    for (std::size_t worker = 0; worker < worker_count; ++worker) {
        const std::size_t end = begin + base + (worker < extra ? 1U : 0U);
        auto *generator = (*instances)[worker];
        threads.emplace_back([&, generator, begin, end] {
            for (std::size_t index = begin; index < end; ++index) {
                try {
                    auto generated = generator->generate_impl(positions[index]);
                    if (!generated) {
                        errors[index] = generated.error();
                    } else {
                        chunks[index] = std::move(*generated);
                    }
                } catch (const std::exception &exception) {
                    errors[index] = fail(error_code::unknown, exception.what());
                } catch (...) {
                    errors[index] =
                        fail(error_code::unknown, "chunk generation failed");
                }
            }
        });
        begin = end;
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

} // namespace refrain::worldgen
