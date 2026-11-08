// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/structure/mansion_layout.hpp>

#include <array>
#include <utility>
#include <vector>

namespace refrain::worldgen::structure {

namespace {

constexpr jint k_room_1x1 = 65536;
constexpr jint k_room_1x2 = 131072;
constexpr jint k_room_2x2 = 262144;
constexpr jint k_room_origin = 1048576;
constexpr jint k_room_door = 2097152;
constexpr jint k_room_stairs = 4194304;
constexpr jint k_room_corridor = 8388608;
constexpr jint k_room_type_mask = 983040;
constexpr jint k_room_id_mask = 65535;

struct placement_data {
    block_pos position{};
    template_rotation rotation{template_rotation::none};
    std::string wall_type{};
};

auto rotation_index(template_rotation rotation) -> jint {
    return static_cast<jint>(rotation);
}

auto add_rotation(template_rotation base, template_rotation delta)
    -> template_rotation {
    return static_cast<template_rotation>(
        (rotation_index(base) + rotation_index(delta)) & 3);
}

void add_piece(std::vector<mansion_piece> &pieces,
               const std::string &template_name, const block_pos &origin,
               template_rotation rotation,
               template_mirror mirror = template_mirror::none) {
    pieces.push_back(mansion_piece{.template_name = template_name,
                                   .origin = origin,
                                   .rotation = rotation,
                                   .mirror = mirror});
}

void entrance(std::vector<mansion_piece> &pieces, placement_data &data) {
    add_piece(pieces, "entrance",
              mansion_relative(data.position, data.rotation,
                               mansion_direction::west, 9),
              data.rotation);
    data.position = mansion_relative(data.position, data.rotation,
                                     mansion_direction::south, 16);
}

void traverse_wall_piece(std::vector<mansion_piece> &pieces,
                         placement_data &data) {
    add_piece(pieces, data.wall_type,
              mansion_relative(data.position, data.rotation,
                               mansion_direction::east, 7),
              data.rotation);
    data.position = mansion_relative(data.position, data.rotation,
                                     mansion_direction::south, 8);
}

void traverse_turn(std::vector<mansion_piece> &pieces, placement_data &data) {
    data.position = mansion_relative(data.position, data.rotation,
                                     mansion_direction::south, -1);
    add_piece(pieces, "wall_corner", data.position, data.rotation);
    data.position = mansion_relative(data.position, data.rotation,
                                     mansion_direction::south, -7);
    data.position = mansion_relative(data.position, data.rotation,
                                     mansion_direction::west, -6);
    data.rotation =
        add_rotation(data.rotation, template_rotation::clockwise_90);
}

void traverse_inner_turn(std::vector<mansion_piece> &pieces,
                         placement_data &data) {
    (void)pieces;
    data.position = mansion_relative(data.position, data.rotation,
                                     mansion_direction::south, 6);
    data.position = mansion_relative(data.position, data.rotation,
                                     mansion_direction::east, 8);
    data.rotation =
        add_rotation(data.rotation, template_rotation::counterclockwise_90);
}

void traverse_outer_walls(std::vector<mansion_piece> &pieces,
                          placement_data &data, const mansion_simple_grid &grid,
                          mansion_direction grid_direction, jint start_x,
                          jint start_y, jint end_x, jint end_y) {
    jint grid_x = start_x;
    jint grid_y = start_y;
    auto direction = grid_direction;
    const auto start_direction = direction;
    const auto at_end = [&] {
        return grid_x == end_x && grid_y == end_y &&
               start_direction == direction;
    };
    do {
        const auto step_x = mansion_step_x(direction);
        const auto step_z = mansion_step_z(direction);
        if (!mansion_grid::is_house(grid, grid_x + step_x, grid_y + step_z)) {
            traverse_turn(pieces, data);
            direction = mansion_clockwise(direction);
            if (!at_end()) {
                traverse_wall_piece(pieces, data);
            }
        } else {
            const auto ccw = mansion_counter_clockwise(direction);
            if (mansion_grid::is_house(grid, grid_x + step_x,
                                       grid_y + step_z) &&
                mansion_grid::is_house(grid,
                                       grid_x + step_x + mansion_step_x(ccw),
                                       grid_y + step_z + mansion_step_z(ccw))) {
                traverse_inner_turn(pieces, data);
                grid_x += step_x;
                grid_y += step_z;
                direction = ccw;
            } else {
                grid_x += step_x;
                grid_y += step_z;
                if (!at_end()) {
                    traverse_wall_piece(pieces, data);
                }
            }
        }
    } while (!at_end());
}

void create_roof(std::vector<mansion_piece> &pieces,
                 const block_pos &roof_origin, template_rotation rotation,
                 const mansion_simple_grid &grid,
                 const mansion_simple_grid *above_grid, jint start_x,
                 jint start_y) {
    const auto roof_pos = [&](jint x, jint y) {
        auto position =
            mansion_relative(roof_origin, rotation, mansion_direction::south,
                             8 + (y - start_y) * 8);
        return mansion_relative(position, rotation, mansion_direction::east,
                                (x - start_x) * 8);
    };
    for (jint y = 0; y < grid.height(); ++y) {
        for (jint x = 0; x < grid.width(); ++x) {
            const auto position = roof_pos(x, y);
            const bool is_above = above_grid != nullptr &&
                                  mansion_grid::is_house(*above_grid, x, y);
            if (!mansion_grid::is_house(grid, x, y) || is_above) {
                continue;
            }
            add_piece(pieces, "roof",
                      block_pos{.x = position.x,
                                .y = position.y + 3,
                                .z = position.z},
                      rotation);
            if (!mansion_grid::is_house(grid, x + 1, y)) {
                add_piece(pieces, "roof_front",
                          mansion_relative(position, rotation,
                                           mansion_direction::east, 6),
                          rotation);
            }
            if (!mansion_grid::is_house(grid, x - 1, y)) {
                auto p = mansion_relative(position, rotation,
                                          mansion_direction::east, 0);
                p = mansion_relative(p, rotation, mansion_direction::south, 7);
                add_piece(
                    pieces, "roof_front", p,
                    add_rotation(rotation, template_rotation::clockwise_180));
            }
            if (!mansion_grid::is_house(grid, x, y - 1)) {
                add_piece(pieces, "roof_front",
                          mansion_relative(position, rotation,
                                           mansion_direction::west, 1),
                          add_rotation(rotation,
                                       template_rotation::counterclockwise_90));
            }
            if (!mansion_grid::is_house(grid, x, y + 1)) {
                auto p = mansion_relative(position, rotation,
                                          mansion_direction::east, 6);
                p = mansion_relative(p, rotation, mansion_direction::south, 6);
                add_piece(
                    pieces, "roof_front", p,
                    add_rotation(rotation, template_rotation::clockwise_90));
            }
        }
    }
    if (above_grid != nullptr) {
        for (jint y = 0; y < grid.height(); ++y) {
            for (jint x = 0; x < grid.width(); ++x) {
                const auto position = roof_pos(x, y);
                if (!mansion_grid::is_house(grid, x, y) ||
                    !mansion_grid::is_house(*above_grid, x, y)) {
                    continue;
                }
                if (!mansion_grid::is_house(grid, x + 1, y)) {
                    add_piece(pieces, "small_wall",
                              mansion_relative(position, rotation,
                                               mansion_direction::east, 7),
                              rotation);
                }
                if (!mansion_grid::is_house(grid, x - 1, y)) {
                    auto p = mansion_relative(position, rotation,
                                              mansion_direction::west, 1);
                    p = mansion_relative(p, rotation, mansion_direction::south,
                                         6);
                    add_piece(pieces, "small_wall", p,
                              add_rotation(rotation,
                                           template_rotation::clockwise_180));
                }
                if (!mansion_grid::is_house(grid, x, y - 1)) {
                    auto p = mansion_relative(position, rotation,
                                              mansion_direction::west, 0);
                    p = mansion_relative(p, rotation, mansion_direction::north,
                                         1);
                    add_piece(
                        pieces, "small_wall", p,
                        add_rotation(rotation,
                                     template_rotation::counterclockwise_90));
                }
                if (!mansion_grid::is_house(grid, x, y + 1)) {
                    auto p = mansion_relative(position, rotation,
                                              mansion_direction::east, 6);
                    p = mansion_relative(p, rotation, mansion_direction::south,
                                         7);
                    add_piece(pieces, "small_wall", p,
                              add_rotation(rotation,
                                           template_rotation::clockwise_90));
                }
                if (!mansion_grid::is_house(grid, x + 1, y)) {
                    if (!mansion_grid::is_house(grid, x, y - 1)) {
                        auto p = mansion_relative(position, rotation,
                                                  mansion_direction::east, 7);
                        p = mansion_relative(p, rotation,
                                             mansion_direction::north, 2);
                        add_piece(pieces, "small_wall_corner", p, rotation);
                    }
                    if (!mansion_grid::is_house(grid, x, y + 1)) {
                        auto p = mansion_relative(position, rotation,
                                                  mansion_direction::east, 8);
                        p = mansion_relative(p, rotation,
                                             mansion_direction::south, 7);
                        add_piece(
                            pieces, "small_wall_corner", p,
                            add_rotation(rotation,
                                         template_rotation::clockwise_90));
                    }
                }
                if (!mansion_grid::is_house(grid, x - 1, y)) {
                    if (!mansion_grid::is_house(grid, x, y - 1)) {
                        auto p = mansion_relative(position, rotation,
                                                  mansion_direction::west, 2);
                        p = mansion_relative(p, rotation,
                                             mansion_direction::north, 1);
                        add_piece(pieces, "small_wall_corner", p,
                                  add_rotation(
                                      rotation,
                                      template_rotation::counterclockwise_90));
                    }
                    if (!mansion_grid::is_house(grid, x, y + 1)) {
                        auto p = mansion_relative(position, rotation,
                                                  mansion_direction::west, 1);
                        p = mansion_relative(p, rotation,
                                             mansion_direction::south, 8);
                        add_piece(
                            pieces, "small_wall_corner", p,
                            add_rotation(rotation,
                                         template_rotation::clockwise_180));
                    }
                }
            }
        }
    }
    for (jint y = 0; y < grid.height(); ++y) {
        for (jint x = 0; x < grid.width(); ++x) {
            const auto position = roof_pos(x, y);
            const bool is_above = above_grid != nullptr &&
                                  mansion_grid::is_house(*above_grid, x, y);
            if (!mansion_grid::is_house(grid, x, y) || is_above) {
                continue;
            }
            if (!mansion_grid::is_house(grid, x + 1, y)) {
                const auto p = mansion_relative(position, rotation,
                                                mansion_direction::east, 6);
                if (!mansion_grid::is_house(grid, x, y + 1)) {
                    add_piece(pieces, "roof_corner",
                              mansion_relative(p, rotation,
                                               mansion_direction::south, 6),
                              rotation);
                } else if (mansion_grid::is_house(grid, x + 1, y + 1)) {
                    add_piece(pieces, "roof_inner_corner",
                              mansion_relative(p, rotation,
                                               mansion_direction::south, 5),
                              rotation);
                }
                if (!mansion_grid::is_house(grid, x, y - 1)) {
                    add_piece(
                        pieces, "roof_corner", p,
                        add_rotation(rotation,
                                     template_rotation::counterclockwise_90));
                } else if (mansion_grid::is_house(grid, x + 1, y - 1)) {
                    auto q = mansion_relative(position, rotation,
                                              mansion_direction::east, 9);
                    q = mansion_relative(q, rotation, mansion_direction::north,
                                         2);
                    add_piece(pieces, "roof_inner_corner", q,
                              add_rotation(rotation,
                                           template_rotation::clockwise_90));
                }
            }
            if (!mansion_grid::is_house(grid, x - 1, y)) {
                const auto p = mansion_relative(position, rotation,
                                                mansion_direction::east, 0);
                if (!mansion_grid::is_house(grid, x, y + 1)) {
                    add_piece(pieces, "roof_corner",
                              mansion_relative(p, rotation,
                                               mansion_direction::south, 6),
                              add_rotation(rotation,
                                           template_rotation::clockwise_90));
                } else if (mansion_grid::is_house(grid, x - 1, y + 1)) {
                    auto q = mansion_relative(p, rotation,
                                              mansion_direction::south, 8);
                    q = mansion_relative(q, rotation, mansion_direction::west,
                                         3);
                    add_piece(
                        pieces, "roof_inner_corner", q,
                        add_rotation(rotation,
                                     template_rotation::counterclockwise_90));
                }
                if (!mansion_grid::is_house(grid, x, y - 1)) {
                    add_piece(pieces, "roof_corner", p,
                              add_rotation(rotation,
                                           template_rotation::clockwise_180));
                } else if (mansion_grid::is_house(grid, x - 1, y - 1)) {
                    add_piece(pieces, "roof_inner_corner",
                              mansion_relative(p, rotation,
                                               mansion_direction::south, 1),
                              add_rotation(rotation,
                                           template_rotation::clockwise_180));
                }
            }
        }
    }
}

} // namespace

auto generate_mansion(worldgen_random &random, const block_pos &origin,
                      template_rotation rotation, const mansion_grid &grid)
    -> std::vector<mansion_piece> {
    (void)random;
    std::vector<mansion_piece> pieces;
    placement_data data{
        .position = origin, .rotation = rotation, .wall_type = "wall_flat"};
    placement_data second{
        .position = block_pos{.x = origin.x, .y = origin.y + 8, .z = origin.z},
        .rotation = rotation,
        .wall_type = "wall_window"};
    entrance(pieces, data);
    second.position = block_pos{
        .x = data.position.x, .y = data.position.y + 8, .z = data.position.z};
    second.rotation = data.rotation;
    const auto &base = grid.base_grid();
    const auto &third = grid.third_floor_grid();
    const jint start_x = grid.entrance_x() + 1;
    const jint start_y = grid.entrance_y() + 1;
    traverse_outer_walls(pieces, data, base, mansion_direction::south, start_x,
                         start_y, grid.entrance_x() + 1, grid.entrance_y());
    traverse_outer_walls(pieces, second, base, mansion_direction::south,
                         start_x, start_y, grid.entrance_x() + 1,
                         grid.entrance_y());
    placement_data third_data{
        .position = block_pos{.x = origin.x, .y = origin.y + 19, .z = origin.z},
        .rotation = rotation,
        .wall_type = "wall_window"};
    bool done = false;
    for (jint y = 0; y < third.height() && !done; ++y) {
        for (jint x = third.width() - 1; x >= 0 && !done; --x) {
            if (!mansion_grid::is_house(third, x, y)) {
                continue;
            }
            third_data.position =
                block_pos{.x = origin.x, .y = origin.y + 19, .z = origin.z};
            third_data.position = mansion_relative(
                third_data.position, rotation, mansion_direction::south,
                8 + (y - start_y) * 8);
            third_data.position =
                mansion_relative(third_data.position, rotation,
                                 mansion_direction::east, (x - start_x) * 8);
            traverse_wall_piece(pieces, third_data);
            traverse_outer_walls(pieces, third_data, third,
                                 mansion_direction::south, x, y, x, y);
            done = true;
        }
    }
    create_roof(pieces,
                block_pos{.x = origin.x, .y = origin.y + 16, .z = origin.z},
                rotation, base, &third, start_x, start_y);
    create_roof(pieces,
                block_pos{.x = origin.x, .y = origin.y + 27, .z = origin.z},
                rotation, third, nullptr, start_x, start_y);
    return pieces;
}

} // namespace refrain::worldgen::structure
