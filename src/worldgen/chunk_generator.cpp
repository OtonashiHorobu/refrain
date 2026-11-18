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
#include <map>
#include <memory>
#include <optional>
#include <ranges>
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

/// Datapack `data` root the generator reads: the bound generation's merged
/// tree when it has one, the configured boot root otherwise.
auto effective_data_root(const chunk_generator_config &config)
    -> const std::string & {
    return config.generation != nullptr ? config.generation->data_root()
                                        : config.data_root;
}

/// Structure template root of the bound generation, or the configured boot
/// root.
auto effective_templates_root(const chunk_generator_config &config)
    -> const std::string & {
    return config.generation != nullptr ? config.generation->templates_root()
                                        : config.templates_root;
}

auto dimension_config_for(const chunk_generator_config &config)
    -> dimension_generator_config {
    auto dimension = dimension_generator_config{
        .data_root = effective_data_root(config),
        .kind = config.kind,
        .seed = config.seed,
        .fixed_biome = config.fixed_biome,
    };
    if (config.generation != nullptr) {
        dimension.block_tags = &config.generation->block_tags();
        dimension.registries = config.generation->registries_handle();
    }
    return dimension;
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
    const auto found = std::ranges::find(positions, center);
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
                if (!std::ranges::contains(ids, id)) {
                    ids.push_back(id);
                }
            }
        }
    }
    return ids;
}

/// Fills the properties a key omits from the content's default-state table and
/// sorts the property names. A structure or feature helper may name a state by
/// the properties it distinguishes (a rail shape, for example) and leave the
/// rest implicit; completing the spelling here gives every writer the same
/// canonical key before the shared table is frozen, so no worker needs to
/// append a first-use id during generation.
auto canonical_state_key(const block_state_registry &states,
                         std::string_view key) -> std::string {
    const auto open = key.find('[');
    const std::string_view block =
        open == std::string_view::npos ? key : key.substr(0, open);
    const auto defaults = states.default_properties_for_block(block);
    if (defaults.empty()) {
        return std::string{key};
    }
    std::map<std::string, std::string, std::less<>> properties;
    if (open != std::string_view::npos) {
        const auto close = key.rfind(']');
        if (close == std::string_view::npos || close <= open) {
            return std::string{key};
        }
        std::string_view body = key.substr(open + 1, close - open - 1);
        while (!body.empty()) {
            const auto comma = body.find(',');
            const auto entry = body.substr(0, comma);
            const auto equals = entry.find('=');
            if (equals != std::string_view::npos) {
                properties.emplace(std::string{entry.substr(0, equals)},
                                   std::string{entry.substr(equals + 1)});
            }
            if (comma == std::string_view::npos) {
                break;
            }
            body = body.substr(comma + 1);
        }
    }
    for (const auto &property : defaults) {
        if (!properties.contains(property.name)) {
            properties.emplace(property.name, property.value);
        }
    }
    if (properties.empty()) {
        return std::string{block};
    }
    std::string canonical{block};
    canonical.push_back('[');
    bool first = true;
    for (const auto &[name, value] : properties) {
        if (!first) {
            canonical.push_back(',');
        }
        first = false;
        canonical.append(name);
        canonical.push_back('=');
        canonical.append(value);
    }
    canonical.push_back(']');
    return canonical;
}

/// Resolves a state key through the generation table. A bare block name
/// resolves through the table's default-state entry, which is the spelling the
/// per-generator registry produces; a key that spells only some properties is
/// completed to the canonical spelling first, because a shared frozen table
/// holds the content's canonical keys and must not append a first-use id.
auto resolve_state_key(block_state_registry &states, std::string_view key)
    -> block_state_id {
    if (const auto found = states.find(key); found.has_value()) {
        return *found;
    }
    if (key.find('[') == std::string_view::npos) {
        const auto interned = states.intern(key, states.infer_traits(key));
        return interned.has_value() ? *interned : no_block_state;
    }
    std::string canonical = canonical_state_key(states, key);
    if (canonical != key) {
        if (const auto found = states.find(canonical); found.has_value()) {
            return *found;
        }
    }
    const auto interned =
        states.intern(canonical, states.infer_traits(canonical));
    return interned.has_value() ? *interned : no_block_state;
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
    /// The structure sets whose structures can appear with one of this
    /// dimension's possible biomes, in the data set's own order. Every worker
    /// shares the list; a set whose structures cannot appear at all never
    /// reaches a scan of the starts window.
    std::vector<const structure::structure_set *> structure_sets{};
    /// The one block-state table of a scheduler, frozen before its workers
    /// start. Null on the per-generator paths, which build a small registry
    /// each.
    std::shared_ptr<block_state_registry> shared_states{};
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
      proto_cache_{static_cast<std::size_t>(
          std::max<jint>(config_.proto_cache_capacity, 1))},
      pipeline_cache_{&proto_cache_},
      has_structures_{config_.target != generation_target::terrain},
      has_features_{!config_.reference_world && has_structures_} {}

namespace {

auto validate_generator_config(const chunk_generator_config &config) -> status {
    if (effective_data_root(config).empty()) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "chunk generator has no content data root")};
    }
    if (config.tile_size < 1 || config.tile_size > 32) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "generation tile edge is outside its supported range")};
    }
    if (config.proto_cache_capacity < 25) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "the per-chunk cache cannot hold a terrain neighbourhood")};
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
    return status_ok();
}

} // namespace

auto chunk_generator::load_shared_data(const chunk_generator_config &config)
    -> result<std::shared_ptr<shared_generator_data>> {
    const bool needs_structures = config.target != generation_target::terrain;
    const bool needs_features = needs_structures && !config.reference_world;

    const auto dimension_config = dimension_config_for(config);
    auto dimension_shared = dimension_generator::load_shared(dimension_config);
    if (!dimension_shared) {
        return std::unexpected{dimension_shared.error()};
    }
    auto shared = std::make_shared<shared_generator_data>();
    shared->dimension = std::move(*dimension_shared);

    if (needs_structures || needs_features) {
        auto loaded = feature::load_feature_data(effective_data_root(config));
        if (!loaded) {
            return std::unexpected{loaded.error()};
        }
        shared->feature_data = std::move(*loaded);
    }
    std::vector<std::string> possible_biomes;
    if (needs_features || needs_structures) {
        auto biomes = dimension_generator::possible_biomes(dimension_config,
                                                           shared->dimension);
        if (!biomes) {
            return std::unexpected{biomes.error()};
        }
        possible_biomes = std::move(*biomes);
    }
    if (needs_features) {
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
        auto loaded =
            structure::load_structure_data(effective_data_root(config));
        if (!loaded) {
            return std::unexpected{loaded.error()};
        }
        shared->structure_data = std::move(*loaded);
        shared->structure_sets = structure::applicable_structure_sets(
            shared->structure_data, possible_biomes);
    }
    return shared;
}

auto chunk_generator::create(chunk_generator_config config)
    -> result<std::unique_ptr<chunk_generator>> {
    if (const auto valid = validate_generator_config(config); !valid) {
        return std::unexpected{valid.error()};
    }
    auto shared = load_shared_data(config);
    if (!shared) {
        return std::unexpected{shared.error()};
    }
    return create_with_shared(
        std::move(config),
        std::shared_ptr<const shared_generator_data>{std::move(*shared)});
}

auto chunk_generator::create_scheduler_shared(chunk_generator_config config)
    -> result<scheduler_shared_state> {
    if (config.generation != nullptr) {
        return std::unexpected{make_error(
            error_code::unsupported,
            "the experimental chunk scheduler cannot rebuild its frozen "
            "shared content for a published content generation; it keeps its "
            "boot content and a reload needs a restart")};
    }
    if (const auto valid = validate_generator_config(config); !valid) {
        return std::unexpected{valid.error()};
    }
    auto shared = load_shared_data(config);
    if (!shared) {
        return std::unexpected{shared.error()};
    }
    const auto dimension_config = dimension_config_for(config);
    auto states = dimension_generator::build_shared_states(
        dimension_config, (*shared)->dimension);
    if (!states) {
        return std::unexpected{states.error()};
    }
    (*shared)->shared_states = std::move(*states);
    // The first worker generator loads the feature and template data, which
    // may still ask the table for a key. Those interns happen before the
    // freeze, in a fixed order; every later worker only reads it.
    auto first = create_with_shared(
        config, std::shared_ptr<const shared_generator_data>{*shared});
    if (!first) {
        return std::unexpected{first.error()};
    }
    (*shared)->shared_states->freeze();
    return scheduler_shared_state{
        .shared = std::shared_ptr<const shared_generator_data>{*shared},
        .first = std::move(*first),
    };
}

auto chunk_generator::create_with_shared(
    chunk_generator_config config,
    std::shared_ptr<const shared_generator_data> shared)
    -> result<std::unique_ptr<chunk_generator>> {
    auto dimension =
        shared->shared_states != nullptr
            ? dimension_generator::create(dimension_config_for(config),
                                          shared->dimension,
                                          shared->shared_states)
            : dimension_generator::create(dimension_config_for(config),
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
            effective_templates_root(config), (*dimension)->states());
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
    context.sets = std::span<const structure::structure_set *const>{
        shared_->structure_sets};
    if (!config_.structure_sets.empty()) {
        const auto &allowed = config_.structure_sets;
        context.set_filter = [&allowed](std::string_view id) {
            return std::ranges::contains(allowed, id);
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
        return resolve_state_key(states, key);
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
        .biome = [this, &region](const block_pos &block) -> std::string_view {
            return structure_biome_at(region, block);
        },
        .temperature =
            [this, &region](const block_pos &block) -> std::optional<float> {
            return structure_temperature_at(region, block);
        },
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
        return resolve_state_key(states, key);
    };
    feature_context.placement.should_freeze =
        feature::make_should_freeze(region, states, temperature);
    feature_context.placement.should_snow =
        feature::make_should_snow(region, states, temperature);
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

auto chunk_generator::structure_biome_at(const chunk_region &region,
                                         const block_pos &block) const
    -> std::string_view {
    if (!config_.fixed_biome.empty()) {
        return config_.fixed_biome;
    }
    const jint chunk_x = block.x >> 4;
    const jint chunk_z = block.z >> 4;
    const auto *chunk = region.find(chunk_x, chunk_z);
    if (chunk == nullptr) {
        return {};
    }
    return dimension_->biome_name(chunk->biome_at(
        block.x - chunk_x * 16, block.y, block.z - chunk_z * 16));
}

auto chunk_generator::structure_temperature_at(const chunk_region &region,
                                               const block_pos &block) const
    -> std::optional<float> {
    const auto biome = structure_biome_at(region, block);
    if (biome.empty()) {
        return std::nullopt;
    }
    return dimension_->biome_temperature(biome, block.x, block.y, block.z);
}

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
            return resolve_state_key(states, key);
        },
        .templates = templates_.get(),
        .registry = &states,
        .seed = config_.seed,
        .sea_level = dimension_->profile().sea_level,
        .level_random = nullptr,
        .adjusted_heights = &placement->adjusted_heights,
        .data = &shared_->structure_data,
        .place_feature = {},
        .biome = [this, &region](const block_pos &block) -> std::string_view {
            return structure_biome_at(region, block);
        },
        .temperature =
            [this, &region](const block_pos &block) -> std::optional<float> {
            return structure_temperature_at(region, block);
        },
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
                starts.append_range(found | std::views::as_rvalue);
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
                        const auto duplicate = std::ranges::find_if(
                            references,
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

// ---------------------------------------------------------------------------
// Per-chunk status pipeline
// ---------------------------------------------------------------------------

namespace {

/// The inclusive square of chunk positions around `center`.
auto square_positions(const chunk_pos &center, jint radius)
    -> std::vector<chunk_pos> {
    auto offsets = square_offsets(radius);
    std::vector<chunk_pos> positions;
    positions.reserve(offsets.size());
    for (const auto &offset : offsets) {
        positions.push_back(
            chunk_pos{.x = center.x + offset.x, .z = center.z + offset.z});
    }
    return positions;
}

/// The inclusive square of chunk coordinates around `center`.
auto square_box(const chunk_pos &center, jint radius) -> chunk_rectangle {
    return chunk_rectangle{.min_x = center.x - radius,
                           .min_z = center.z - radius,
                           .max_x = center.x + radius,
                           .max_z = center.z + radius};
}

} // namespace

void chunk_generator::clear_proto_cache() noexcept {
    pipeline_cache_->clear();
    start_cache_.clear();
    start_cache_order_.clear();
    pipeline_window_cache_.reset();
}

auto chunk_generator::cached_start_at(const chunk_pos &pos)
    -> result<const std::vector<structure::structure_start> *> {
    if (!has_structures_) {
        static const std::vector<structure::structure_start> empty{};
        return &empty;
    }
    if (const auto found = start_cache_.find(pos);
        found != start_cache_.end()) {
        return &found->second;
    }
    std::optional<error> height_error;
    auto context = make_start_context(height_error);
    // The starts of a chunk must not depend on which chunk this worker scanned
    // before it, so the biome search starts from the same empty hint every
    // time. That is what makes a cached start reusable across request orders.
    dimension_->reset_biome_search();
    auto generated = structure::generate_structure_starts(pos, context);
    if (height_error) {
        return std::unexpected{*height_error};
    }
    while (start_cache_order_.size() >= start_cache_capacity) {
        start_cache_.erase(start_cache_order_.front());
        start_cache_order_.pop_front();
    }
    start_cache_order_.push_back(pos);
    const auto [found, inserted] =
        start_cache_.try_emplace(pos, std::move(generated));
    return &found->second;
}

auto chunk_generator::start_window(const chunk_rectangle &box)
    -> result<cached_start_window> {
    cached_start_window window;
    if (!has_structures_ || box.empty()) {
        return window;
    }
    for (jint x = box.min_x; x <= box.max_x; ++x) {
        for (jint z = box.min_z; z <= box.max_z; ++z) {
            const auto found = cached_start_at(chunk_pos{.x = x, .z = z});
            if (!found) {
                return std::unexpected{found.error()};
            }
            window.starts.append_range(**found);
        }
    }
    return window;
}

auto chunk_generator::pipeline_window(const chunk_pos &pos)
    -> result<const cached_start_window *> {
    if (pipeline_window_cache_.has_value() &&
        pipeline_window_cache_->first == pos) {
        return &pipeline_window_cache_->second;
    }
    const chunk_rectangle decorated =
        square_box(pos, pipeline_decoration_radius);
    const chunk_rectangle start_box =
        expand(decorated, pipeline_structure_radius);
    auto window = start_window(start_box);
    if (!window) {
        return std::unexpected{window.error()};
    }
    pipeline_window_cache_.emplace(pos, std::move(*window));
    return &pipeline_window_cache_->second;
}

auto chunk_generator::attach_cached(chunk_region &region,
                                    const chunk_rectangle &box) -> void {
    const auto &readable = std::as_const(*pipeline_cache_);
    for (jint x = box.min_x; x <= box.max_x; ++x) {
        for (jint z = box.min_z; z <= box.max_z; ++z) {
            const auto *entry = readable.find(chunk_pos{.x = x, .z = z});
            if (entry == nullptr || entry->chunk == nullptr) {
                continue;
            }
            if (entry->sealed) {
                region.attach_read_only(*entry->chunk);
            } else {
                region.attach(*entry->chunk);
            }
        }
    }
}

auto chunk_generator::ensure_terrain(
    const chunk_rectangle &box,
    const std::vector<structure::structure_start> &starts) -> result<void> {
    for (jint x = box.min_x; x <= box.max_x; ++x) {
        for (jint z = box.min_z; z <= box.max_z; ++z) {
            const chunk_pos position{.x = x, .z = z};
            if (pipeline_cache_->find(position) != nullptr) {
                continue;
            }
            std::optional<structure::structure_beard> beard;
            if (has_structures_) {
                const stage_timer timer{profile_, "structure.beard"};
                auto collected = structure::collect_beard(
                    starts, shared_->structure_data, position);
                if (!collected.pieces.empty() || !collected.junctions.empty() ||
                    collected.affected_box.has_value()) {
                    beard = std::move(collected);
                }
            }
            auto generated =
                fill_terrain(position, beard.has_value() ? &*beard : nullptr);
            if (!generated) {
                return std::unexpected{generated.error()};
            }
            (void)pipeline_cache_->insert(position, std::move(*generated));
        }
    }
    return {};
}

auto chunk_generator::ensure_structure_pieces(
    std::span<const chunk_pos> positions, const chunk_rectangle &box,
    const std::vector<structure::structure_start> &starts) -> result<void> {
    if (positions.empty()) {
        return {};
    }
    bool pending = false;
    for (const auto &position : positions) {
        const auto *entry = pipeline_cache_->find(position);
        if (entry == nullptr) {
            return std::unexpected{fail(
                error_code::unknown,
                "the per-chunk pipeline lost a chunk of its terrain ring")};
        }
        pending = pending || !entry->structures;
    }
    if (!pending) {
        return {};
    }
    if (!has_structures_) {
        for (const auto &position : positions) {
            if (auto *entry = pipeline_cache_->find(position);
                entry != nullptr) {
                entry->structures = true;
            }
        }
        return {};
    }
    auto *center = pipeline_cache_->find(positions.front());
    if (center == nullptr || center->chunk == nullptr) {
        return std::unexpected{
            fail(error_code::unknown,
                 "the per-chunk pipeline lost its structure centre")};
    }
    chunk_region region{*center->chunk, 1};
    attach_cached(region, box);
    // Only the starts that reach one of the pending chunks can place a piece
    // in this pass, so only their feature pools are resolved. A window around
    // a village can hold dozens of starts whose pieces stay in other chunks.
    std::vector<const structure::structure_start *> start_pointers;
    start_pointers.reserve(starts.size());
    for (const auto &start : starts) {
        const bool reaches =
            std::ranges::any_of(positions, [&start](const chunk_pos &position) {
                return start.bounds.intersects_chunk(position);
            });
        if (reaches) {
            start_pointers.push_back(&start);
        }
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
    for (const auto &position : positions) {
        auto *entry = pipeline_cache_->find(position);
        if (entry == nullptr || entry->chunk == nullptr) {
            return std::unexpected{fail(
                error_code::unknown,
                "the per-chunk pipeline lost a chunk of its structure ring")};
        }
        if (entry->structures) {
            continue;
        }
        auto piece_context = placement->context;
        piece_context.chunk = entry->chunk.get();
        region.set_write_focus(position, 0);
        worldgen_random decoration{
            std::make_unique<xoroshiro_random_source>(0)};
        const jlong decoration_seed = decoration.set_decoration_seed(
            config_.seed, position.x * 16, position.z * 16);
        // The reference starts stage places the starts owned by the chunk
        // before the ones that reach it from elsewhere, and both groups in
        // scan order. The placement random stream of each start is derived
        // from the decorated chunk, not from this list, so only overlapping
        // writes between two starts can depend on the order.
        for (const auto &start : starts) {
            if (start.chunk != position ||
                !start.bounds.intersects_chunk(position)) {
                continue;
            }
            const auto structure_index = structure::structure_step_index(
                shared_->structure_data, start.structure);
            if (!structure_index.has_value()) {
                continue;
            }
            auto start_context = piece_context;
            if (!start.pieces.empty()) {
                start_context.reference = structure::start_reference_position(
                    structure::to_piece_record(start.pieces.front()));
            }
            worldgen_random random{
                std::make_unique<xoroshiro_random_source>(0)};
            random.set_feature_seed(
                decoration_seed, *structure_index,
                structure::generation_step_ordinal(start.step));
            for (const auto &piece : start.pieces) {
                if (!piece.bounds.intersects_chunk(position)) {
                    continue;
                }
                entry->shape_dirty = true;
                structure::place_structure_piece(
                    start_context, structure::to_piece_record(piece), random);
            }
        }
        for (const auto &start : starts) {
            if (start.chunk == position ||
                !start.bounds.intersects_chunk(position)) {
                continue;
            }
            const auto structure_index = structure::structure_step_index(
                shared_->structure_data, start.structure);
            if (!structure_index.has_value()) {
                continue;
            }
            auto start_context = piece_context;
            if (!start.pieces.empty()) {
                start_context.reference = structure::start_reference_position(
                    structure::to_piece_record(start.pieces.front()));
            }
            worldgen_random random{
                std::make_unique<xoroshiro_random_source>(0)};
            random.set_feature_seed(
                decoration_seed, *structure_index,
                structure::generation_step_ordinal(start.step));
            for (const auto &piece : start.pieces) {
                if (!piece.bounds.intersects_chunk(position)) {
                    continue;
                }
                entry->shape_dirty = true;
                structure::place_structure_piece(
                    start_context, structure::to_piece_record(piece), random);
            }
        }
        entry->structures = true;
    }
    return {};
}

auto chunk_generator::ensure_decorated(std::span<const chunk_pos> positions,
                                       const chunk_rectangle &box)
    -> result<void> {
    std::vector<chunk_pos> pending;
    pending.reserve(positions.size());
    for (const auto &position : positions) {
        const auto *entry = pipeline_cache_->find(position);
        if (entry == nullptr) {
            return std::unexpected{fail(
                error_code::unknown,
                "the per-chunk pipeline lost a chunk of its terrain ring")};
        }
        if (entry->stage != proto_stage::decorated) {
            pending.push_back(position);
        }
    }
    if (pending.empty()) {
        return {};
    }
    if (!has_features_) {
        for (const auto &position : pending) {
            if (auto *entry = pipeline_cache_->find(position);
                entry != nullptr) {
                entry->stage = proto_stage::decorated;
                entry->chunk->set_status(chunk_status::features);
            }
        }
        return {};
    }
    auto *center = pipeline_cache_->find(pending.front());
    if (center == nullptr || center->chunk == nullptr) {
        return std::unexpected{
            fail(error_code::unknown,
                 "the per-chunk pipeline lost its decoration centre")};
    }
    chunk_region region{*center->chunk, 1};
    attach_cached(region, box);
    // The candidate biomes of a decorated chunk are the palettes of its own
    // three-by-three, exactly the reference possible-biome set of the features
    // step, so the plan neither depends on the request order nor on the size of
    // the requested rectangle.
    std::vector<std::vector<std::string>> plans;
    plans.reserve(pending.size());
    for (const auto &position : pending) {
        plans.push_back(candidate_biomes_at(region, position));
    }
    std::vector<decoration_target> targets;
    targets.reserve(pending.size());
    for (std::size_t index = 0; index < pending.size(); ++index) {
        auto *entry = pipeline_cache_->find(pending[index]);
        if (entry == nullptr || entry->chunk == nullptr) {
            return std::unexpected{
                fail(error_code::unknown,
                     "the per-chunk pipeline lost a decoration chunk")};
        }
        targets.push_back(decoration_target{.position = pending[index],
                                            .chunk = entry->chunk.get(),
                                            .candidate_biomes = &plans[index]});
    }
    if (const auto decorated = decorate_chunks(region, targets); !decorated) {
        return std::unexpected{decorated.error()};
    }
    for (const auto &position : pending) {
        if (auto *entry = pipeline_cache_->find(position); entry != nullptr) {
            entry->stage = proto_stage::decorated;
            entry->chunk->set_status(chunk_status::features);
        }
    }
    return {};
}

auto chunk_generator::serve_cached(
    const chunk_pos &pos, const std::vector<structure::structure_start> &starts)
    -> result<std::unique_ptr<proto_chunk>> {
    auto *entry = pipeline_cache_->find(pos);
    if (entry == nullptr || entry->chunk == nullptr) {
        return std::unexpected{
            fail(error_code::unknown,
                 "the per-chunk pipeline lost the served chunk")};
    }
    auto served = entry->chunk->clone();
    // The structure records of the served chunk are the ones the reference
    // starts and references stages write: its own starts plus one reference
    // for every start of the window whose box overlaps it. They live on the
    // returned copy, so a repeat request produces the same records again.
    for (const auto &start : starts) {
        if (start.chunk == pos) {
            served->add_structure_start(make_start_record(start));
        }
    }
    auto reference_starts = starts;
    for (const auto &reference : structure::collect_references(
             reference_starts, pos, pipeline_structure_radius)) {
        served->add_structure_reference(reference.structure, reference.chunk_x,
                                        reference.chunk_z, reference.count);
    }
    served->set_status(target_status(config_.target));
    entry->sealed = true;
    return served;
}

auto chunk_generator::advance_terrain(const chunk_pos &pos) -> result<void> {
    if (config_.target == generation_target::terrain) {
        // A terrain target has no neighbour writes to settle, so the
        // single-chunk path already produces exactly the requested chunk.
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "a terrain target has no per-chunk scheduler steps")};
    }
    auto window = pipeline_window(pos);
    if (!window) {
        return std::unexpected{window.error()};
    }
    const chunk_rectangle terrain = square_box(pos, pipeline_terrain_radius);
    return ensure_terrain(terrain, (*window)->starts);
}

auto chunk_generator::advance_terrain_for(const chunk_pos &window_pos,
                                          std::span<const chunk_pos> positions)
    -> result<void> {
    if (config_.target == generation_target::terrain) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "a terrain target has no per-chunk scheduler steps")};
    }
    auto window = pipeline_window(window_pos);
    if (!window) {
        return std::unexpected{window.error()};
    }
    for (const auto &position : positions) {
        if (pipeline_cache_->find(position) != nullptr) {
            continue;
        }
        std::optional<structure::structure_beard> beard;
        if (has_structures_) {
            const stage_timer timer{profile_, "structure.beard"};
            auto collected = structure::collect_beard(
                (*window)->starts, shared_->structure_data, position);
            if (!collected.pieces.empty() || !collected.junctions.empty() ||
                collected.affected_box.has_value()) {
                beard = std::move(collected);
            }
        }
        auto generated =
            fill_terrain(position, beard.has_value() ? &*beard : nullptr);
        if (!generated) {
            return std::unexpected{generated.error()};
        }
        (void)pipeline_cache_->insert(position, std::move(*generated));
    }
    return {};
}

auto chunk_generator::advance_decorated(const chunk_pos &pos) -> result<void> {
    if (config_.target == generation_target::terrain) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "a terrain target has no per-chunk scheduler steps")};
    }
    auto window = pipeline_window(pos);
    if (!window) {
        return std::unexpected{window.error()};
    }
    const chunk_rectangle terrain = square_box(pos, pipeline_terrain_radius);
    const auto positions = square_positions(pos, pipeline_decoration_radius);
    // The decorated step may run without the terrain step having run first: it
    // fills the dependency ring itself, which is a cache lookup for the chunks
    // the terrain step already produced.
    if (auto ensured = ensure_terrain(terrain, (*window)->starts); !ensured) {
        return std::unexpected{ensured.error()};
    }
    if (auto placed =
            ensure_structure_pieces(positions, terrain, (*window)->starts);
        !placed) {
        return std::unexpected{placed.error()};
    }
    // The shape repair of the served chunk runs after every piece of its
    // three-by-three landed, so a border wall sees the pieces on both sides of
    // the seam, and before the features write over it, like the reference
    // single-chunk stage order.
    if (auto *entry = pipeline_cache_->find(pos);
        entry != nullptr && entry->shape_dirty && !entry->shapes) {
        chunk_region region{*entry->chunk, 1};
        attach_cached(region, terrain);
        region.set_write_focus(pos, 0);
        const stage_timer timer{profile_, "structure.shapes"};
        structure::refresh_chunk_shapes(*entry->chunk, region,
                                        dimension_->states());
        entry->shapes = true;
    }
    return ensure_decorated(positions, terrain);
}

auto chunk_generator::take_served(const chunk_pos &pos)
    -> result<std::unique_ptr<proto_chunk>> {
    if (config_.target == generation_target::terrain) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "a terrain target has no per-chunk scheduler steps")};
    }
    auto window = pipeline_window(pos);
    if (!window) {
        return std::unexpected{window.error()};
    }
    return serve_cached(pos, (*window)->starts);
}

auto chunk_generator::generate_chunk(const chunk_pos &pos)
    -> result<std::unique_ptr<proto_chunk>> {
    if (config_.target == generation_target::terrain) {
        // A terrain target has no neighbour writes to settle, so the single
        // chunk path already produces exactly the requested chunk.
        return generate_impl(pos);
    }
    if (auto advanced = advance_terrain(pos); !advanced) {
        return std::unexpected{advanced.error()};
    }
    if (auto decorated = advance_decorated(pos); !decorated) {
        return std::unexpected{decorated.error()};
    }
    return take_served(pos);
}

auto chunk_generator::generate_chunk_area(chunk_pos origin, jint width,
                                          jint depth, std::size_t workers)
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
    std::vector<std::unique_ptr<proto_chunk>> chunks(positions.size());
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
            auto generated = generate_chunk(positions[index]);
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
    // The neighbour writes of one chunk reach into the next, so a worker that
    // serves the later part of the rectangle replays the earlier requests on
    // its own cache instead of starting from its own first served chunk. The
    // served blocks are then exactly the serial blocks, which is what the
    // worker-count digest compares; the scheduler that removes the replay is a
    // separate step.
    const std::size_t base = positions.size() / worker_count;
    const std::size_t extra = positions.size() % worker_count;
    std::vector<std::optional<error>> errors(positions.size());
    std::vector<std::jthread> threads;
    threads.reserve(worker_count);
    std::size_t begin = 0;
    for (std::size_t worker = 0; worker < worker_count; ++worker) {
        const std::size_t end = begin + base + (worker < extra ? 1U : 0U);
        auto *generator = (*instances)[worker];
        threads.emplace_back([&, generator, begin, end] {
            for (std::size_t index = 0; index < end; ++index) {
                try {
                    auto generated =
                        generator->generate_chunk(positions[index]);
                    if (!generated) {
                        errors[index] = generated.error();
                        return;
                    }
                    if (index >= begin) {
                        chunks[index] = std::move(*generated);
                    }
                } catch (const std::exception &exception) {
                    errors[index] = fail(error_code::unknown, exception.what());
                    return;
                } catch (...) {
                    errors[index] =
                        fail(error_code::unknown, "chunk generation failed");
                    return;
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

// ---------------------------------------------------------------------------
// Per-chunk status scheduler
// ---------------------------------------------------------------------------

auto pipeline_step_name(pipeline_step step) -> std::string_view {
    switch (step) {
    case pipeline_step::terrain:
        return "terrain";
    case pipeline_step::decorated:
        return "decorated";
    case pipeline_step::served:
        return "served";
    }
    return "terrain";
}

namespace {

/// Terrain a demand publishes is the five-by-five around it, the dependency
/// radius of the feature step plus its own read ring.
constexpr jint scheduler_terrain_radius = 2;
/// A left-behind terrain entry is evicted only after this many further columns
/// were served, so the sliding window keeps the fills a decoration pass reads.
constexpr std::size_t max_retention_columns = 8;
constexpr std::size_t terrain_chunks_per_column = 25;

/// Terrain fills run ahead of the ordered lane so the decoration steps never
/// wait for a fill, but not so far that a stale window would be evicted again
/// before it reaches the lane. The bound is relative to the lane front and the
/// worker count.
[[nodiscard]] auto terrain_lookahead(std::size_t frontier,
                                     std::size_t workers) noexcept
    -> std::size_t {
    return frontier + std::max<std::size_t>(workers * 4U, 8U);
}

} // namespace

chunk_scheduler::chunk_scheduler(config settings)
    : settings_{std::move(settings)} {
    settings_.workers = std::clamp<std::size_t>(settings_.workers, 1, 64);
    settings_.generator.proto_cache_capacity =
        std::clamp<jint>(settings_.generator.proto_cache_capacity, 25, 4096);
    const auto capacity =
        static_cast<std::size_t>(settings_.generator.proto_cache_capacity);
    cache_ = std::make_unique<proto_chunk_cache>(capacity, &cache_mutex_);
    workers_.resize(settings_.workers);
}

auto chunk_scheduler::create(config settings)
    -> result<std::unique_ptr<chunk_scheduler>> {
    if (settings.generator.data_root.empty()) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "the per-chunk scheduler has no content data root")};
    }
    if (settings.workers == 0) {
        settings.workers = 1;
    }
    auto scheduler = std::unique_ptr<chunk_scheduler>{
        new chunk_scheduler{std::move(settings)}};
    // The shared state is the scheduler's cost: load the content block table
    // and the first worker generator now, so a configuration the content
    // cannot support fails at start instead of on the first demanded column,
    // and the first column does not pay for the load.
    if (const auto ready = scheduler->initialize_shared_state(); !ready) {
        return std::unexpected{ready.error()};
    }
    return scheduler;
}

auto chunk_scheduler::retention_columns() const noexcept -> std::size_t {
    const auto capacity = cache_->capacity();
    const auto columns = capacity / terrain_chunks_per_column;
    if (columns == 0) {
        return 0;
    }
    return std::min(max_retention_columns, columns - 1);
}

auto chunk_scheduler::initialize_shared_state() -> result<void> {
    const std::lock_guard lock{create_mutex_};
    if (shared_ != nullptr) {
        return {};
    }
    if (generator_error_.has_value()) {
        return std::unexpected{fail(error_code::unknown, *generator_error_)};
    }
    auto state = chunk_generator::create_scheduler_shared(settings_.generator);
    if (!state) {
        generator_error_ = state.error().message;
        return std::unexpected{state.error()};
    }
    shared_ = std::move(state->shared);
    states_ = shared_->shared_states;
    state->first->attach_pipeline_cache(cache_.get());
    biome_names_ = state->first->biome_names();
    workers_[0] = std::move(state->first);
    return {};
}

auto chunk_scheduler::worker_generator(std::size_t worker)
    -> result<chunk_generator *> {
    if (worker >= workers_.size()) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "a scheduler job names a worker that does not exist")};
    }
    std::shared_ptr<const chunk_generator::shared_generator_data> shared;
    {
        const std::lock_guard lock{create_mutex_};
        if (workers_[worker] != nullptr) {
            return workers_[worker].get();
        }
        if (generator_error_.has_value()) {
            return std::unexpected{
                fail(error_code::unknown, *generator_error_)};
        }
        shared = shared_;
    }
    if (shared == nullptr) {
        if (const auto ready = initialize_shared_state(); !ready) {
            return std::unexpected{ready.error()};
        }
        const std::lock_guard lock{create_mutex_};
        if (worker == 0) {
            return workers_[0].get();
        }
        shared = shared_;
    }
    // The table is frozen, so every later worker only reads it and the
    // immutable data; the builds can run concurrently.
    auto built =
        chunk_generator::create_with_shared(settings_.generator, shared);
    if (!built) {
        const std::lock_guard lock{create_mutex_};
        generator_error_ = built.error().message;
        return std::unexpected{built.error()};
    }
    (*built)->attach_pipeline_cache(cache_.get());
    const std::lock_guard lock{create_mutex_};
    biome_names_ = (*built)->biome_names();
    workers_[worker] = std::move(*built);
    return workers_[worker].get();
}

auto chunk_scheduler::demand(const chunk_pos &pos) -> bool {
    const std::lock_guard lock{mutex_};
    if (active_.contains(pos)) {
        return false;
    }
    demand_chain chain{};
    chain.position = pos;
    const auto index = chains_.size();
    chains_.push_back(chain);
    auto &stored = chains_.back();
    // The positions this demand publishes are the missing chunks of its
    // dependency window, x then z. A position is published once, by the first
    // demand that needs it, so which window fills a chunk is a function of the
    // demand sequence and not of worker timing.
    for (jint dx = -scheduler_terrain_radius; dx <= scheduler_terrain_radius;
         ++dx) {
        for (jint dz = -scheduler_terrain_radius;
             dz <= scheduler_terrain_radius; ++dz) {
            const chunk_pos position{.x = pos.x + dx, .z = pos.z + dz};
            if (terrain_owner_.contains(position)) {
                continue;
            }
            terrain_owner_.emplace(position, index);
            stored.terrain.push_back(position);
        }
    }
    // One terrain job fills the whole published list, so the count is one job.
    stored.terrain_remaining = stored.terrain.empty() ? 0U : 1U;
    if (stored.terrain_remaining == 0) {
        stored.terrain_done = true;
    }
    if (!stored.terrain.empty()) {
        terrain_queue_.push_back(index);
    }
    active_.emplace(pos, index);
    return true;
}

auto chunk_scheduler::ordered_front() const -> const demand_chain * {
    std::size_t index = ordered_front_;
    while (index < chains_.size()) {
        const auto &chain = chains_[index];
        if (chain.failed || chain.served) {
            ++index;
            continue;
        }
        return &chain;
    }
    return nullptr;
}

auto chunk_scheduler::has_job(std::size_t worker) const -> bool {
    (void)worker;
    const std::lock_guard lock{mutex_};
    if (!ordered_busy_) {
        if (const auto *front = ordered_front();
            front != nullptr && front->terrain_done) {
            return true;
        }
    }
    if (terrain_queue_.empty()) {
        return false;
    }
    const auto *front = ordered_front();
    const std::size_t frontier =
        front == nullptr ? chains_.size()
                         : static_cast<std::size_t>(front - chains_.data());
    return terrain_queue_.front() <=
           terrain_lookahead(frontier, settings_.workers);
}

auto chunk_scheduler::next_job(std::size_t worker)
    -> std::optional<pipeline_job> {
    const std::lock_guard lock{mutex_};
    if (!ordered_busy_) {
        if (const auto *front = ordered_front();
            front != nullptr && front->terrain_done) {
            ordered_busy_ = true;
            return pipeline_job{
                .step = front->decorated ? pipeline_step::served
                                         : pipeline_step::decorated,
                .position = front->position,
                .demand = front->position,
                .worker = worker,
            };
        }
    }
    const auto *front = ordered_front();
    const std::size_t frontier =
        front == nullptr ? chains_.size()
                         : static_cast<std::size_t>(front - chains_.data());
    const std::size_t limit = terrain_lookahead(frontier, settings_.workers);
    while (!terrain_queue_.empty()) {
        const auto index = terrain_queue_.front();
        if (index > limit) {
            return std::nullopt;
        }
        terrain_queue_.pop_front();
        const auto &chain = chains_[index];
        if (chain.failed || chain.served) {
            // The chain died or a repeated demand already served it.
            continue;
        }
        return pipeline_job{
            .step = pipeline_step::terrain,
            .position = chain.position,
            .demand = chain.position,
            .worker = worker,
        };
    }
    return std::nullopt;
}

auto chunk_scheduler::run(const pipeline_job &job)
    -> result<std::unique_ptr<proto_chunk>> {
    auto generator = worker_generator(job.worker);
    if (!generator) {
        return std::unexpected{generator.error()};
    }
    switch (job.step) {
    case pipeline_step::terrain: {
        std::vector<chunk_pos> positions;
        {
            const std::lock_guard lock{mutex_};
            const auto found = active_.find(job.demand);
            if (found != active_.end() && found->second < chains_.size()) {
                positions = chains_[found->second].terrain;
            }
        }
        if (auto filled =
                (*generator)->advance_terrain_for(job.demand, positions);
            !filled) {
            return std::unexpected{filled.error()};
        }
        return std::unique_ptr<proto_chunk>{};
    }
    case pipeline_step::decorated: {
        if (const auto refilled = refill_terrain(**generator, job.position);
            !refilled) {
            return std::unexpected{refilled.error()};
        }
        if (auto advanced = (*generator)->advance_decorated(job.position);
            !advanced) {
            return std::unexpected{advanced.error()};
        }
        return std::unique_ptr<proto_chunk>{};
    }
    case pipeline_step::served: {
        return (*generator)->take_served(job.position);
    }
    }
    return std::unexpected{
        fail(error_code::invalid_argument, "unknown per-chunk pipeline step")};
}

auto chunk_scheduler::refill_terrain(chunk_generator &generator,
                                     const chunk_pos &pos) -> result<void> {
    const auto box = square_box(pos, scheduler_terrain_radius);
    for (jint x = box.min_x; x <= box.max_x; ++x) {
        for (jint z = box.min_z; z <= box.max_z; ++z) {
            const chunk_pos position{.x = x, .z = z};
            if (cache_->contains(position)) {
                continue;
            }
            chunk_pos window = pos;
            {
                const std::lock_guard lock{mutex_};
                if (const auto owner = terrain_owner_.find(position);
                    owner != terrain_owner_.end() &&
                    owner->second < chains_.size()) {
                    window = chains_[owner->second].position;
                }
            }
            const auto positions = std::array<chunk_pos, 1>{position};
            if (auto filled = generator.advance_terrain_for(window, positions);
                !filled) {
                return std::unexpected{filled.error()};
            }
        }
    }
    return {};
}

void chunk_scheduler::evict_old_terrain() {
    const auto retention = retention_columns();
    const auto threshold = ordered_front_ > retention
                               ? ordered_front_ - retention
                               : std::size_t{0};
    (void)cache_->evict_terrain_if(
        [this, threshold](const chunk_pos &position) {
            const auto owner = terrain_owner_.find(position);
            // A position without an owner cannot be reproduced from a demand
            // window, so it is kept.
            return owner == terrain_owner_.end() || owner->second >= threshold;
        });
}

void chunk_scheduler::complete(const pipeline_job &job,
                               const std::string *error_message) {
    const std::lock_guard lock{mutex_};
    // The ordered lane carries one job at a time whatever happens to it.
    if (job.step != pipeline_step::terrain) {
        ordered_busy_ = false;
    }
    const auto finished = [&](demand_chain &chain) {
        if (error_message != nullptr) {
            if (!chain.failed) {
                chain.failed = true;
                failures_.push_back(pipeline_failure{
                    .position = chain.position, .message = *error_message});
                ++failed_count_;
            }
            active_.erase(chain.position);
            return;
        }
        if (job.step == pipeline_step::terrain) {
            chain.terrain_remaining = 0;
            chain.terrain_done = true;
            return;
        }
        if (job.step == pipeline_step::decorated) {
            chain.decorated = true;
            return;
        }
        chain.served = true;
        active_.erase(chain.position);
        ++served_;
        if (ordered_front_ < chains_.size()) {
            ++ordered_front_;
        }
        evict_old_terrain();
    };
    if (job.step == pipeline_step::terrain) {
        const auto found = active_.find(job.demand);
        if (found == active_.end()) {
            return;
        }
        finished(chains_[found->second]);
        return;
    }
    const auto found = active_.find(job.position);
    if (found == active_.end()) {
        return;
    }
    finished(chains_[found->second]);
}

auto chunk_scheduler::take_failures() -> std::vector<pipeline_failure> {
    std::vector<pipeline_failure> failures;
    {
        const std::lock_guard lock{mutex_};
        failures = std::exchange(failures_, {});
    }
    // Position order, so a caller's log and retry order never depends on the
    // order the workers failed in.
    std::sort(failures.begin(), failures.end(),
              [](const pipeline_failure &left, const pipeline_failure &right) {
                  return std::pair{left.position.x, left.position.z} <
                         std::pair{right.position.x, right.position.z};
              });
    return failures;
}

auto chunk_scheduler::biome_names() const -> const std::vector<std::string> & {
    const std::lock_guard lock{create_mutex_};
    return biome_names_;
}

auto chunk_scheduler::pending() const -> std::size_t {
    const std::lock_guard lock{mutex_};
    return active_.size();
}

auto chunk_scheduler::stats() const -> chunk_scheduler_stats {
    chunk_scheduler_stats result;
    {
        const std::lock_guard lock{mutex_};
        result.demanded = chains_.size();
        result.served = served_;
        result.failed = failed_count_;
        result.pending = active_.size();
    }
    const auto cache = cache_->stats();
    result.cache_entries = cache.entries;
    result.cache_terrain_entries = cache.terrain_entries;
    result.cache_evictions = cache.evictions;
    return result;
}

void chunk_scheduler::clear() {
    {
        const std::lock_guard lock{mutex_};
        chains_.clear();
        active_.clear();
        terrain_owner_.clear();
        terrain_queue_.clear();
        ordered_front_ = 0;
        ordered_busy_ = false;
        served_ = 0;
        failed_count_ = 0;
        failures_.clear();
    }
    cache_->clear();
}

} // namespace refrain::worldgen
