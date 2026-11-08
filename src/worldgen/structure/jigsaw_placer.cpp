// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/structure/generation.hpp>

#include <refrain/worldgen/chunk/block_state_codec.hpp>
#include <refrain/worldgen/chunk/proto_chunk.hpp>
#include <refrain/worldgen/structure/jigsaw.hpp>
#include <refrain/worldgen/structure/template.hpp>

#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace refrain::worldgen::structure {

namespace {

// Face order: down, up, north, south, west, east.
constexpr jint direction_down = 0;
constexpr jint direction_up = 1;
constexpr jint direction_north = 2;
constexpr jint direction_south = 3;
constexpr jint direction_west = 4;
constexpr jint direction_east = 5;

auto direction_from(std::string_view name) -> jint {
    if (name == "down") {
        return direction_down;
    }
    if (name == "up") {
        return direction_up;
    }
    if (name == "north") {
        return direction_north;
    }
    if (name == "south") {
        return direction_south;
    }
    if (name == "west") {
        return direction_west;
    }
    return direction_east;
}

auto direction_step_y(jint direction) -> jint {
    if (direction == direction_down) {
        return -1;
    }
    if (direction == direction_up) {
        return 1;
    }
    return 0;
}

auto direction_axis_y(jint direction) -> bool {
    return direction == direction_down || direction == direction_up;
}

struct orientation {
    jint front{direction_north};
    jint top{direction_up};
};

/// Parses the `orientation=<front>_<top>` property of a jigsaw block state.
auto orientation_of(std::string_view state) -> std::optional<orientation> {
    const auto key = state.find("orientation=");
    if (key == std::string_view::npos) {
        return std::nullopt;
    }
    auto value = state.substr(key + 12);
    const auto end = value.find_first_of(",]");
    if (end != std::string_view::npos) {
        value = value.substr(0, end);
    }
    static constexpr std::string_view fronts[]{"down",  "up",   "north",
                                               "south", "west", "east"};
    for (const auto front : fronts) {
        if (!value.starts_with(front) || value.size() <= front.size() ||
            value[front.size()] != '_') {
            continue;
        }
        const auto top = value.substr(front.size() + 1);
        return orientation{.front = direction_from(front),
                           .top = direction_from(top)};
    }
    return std::nullopt;
}

auto box_contains(const structure_bbox &box, const block_pos &pos) -> bool {
    return pos.x >= box.min_x && pos.x <= box.max_x && pos.y >= box.min_y &&
           pos.y <= box.max_y && pos.z >= box.min_z && pos.z <= box.max_z;
}

auto box_intersects(const structure_bbox &left, const structure_bbox &right)
    -> bool {
    return left.intersects(right);
}

auto to_element_info(const pool_element &element) -> jigsaw_element_info {
    jigsaw_element_info info{
        .kind = element.kind,
        .projection = element.projection,
        .template_id = element.template_id,
        .processor_list = element.processor_list,
        .inline_processors = element.inline_processors,
        .feature = element.feature,
        .liquid_override = element.liquid_override,
    };
    info.elements.reserve(element.elements.size());
    for (const auto &child : element.elements) {
        info.elements.push_back(to_element_info(child));
    }
    return info;
}

auto element_bounds(const jigsaw_element_info &element,
                    structure_template_manager &templates,
                    const block_pos &position, template_rotation rotation)
    -> structure_bbox {
    switch (element.kind) {
    case pool_element_kind::single:
    case pool_element_kind::legacy_single: {
        const auto &templ = templates.get_or_create(element.template_id);
        const template_transform transform{
            .mirror = template_mirror::none,
            .rotation = rotation,
            .pivot = block_pos{},
        };
        const auto bounds = template_bounding_box(templ, transform, position);
        return {.min_x = bounds.min_x,
                .min_y = bounds.min_y,
                .min_z = bounds.min_z,
                .max_x = bounds.max_x,
                .max_y = bounds.max_y,
                .max_z = bounds.max_z};
    }
    case pool_element_kind::list: {
        std::optional<structure_bbox> combined;
        for (const auto &child : element.elements) {
            if (child.kind == pool_element_kind::empty) {
                continue;
            }
            const auto bounds =
                element_bounds(child, templates, position, rotation);
            if (!combined) {
                combined = bounds;
                continue;
            }
            combined->min_x = std::min(combined->min_x, bounds.min_x);
            combined->min_y = std::min(combined->min_y, bounds.min_y);
            combined->min_z = std::min(combined->min_z, bounds.min_z);
            combined->max_x = std::max(combined->max_x, bounds.max_x);
            combined->max_y = std::max(combined->max_y, bounds.max_y);
            combined->max_z = std::max(combined->max_z, bounds.max_z);
        }
        return combined.value_or(structure_bbox{.min_x = position.x,
                                                .min_y = position.y,
                                                .min_z = position.z,
                                                .max_x = position.x,
                                                .max_y = position.y,
                                                .max_z = position.z});
    }
    case pool_element_kind::feature:
        return {.min_x = position.x,
                .min_y = position.y,
                .min_z = position.z,
                .max_x = position.x,
                .max_y = position.y,
                .max_z = position.z};
    case pool_element_kind::empty:
        break;
    }
    return {};
}

struct runtime_jigsaw {
    block_pos position{};
    block_state_id state{no_block_state};
    jint front{direction_north};
    jint top{direction_up};
    bool rollable{false};
    bool has_name{false};
    std::string name{};
    std::string pool{"minecraft:empty"};
    std::string target{"minecraft:empty"};
    jint placement_priority{0};
    jint selection_priority{0};
};

auto nbt_string(const data::nbt::compound &tag, std::string_view key)
    -> std::optional<std::string> {
    const auto found = tag.find(std::string{key});
    if (found == tag.end()) {
        return std::nullopt;
    }
    const auto text = found->second.as_string();
    if (!text) {
        return std::nullopt;
    }
    return std::string{*text};
}

auto nbt_int(const data::nbt::compound &tag, std::string_view key,
             jint fallback) -> jint {
    const auto found = tag.find(std::string{key});
    if (found == tag.end()) {
        return fallback;
    }
    const auto number = found->second.as_int();
    return number ? *number : fallback;
}

/// The palette selected for a placement. The reference uses a positional
/// random at the piece origin; a single palette consumes nothing.
auto palette_for(const structure_template &templ, const block_pos &position)
    -> const std::vector<template_block_info> & {
    static const std::vector<template_block_info> empty{};
    if (templ.palettes.empty()) {
        return empty;
    }
    if (templ.palettes.size() == 1) {
        return templ.palettes.front();
    }
    legacy_random_source random{
        mth_get_seed(position.x, position.y, position.z)};
    const auto index = static_cast<std::size_t>(
        random.next_int(static_cast<jint>(templ.palettes.size())));
    return templ.palettes[index];
}

auto template_jigsaws(const structure_template &templ,
                      block_state_registry &states, const block_pos &position,
                      template_rotation rotation)
    -> std::vector<runtime_jigsaw> {
    std::vector<runtime_jigsaw> result;
    const auto &palette = palette_for(templ, position);
    const template_transform transform{
        .mirror = template_mirror::none,
        .rotation = rotation,
        .pivot = block_pos{},
    };
    for (const auto &block : palette) {
        if (state_block_name(states.name(block.state)) != "minecraft:jigsaw" ||
            block.nbt == nullptr) {
            continue;
        }
        const auto state = transform_state(states, block.state, transform);
        const auto orientation = orientation_of(states.name(state));
        if (!orientation) {
            continue;
        }
        const auto local = transform_position(
            block_pos{.x = block.x, .y = block.y, .z = block.z}, transform);
        runtime_jigsaw jigsaw{
            .position = block_pos{.x = position.x + local.x,
                                  .y = position.y + local.y,
                                  .z = position.z + local.z},
            .state = state,
            .front = orientation->front,
            .top = orientation->top,
            .placement_priority = nbt_int(*block.nbt, "placement_priority", 0),
            .selection_priority = nbt_int(*block.nbt, "selection_priority", 0),
        };
        if (const auto joint = nbt_string(*block.nbt, "joint"); joint) {
            jigsaw.rollable = *joint == "rollable";
        } else {
            jigsaw.rollable = !direction_axis_y(orientation->front);
        }
        jigsaw.has_name = true;
        if (const auto name = nbt_string(*block.nbt, "name"); name) {
            jigsaw.name = *name;
        } else {
            jigsaw.name = "minecraft:empty";
        }
        if (const auto pool = nbt_string(*block.nbt, "pool"); pool) {
            jigsaw.pool = *pool;
        }
        if (const auto target = nbt_string(*block.nbt, "target"); target) {
            jigsaw.target = *target;
        }
        result.push_back(std::move(jigsaw));
    }
    return result;
}

/// One anchor pointing down, independent of the requested rotation.
auto feature_jigsaws(const block_pos &position) -> std::vector<runtime_jigsaw> {
    return {runtime_jigsaw{
        .position = position,
        .state = no_block_state,
        .front = direction_down,
        .top = direction_south,
        .rollable = true,
        .has_name = false,
        .name = {},
        .pool = "minecraft:empty",
        .target = "minecraft:empty",
        .placement_priority = 0,
        .selection_priority = 0,
    }};
}

auto element_jigsaws(const jigsaw_element_info &element,
                     structure_template_manager &templates,
                     const block_pos &position, template_rotation rotation)
    -> std::vector<runtime_jigsaw> {
    switch (element.kind) {
    case pool_element_kind::single:
    case pool_element_kind::legacy_single:
        return template_jigsaws(templates.get_or_create(element.template_id),
                                templates.states(), position, rotation);
    case pool_element_kind::list:
        if (!element.elements.empty()) {
            return element_jigsaws(element.elements.front(), templates,
                                   position, rotation);
        }
        return {};
    case pool_element_kind::feature:
        return feature_jigsaws(position);
    case pool_element_kind::empty:
        break;
    }
    return {};
}

auto shuffled_jigsaws(const jigsaw_element_info &element,
                      structure_template_manager &templates,
                      const block_pos &position, template_rotation rotation,
                      worldgen_random &random) -> std::vector<runtime_jigsaw> {
    auto jigsaws = element_jigsaws(element, templates, position, rotation);
    for (std::size_t i = jigsaws.size(); i > 1; --i) {
        const auto swap_to =
            static_cast<std::size_t>(random.next_int(static_cast<jint>(i)));
        std::swap(jigsaws[i - 1], jigsaws[swap_to]);
    }
    std::stable_sort(
        jigsaws.begin(), jigsaws.end(),
        [](const runtime_jigsaw &left, const runtime_jigsaw &right) {
            return left.selection_priority > right.selection_priority;
        });
    return jigsaws;
}

auto can_attach(const runtime_jigsaw &source, const runtime_jigsaw &target)
    -> bool {
    return connectors_attach(
        jigsaw_connector{
            .front = static_cast<jigsaw_front>(source.front),
            .top = static_cast<jigsaw_front>(source.top),
            .rollable = source.rollable,
            .has_name = source.has_name,
            .name = source.name,
            .target = source.target,
        },
        jigsaw_connector{
            .front = static_cast<jigsaw_front>(target.front),
            .top = static_cast<jigsaw_front>(target.top),
            .rollable = target.rollable,
            .has_name = target.has_name,
            .name = target.name,
            .target = target.target,
        });
}

auto rotated_rotations(worldgen_random &random)
    -> std::vector<template_rotation> {
    std::vector<template_rotation> rotations{
        template_rotation::none, template_rotation::clockwise_90,
        template_rotation::clockwise_180,
        template_rotation::counterclockwise_90};
    for (std::size_t i = rotations.size(); i > 1; --i) {
        const auto swap_to =
            static_cast<std::size_t>(random.next_int(static_cast<jint>(i)));
        std::swap(rotations[i - 1], rotations[swap_to]);
    }
    return rotations;
}

const jigsaw_template_pool *find_pool(const structure_data_set &data,
                                      std::string_view id) {
    const auto found = data.template_pools.find(std::string{id});
    return found == data.template_pools.end() ? nullptr : &found->second;
}

struct free_space {
    structure_bbox outer{};
    std::optional<structure_bbox> hole{};
    std::vector<structure_bbox> removed{};

    [[nodiscard]] auto covers(const structure_bbox &target) const -> bool {
        if (target.min_x < outer.min_x || target.max_x > outer.max_x ||
            target.min_y < outer.min_y || target.max_y > outer.max_y ||
            target.min_z < outer.min_z || target.max_z > outer.max_z) {
            return false;
        }
        if (hole.has_value() && box_intersects(*hole, target)) {
            return false;
        }
        for (const auto &box : removed) {
            if (box_intersects(box, target)) {
                return false;
            }
        }
        return true;
    }

    void remove(const structure_bbox &target) { removed.push_back(target); }
};

struct piece_state {
    std::size_t piece{0};
    std::shared_ptr<free_space> free{};
    jint depth{0};
};

/// Expands one start pool depth by depth in priority order and records
/// junctions on both sides of each connection.
class jigsaw_placer {
  public:
    jigsaw_placer(const jigsaw_generation_context &context,
                  const jigsaw_structure_config &config,
                  pool_alias_lookup aliases, worldgen_random &random)
        : context_{context}, config_{config}, templates_{*context.templates},
          random_{random}, aliases_{std::move(aliases)}, data_{*context.data} {}

    [[nodiscard]] auto pieces() -> std::vector<structure_piece> & {
        return pieces_;
    }

    void set_context_free(std::shared_ptr<free_space> free) {
        context_free_ = std::move(free);
    }

    void expand();

  private:
    void try_placing_children(std::size_t source_index, jint depth,
                              const std::shared_ptr<free_space> &context_free);

    [[nodiscard]] auto try_place_target(
        std::size_t source_index, jint depth,
        const runtime_jigsaw &source_jigsaw,
        const jigsaw_element_info &source_element, jint source_ground_delta,
        bool source_rigid, jint source_box_y, jint source_jigsaw_local_y,
        const block_pos &target_jigsaw_pos,
        const std::shared_ptr<free_space> &children_free,
        std::optional<jint> &base_height,
        const std::vector<const pool_element *> &target_pieces) -> bool;

    [[nodiscard]] auto first_free_height(jint x, jint z) const -> jint {
        return context_.height_at(x, z, heightmap_kind::world_surface_wg);
    }

    [[nodiscard]] auto pool_of(std::string_view id) const
        -> const jigsaw_template_pool * {
        return find_pool(data_, aliases_.lookup(id));
    }

    const jigsaw_generation_context &context_;
    const jigsaw_structure_config &config_;
    structure_template_manager &templates_;
    worldgen_random &random_;
    pool_alias_lookup aliases_;
    const structure_data_set &data_;
    std::vector<structure_piece> pieces_{};
    std::shared_ptr<free_space> context_free_{};
    std::map<jint, std::deque<piece_state>, std::greater<jint>> placing_{};
};

void jigsaw_placer::expand() {
    if (pieces_.empty()) {
        return;
    }
    try_placing_children(0, 0, context_free_);
    while (!placing_.empty()) {
        auto queue = placing_.begin();
        auto state = queue->second.front();
        queue->second.pop_front();
        if (queue->second.empty()) {
            placing_.erase(queue);
        }
        try_placing_children(state.piece, state.depth, state.free);
    }
}

void jigsaw_placer::try_placing_children(
    std::size_t source_index, jint depth,
    const std::shared_ptr<free_space> &context_free) {
    const auto source_bounds = pieces_[source_index].bounds;
    const auto source_position = pieces_[source_index].template_origin;
    const auto source_rotation =
        static_cast<template_rotation>(pieces_[source_index].template_rotation);
    const auto source_element = pieces_[source_index].jigsaw->element;
    const jint source_ground_delta =
        pieces_[source_index].jigsaw->ground_level_delta;
    const bool source_rigid =
        source_element.projection == pool_projection::rigid;
    const jint source_box_y = source_bounds.min_y;
    std::shared_ptr<free_space> source_free;

    const auto jigsaws = shuffled_jigsaws(
        source_element, templates_, source_position, source_rotation, random_);
    for (const auto &source_jigsaw : jigsaws) {
        const auto *target_pool = pool_of(source_jigsaw.pool);
        if (target_pool == nullptr) {
            continue;
        }
        std::optional<jint> base_height;
        const block_pos target_jigsaw_pos{
            .x = source_jigsaw.position.x +
                 (source_jigsaw.front == direction_east   ? 1
                  : source_jigsaw.front == direction_west ? -1
                                                          : 0),
            .y = source_jigsaw.position.y +
                 direction_step_y(source_jigsaw.front),
            .z = source_jigsaw.position.z +
                 (source_jigsaw.front == direction_south   ? 1
                  : source_jigsaw.front == direction_north ? -1
                                                           : 0),
        };
        const jint source_jigsaw_local_y =
            source_jigsaw.position.y - source_box_y;
        const bool attach_inside =
            box_contains(source_bounds, target_jigsaw_pos);
        // A piece placed inside its parent inherits the parent's box as the
        // space its children must fit in; every other piece inherits the
        // context that was current when it was queued.
        std::shared_ptr<free_space> children_free = context_free;
        if (attach_inside) {
            if (!source_free) {
                source_free = std::make_shared<free_space>();
                source_free->outer = source_bounds;
            }
            children_free = source_free;
        }

        std::vector<const pool_element *> target_pieces;
        if (depth != config_.max_depth) {
            const auto candidates =
                target_pool->get_shuffled_templates(random_);
            target_pieces.insert(target_pieces.end(), candidates.begin(),
                                 candidates.end());
        }
        if (const auto *fallback = find_pool(data_, target_pool->fallback);
            fallback != nullptr) {
            const auto candidates = fallback->get_shuffled_templates(random_);
            target_pieces.insert(target_pieces.end(), candidates.begin(),
                                 candidates.end());
        }

        if (try_place_target(source_index, depth, source_jigsaw, source_element,
                             source_ground_delta, source_rigid, source_box_y,
                             source_jigsaw_local_y, target_jigsaw_pos,
                             children_free, base_height, target_pieces)) {
            continue;
        }
    }
}

auto jigsaw_placer::try_place_target(
    std::size_t source_index, jint depth, const runtime_jigsaw &source_jigsaw,
    const jigsaw_element_info &source_element, jint source_ground_delta,
    bool source_rigid, jint source_box_y, jint source_jigsaw_local_y,
    const block_pos &target_jigsaw_pos,
    const std::shared_ptr<free_space> &children_free,
    std::optional<jint> &base_height,
    const std::vector<const pool_element *> &target_pieces) -> bool {
    for (const auto *target_element : target_pieces) {
        if (target_element->kind == pool_element_kind::empty) {
            break;
        }
        const auto target_info = to_element_info(*target_element);
        const bool target_rigid =
            target_info.projection == pool_projection::rigid;
        for (const auto target_rotation : rotated_rotations(random_)) {
            const auto target_jigsaws = shuffled_jigsaws(
                target_info, templates_, block_pos{}, target_rotation, random_);
            const auto hack_box = element_bounds(target_info, templates_,
                                                 block_pos{}, target_rotation);
            jint expand_to = 0;
            if (config_.use_expansion_hack &&
                hack_box.max_y - hack_box.min_y + 1 <= 16) {
                for (const auto &target_jigsaw : target_jigsaws) {
                    const block_pos anchor{
                        .x = target_jigsaw.position.x +
                             (target_jigsaw.front == direction_east   ? 1
                              : target_jigsaw.front == direction_west ? -1
                                                                      : 0),
                        .y = target_jigsaw.position.y +
                             direction_step_y(target_jigsaw.front),
                        .z = target_jigsaw.position.z +
                             (target_jigsaw.front == direction_south   ? 1
                              : target_jigsaw.front == direction_north ? -1
                                                                       : 0),
                    };
                    if (!box_contains(hack_box, anchor)) {
                        continue;
                    }
                    const auto *child_pool = pool_of(target_jigsaw.pool);
                    jint child_size = 0;
                    jint fallback_size = 0;
                    if (child_pool != nullptr) {
                        child_size = child_pool->get_max_size(templates_);
                        if (const auto *child_fallback =
                                find_pool(data_, child_pool->fallback);
                            child_fallback != nullptr) {
                            fallback_size =
                                child_fallback->get_max_size(templates_);
                        }
                    }
                    expand_to = std::max(expand_to,
                                         std::max(child_size, fallback_size));
                }
            }
            for (const auto &target_jigsaw : target_jigsaws) {
                if (!can_attach(source_jigsaw, target_jigsaw)) {
                    continue;
                }
                const block_pos raw_target_box_pos{
                    .x = target_jigsaw_pos.x - target_jigsaw.position.x,
                    .y = target_jigsaw_pos.y - target_jigsaw.position.y,
                    .z = target_jigsaw_pos.z - target_jigsaw.position.z,
                };
                const auto raw_bounds =
                    element_bounds(target_info, templates_, raw_target_box_pos,
                                   target_rotation);
                const jint raw_target_y = raw_bounds.min_y;
                const jint target_jigsaw_local_y = target_jigsaw.position.y;
                const jint delta_y = source_jigsaw_local_y -
                                     target_jigsaw_local_y +
                                     direction_step_y(source_jigsaw.front);
                jint target_box_y = 0;
                if (source_rigid && target_rigid) {
                    target_box_y = source_box_y + delta_y;
                } else {
                    if (!base_height) {
                        base_height = first_free_height(
                            source_jigsaw.position.x, source_jigsaw.position.z);
                    }
                    target_box_y = *base_height - target_jigsaw_local_y;
                }
                const jint y_offset = target_box_y - raw_target_y;
                auto target_bounds = raw_bounds;
                target_bounds.min_y += y_offset;
                target_bounds.max_y += y_offset;
                const block_pos target_box_position{
                    .x = raw_target_box_pos.x,
                    .y = raw_target_box_pos.y + y_offset,
                    .z = raw_target_box_pos.z,
                };
                if (expand_to > 0) {
                    const jint new_size =
                        std::max(expand_to + 1,
                                 target_bounds.max_y - target_bounds.min_y);
                    target_bounds.max_y = std::max(
                        target_bounds.max_y, target_bounds.min_y + new_size);
                }
                if (children_free == nullptr ||
                    !children_free->covers(target_bounds)) {
                    continue;
                }
                children_free->remove(target_bounds);
                const jint target_ground_delta =
                    target_rigid ? source_ground_delta - delta_y : 1;
                jint junction_y = 0;
                if (source_rigid) {
                    junction_y = source_box_y + source_jigsaw_local_y;
                } else if (target_rigid) {
                    junction_y = target_box_y + target_jigsaw_local_y;
                } else {
                    if (!base_height) {
                        base_height = first_free_height(
                            source_jigsaw.position.x, source_jigsaw.position.z);
                    }
                    junction_y = *base_height + delta_y / 2;
                }

                structure_piece target_piece{
                    .type = "minecraft:jigsaw",
                    .bounds = target_bounds,
                    .orientation = -1,
                    .template_origin = target_box_position,
                    .template_rotation = static_cast<jint>(target_rotation),
                    .template_mirror = 0,
                    .template_pivot = block_pos{},
                };
                target_piece.jigsaw = jigsaw_piece_data{
                    .element = target_info,
                    .ground_level_delta = target_ground_delta,
                    .liquid = config_.liquid,
                    .junctions = {},
                };
                auto &source_data = *pieces_[source_index].jigsaw;
                source_data.junctions.push_back(jigsaw_junction_info{
                    .source_x = target_jigsaw_pos.x,
                    .source_ground_y = junction_y - source_jigsaw_local_y +
                                       source_ground_delta,
                    .source_z = target_jigsaw_pos.z,
                    .delta_y = delta_y,
                    .dest_projection = target_info.projection,
                });
                target_piece.jigsaw->junctions.push_back(jigsaw_junction_info{
                    .source_x = source_jigsaw.position.x,
                    .source_ground_y = junction_y - target_jigsaw_local_y +
                                       target_ground_delta,
                    .source_z = source_jigsaw.position.z,
                    .delta_y = -delta_y,
                    .dest_projection = source_element.projection,
                });
                pieces_.push_back(std::move(target_piece));
                if (depth + 1 <= config_.max_depth) {
                    placing_[source_jigsaw.placement_priority].push_back(
                        piece_state{.piece = pieces_.size() - 1,
                                    .free = children_free,
                                    .depth = depth + 1});
                }
                return true;
            }
        }
    }
    return false;
}

auto heightmap_from_name(std::string_view name)
    -> std::optional<heightmap_kind> {
    if (name == "WORLD_SURFACE_WG") {
        return heightmap_kind::world_surface_wg;
    }
    if (name == "WORLD_SURFACE") {
        return heightmap_kind::world_surface;
    }
    if (name == "OCEAN_FLOOR_WG") {
        return heightmap_kind::ocean_floor_wg;
    }
    if (name == "OCEAN_FLOOR") {
        return heightmap_kind::ocean_floor;
    }
    if (name == "MOTION_BLOCKING") {
        return heightmap_kind::motion_blocking;
    }
    if (name == "MOTION_BLOCKING_NO_LEAVES") {
        return heightmap_kind::motion_blocking_no_leaves;
    }
    return std::nullopt;
}

} // namespace

auto jigsaw_template_pool::get_max_size(
    structure_template_manager &manager) const -> jint {
    if (max_size.has_value()) {
        return *max_size;
    }
    jint best = 0;
    for (const auto &weighted : raw_elements) {
        if (weighted.element.kind == pool_element_kind::empty) {
            continue;
        }
        const auto info = to_element_info(weighted.element);
        const auto bounds =
            element_bounds(info, manager, block_pos{}, template_rotation::none);
        best = std::max(best, bounds.max_y - bounds.min_y + 1);
    }
    max_size = best;
    return best;
}

auto jigsaw_start(const chunk_pos &chunk,
                  const jigsaw_generation_context &context)
    -> std::optional<structure_start> {
    if (context.structure == nullptr || context.data == nullptr ||
        context.templates == nullptr || !context.structure->jigsaw) {
        return std::nullopt;
    }
    const auto &config = *context.structure->jigsaw;
    worldgen_random random{std::make_unique<legacy_random_source>(0)};
    random.set_large_feature_seed(context.seed, chunk.x, chunk.z);
    const jint provider_min = context.generation_height > 0
                                  ? context.generation_min_y
                                  : context.min_y;
    const jint provider_height = context.generation_height > 0
                                     ? context.generation_height
                                     : context.world_height;
    const jint height = config.start_height.sample(
        random, provider_min, provider_height, context.sea_level);
    const block_pos start_position{
        .x = chunk.x * 16, .y = height, .z = chunk.z * 16};
    const auto center_rotation =
        static_cast<template_rotation>(random.next_int(4));
    auto aliases = pool_alias_lookup::create(config.pool_aliases,
                                             start_position, context.seed);
    const auto *start_pool =
        find_pool(*context.data, aliases.lookup(config.start_pool));
    if (start_pool == nullptr) {
        return std::nullopt;
    }
    const auto *center_element = start_pool->get_random_template(random);
    if (center_element == nullptr ||
        center_element->kind == pool_element_kind::empty) {
        return std::nullopt;
    }
    const auto center_info = to_element_info(*center_element);

    block_pos adjusted_position = start_position;
    block_pos local_anchor{};
    if (!config.start_jigsaw_name.empty()) {
        const auto jigsaws =
            shuffled_jigsaws(center_info, *context.templates, start_position,
                             center_rotation, random);
        std::optional<block_pos> anchor;
        for (const auto &jigsaw : jigsaws) {
            if (jigsaw.has_name && jigsaw.name == config.start_jigsaw_name) {
                anchor = jigsaw.position;
                break;
            }
        }
        if (!anchor) {
            return std::nullopt;
        }
        local_anchor = block_pos{.x = anchor->x - start_position.x,
                                 .y = anchor->y - start_position.y,
                                 .z = anchor->z - start_position.z};
        adjusted_position = block_pos{.x = start_position.x - local_anchor.x,
                                      .y = start_position.y - local_anchor.y,
                                      .z = start_position.z - local_anchor.z};
    }

    auto center_bounds = element_bounds(center_info, *context.templates,
                                        adjusted_position, center_rotation);
    const jint center_x = (center_bounds.min_x + center_bounds.max_x) / 2;
    const jint center_z = (center_bounds.min_z + center_bounds.max_z) / 2;
    const jint max_y = context.min_y + context.world_height - 1;
    jint bottom_y = adjusted_position.y;
    if (!config.project_start_to_heightmap.empty()) {
        const auto kind =
            heightmap_from_name(config.project_start_to_heightmap);
        if (!kind) {
            return std::nullopt;
        }
        if (context.column_allowed &&
            !context.column_allowed(center_x, center_z)) {
            return std::nullopt;
        }
        bottom_y =
            start_position.y + context.height_at(center_x, center_z, *kind);
    }
    const jint center_ground_delta = 1;
    const jint old_absolute_ground_y =
        center_bounds.min_y + center_ground_delta;
    const jint shift = bottom_y - old_absolute_ground_y;
    center_bounds.min_y += shift;
    center_bounds.max_y += shift;
    adjusted_position.y += shift;
    if ((config.padding_bottom != 0 || config.padding_top != 0) &&
        (center_bounds.min_y < context.min_y + config.padding_bottom ||
         center_bounds.max_y > max_y - config.padding_top)) {
        return std::nullopt;
    }
    const jint center_y = bottom_y + local_anchor.y;
    if (context.valid_biome &&
        !context.valid_biome(
            block_pos{.x = center_x, .y = center_y, .z = center_z})) {
        return std::nullopt;
    }

    structure_piece center_piece{
        .type = "minecraft:jigsaw",
        .bounds = center_bounds,
        .orientation = -1,
        .template_origin = adjusted_position,
        .template_rotation = static_cast<jint>(center_rotation),
        .template_mirror = 0,
        .template_pivot = block_pos{},
    };
    center_piece.jigsaw = jigsaw_piece_data{
        .element = center_info,
        .ground_level_delta = center_ground_delta,
        .liquid = config.liquid,
        .junctions = {},
    };

    jigsaw_placer placer{context, config, std::move(aliases), random};
    placer.pieces().push_back(std::move(center_piece));
    if (config.max_depth > 0) {
        const auto free = std::make_shared<free_space>();
        free->outer = structure_bbox{
            .min_x = center_x - config.max_distance_horizontal,
            .min_y = std::max(center_y - config.max_distance_vertical,
                              context.min_y + config.padding_bottom),
            .min_z = center_z - config.max_distance_horizontal,
            .max_x = center_x + config.max_distance_horizontal,
            .max_y = std::min(center_y + config.max_distance_vertical,
                              max_y - config.padding_top),
            .max_z = center_z + config.max_distance_horizontal,
        };
        free->hole = center_bounds;
        placer.set_context_free(free);
        placer.expand();
    }

    auto &pieces = placer.pieces();
    structure_start start{
        .structure = context.structure->id,
        .step = context.structure->step,
        .chunk = chunk,
        .pieces = std::move(pieces),
        .references = 0,
        .max_references = 1,
        .bounds = {},
    };
    if (start.pieces.empty()) {
        return std::nullopt;
    }
    start.bounds = start.pieces.front().bounds;
    for (const auto &piece : start.pieces) {
        start.bounds.min_x = std::min(start.bounds.min_x, piece.bounds.min_x);
        start.bounds.min_y = std::min(start.bounds.min_y, piece.bounds.min_y);
        start.bounds.min_z = std::min(start.bounds.min_z, piece.bounds.min_z);
        start.bounds.max_x = std::max(start.bounds.max_x, piece.bounds.max_x);
        start.bounds.max_y = std::max(start.bounds.max_y, piece.bounds.max_y);
        start.bounds.max_z = std::max(start.bounds.max_z, piece.bounds.max_z);
    }
    return start;
}

} // namespace refrain::worldgen::structure
