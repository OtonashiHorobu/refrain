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

auto same_box(const bounding_box &left, const bounding_box &right) -> bool {
    return left.min_x == right.min_x && left.min_y == right.min_y &&
           left.min_z == right.min_z && left.max_x == right.max_x &&
           left.max_y == right.max_y && left.max_z == right.max_z;
}

/// Exact comparison of two beardifier inputs. The fill samples the density
/// with the beardifier added, so two fills are interchangeable exactly when
/// these inputs are equal.
auto same_beard(const structure::structure_beard &left,
                const structure::structure_beard &right) -> bool {
    if (left.pieces.size() != right.pieces.size() ||
        left.junctions.size() != right.junctions.size()) {
        return false;
    }
    if (left.affected_box.has_value() != right.affected_box.has_value()) {
        return false;
    }
    if (left.affected_box.has_value() &&
        !same_box(*left.affected_box, *right.affected_box)) {
        return false;
    }
    for (std::size_t index = 0; index < left.pieces.size(); ++index) {
        const auto &first = left.pieces[index];
        const auto &second = right.pieces[index];
        if (first.adjustment != second.adjustment ||
            first.ground_level_delta != second.ground_level_delta ||
            !same_box(first.box, second.box)) {
            return false;
        }
    }
    for (std::size_t index = 0; index < left.junctions.size(); ++index) {
        const auto &first = left.junctions[index];
        const auto &second = right.junctions[index];
        if (first.source_x != second.source_x ||
            first.source_ground_y != second.source_ground_y ||
            first.source_z != second.source_z) {
            return false;
        }
    }
    return true;
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

/// The rectangle grown by `radius` chunks on every side.
auto expand(const chunk_rectangle &box, jint radius) -> chunk_rectangle {
    return chunk_rectangle{.min_x = box.min_x - radius,
                           .min_z = box.min_z - radius,
                           .max_x = box.max_x + radius,
                           .max_z = box.max_z + radius};
}

auto contains(const chunk_rectangle &box, const chunk_pos &pos) -> bool {
    return pos.x >= box.min_x && pos.x <= box.max_x && pos.z >= box.min_z &&
           pos.z <= box.max_z;
}

/// The distinct biome ids a chunk's palette holds, one per 4x4x4 cell.
auto chunk_biomes(const proto_chunk &chunk) -> std::vector<std::uint16_t> {
    std::vector<std::uint16_t> ids;
    ids.reserve(16);
    const jint top = chunk.min_y() + chunk.height();
    for (jint y = chunk.min_y(); y < top; y += 4) {
        for (jint z = 0; z < 16; z += 4) {
            for (jint x = 0; x < 16; x += 4) {
                const auto id = chunk.biome_at(x, y, z);
                if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
                    ids.push_back(id);
                }
            }
        }
    }
    return ids;
}

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
    if (config.tile_size < 1 || config.tile_size > 32) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "generation tile edge is outside its supported range")};
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

auto chunk_generator::cached_terrain(
    const chunk_pos &pos, const structure::structure_beard *beard) const
    -> const proto_chunk * {
    const auto found = terrain_cache_.find(pos);
    if (found == terrain_cache_.end()) {
        return nullptr;
    }
    const auto &entry = found->second;
    if (beard == nullptr) {
        return entry.bearded ? nullptr : entry.chunk.get();
    }
    return entry.bearded && same_beard(entry.beard, *beard) ? entry.chunk.get()
                                                            : nullptr;
}

void chunk_generator::store_terrain(const chunk_pos &pos,
                                    const structure::structure_beard *beard,
                                    std::unique_ptr<proto_chunk> chunk) {
    if (const auto found = terrain_cache_.find(pos);
        found != terrain_cache_.end()) {
        // A different beardifier input replaces the old fill; the position
        // stays at its place in the eviction order.
        found->second = cached_terrain_entry{
            .bearded = beard != nullptr,
            .beard = beard != nullptr ? *beard : structure::structure_beard{},
            .chunk = std::move(chunk),
        };
        return;
    }
    while (terrain_cache_order_.size() >= terrain_cache_capacity) {
        terrain_cache_.erase(terrain_cache_order_.front());
        terrain_cache_order_.pop_front();
    }
    terrain_cache_order_.push_back(pos);
    terrain_cache_.emplace(
        pos,
        cached_terrain_entry{
            .bearded = beard != nullptr,
            .beard = beard != nullptr ? *beard : structure::structure_beard{},
            .chunk = std::move(chunk),
        });
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

auto chunk_generator::make_start_context(std::optional<error> &error)
    -> structure::structure_start_context {
    auto &states = dimension_->states();
    const auto &profile = dimension_->profile();
    // The height and block probes are lazily read while a start expands. The
    // first failure is stored and every later probe returns a sentinel, so the
    // caller reports one error after the stage instead of throwing mid-scan.
    const auto height_at = [&error, &profile,
                            this](jint x, jint z, heightmap_kind kind) -> jint {
        if (error.has_value()) {
            return profile.min_y;
        }
        const chunk_pos column{.x = x >> 4, .z = z >> 4};
        const auto found = start_height_at(column);
        if (!found) {
            error = found.error();
            return profile.min_y;
        }
        return (*found)->heights[static_cast<std::size_t>(kind)]
                                [static_cast<std::size_t>(z & 15) * 16U +
                                 static_cast<std::size_t>(x & 15)];
    };
    const auto block_at = [&error, &states, this](jint x, jint y,
                                                  jint z) -> block_state_id {
        if (error.has_value()) {
            return states.air();
        }
        const chunk_pos column{.x = x >> 4, .z = z >> 4};
        const auto found = start_block_at(column, x & 15, y, z & 15);
        if (!found) {
            error = found.error();
            return states.air();
        }
        return *found;
    };

    structure::structure_start_context context{
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
        context.biome_at = [this](jint quart_x, jint quart_y, jint quart_z) {
            return dimension_->biome_at_quart(quart_x, quart_y, quart_z);
        };
        context.biome_name = [this](std::uint16_t id) {
            return dimension_->biome_name(id);
        };
    } else {
        context.biome_at = [](jint, jint, jint) { return std::uint16_t{0}; };
        context.biome_name = [this](std::uint16_t) {
            return std::string_view{config_.fixed_biome};
        };
    }
    context.height_at = height_at;
    context.block_at = block_at;
    context.states = &states;
    context.templates = templates_.get();
    context.profile = profile_;
    if (!config_.structure_sets.empty()) {
        const auto &allowed = config_.structure_sets;
        context.set_filter = [&allowed](std::string_view id) {
            return std::find(allowed.begin(), allowed.end(), id) !=
                   allowed.end();
        };
    }
    return context;
}

auto chunk_generator::fill_terrain(const chunk_pos &pos,
                                   const structure::structure_beard *beard)
    -> result<std::unique_ptr<proto_chunk>> {
    density::sampler_context fill_context;
    std::optional<beardifier> beardifier;
    if (beard != nullptr) {
        beardifier.emplace(beard->pieces, beard->junctions,
                           beard->affected_box);
        fill_context.set_field(density::context_key::beardifier, &*beardifier);
    }
    // A reference world strips the surface pass and carvers unless the carver
    // reference mode asks for the terrain stages of the dimension.
    const bool reference_terrain =
        !config_.reference_world || config_.reference_carvers;
    const terrain_stages stages{
        .fill_context = beard != nullptr ? &fill_context : nullptr,
        .carvers = reference_terrain ? dimension_->carvers() : nullptr,
        .surface = reference_terrain,
        .profile = profile_,
    };
    // A beardifier changes the density the fill samples, so its row overlaps
    // the terrain rows and reports how much of the fill work it costs.
    std::optional<stage_timer> bearded_fill;
    if (beard != nullptr) {
        bearded_fill.emplace(profile_, "terrain.bearded_fill");
    }
    dimension_->reset_biome_search();
    auto generated = dimension_->generate_terrain(pos, stages);
    if (!generated) {
        return std::unexpected{generated.error()};
    }
    return std::move(*generated);
}

auto chunk_generator::candidate_biomes_at(const chunk_region &region,
                                          const chunk_pos &position) const
    -> std::vector<std::string> {
    if (!config_.fixed_biome.empty()) {
        return {config_.fixed_biome};
    }
    // The reference possible-biome set of a chunk is the palette of its
    // three-by-three. Reading the filled palettes reuses the biome grid the
    // fill already sampled, so the set does not depend on the query order.
    std::set<std::uint16_t> ids;
    for (jint dx = -1; dx <= 1; ++dx) {
        for (jint dz = -1; dz <= 1; ++dz) {
            const auto *chunk = region.find(position.x + dx, position.z + dz);
            if (chunk == nullptr) {
                continue;
            }
            const jint top = chunk->min_y() + chunk->height();
            for (jint y = chunk->min_y(); y < top; y += 4) {
                for (jint z = 0; z < 16; z += 4) {
                    for (jint x = 0; x < 16; x += 4) {
                        ids.insert(chunk->biome_at(x, y, z));
                    }
                }
            }
        }
    }
    std::set<std::string> names;
    for (const auto id : ids) {
        names.emplace(dimension_->biome_name(id));
    }
    return {names.begin(), names.end()};
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
        auto start_context = make_start_context(height_error);
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
        std::optional<structure::structure_beard> collected_beard;
        if (area.has_value()) {
            const stage_timer timer{profile_, "structure.beard"};
            auto collected = structure::collect_beard(
                area->starts, shared_->structure_data, target);
            if (!collected.pieces.empty() || !collected.junctions.empty() ||
                collected.affected_box.has_value()) {
                collected_beard = std::move(collected);
            }
        }
        // Terrain is a pure function of the seed, the data, the position and
        // the beardifier input, so a fill repeats neither across the batch nor
        // within a target. A cached chunk is cloned because the structure and
        // feature stages write into the generation region. Two fills of one
        // position are interchangeable exactly when their beardifier inputs
        // match, which is why the cache compares the collected beard rather
        // than keying on the position alone.
        if (needs_neighbours) {
            if (const auto *cached = cached_terrain(
                    target,
                    collected_beard.has_value() ? &*collected_beard : nullptr);
                cached != nullptr) {
                const stage_timer timer{profile_, "terrain.cached_clone"};
                chunks[index] = cached->clone();
                continue;
            }
        }
        auto generated = fill_terrain(
            target, collected_beard.has_value() ? &*collected_beard : nullptr);
        if (!generated) {
            return std::unexpected{generated.error()};
        }
        if (needs_neighbours) {
            const stage_timer timer{profile_, "terrain.cache_store"};
            store_terrain(target,
                          collected_beard.has_value() ? &*collected_beard
                                                      : nullptr,
                          (*generated)->clone());
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
    // Terrain never contains a shape-sensitive block, so the shape refresh
    // only has work when a piece reached the target chunk. The flag is set on
    // the same intersection test that gates the piece placement.
    bool center_shapes{false};
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
            center_shapes = true;
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
    if (center_shapes) {
        const stage_timer timer{profile_, "structure.shapes"};
        structure::refresh_chunk_shapes(*center, region, states);
    }

    if (has_features_) {
        // The decoration order: a reference world saves every chunk of the
        // generated area, so its chunks contain the feature writes of their
        // neighbours. Decorating the target chunk first and then its
        // neighbours reproduces those writes for a target chunk that the
        // reference generated before its neighbours. The generation-order
        // mode follows the ticket order of a force-load instead: every chunk
        // that precedes the target in ascending x and z decorates first, then
        // the target, then the rest.
        std::vector<chunk_pos> ordered_positions;
        ordered_positions.reserve(decorated_positions.size());
        const auto add = [&](const chunk_pos &position) {
            if (position == pos) {
                ordered_positions.push_back(pos);
                return;
            }
            if (region.find(position) != nullptr) {
                // The reference generation region of this chunk allows writes
                // in its eight neighbours, not around the compared chunk.
                ordered_positions.push_back(position);
            }
        };
        if (!config_.decorate_neighbours) {
            add(pos);
        } else if (config_.neighbours_in_generation_order ||
                   config_.neighbours_ticket_sweep) {
            for (const auto &position : decorated_positions) {
                add(position);
            }
        } else if (config_.neighbours_before_target) {
            for (const auto &position : decorated_positions) {
                if (position != pos) {
                    add(position);
                }
            }
            add(pos);
        } else {
            add(pos);
            for (const auto &position : decorated_positions) {
                if (position != pos) {
                    add(position);
                }
            }
        }
        // Every decorated chunk runs its own candidate-biome plan, so a
        // neighbour with a different set of palettes considers a different
        // feature set than the target does.
        std::vector<std::vector<std::string>> plans;
        plans.reserve(ordered_positions.size());
        for (const auto &position : ordered_positions) {
            plans.push_back(candidate_biomes_at(region, position));
        }
        std::vector<decoration_target> order;
        order.reserve(ordered_positions.size());
        for (std::size_t index = 0; index < ordered_positions.size(); ++index) {
            const auto &position = ordered_positions[index];
            order.push_back(decoration_target{
                .position = position,
                .chunk = position == pos ? center : region.find(position),
                .candidate_biomes = &plans[index]});
        }
        if (const auto decorated = decorate_chunks(region, order); !decorated) {
            return std::unexpected{decorated.error()};
        }
    }

    center->set_status(target_status(config_.target));
    return std::move(chunks[0]);
}

auto chunk_generator::decorate_chunks(
    chunk_region &region, std::span<const decoration_target> targets)
    -> result<void> {
    if (targets.empty()) {
        return {};
    }
    auto &states = dimension_->states();
    const auto &profile = dimension_->profile();
    // Feature placement reads biomes through the chunks of the region and
    // falls back to the noise source only outside it. Starting that fallback
    // from a fresh candidate keeps the decoration independent of how many
    // fills the pass needed.
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
        return dimension_->biome_temperature(biome_of(block), block.x, block.y,
                                             block.z);
    };
    const auto world_state = [&region](const block_pos &block) {
        return region.state_at(block.x, block.y, block.z);
    };

    feature::feature_context feature_context{
        .placement =
            feature::placement_context{
                .region = &region,
                .chunk = &region.center(),
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
    feature_context.placement.height = [&region](heightmap_kind kind, jint x,
                                                 jint z) {
        return region.height_at(kind, x, z);
    };
    feature_context.place_configured =
        [&feature_context, this](std::string_view id, worldgen_random &random,
                                 const block_pos &position) {
            const auto *configured =
                features_ == nullptr ? nullptr : features_->configured(id);
            return configured != nullptr &&
                   configured->place(feature_context, random, position);
        };
    feature_context.placement.biome = biome_of;
    feature_context.placement.resolve_state =
        [&states](std::string_view key) -> block_state_id {
        if (const auto found = states.find(key); found.has_value()) {
            return *found;
        }
        const auto interned = states.intern(key, states.infer_traits(key));
        return interned.has_value() ? *interned : no_block_state;
    };
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
                                             &states](const block_pos &block) {
        const auto value = temperature(block);
        if (!value.has_value() || *value >= 0.15F) {
            return false;
        }
        const auto state = world_state(block);
        const auto name = states.name(state);
        if (!states.traits(state).air && !name.starts_with("minecraft:snow")) {
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
            const auto found =
                shared_->structure_data.processor_lists.find(std::string{id});
            if (found == shared_->structure_data.processor_lists.end()) {
                return nullptr;
            }
            return &templates_->processor_list(id, found->second);
        };
    }
    prepare.stop();
    for (const auto &target : targets) {
        region.set_write_focus(target.position, 1);
        feature_context.placement.chunk = target.chunk;
        feature_context.placement.generation.min_y =
            dimension_->generation_min_y();
        feature_context.placement.generation.height =
            dimension_->generation_height();
        const stage_timer timer{profile_, "feature.decorate"};
        feature::decorate(feature_context, shared_->feature_plan,
                          features_->placed(), *target.candidate_biomes,
                          shared_->feature_data.biomes, config_.seed);
    }
    return {};
}

struct chunk_generator::structure_placement {
    structure::structure_place_context context{};
    std::unordered_map<std::string, jint> adjusted_heights{};
    std::optional<structure::structure_feature_placer> features{};
};

auto chunk_generator::make_structure_placement(
    chunk_region &region,
    std::span<const structure::structure_start *const> starts)
    -> result<std::unique_ptr<structure_placement>> {
    auto &states = dimension_->states();
    auto placement = std::make_unique<structure_placement>();
    placement->context = structure::structure_place_context{
        .region = &region,
        .chunk = nullptr,
        .states = &states,
        .state = [&states](std::string_view key) -> block_state_id {
            if (const auto found = states.find(key); found.has_value()) {
                return *found;
            }
            const auto interned = states.intern(key, states.infer_traits(key));
            return interned.has_value() ? *interned : no_block_state;
        },
        .templates = templates_.get(),
        .registry = &states,
        .seed = config_.seed,
        .sea_level = dimension_->profile().sea_level,
        .level_random = nullptr,
        .adjusted_heights = &placement->adjusted_heights,
        .data = &shared_->structure_data,
        .place_feature = {},
    };
    std::vector<std::string> feature_ids;
    for (const auto *start : starts) {
        for (const auto &piece : start->pieces) {
            structure::collect_feature_ids(structure::to_piece_record(piece),
                                           feature_ids);
        }
    }
    if (!feature_ids.empty()) {
        placement->features.emplace(placement->context, shared_->feature_data);
        if (const auto loaded = placement->features->load(feature_ids);
            !loaded) {
            return std::unexpected{loaded.error()};
        }
        placement->context.place_feature =
            [pointer = placement.get()](std::string_view id,
                                        worldgen_random &random,
                                        const block_pos &position) {
                return pointer->features->place(id, random, position);
            };
    }
    return placement;
}

auto chunk_generator::generate_tile(const chunk_pos &origin)
    -> result<std::vector<std::unique_ptr<proto_chunk>>> {
    return generate_tile_impl(origin);
}

auto chunk_generator::generate_tile_impl(const chunk_pos &origin)
    -> result<std::vector<std::unique_ptr<proto_chunk>>> {
    const jint size = config_.tile_size;
    if (size <= 0) {
        return std::unexpected{fail(error_code::invalid_argument,
                                    "a generation tile needs a positive edge")};
    }
    auto &states = dimension_->states();
    // The served tile, the one-chunk ring around it every feature write may
    // land in, and the one further ring every decorated chunk reads. The ring
    // sizes follow the reference generation region: a features step reads its
    // three-by-three at the terrain status, so the terrain ring is the
    // decorated ring plus one.
    const chunk_rectangle served{.min_x = origin.x,
                                 .min_z = origin.z,
                                 .max_x = origin.x + size - 1,
                                 .max_z = origin.z + size - 1};
    const chunk_rectangle decorated =
        has_features_ ? expand(served, 1) : served;
    const chunk_rectangle filled =
        has_features_ ? expand(decorated, 1) : served;

    // The starts stage feeds the beardifier, so it runs before the fill. Its
    // height probes read unbearded fill columns, not surface or carvers. The
    // scanned rectangle reaches eight chunks past the filled one, because a
    // start up to eight chunks away can still beard or reference a chunk.
    std::vector<structure::structure_start> starts;
    std::optional<error> height_error;
    if (has_structures_) {
        const stage_timer timer{profile_, "structure.starts"};
        dimension_->reset_biome_search();
        auto start_context = make_start_context(height_error);
        const auto scanned = expand(filled, 8);
        for (jint x = scanned.min_x; x <= scanned.max_x; ++x) {
            for (jint z = scanned.min_z; z <= scanned.max_z; ++z) {
                auto found = structure::generate_structure_starts(
                    chunk_pos{.x = x, .z = z}, start_context);
                starts.insert(starts.end(),
                              std::make_move_iterator(found.begin()),
                              std::make_move_iterator(found.end()));
            }
        }
        if (height_error) {
            return std::unexpected{*height_error};
        }
    }

    const jint filled_width = filled.max_x - filled.min_x + 1;
    const jint filled_depth = filled.max_z - filled.min_z + 1;
    const auto index_of = [&filled, filled_depth](const chunk_pos &pos) {
        return static_cast<std::size_t>(pos.x - filled.min_x) *
                   static_cast<std::size_t>(filled_depth) +
               static_cast<std::size_t>(pos.z - filled.min_z);
    };
    std::vector<std::unique_ptr<proto_chunk>> chunks(
        static_cast<std::size_t>(filled_width) *
        static_cast<std::size_t>(filled_depth));
    for (jint x = filled.min_x; x <= filled.max_x; ++x) {
        for (jint z = filled.min_z; z <= filled.max_z; ++z) {
            const chunk_pos position{.x = x, .z = z};
            std::optional<structure::structure_beard> collected_beard;
            if (has_structures_) {
                const stage_timer timer{profile_, "structure.beard"};
                auto collected = structure::collect_beard(
                    starts, shared_->structure_data, position);
                if (!collected.pieces.empty() || !collected.junctions.empty() ||
                    collected.affected_box.has_value()) {
                    collected_beard = std::move(collected);
                }
            }
            auto generated = fill_terrain(position, collected_beard.has_value()
                                                        ? &*collected_beard
                                                        : nullptr);
            if (!generated) {
                return std::unexpected{generated.error()};
            }
            chunks[index_of(position)] = std::move(*generated);
        }
    }

    chunk_region region{*chunks[index_of(origin)], 1};
    for (auto &chunk : chunks) {
        region.attach(*chunk);
    }
    std::vector<bool> shaped(chunks.size(), false);

    // Structures are placed once per decorated chunk, in force-load order.
    // A piece is clipped to the chunk whose step places it, which is what the
    // reference does: the chunk's writable area is its own column, so a piece
    // never writes outside the chunk running the step.
    if (has_structures_) {
        std::vector<const structure::structure_start *> start_pointers;
        start_pointers.reserve(starts.size());
        for (const auto &start : starts) {
            start_pointers.push_back(&start);
        }
        std::unique_ptr<structure_placement> placement;
        {
            const stage_timer timer{profile_, "structure.features"};
            auto created = make_structure_placement(region, start_pointers);
            if (!created) {
                return std::unexpected{created.error()};
            }
            placement = std::move(*created);
        }
        stage_timer pieces{profile_, "structure.pieces"};
        for (jint x = decorated.min_x; x <= decorated.max_x; ++x) {
            for (jint z = decorated.min_z; z <= decorated.max_z; ++z) {
                const chunk_pos position{.x = x, .z = z};
                auto *chunk = region.find(position);
                if (chunk == nullptr) {
                    continue;
                }
                // The reference places a start's pieces in the chunk whose
                // features step runs them, clipped to that chunk's column.
                region.set_write_focus(position, 0);
                worldgen_random decoration{
                    std::make_unique<xoroshiro_random_source>(0)};
                const jlong decoration_seed = decoration.set_decoration_seed(
                    config_.seed, position.x * 16, position.z * 16);
                std::vector<structure_reference> references;
                bool placed_piece = false;
                for (const auto *start : start_pointers) {
                    const bool owned = start->chunk.x == position.x &&
                                       start->chunk.z == position.z;
                    if (!owned && !start->bounds.intersects_chunk(position)) {
                        continue;
                    }
                    if (contains(served, position)) {
                        if (owned) {
                            chunk->add_structure_start(
                                make_start_record(*start));
                        }
                        const auto duplicate = std::find_if(
                            references.begin(), references.end(),
                            [start](const structure_reference &reference) {
                                return reference.structure ==
                                           start->structure &&
                                       reference.chunk_x == start->chunk.x &&
                                       reference.chunk_z == start->chunk.z;
                            });
                        if (duplicate != references.end()) {
                            ++duplicate->count;
                        } else {
                            references.push_back(structure_reference{
                                .structure = start->structure,
                                .chunk_x = start->chunk.x,
                                .chunk_z = start->chunk.z,
                                .count = 1});
                        }
                    }
                    const auto structure_index =
                        structure::structure_step_index(shared_->structure_data,
                                                        start->structure);
                    if (!structure_index.has_value()) {
                        continue;
                    }
                    auto piece_context = placement->context;
                    piece_context.chunk = chunk;
                    if (!start->pieces.empty()) {
                        piece_context.reference =
                            structure::start_reference_position(
                                structure::to_piece_record(
                                    start->pieces.front()));
                    }
                    worldgen_random random{
                        std::make_unique<xoroshiro_random_source>(0)};
                    random.set_feature_seed(
                        decoration_seed, *structure_index,
                        structure::generation_step_ordinal(start->step));
                    for (const auto &piece : start->pieces) {
                        if (!piece.bounds.intersects_chunk(position)) {
                            continue;
                        }
                        placed_piece = true;
                        structure::place_structure_piece(
                            piece_context, structure::to_piece_record(piece),
                            random);
                    }
                }
                for (const auto &reference : references) {
                    chunk->add_structure_reference(
                        reference.structure, reference.chunk_x,
                        reference.chunk_z, reference.count);
                }
                if (contains(served, position)) {
                    shaped[index_of(position)] = placed_piece;
                }
            }
        }
        pieces.stop();
        // Shape-sensitive blocks are repaired once every piece landed, so a
        // border wall or fence sees the pieces on both sides of the seam.
        for (jint x = served.min_x; x <= served.max_x; ++x) {
            for (jint z = served.min_z; z <= served.max_z; ++z) {
                const chunk_pos position{.x = x, .z = z};
                if (!shaped[index_of(position)]) {
                    continue;
                }
                const stage_timer timer{profile_, "structure.shapes"};
                region.set_write_focus(position, 0);
                structure::refresh_chunk_shapes(*region.find(position), region,
                                                states);
            }
        }
    }

    if (has_features_) {
        // Each decorated chunk considers the features of the biomes its own
        // three-by-three palette holds, exactly like the reference possible
        // biome set. The plans are taken from the fill's biome grid, so a
        // chunk's candidate set never depends on the query order.
        std::vector<std::vector<std::uint16_t>> palettes(chunks.size());
        if (config_.fixed_biome.empty()) {
            for (std::size_t index = 0; index < chunks.size(); ++index) {
                palettes[index] = chunk_biomes(*chunks[index]);
            }
        }
        std::vector<std::vector<std::string>> candidates(
            static_cast<std::size_t>(decorated.max_x - decorated.min_x + 1) *
            static_cast<std::size_t>(decorated.max_z - decorated.min_z + 1));
        const auto candidates_at =
            [&](const chunk_pos &position) -> std::vector<std::string> & {
            return candidates
                [static_cast<std::size_t>(position.x - decorated.min_x) *
                     static_cast<std::size_t>(decorated.max_z -
                                              decorated.min_z + 1) +
                 static_cast<std::size_t>(position.z - decorated.min_z)];
        };
        std::vector<decoration_target> targets;
        targets.reserve(candidates.size());
        for (jint x = decorated.min_x; x <= decorated.max_x; ++x) {
            for (jint z = decorated.min_z; z <= decorated.max_z; ++z) {
                const chunk_pos position{.x = x, .z = z};
                auto &list = candidates_at(position);
                if (!config_.fixed_biome.empty()) {
                    list.push_back(config_.fixed_biome);
                } else {
                    std::set<std::string> names;
                    for (jint dx = -1; dx <= 1; ++dx) {
                        for (jint dz = -1; dz <= 1; ++dz) {
                            const chunk_pos source{.x = x + dx, .z = z + dz};
                            if (!contains(filled, source)) {
                                continue;
                            }
                            for (const auto id : palettes[index_of(source)]) {
                                names.emplace(dimension_->biome_name(id));
                            }
                        }
                    }
                    list.assign(names.begin(), names.end());
                }
                targets.push_back(
                    decoration_target{.position = position,
                                      .chunk = region.find(position),
                                      .candidate_biomes = &list});
            }
        }
        if (const auto decorated_chunks = decorate_chunks(region, targets);
            !decorated_chunks) {
            return std::unexpected{decorated_chunks.error()};
        }
    }

    // Serve only the inner tile. The status is raised after every neighbour
    // write landed, so the heightmaps the served chunks report are final.
    std::vector<std::unique_ptr<proto_chunk>> result;
    result.reserve(static_cast<std::size_t>(size) *
                   static_cast<std::size_t>(size));
    for (jint x = served.min_x; x <= served.max_x; ++x) {
        for (jint z = served.min_z; z <= served.max_z; ++z) {
            auto &chunk = chunks[index_of(chunk_pos{.x = x, .z = z})];
            chunk->set_status(target_status(config_.target));
            result.push_back(std::move(chunk));
        }
    }
    return result;
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

auto chunk_generator::generate_area_tiled(chunk_pos origin, jint width,
                                          jint depth, std::size_t workers)
    -> result<std::vector<std::unique_ptr<proto_chunk>>> {
    if (width <= 0 || depth <= 0) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "generation area needs a positive width and depth")};
    }
    const jint size = config_.tile_size;
    if (size <= 0) {
        return std::unexpected{fail(error_code::invalid_argument,
                                    "a generation tile needs a positive edge")};
    }
    std::vector<chunk_pos> tile_origins;
    const jint first_x = floor_div(origin.x, size) * size;
    const jint first_z = floor_div(origin.z, size) * size;
    const jint last_x = floor_div(origin.x + width - 1, size) * size;
    const jint last_z = floor_div(origin.z + depth - 1, size) * size;
    for (jint x = first_x; x <= last_x; x += size) {
        for (jint z = first_z; z <= last_z; z += size) {
            tile_origins.push_back(chunk_pos{.x = x, .z = z});
        }
    }
    // Every tile in the rectangle is generated whole so the served chunks
    // keep their neighbours' writes; the chunks of a tile that fall outside
    // the requested rectangle are discarded.
    std::vector<std::unique_ptr<proto_chunk>> chunks(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(depth));
    const auto take = [&](std::vector<std::unique_ptr<proto_chunk>> &tile) {
        for (auto &chunk : tile) {
            if (chunk == nullptr) {
                continue;
            }
            const jint x = chunk->chunk_x() - origin.x;
            const jint z = chunk->chunk_z() - origin.z;
            if (x < 0 || x >= width || z < 0 || z >= depth) {
                continue;
            }
            chunks[static_cast<std::size_t>(x) *
                       static_cast<std::size_t>(depth) +
                   static_cast<std::size_t>(z)] = std::move(chunk);
        }
    };
    std::size_t worker_count = workers;
    if (worker_count == 0) {
        worker_count = std::thread::hardware_concurrency();
        if (worker_count == 0) {
            worker_count = 1;
        }
    }
    worker_count = std::min(worker_count, tile_origins.size());
    if (worker_count <= 1) {
        for (const auto &tile_origin : tile_origins) {
            auto generated = generate_tile_impl(tile_origin);
            if (!generated) {
                return std::unexpected{generated.error()};
            }
            take(*generated);
        }
        return chunks;
    }
    auto instances = worker_instances(worker_count);
    if (!instances) {
        return std::unexpected{instances.error()};
    }
    std::vector<std::optional<error>> errors(tile_origins.size());
    std::vector<std::jthread> threads;
    threads.reserve(worker_count);
    const std::size_t base = tile_origins.size() / worker_count;
    const std::size_t extra = tile_origins.size() % worker_count;
    std::size_t begin = 0;
    for (std::size_t worker = 0; worker < worker_count; ++worker) {
        const std::size_t end = begin + base + (worker < extra ? 1U : 0U);
        auto *generator = (*instances)[worker];
        threads.emplace_back([&, generator, begin, end] {
            for (std::size_t index = begin; index < end; ++index) {
                try {
                    auto generated =
                        generator->generate_tile_impl(tile_origins[index]);
                    if (!generated) {
                        errors[index] = generated.error();
                    } else {
                        take(*generated);
                    }
                } catch (const std::exception &exception) {
                    errors[index] = fail(error_code::unknown, exception.what());
                } catch (...) {
                    errors[index] =
                        fail(error_code::unknown, "tile generation failed");
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
