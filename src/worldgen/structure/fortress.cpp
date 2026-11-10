// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Nether fortress generation. The start is a bridge crossing at generation
// height 64; every section asks a weighted table for one of the bridge or
// castle pieces, tries up to five draws, and falls back to a ragged bridge end
// when no candidate fits. After the graph is built the whole start is moved
// into the allowed height band.

#include <refrain/worldgen/structure/fortress.hpp>

#include <refrain/worldgen/core/rng.hpp>
#include <refrain/worldgen/structure/generation.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace refrain::worldgen::structure {

namespace {

using fortress_piece_kind::bridge_crossing;
using fortress_piece_kind::bridge_end_filler;
using fortress_piece_kind::bridge_straight;
using fortress_piece_kind::castle_corridor_stairs;
using fortress_piece_kind::castle_corridor_t_balcony;
using fortress_piece_kind::castle_entrance;
using fortress_piece_kind::castle_small_corridor;
using fortress_piece_kind::castle_small_corridor_crossing;
using fortress_piece_kind::castle_small_corridor_left_turn;
using fortress_piece_kind::castle_small_corridor_right_turn;
using fortress_piece_kind::castle_stalk_room;
using fortress_piece_kind::monster_throne;
using fortress_piece_kind::room_crossing;
using fortress_piece_kind::stairs_room;

constexpr jint max_depth = 30;
constexpr jint lowest_y_position = 10;
constexpr jint magic_start_y = 64;
constexpr jint lowest_move_y = 48;
constexpr jint highest_move_y = 70;
constexpr jint max_horizontal_distance = 112;

// Horizontal orientations stored as 2D direction indices.
constexpr jint orientation_south = 0;
constexpr jint orientation_west = 1;
constexpr jint orientation_north = 2;
constexpr jint orientation_east = 3;

auto random_horizontal(worldgen_random &random) -> jint {
    static constexpr std::array<jint, 4> orientations{
        orientation_north, orientation_east, orientation_south,
        orientation_west};
    return orientations[static_cast<std::size_t>(random.next_int(4))];
}

auto make_bounding_box(jint west, jint floor, jint north, jint orientation,
                       jint width, jint height, jint depth) -> structure_bbox {
    const bool along_z =
        orientation == orientation_north || orientation == orientation_south;
    return structure_bbox{
        .min_x = west,
        .min_y = floor,
        .min_z = north,
        .max_x = west + (along_z ? width : depth) - 1,
        .max_y = floor + height - 1,
        .max_z = north + (along_z ? depth : width) - 1,
    };
}

auto oriented_box(jint foot_x, jint foot_y, jint foot_z, jint off_x, jint off_y,
                  jint off_z, jint width, jint height, jint depth,
                  jint orientation) -> structure_bbox {
    switch (orientation) {
    case orientation_north:
        return structure_bbox{
            .min_x = foot_x + off_x,
            .min_y = foot_y + off_y,
            .min_z = foot_z - depth + 1 + off_z,
            .max_x = foot_x + width - 1 + off_x,
            .max_y = foot_y + height - 1 + off_y,
            .max_z = foot_z + off_z,
        };
    case orientation_west:
        return structure_bbox{
            .min_x = foot_x - depth + 1 + off_z,
            .min_y = foot_y + off_y,
            .min_z = foot_z + off_x,
            .max_x = foot_x + off_z,
            .max_y = foot_y + height - 1 + off_y,
            .max_z = foot_z + width - 1 + off_x,
        };
    case orientation_east:
        return structure_bbox{
            .min_x = foot_x + off_z,
            .min_y = foot_y + off_y,
            .min_z = foot_z + off_x,
            .max_x = foot_x + depth - 1 + off_z,
            .max_y = foot_y + height - 1 + off_y,
            .max_z = foot_z + width - 1 + off_x,
        };
    default: // south
        return structure_bbox{
            .min_x = foot_x + off_x,
            .min_y = foot_y + off_y,
            .min_z = foot_z + off_z,
            .max_x = foot_x + width - 1 + off_x,
            .max_y = foot_y + height - 1 + off_y,
            .max_z = foot_z + depth - 1 + off_z,
        };
    }
}

auto intersects(const structure_bbox &left, const structure_bbox &right)
    -> bool {
    return left.intersects(right);
}

struct bridge_weight {
    fortress_piece_kind kind;
    jint weight;
    jint max_place_count;
    bool allow_in_row;
    jint place_count{0};
};

constexpr std::array<bridge_weight, 6> bridge_piece_weights{{
    {.kind = bridge_straight,
     .weight = 30,
     .max_place_count = 0,
     .allow_in_row = true},
    {.kind = bridge_crossing,
     .weight = 10,
     .max_place_count = 4,
     .allow_in_row = false},
    {.kind = room_crossing,
     .weight = 10,
     .max_place_count = 4,
     .allow_in_row = false},
    {.kind = stairs_room,
     .weight = 10,
     .max_place_count = 3,
     .allow_in_row = false},
    {.kind = monster_throne,
     .weight = 5,
     .max_place_count = 2,
     .allow_in_row = false},
    {.kind = castle_entrance,
     .weight = 5,
     .max_place_count = 1,
     .allow_in_row = false},
}};

constexpr std::array<bridge_weight, 7> castle_piece_weights{{
    {.kind = castle_small_corridor,
     .weight = 25,
     .max_place_count = 0,
     .allow_in_row = true},
    {.kind = castle_small_corridor_crossing,
     .weight = 15,
     .max_place_count = 5,
     .allow_in_row = false},
    {.kind = castle_small_corridor_right_turn,
     .weight = 5,
     .max_place_count = 10,
     .allow_in_row = false},
    {.kind = castle_small_corridor_left_turn,
     .weight = 5,
     .max_place_count = 10,
     .allow_in_row = false},
    {.kind = castle_corridor_stairs,
     .weight = 10,
     .max_place_count = 3,
     .allow_in_row = true},
    {.kind = castle_corridor_t_balcony,
     .weight = 7,
     .max_place_count = 2,
     .allow_in_row = false},
    {.kind = castle_stalk_room,
     .weight = 5,
     .max_place_count = 2,
     .allow_in_row = false},
}};

struct fortress_piece_entry {
    structure_piece piece;
    jint depth{0};
};

struct fortress_state {
    worldgen_random *random{nullptr};
    structure_bbox start_box{};
    std::vector<bridge_weight> bridge_weights{};
    std::vector<bridge_weight> castle_weights{};
    std::optional<fortress_piece_kind> previous_piece{};
    std::vector<fortress_piece_entry> pieces{};
    std::vector<std::size_t> pending{};
};

auto find_collision(const fortress_state &state, const structure_bbox &box)
    -> const fortress_piece_entry * {
    for (const auto &piece : state.pieces) {
        if (intersects(piece.piece.bounds, box)) {
            return &piece;
        }
    }
    return nullptr;
}

auto is_ok_box(const structure_bbox &box) -> bool {
    return box.min_y > lowest_y_position;
}

auto create_piece(fortress_state &state, fortress_piece_kind kind,
                  const block_pos &foot, jint orientation, jint depth)
    -> std::optional<fortress_piece_entry> {
    structure_bbox box{};
    switch (kind) {
    case bridge_crossing:
        box = oriented_box(foot.x, foot.y, foot.z, -8, -3, 0, 19, 10, 19,
                           orientation);
        break;
    case bridge_end_filler:
        box = oriented_box(foot.x, foot.y, foot.z, -1, -3, 0, 5, 10, 8,
                           orientation);
        break;
    case bridge_straight:
        box = oriented_box(foot.x, foot.y, foot.z, -1, -3, 0, 5, 10, 19,
                           orientation);
        break;
    case castle_corridor_stairs:
        box = oriented_box(foot.x, foot.y, foot.z, -1, -7, 0, 5, 14, 10,
                           orientation);
        break;
    case castle_corridor_t_balcony:
        box = oriented_box(foot.x, foot.y, foot.z, -3, 0, 0, 9, 7, 9,
                           orientation);
        break;
    case castle_entrance:
    case castle_stalk_room:
        box = oriented_box(foot.x, foot.y, foot.z, -5, -3, 0, 13, 14, 13,
                           orientation);
        break;
    case castle_small_corridor:
    case castle_small_corridor_crossing:
    case castle_small_corridor_left_turn:
    case castle_small_corridor_right_turn:
        box = oriented_box(foot.x, foot.y, foot.z, -1, 0, 0, 5, 7, 5,
                           orientation);
        break;
    case monster_throne:
        box = oriented_box(foot.x, foot.y, foot.z, -2, 0, 0, 7, 8, 9,
                           orientation);
        break;
    case room_crossing:
        box = oriented_box(foot.x, foot.y, foot.z, -2, 0, 0, 7, 9, 7,
                           orientation);
        break;
    case stairs_room:
        box = oriented_box(foot.x, foot.y, foot.z, -2, 0, 0, 7, 11, 7,
                           orientation);
        break;
    }
    if (!is_ok_box(box) || find_collision(state, box) != nullptr) {
        return std::nullopt;
    }
    fortress_piece_data data{.kind = kind, .self_seed = 0, .chest = false};
    if (kind == bridge_end_filler) {
        data.self_seed = state.random->next_int();
    } else if (kind == castle_small_corridor_left_turn ||
               kind == castle_small_corridor_right_turn) {
        data.chest = state.random->next_int(3) == 0;
    }
    structure_piece piece{
        .type = "minecraft:fortress",
        .bounds = box,
        .orientation = orientation,
        .fortress = data,
    };
    return fortress_piece_entry{.piece = std::move(piece), .depth = depth};
}

auto update_piece_weight(const std::vector<bridge_weight> &current) -> jint {
    bool has_any = false;
    jint total = 0;
    for (const auto &piece : current) {
        if (piece.max_place_count > 0 &&
            piece.place_count < piece.max_place_count) {
            has_any = true;
        }
        total += piece.weight;
    }
    return has_any ? total : -1;
}

auto do_place(const bridge_weight &piece, jint depth) -> bool {
    (void)depth;
    return piece.max_place_count == 0 ||
           piece.place_count < piece.max_place_count;
}

auto is_valid(const bridge_weight &piece) -> bool {
    return piece.max_place_count == 0 ||
           piece.place_count < piece.max_place_count;
}

auto generate_piece(fortress_state &state, std::vector<bridge_weight> &current,
                    const block_pos &foot, jint orientation, jint depth)
    -> std::optional<fortress_piece_entry> {
    const jint total = update_piece_weight(current);
    const bool do_stuff = total > 0 && depth <= max_depth;
    jint attempts = 0;
    while (attempts < 5 && do_stuff) {
        ++attempts;
        jint selection = state.random->next_int(total);
        for (std::size_t index = 0; index < current.size(); ++index) {
            auto &piece = current[index];
            selection -= piece.weight;
            if (selection >= 0) {
                continue;
            }
            if (!do_place(piece, depth) ||
                (state.previous_piece.has_value() &&
                 *state.previous_piece == piece.kind && !piece.allow_in_row)) {
                break;
            }
            auto created =
                create_piece(state, piece.kind, foot, orientation, depth);
            if (created.has_value()) {
                ++piece.place_count;
                state.previous_piece = piece.kind;
                if (!is_valid(piece)) {
                    current.erase(current.begin() +
                                  static_cast<std::ptrdiff_t>(index));
                }
                return created;
            }
        }
    }
    return create_piece(state, bridge_end_filler, foot, orientation, depth);
}

auto add_piece(fortress_state &state, fortress_piece_entry piece)
    -> std::size_t {
    const auto index = state.pieces.size();
    state.pieces.push_back(std::move(piece));
    state.pending.push_back(index);
    return index;
}

auto generate_and_add(fortress_state &state, jint foot_x, jint foot_y,
                      jint foot_z, jint orientation, jint depth, bool castle)
    -> std::optional<std::size_t> {
    if (std::abs(foot_x - state.start_box.min_x) <= max_horizontal_distance &&
        std::abs(foot_z - state.start_box.min_z) <= max_horizontal_distance) {
        auto &available = castle ? state.castle_weights : state.bridge_weights;
        auto created = generate_piece(
            state, available, block_pos{.x = foot_x, .y = foot_y, .z = foot_z},
            orientation, depth + 1);
        if (created.has_value()) {
            return add_piece(state, std::move(*created));
        }
        return std::nullopt;
    }
    // A section outside the allowed horizontal distance ends in a ragged
    // bridge end that is not part of the start.
    (void)create_piece(state, bridge_end_filler,
                       block_pos{.x = foot_x, .y = foot_y, .z = foot_z},
                       orientation, depth);
    return std::nullopt;
}

auto generate_child_forward(fortress_state &state,
                            const fortress_piece_entry &piece, jint x_off,
                            jint y_off, bool castle)
    -> std::optional<std::size_t> {
    const auto &box = piece.piece.bounds;
    switch (piece.piece.orientation) {
    case orientation_north:
        return generate_and_add(state, box.min_x + x_off, box.min_y + y_off,
                                box.min_z - 1, orientation_north, piece.depth,
                                castle);
    case orientation_south:
        return generate_and_add(state, box.min_x + x_off, box.min_y + y_off,
                                box.max_z + 1, orientation_south, piece.depth,
                                castle);
    case orientation_west:
        return generate_and_add(state, box.min_x - 1, box.min_y + y_off,
                                box.min_z + x_off, orientation_west,
                                piece.depth, castle);
    case orientation_east:
        return generate_and_add(state, box.max_x + 1, box.min_y + y_off,
                                box.min_z + x_off, orientation_east,
                                piece.depth, castle);
    default:
        return std::nullopt;
    }
}

auto generate_child_left(fortress_state &state,
                         const fortress_piece_entry &piece, jint y_off,
                         jint z_off, bool castle)
    -> std::optional<std::size_t> {
    const auto &box = piece.piece.bounds;
    switch (piece.piece.orientation) {
    case orientation_north:
    case orientation_south:
        return generate_and_add(state, box.min_x - 1, box.min_y + y_off,
                                box.min_z + z_off, orientation_west,
                                piece.depth, castle);
    case orientation_west:
    case orientation_east:
        return generate_and_add(state, box.min_x + z_off, box.min_y + y_off,
                                box.min_z - 1, orientation_north, piece.depth,
                                castle);
    default:
        return std::nullopt;
    }
}

auto generate_child_right(fortress_state &state,
                          const fortress_piece_entry &piece, jint y_off,
                          jint z_off, bool castle)
    -> std::optional<std::size_t> {
    const auto &box = piece.piece.bounds;
    switch (piece.piece.orientation) {
    case orientation_north:
    case orientation_south:
        return generate_and_add(state, box.max_x + 1, box.min_y + y_off,
                                box.min_z + z_off, orientation_east,
                                piece.depth, castle);
    case orientation_west:
    case orientation_east:
        return generate_and_add(state, box.min_x + z_off, box.min_y + y_off,
                                box.max_z + 1, orientation_south, piece.depth,
                                castle);
    default:
        return std::nullopt;
    }
}

void add_children(fortress_state &state, std::size_t index) {
    const auto kind = state.pieces[index].piece.fortress->kind;
    switch (kind) {
    case bridge_crossing:
        (void)generate_child_forward(state, state.pieces[index], 8, 3, false);
        (void)generate_child_left(state, state.pieces[index], 3, 8, false);
        (void)generate_child_right(state, state.pieces[index], 3, 8, false);
        break;
    case bridge_end_filler:
    case monster_throne:
        break;
    case bridge_straight:
        (void)generate_child_forward(state, state.pieces[index], 1, 3, false);
        break;
    case castle_corridor_stairs:
        (void)generate_child_forward(state, state.pieces[index], 1, 0, true);
        break;
    case castle_corridor_t_balcony: {
        const jint z_off =
            state.pieces[index].piece.orientation == orientation_west ||
                    state.pieces[index].piece.orientation == orientation_north
                ? 5
                : 1;
        const bool left_castle = state.random->next_int(8) > 0;
        (void)generate_child_left(state, state.pieces[index], 0, z_off,
                                  left_castle);
        const bool right_castle = state.random->next_int(8) > 0;
        (void)generate_child_right(state, state.pieces[index], 0, z_off,
                                   right_castle);
        break;
    }
    case castle_entrance:
        (void)generate_child_forward(state, state.pieces[index], 5, 3, true);
        break;
    case castle_small_corridor:
        (void)generate_child_forward(state, state.pieces[index], 1, 0, true);
        break;
    case castle_small_corridor_crossing:
        (void)generate_child_forward(state, state.pieces[index], 1, 0, true);
        (void)generate_child_left(state, state.pieces[index], 0, 1, true);
        (void)generate_child_right(state, state.pieces[index], 0, 1, true);
        break;
    case castle_small_corridor_left_turn:
        (void)generate_child_left(state, state.pieces[index], 0, 1, true);
        break;
    case castle_small_corridor_right_turn:
        (void)generate_child_right(state, state.pieces[index], 0, 1, true);
        break;
    case castle_stalk_room:
        (void)generate_child_forward(state, state.pieces[index], 5, 3, true);
        (void)generate_child_forward(state, state.pieces[index], 5, 11, true);
        break;
    case room_crossing:
        (void)generate_child_forward(state, state.pieces[index], 2, 0, false);
        (void)generate_child_left(state, state.pieces[index], 0, 2, false);
        (void)generate_child_right(state, state.pieces[index], 0, 2, false);
        break;
    case stairs_room:
        (void)generate_child_right(state, state.pieces[index], 6, 2, false);
        break;
    }
}

auto union_bounds(const std::vector<fortress_piece_entry> &pieces)
    -> structure_bbox {
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

auto fortress_start(const chunk_pos &chunk, jlong seed) -> structure_start {
    auto random = worldgen_random{std::make_unique<legacy_random_source>(0)};
    random.set_large_feature_seed(seed, chunk.x, chunk.z);
    const jint orientation = random_horizontal(random);
    fortress_state state{
        .random = &random,
        .start_box =
            make_bounding_box(chunk.x * 16 + 2, magic_start_y, chunk.z * 16 + 2,
                              orientation, 19, 10, 19),
        .bridge_weights = std::vector<bridge_weight>(
            bridge_piece_weights.begin(), bridge_piece_weights.end()),
        .castle_weights = std::vector<bridge_weight>(
            castle_piece_weights.begin(), castle_piece_weights.end()),
    };
    structure_piece start_piece{
        .type = "minecraft:fortress",
        .bounds = state.start_box,
        .orientation = orientation,
        .fortress = fortress_piece_data{.kind = bridge_crossing},
    };
    state.pieces.push_back(
        fortress_piece_entry{.piece = std::move(start_piece), .depth = 0});
    add_children(state, 0);
    while (!state.pending.empty()) {
        const auto index = static_cast<std::size_t>(
            random.next_int(static_cast<jint>(state.pending.size())));
        const auto piece_index = state.pending[index];
        state.pending.erase(state.pending.begin() +
                            static_cast<std::ptrdiff_t>(index));
        add_children(state, piece_index);
    }

    auto bounds = union_bounds(state.pieces);
    const jint height_span =
        highest_move_y - lowest_move_y + 1 - (bounds.max_y - bounds.min_y + 1);
    jint y0 = lowest_move_y;
    if (height_span > 1) {
        y0 += random.next_int(height_span);
    }
    const jint dy = y0 - bounds.min_y;
    for (auto &piece : state.pieces) {
        piece.piece.bounds.min_y += dy;
        piece.piece.bounds.max_y += dy;
    }
    bounds = union_bounds(state.pieces);

    structure_start start{
        .structure = "minecraft:fortress",
        .chunk = chunk,
        .references = 0,
        .max_references = 1,
        .bounds = bounds,
    };
    start.pieces.reserve(state.pieces.size());
    for (auto &entry : state.pieces) {
        start.pieces.push_back(std::move(entry.piece));
    }
    return start;
}

} // namespace refrain::worldgen::structure
