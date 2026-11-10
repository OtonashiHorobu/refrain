// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// End city generation. A start is a fixed base floor, two floors and a roof,
// followed by a recursive section graph: towers grow upward and either bridge
// out or hand over to a fat tower; bridges may chain houses or stop at a ship;
// houses, bridges and fat towers each have their own random draws. Collisions
// are resolved against the pieces of the enclosing section, which is why a
// rejected section never enters the start.

#include <refrain/worldgen/structure/end_city.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace refrain::worldgen::structure {

namespace {

constexpr jint max_gen_depth = 8;

struct city_piece {
    structure_piece piece;
    jint gen_depth{0};
};

struct city_state {
    structure_template_manager *templates{nullptr};
    worldgen_random *random{nullptr};
    bool ship_created{false};
};

using section_generator = auto (*)(city_state &, jint, const city_piece &,
                                   const block_pos &, std::vector<city_piece> &)
    -> bool;

auto tower_generator(city_state &state, jint gen_depth,
                     const city_piece &parent, const block_pos &offset,
                     std::vector<city_piece> &pieces) -> bool;
auto tower_bridge_generator(city_state &state, jint gen_depth,
                            const city_piece &parent, const block_pos &offset,
                            std::vector<city_piece> &pieces) -> bool;

auto rotation_after(template_rotation base, template_rotation added)
    -> template_rotation {
    return static_cast<template_rotation>(
        (static_cast<jint>(base) + static_cast<jint>(added)) % 4);
}

auto make_piece(structure_template_manager &templates, const block_pos &origin,
                template_rotation rotation, std::string_view name,
                bool overwrite) -> city_piece {
    const std::string id{"minecraft:end_city/" + std::string{name}};
    const auto &templ = templates.get_or_create(id);
    const auto transform = template_transform{
        .mirror = template_mirror::none,
        .rotation = rotation,
        .pivot = block_pos{},
    };
    const auto bounds = template_bounding_box(templ, transform, origin);
    structure_piece piece{
        .type = "minecraft:end_city",
        .bounds = {.min_x = bounds.min_x,
                   .min_y = bounds.min_y,
                   .min_z = bounds.min_z,
                   .max_x = bounds.max_x,
                   .max_y = bounds.max_y,
                   .max_z = bounds.max_z},
        .orientation = -1,
        .template_id = id,
        .template_origin = origin,
        .template_rotation = static_cast<jint>(rotation),
        .template_mirror = 0,
        .template_pivot = block_pos{},
        .overwrite = overwrite,
    };
    return city_piece{.piece = std::move(piece), .gen_depth = 0};
}

/// Adds a child at the parent origin moved by `offset` under the parent
/// rotation and returns its index in `pieces`.
auto add_child(std::vector<city_piece> &pieces, city_state &state,
               const city_piece &parent, const block_pos &offset,
               std::string_view name, template_rotation rotation,
               bool overwrite) -> std::size_t {
    const auto relative = transform_position(
        offset, template_transform{.mirror = template_mirror::none,
                                   .rotation = static_cast<template_rotation>(
                                       parent.piece.template_rotation),
                                   .pivot = block_pos{}});
    const block_pos origin{
        .x = parent.piece.template_origin.x + relative.x,
        .y = parent.piece.template_origin.y + relative.y,
        .z = parent.piece.template_origin.z + relative.z,
    };
    pieces.push_back(
        make_piece(*state.templates, origin, rotation, name, overwrite));
    return pieces.size() - 1;
}

auto intersects_any(const std::vector<city_piece> &pieces,
                    const structure_bbox &box, jint parent_depth) -> bool {
    for (const auto &piece : pieces) {
        if (piece.piece.bounds.intersects(box) &&
            piece.gen_depth != parent_depth) {
            return true;
        }
    }
    return false;
}

auto recursive_children(city_state &state, section_generator generator,
                        jint gen_depth, const city_piece &parent,
                        const block_pos &offset,
                        std::vector<city_piece> &target) -> bool {
    if (gen_depth > max_gen_depth) {
        return false;
    }
    std::vector<city_piece> children;
    if (!generator(state, gen_depth, parent, offset, children)) {
        return false;
    }
    const jint child_tag = state.random->next_int();
    bool collision = false;
    for (auto &child : children) {
        child.gen_depth = child_tag;
        if (intersects_any(target, child.piece.bounds, parent.gen_depth)) {
            collision = true;
            break;
        }
    }
    if (collision) {
        return false;
    }
    target.insert(target.end(), std::make_move_iterator(children.begin()),
                  std::make_move_iterator(children.end()));
    return true;
}

auto house_tower_generator(city_state &state, jint gen_depth,
                           const city_piece &parent, const block_pos &offset,
                           std::vector<city_piece> &pieces) -> bool {
    if (gen_depth > max_gen_depth) {
        return false;
    }
    const auto rotation =
        static_cast<template_rotation>(parent.piece.template_rotation);
    auto last =
        add_child(pieces, state, parent, offset, "base_floor", rotation, true);
    const jint num_floors = state.random->next_int(3);
    if (num_floors == 0) {
        last = add_child(pieces, state, pieces[last],
                         block_pos{.x = -1, .y = 4, .z = -1}, "base_roof",
                         rotation, true);
    } else if (num_floors == 1) {
        last = add_child(pieces, state, pieces[last],
                         block_pos{.x = -1, .y = 0, .z = -1}, "second_floor_2",
                         rotation, false);
        last = add_child(pieces, state, pieces[last],
                         block_pos{.x = -1, .y = 8, .z = -1}, "second_roof",
                         rotation, false);
        (void)recursive_children(state, tower_generator, gen_depth + 1,
                                 pieces[last], block_pos{}, pieces);
    } else if (num_floors == 2) {
        last = add_child(pieces, state, pieces[last],
                         block_pos{.x = -1, .y = 0, .z = -1}, "second_floor_2",
                         rotation, false);
        last = add_child(pieces, state, pieces[last],
                         block_pos{.x = -1, .y = 4, .z = -1}, "third_floor_2",
                         rotation, false);
        last = add_child(pieces, state, pieces[last],
                         block_pos{.x = -1, .y = 8, .z = -1}, "third_roof",
                         rotation, true);
        (void)recursive_children(state, tower_generator, gen_depth + 1,
                                 pieces[last], block_pos{}, pieces);
    }
    return true;
}

struct bridge_spec {
    template_rotation rotation;
    block_pos offset;
};

constexpr std::array<bridge_spec, 4> tower_bridges{{
    {.rotation = template_rotation::none, .offset = {.x = 1, .y = -1, .z = 0}},
    {.rotation = template_rotation::clockwise_90,
     .offset = {.x = 6, .y = -1, .z = 1}},
    {.rotation = template_rotation::counterclockwise_90,
     .offset = {.x = 0, .y = -1, .z = 5}},
    {.rotation = template_rotation::clockwise_180,
     .offset = {.x = 5, .y = -1, .z = 6}},
}};

constexpr std::array<bridge_spec, 4> fat_tower_bridges{{
    {.rotation = template_rotation::none, .offset = {.x = 4, .y = -1, .z = 0}},
    {.rotation = template_rotation::clockwise_90,
     .offset = {.x = 12, .y = -1, .z = 4}},
    {.rotation = template_rotation::counterclockwise_90,
     .offset = {.x = 0, .y = -1, .z = 8}},
    {.rotation = template_rotation::clockwise_180,
     .offset = {.x = 8, .y = -1, .z = 12}},
}};

auto fat_tower_generator(city_state &state, jint gen_depth,
                         const city_piece &parent, const block_pos &offset,
                         std::vector<city_piece> &pieces) -> bool {
    (void)offset;
    const auto rotation =
        static_cast<template_rotation>(parent.piece.template_rotation);
    auto last =
        add_child(pieces, state, parent, block_pos{.x = -3, .y = 4, .z = -3},
                  "fat_tower_base", rotation, true);
    last = add_child(pieces, state, pieces[last],
                     block_pos{.x = 0, .y = 4, .z = 0}, "fat_tower_middle",
                     rotation, true);
    for (jint i = 0; i < 2 && state.random->next_int(3) != 0; ++i) {
        last = add_child(pieces, state, pieces[last],
                         block_pos{.x = 0, .y = 8, .z = 0}, "fat_tower_middle",
                         rotation, true);
        for (const auto &bridge : fat_tower_bridges) {
            if (!state.random->next_boolean()) {
                continue;
            }
            const auto bridge_index = add_child(
                pieces, state, pieces[last], bridge.offset, "bridge_end",
                rotation_after(rotation, bridge.rotation), true);
            (void)recursive_children(state, tower_bridge_generator,
                                     gen_depth + 1, pieces[bridge_index],
                                     block_pos{}, pieces);
        }
    }
    last = add_child(pieces, state, pieces[last],
                     block_pos{.x = -2, .y = 8, .z = -2}, "fat_tower_top",
                     rotation, true);
    return true;
}

auto tower_generator(city_state &state, jint gen_depth,
                     const city_piece &parent, const block_pos &offset,
                     std::vector<city_piece> &pieces) -> bool {
    (void)offset;
    const auto rotation =
        static_cast<template_rotation>(parent.piece.template_rotation);
    const jint base_x = 3 + state.random->next_int(2);
    const jint base_z = 3 + state.random->next_int(2);
    auto last = add_child(pieces, state, parent,
                          block_pos{.x = base_x, .y = -3, .z = base_z},
                          "tower_base", rotation, true);
    last = add_child(pieces, state, pieces[last],
                     block_pos{.x = 0, .y = 7, .z = 0}, "tower_piece", rotation,
                     true);
    std::optional<city_piece> bridge_piece;
    if (state.random->next_int(3) == 0) {
        bridge_piece = pieces[last];
    }
    const jint tower_height = 1 + state.random->next_int(3);
    for (jint i = 0; i < tower_height; ++i) {
        last = add_child(pieces, state, pieces[last],
                         block_pos{.x = 0, .y = 4, .z = 0}, "tower_piece",
                         rotation, true);
        if (i < tower_height - 1 && state.random->next_boolean()) {
            bridge_piece = pieces[last];
        }
    }
    if (bridge_piece.has_value()) {
        for (const auto &bridge : tower_bridges) {
            if (!state.random->next_boolean()) {
                continue;
            }
            const auto bridge_index = add_child(
                pieces, state, *bridge_piece, bridge.offset, "bridge_end",
                rotation_after(rotation, bridge.rotation), true);
            (void)recursive_children(state, tower_bridge_generator,
                                     gen_depth + 1, pieces[bridge_index],
                                     block_pos{}, pieces);
        }
        last = add_child(pieces, state, pieces[last],
                         block_pos{.x = -1, .y = 4, .z = -1}, "tower_top",
                         rotation, true);
    } else {
        if (gen_depth != 7) {
            return recursive_children(state, fat_tower_generator, gen_depth + 1,
                                      pieces[last], block_pos{}, pieces);
        }
        last = add_child(pieces, state, pieces[last],
                         block_pos{.x = -1, .y = 4, .z = -1}, "tower_top",
                         rotation, true);
    }
    return true;
}

auto tower_bridge_generator(city_state &state, jint gen_depth,
                            const city_piece &parent, const block_pos &offset,
                            std::vector<city_piece> &pieces) -> bool {
    (void)offset;
    const auto rotation =
        static_cast<template_rotation>(parent.piece.template_rotation);
    const jint bridge_length = state.random->next_int(4) + 1;
    auto last =
        add_child(pieces, state, parent, block_pos{.x = 0, .y = 0, .z = -4},
                  "bridge_piece", rotation, true);
    pieces[last].gen_depth = -1;
    jint next_y = 0;
    for (jint i = 0; i < bridge_length; ++i) {
        if (state.random->next_boolean()) {
            last = add_child(pieces, state, pieces[last],
                             block_pos{.x = 0, .y = next_y, .z = -4},
                             "bridge_piece", rotation, true);
            next_y = 0;
        } else {
            if (state.random->next_boolean()) {
                last = add_child(pieces, state, pieces[last],
                                 block_pos{.x = 0, .y = next_y, .z = -4},
                                 "bridge_steep_stairs", rotation, true);
            } else {
                last = add_child(pieces, state, pieces[last],
                                 block_pos{.x = 0, .y = next_y, .z = -8},
                                 "bridge_gentle_stairs", rotation, true);
            }
            next_y = 4;
        }
    }
    if (!state.ship_created && state.random->next_int(10 - gen_depth) == 0) {
        const jint ship_x = -8 + state.random->next_int(8);
        const jint ship_z = -70 + state.random->next_int(10);
        (void)add_child(pieces, state, pieces[last],
                        block_pos{.x = ship_x, .y = next_y, .z = ship_z},
                        "ship", rotation, true);
        state.ship_created = true;
    } else if (!recursive_children(
                   state, house_tower_generator, gen_depth + 1, pieces[last],
                   block_pos{.x = -3, .y = next_y + 1, .z = -11}, pieces)) {
        return false;
    }
    last = add_child(pieces, state, pieces[last],
                     block_pos{.x = 4, .y = next_y, .z = 0}, "bridge_end",
                     rotation_after(rotation, template_rotation::clockwise_180),
                     true);
    pieces[last].gen_depth = -1;
    return true;
}

auto union_bounds(const std::vector<city_piece> &pieces) -> structure_bbox {
    structure_bbox bounds = pieces.front().piece.bounds;
    for (const auto &piece : pieces) {
        bounds.min_x = std::min(bounds.min_x, piece.piece.bounds.min_x);
        bounds.min_y = std::min(bounds.min_y, piece.piece.bounds.min_y);
        bounds.min_z = std::min(bounds.min_z, piece.piece.bounds.min_z);
        bounds.max_x = std::max(bounds.max_x, piece.piece.bounds.max_x);
        bounds.max_y = std::max(bounds.max_y, piece.piece.bounds.max_y);
        bounds.max_z = std::max(bounds.max_z, piece.piece.bounds.max_z);
    }
    return bounds;
}

} // namespace

auto end_city_start(structure_template_manager &templates,
                    const block_pos &origin, template_rotation rotation,
                    worldgen_random &random) -> std::optional<structure_start> {
    city_state state{.templates = &templates, .random = &random};
    std::vector<city_piece> pieces;
    pieces.push_back(
        make_piece(templates, origin, rotation, "base_floor", true));
    auto last = add_child(pieces, state, pieces.front(),
                          block_pos{.x = -1, .y = 0, .z = -1}, "second_floor_1",
                          rotation, false);
    last = add_child(pieces, state, pieces[last],
                     block_pos{.x = -1, .y = 4, .z = -1}, "third_floor_1",
                     rotation, false);
    last = add_child(pieces, state, pieces[last],
                     block_pos{.x = -1, .y = 8, .z = -1}, "third_roof",
                     rotation, true);
    (void)recursive_children(state, tower_generator, 1, pieces[last],
                             block_pos{}, pieces);

    if (pieces.empty()) {
        return std::nullopt;
    }
    structure_start start{
        .structure = "minecraft:end_city",
        .chunk = {},
        .references = 0,
        .max_references = 1,
        .bounds = union_bounds(pieces),
    };
    start.pieces.reserve(pieces.size());
    for (auto &piece : pieces) {
        start.pieces.push_back(std::move(piece.piece));
    }
    return start;
}

} // namespace refrain::worldgen::structure
