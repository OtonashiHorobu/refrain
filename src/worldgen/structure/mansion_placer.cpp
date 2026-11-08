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

struct door_dir {
    std::optional<mansion_direction> direction{};
    bool up{false};
};

auto is_door(const door_dir &door, mansion_direction direction) -> bool {
    return door.direction.has_value() && *door.direction == direction;
}

void add_room_1x1(std::vector<mansion_piece> &pieces, const block_pos &room_pos,
                  template_rotation rotation, const door_dir &door,
                  const mansion_room_collection &rooms,
                  worldgen_random &random) {
    auto piece_rotation = template_rotation::none;
    auto name = rooms.room_1x1(random);
    if (!is_door(door, mansion_direction::east)) {
        if (is_door(door, mansion_direction::north)) {
            piece_rotation = template_rotation::counterclockwise_90;
        } else if (is_door(door, mansion_direction::west)) {
            piece_rotation = template_rotation::clockwise_180;
        } else if (is_door(door, mansion_direction::south)) {
            piece_rotation = template_rotation::clockwise_90;
        } else {
            name = rooms.room_1x1_secret(random);
        }
    }
    const auto zero =
        mansion_zero_position(block_pos{.x = 1, .y = 0, .z = 0},
                              template_mirror::none, piece_rotation, 7, 7);
    const auto orientation = mansion_rotate_2d(rotation, zero.x, zero.z);
    const auto position = block_pos{.x = room_pos.x + orientation.x,
                                    .y = room_pos.y,
                                    .z = room_pos.z + orientation.z};
    add_piece(pieces, name, position, add_rotation(piece_rotation, rotation));
}

void add_room_1x2(std::vector<mansion_piece> &pieces, const block_pos &room_pos,
                  template_rotation rotation, mansion_direction room_direction,
                  const door_dir &door, const mansion_room_collection &rooms,
                  bool stairs_room, worldgen_random &random) {
    const auto east = [&](jint amount) {
        return mansion_relative(room_pos, rotation, mansion_direction::east,
                                amount);
    };
    const auto south = [&](const block_pos &pos, jint amount) {
        return mansion_relative(pos, rotation, mansion_direction::south,
                                amount);
    };
    const auto north = [&](const block_pos &pos, jint amount) {
        return mansion_relative(pos, rotation, mansion_direction::north,
                                amount);
    };
    const auto west = [&](const block_pos &pos, jint amount) {
        return mansion_relative(pos, rotation, mansion_direction::west, amount);
    };
    const auto side_name = [&] {
        return rooms.room_1x2_side(random, stairs_room);
    };
    const auto front_name = [&] {
        return rooms.room_1x2_front(random, stairs_room);
    };
    if (is_door(door, mansion_direction::east) &&
        room_direction == mansion_direction::south) {
        add_piece(pieces, side_name(), east(1), rotation);
    } else if (is_door(door, mansion_direction::east) &&
               room_direction == mansion_direction::north) {
        add_piece(pieces, side_name(), south(east(1), 6), rotation,
                  template_mirror::left_right);
    } else if (is_door(door, mansion_direction::west) &&
               room_direction == mansion_direction::north) {
        add_piece(pieces, side_name(), south(east(7), 6),
                  add_rotation(rotation, template_rotation::clockwise_180));
    } else if (is_door(door, mansion_direction::west) &&
               room_direction == mansion_direction::south) {
        add_piece(pieces, side_name(), east(7), rotation,
                  template_mirror::front_back);
    } else if (is_door(door, mansion_direction::south) &&
               room_direction == mansion_direction::east) {
        add_piece(pieces, side_name(), east(1),
                  add_rotation(rotation, template_rotation::clockwise_90),
                  template_mirror::left_right);
    } else if (is_door(door, mansion_direction::south) &&
               room_direction == mansion_direction::west) {
        add_piece(pieces, side_name(), east(7),
                  add_rotation(rotation, template_rotation::clockwise_90));
    } else if (is_door(door, mansion_direction::north) &&
               room_direction == mansion_direction::west) {
        add_piece(pieces, side_name(), south(east(7), 6),
                  add_rotation(rotation, template_rotation::clockwise_90),
                  template_mirror::front_back);
    } else if (is_door(door, mansion_direction::north) &&
               room_direction == mansion_direction::east) {
        add_piece(
            pieces, side_name(), south(east(1), 6),
            add_rotation(rotation, template_rotation::counterclockwise_90));
    } else if (is_door(door, mansion_direction::south) &&
               room_direction == mansion_direction::north) {
        add_piece(pieces, front_name(), north(east(1), 8), rotation);
    } else if (is_door(door, mansion_direction::north) &&
               room_direction == mansion_direction::south) {
        add_piece(pieces, front_name(), south(east(7), 14),
                  add_rotation(rotation, template_rotation::clockwise_180));
    } else if (is_door(door, mansion_direction::west) &&
               room_direction == mansion_direction::east) {
        add_piece(pieces, front_name(), east(15),
                  add_rotation(rotation, template_rotation::clockwise_90));
    } else if (is_door(door, mansion_direction::east) &&
               room_direction == mansion_direction::west) {
        add_piece(
            pieces, front_name(), south(west(east(0), 7), 6),
            add_rotation(rotation, template_rotation::counterclockwise_90));
    } else if (door.up && room_direction == mansion_direction::east) {
        add_piece(pieces, rooms.room_1x2_secret(random), east(15),
                  add_rotation(rotation, template_rotation::clockwise_90));
    } else if (door.up && room_direction == mansion_direction::south) {
        add_piece(pieces, rooms.room_1x2_secret(random), north(east(1), 0),
                  rotation);
    }
}

void add_room_2x2(std::vector<mansion_piece> &pieces, const block_pos &room_pos,
                  template_rotation rotation, mansion_direction room_direction,
                  const door_dir &door, const mansion_room_collection &rooms,
                  worldgen_random &random) {
    jint east = 0;
    jint south_offset = 0;
    auto rot = rotation;
    auto mirror = template_mirror::none;
    if (is_door(door, mansion_direction::east) &&
        room_direction == mansion_direction::south) {
        east = -7;
    } else if (is_door(door, mansion_direction::east) &&
               room_direction == mansion_direction::north) {
        east = -7;
        south_offset = 6;
        mirror = template_mirror::left_right;
    } else if (is_door(door, mansion_direction::north) &&
               room_direction == mansion_direction::east) {
        east = 1;
        south_offset = 14;
        rot = add_rotation(rotation, template_rotation::counterclockwise_90);
    } else if (is_door(door, mansion_direction::north) &&
               room_direction == mansion_direction::west) {
        east = 7;
        south_offset = 14;
        rot = add_rotation(rotation, template_rotation::counterclockwise_90);
        mirror = template_mirror::left_right;
    } else if (is_door(door, mansion_direction::south) &&
               room_direction == mansion_direction::west) {
        east = 7;
        south_offset = -8;
        rot = add_rotation(rotation, template_rotation::clockwise_90);
    } else if (is_door(door, mansion_direction::south) &&
               room_direction == mansion_direction::east) {
        east = 1;
        south_offset = -8;
        rot = add_rotation(rotation, template_rotation::clockwise_90);
        mirror = template_mirror::left_right;
    } else if (is_door(door, mansion_direction::west) &&
               room_direction == mansion_direction::north) {
        east = 15;
        south_offset = 6;
        rot = add_rotation(rotation, template_rotation::clockwise_180);
    } else if (is_door(door, mansion_direction::west) &&
               room_direction == mansion_direction::south) {
        east = 15;
        mirror = template_mirror::front_back;
    }
    auto position =
        mansion_relative(room_pos, rotation, mansion_direction::east, east);
    position = mansion_relative(position, rotation, mansion_direction::south,
                                south_offset);
    add_piece(pieces, rooms.room_2x2(random), position, rot, mirror);
}

void add_room_2x2_secret(std::vector<mansion_piece> &pieces,
                         const block_pos &room_pos, template_rotation rotation,
                         const mansion_room_collection &rooms,
                         worldgen_random &random) {
    add_piece(pieces, rooms.room_2x2_secret(random),
              mansion_relative(room_pos, rotation, mansion_direction::east, 1),
              rotation);
}

} // namespace

auto generate_mansion(worldgen_random &random, const block_pos &origin,
                      template_rotation rotation, const mansion_grid &grid)
    -> std::vector<mansion_piece> {
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

    const std::array<mansion_room_collection, 3> collections{
        mansion_room_collection{0}, mansion_room_collection{1},
        mansion_room_collection{2}};
    for (jint floor = 0; floor < 3; ++floor) {
        const block_pos floor_origin{.x = origin.x,
                                     .y = origin.y + 8 * floor +
                                          (floor == 2 ? 3 : 0),
                                     .z = origin.z};
        const auto &room_grid = grid.floor_rooms(floor);
        const auto &layout_grid = floor == 2 ? third : base;
        const std::string south_piece =
            floor == 0 ? "carpet_south_1" : "carpet_south_2";
        const std::string west_piece =
            floor == 0 ? "carpet_west_1" : "carpet_west_2";

        for (jint y = 0; y < layout_grid.height(); ++y) {
            for (jint x = 0; x < layout_grid.width(); ++x) {
                if (layout_grid.get(x, y) != 1) {
                    continue;
                }
                auto pos = mansion_relative(floor_origin, rotation,
                                            mansion_direction::south,
                                            8 + (y - start_y) * 8);
                pos = mansion_relative(pos, rotation, mansion_direction::east,
                                       (x - start_x) * 8);
                add_piece(pieces, "corridor_floor", pos, rotation);
                const auto above =
                    block_pos{.x = pos.x, .y = pos.y + 1, .z = pos.z};
                if (layout_grid.get(x, y - 1) == 1 ||
                    (room_grid.get(x, y - 1) & k_room_corridor) ==
                        k_room_corridor) {
                    add_piece(pieces, "carpet_north",
                              mansion_relative(above, rotation,
                                               mansion_direction::east, 1),
                              rotation);
                }
                if (layout_grid.get(x + 1, y) == 1 ||
                    (room_grid.get(x + 1, y) & k_room_corridor) ==
                        k_room_corridor) {
                    auto p = mansion_relative(pos, rotation,
                                              mansion_direction::south, 1);
                    p = mansion_relative(p, rotation, mansion_direction::east,
                                         5);
                    add_piece(pieces, "carpet_east",
                              block_pos{.x = p.x, .y = p.y + 1, .z = p.z},
                              rotation);
                }
                if (layout_grid.get(x, y + 1) == 1 ||
                    (room_grid.get(x, y + 1) & k_room_corridor) ==
                        k_room_corridor) {
                    auto p = mansion_relative(pos, rotation,
                                              mansion_direction::south, 5);
                    p = mansion_relative(p, rotation, mansion_direction::west,
                                         1);
                    add_piece(pieces, south_piece, p, rotation);
                }
                if (layout_grid.get(x - 1, y) == 1 ||
                    (room_grid.get(x - 1, y) & k_room_corridor) ==
                        k_room_corridor) {
                    auto p = mansion_relative(pos, rotation,
                                              mansion_direction::west, 1);
                    p = mansion_relative(p, rotation, mansion_direction::north,
                                         1);
                    add_piece(pieces, west_piece, p, rotation);
                }
            }
        }

        const std::string wall_piece =
            floor == 0 ? "indoors_wall_1" : "indoors_wall_2";
        const std::string door_piece =
            floor == 0 ? "indoors_door_1" : "indoors_door_2";
        std::vector<mansion_direction> door_directions;
        for (jint y = 0; y < layout_grid.height(); ++y) {
            for (jint x = 0; x < layout_grid.width(); ++x) {
                bool third_floor_start_room =
                    floor == 2 && layout_grid.get(x, y) == 3;
                if (layout_grid.get(x, y) != 2 && !third_floor_start_room) {
                    continue;
                }
                const jint room_data = room_grid.get(x, y);
                const jint room_type = room_data & k_room_type_mask;
                const jint room_id = room_data & k_room_id_mask;
                third_floor_start_room =
                    third_floor_start_room &&
                    (room_data & k_room_corridor) == k_room_corridor;
                door_directions.clear();
                if ((room_data & k_room_door) == k_room_door) {
                    static constexpr std::array<mansion_direction, 4>
                        horizontal{
                            mansion_direction::north, mansion_direction::east,
                            mansion_direction::south, mansion_direction::west};
                    for (const auto direction : horizontal) {
                        if (layout_grid.get(x + mansion_step_x(direction),
                                            y + mansion_step_z(direction)) ==
                            1) {
                            door_directions.push_back(direction);
                        }
                    }
                }
                door_dir door{};
                if (!door_directions.empty()) {
                    door.direction = door_directions[static_cast<std::size_t>(
                        random.next_int(
                            static_cast<jint>(door_directions.size())))];
                } else if ((room_data & k_room_origin) == k_room_origin) {
                    door.up = true;
                }
                auto room_pos = mansion_relative(floor_origin, rotation,
                                                 mansion_direction::south,
                                                 8 + (y - start_y) * 8);
                room_pos = mansion_relative(room_pos, rotation,
                                            mansion_direction::east,
                                            -1 + (x - start_x) * 8);
                if (mansion_grid::is_house(layout_grid, x - 1, y) &&
                    !grid.is_room_id(layout_grid, x - 1, y, floor, room_id)) {
                    add_piece(pieces,
                              is_door(door, mansion_direction::west)
                                  ? door_piece
                                  : wall_piece,
                              room_pos, rotation);
                }
                if (layout_grid.get(x + 1, y) == 1 && !third_floor_start_room) {
                    add_piece(pieces,
                              is_door(door, mansion_direction::east)
                                  ? door_piece
                                  : wall_piece,
                              mansion_relative(room_pos, rotation,
                                               mansion_direction::east, 8),
                              rotation);
                }
                if (mansion_grid::is_house(layout_grid, x, y + 1) &&
                    !grid.is_room_id(layout_grid, x, y + 1, floor, room_id)) {
                    auto p = mansion_relative(room_pos, rotation,
                                              mansion_direction::south, 7);
                    p = mansion_relative(p, rotation, mansion_direction::east,
                                         7);
                    add_piece(pieces,
                              is_door(door, mansion_direction::south)
                                  ? door_piece
                                  : wall_piece,
                              p,
                              add_rotation(rotation,
                                           template_rotation::clockwise_90));
                }
                if (layout_grid.get(x, y - 1) == 1 && !third_floor_start_room) {
                    auto p = mansion_relative(room_pos, rotation,
                                              mansion_direction::north, 1);
                    p = mansion_relative(p, rotation, mansion_direction::east,
                                         7);
                    add_piece(pieces,
                              is_door(door, mansion_direction::north)
                                  ? door_piece
                                  : wall_piece,
                              p,
                              add_rotation(rotation,
                                           template_rotation::clockwise_90));
                }
                if (room_type == k_room_1x1) {
                    add_room_1x1(pieces, room_pos, rotation, door,
                                 collections[static_cast<std::size_t>(floor)],
                                 random);
                } else if (room_type == k_room_1x2 && door.direction) {
                    const auto room_direction = grid.get_1x2_room_direction(
                        layout_grid, x, y, floor, room_id);
                    if (room_direction) {
                        const bool stairs_room =
                            (room_data & k_room_stairs) == k_room_stairs;
                        add_room_1x2(
                            pieces, room_pos, rotation, *room_direction, door,
                            collections[static_cast<std::size_t>(floor)],
                            stairs_room, random);
                    }
                } else if (room_type == k_room_2x2 && door.direction &&
                           !door.up) {
                    auto room_direction = mansion_clockwise(*door.direction);
                    if (!grid.is_room_id(layout_grid,
                                         x + mansion_step_x(room_direction),
                                         y + mansion_step_z(room_direction),
                                         floor, room_id)) {
                        room_direction = mansion_opposite(room_direction);
                    }
                    add_room_2x2(
                        pieces, room_pos, rotation, room_direction, door,
                        collections[static_cast<std::size_t>(floor)], random);
                } else if (room_type == k_room_2x2 && door.up) {
                    add_room_2x2_secret(
                        pieces, room_pos, rotation,
                        collections[static_cast<std::size_t>(floor)], random);
                }
            }
        }
    }
    return pieces;
}

auto mansion_zero_position(const block_pos &zero, template_mirror mirror,
                           template_rotation rotation, jint size_x, jint size_z)
    -> block_pos {
    --size_x;
    --size_z;
    const jint mirror_delta_x =
        mirror == template_mirror::front_back ? size_x : 0;
    const jint mirror_delta_z =
        mirror == template_mirror::left_right ? size_z : 0;
    switch (rotation) {
    case template_rotation::counterclockwise_90:
        return {.x = zero.x + mirror_delta_z,
                .y = zero.y,
                .z = zero.z + size_x - mirror_delta_x};
    case template_rotation::clockwise_90:
        return {.x = zero.x + size_z - mirror_delta_z,
                .y = zero.y,
                .z = zero.z + mirror_delta_x};
    case template_rotation::clockwise_180:
        return {.x = zero.x + size_x - mirror_delta_x,
                .y = zero.y,
                .z = zero.z + size_z - mirror_delta_z};
    case template_rotation::none:
    default:
        return {.x = zero.x + mirror_delta_x,
                .y = zero.y,
                .z = zero.z + mirror_delta_z};
    }
}

} // namespace refrain::worldgen::structure