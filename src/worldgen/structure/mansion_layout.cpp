// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/structure/mansion_layout.hpp>

namespace refrain::worldgen::structure {

mansion_simple_grid::mansion_simple_grid(jint width, jint height,
                                         jint outside_value)
    : width_{width}, height_{height}, outside_{outside_value},
      cells_(static_cast<std::size_t>(width * height), 0) {}

auto mansion_simple_grid::index(jint x, jint y) const -> std::size_t {
    return static_cast<std::size_t>(y * width_ + x);
}

void mansion_simple_grid::set(jint x, jint y, jint value) {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) {
        return;
    }
    cells_[index(x, y)] = value;
}

void mansion_simple_grid::set(jint x0, jint y0, jint x1, jint y1, jint value) {
    for (jint y = y0; y <= y1; ++y) {
        for (jint x = x0; x <= x1; ++x) {
            set(x, y, value);
        }
    }
}

auto mansion_simple_grid::get(jint x, jint y) const -> jint {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) {
        return outside_;
    }
    return cells_[index(x, y)];
}

void mansion_simple_grid::set_if(jint x, jint y, jint if_value, jint value) {
    if (get(x, y) == if_value) {
        set(x, y, value);
    }
}

auto mansion_simple_grid::edges_to(jint x, jint y, jint target) const -> bool {
    return get(x - 1, y) == target || get(x + 1, y) == target ||
           get(x, y - 1) == target || get(x, y + 1) == target;
}

} // namespace refrain::worldgen::structure

using refrain::worldgen::jint;
using refrain::worldgen::worldgen_random;

namespace {

constexpr jint k_clear = 0;
constexpr jint k_corridor = 1;
constexpr jint k_room = 2;
constexpr jint k_start_room = 3;
constexpr jint k_test_room = 4;
constexpr jint k_blocked = 5;
constexpr jint k_room_1x1 = 65536;
constexpr jint k_room_1x2 = 131072;
constexpr jint k_room_2x2 = 262144;
constexpr jint k_room_origin = 1048576;
constexpr jint k_room_door = 2097152;
constexpr jint k_room_stairs = 4194304;
constexpr jint k_room_corridor = 8388608;
constexpr jint k_room_type_mask = 983040;
constexpr jint k_room_id_mask = 65535;

struct grid_pos {
    jint x{0};
    jint y{0};
};

auto is_house_value(jint value) -> bool {
    return value == k_corridor || value == k_room || value == k_start_room ||
           value == k_test_room;
}

void shuffle(std::vector<grid_pos> &positions, worldgen_random &random) {
    for (std::size_t i = positions.size(); i > 1; --i) {
        const auto next =
            static_cast<std::size_t>(random.next_int(static_cast<jint>(i)));
        std::swap(positions[i - 1], positions[next]);
    }
}

} // namespace

namespace refrain::worldgen::structure {

auto mansion_step_x(mansion_direction direction) -> jint {
    switch (direction) {
    case mansion_direction::east:
        return 1;
    case mansion_direction::west:
        return -1;
    default:
        return 0;
    }
}

auto mansion_step_z(mansion_direction direction) -> jint {
    switch (direction) {
    case mansion_direction::south:
        return 1;
    case mansion_direction::north:
        return -1;
    default:
        return 0;
    }
}

auto mansion_opposite(mansion_direction direction) -> mansion_direction {
    switch (direction) {
    case mansion_direction::north:
        return mansion_direction::south;
    case mansion_direction::south:
        return mansion_direction::north;
    case mansion_direction::east:
        return mansion_direction::west;
    case mansion_direction::west:
    default:
        return mansion_direction::east;
    }
}

auto mansion_clockwise(mansion_direction direction) -> mansion_direction {
    switch (direction) {
    case mansion_direction::north:
        return mansion_direction::east;
    case mansion_direction::east:
        return mansion_direction::south;
    case mansion_direction::south:
        return mansion_direction::west;
    case mansion_direction::west:
    default:
        return mansion_direction::north;
    }
}

auto mansion_counter_clockwise(mansion_direction direction)
    -> mansion_direction {
    return mansion_clockwise(mansion_clockwise(mansion_clockwise(direction)));
}

auto mansion_from_2d_data(jint data) -> mansion_direction {
    static constexpr std::array<mansion_direction, 4> values{
        mansion_direction::south, mansion_direction::west,
        mansion_direction::north, mansion_direction::east};
    return values[static_cast<std::size_t>(data & 3)];
}

mansion_grid::mansion_grid(worldgen_random &random) : random_{&random} {
    base_grid_.set(entrance_x_, entrance_y_, entrance_x_ + 1, entrance_y_ + 1,
                   k_start_room);
    base_grid_.set(entrance_x_ - 1, entrance_y_, entrance_x_ - 1,
                   entrance_y_ + 1, k_room);
    base_grid_.set(entrance_x_ + 2, entrance_y_ - 2, entrance_x_ + 3,
                   entrance_y_ + 3, k_blocked);
    base_grid_.set(entrance_x_ + 1, entrance_y_ - 2, entrance_x_ + 1,
                   entrance_y_ - 1, k_corridor);
    base_grid_.set(entrance_x_ + 1, entrance_y_ + 2, entrance_x_ + 1,
                   entrance_y_ + 3, k_corridor);
    base_grid_.set(entrance_x_ - 1, entrance_y_ - 1, k_corridor);
    base_grid_.set(entrance_x_ - 1, entrance_y_ + 2, k_corridor);
    base_grid_.set(0, 0, 11, 1, k_blocked);
    base_grid_.set(0, 9, 11, 11, k_blocked);
    recursive_corridor(base_grid_, entrance_x_, entrance_y_ - 2,
                       mansion_direction::west, 6);
    recursive_corridor(base_grid_, entrance_x_, entrance_y_ + 3,
                       mansion_direction::west, 6);
    recursive_corridor(base_grid_, entrance_x_ - 2, entrance_y_ - 1,
                       mansion_direction::west, 3);
    recursive_corridor(base_grid_, entrance_x_ - 2, entrance_y_ + 2,
                       mansion_direction::west, 3);
    while (clean_edges(base_grid_)) {
    }

    for (auto &rooms : floor_rooms_) {
        rooms = mansion_simple_grid{11, 11, 5};
    }
    identify_rooms(base_grid_, floor_rooms_[0]);
    identify_rooms(base_grid_, floor_rooms_[1]);
    floor_rooms_[0].set(entrance_x_ + 1, entrance_y_, entrance_x_ + 1,
                        entrance_y_ + 1, k_room_corridor);
    floor_rooms_[1].set(entrance_x_ + 1, entrance_y_, entrance_x_ + 1,
                        entrance_y_ + 1, k_room_corridor);
    third_floor_grid_ =
        mansion_simple_grid{base_grid_.width(), base_grid_.height(), 5};
    setup_third_floor();
    identify_rooms(third_floor_grid_, floor_rooms_[2]);
}

auto mansion_grid::is_house(const mansion_simple_grid &grid, jint x, jint y)
    -> bool {
    return is_house_value(grid.get(x, y));
}

auto mansion_grid::is_room_id(const mansion_simple_grid &grid, jint x, jint y,
                              jint floor, jint room_id) const -> bool {
    (void)grid;
    return (floor_rooms_[static_cast<std::size_t>(floor)].get(x, y) &
            k_room_id_mask) == room_id;
}

auto mansion_grid::get_1x2_room_direction(const mansion_simple_grid &grid,
                                          jint x, jint y, jint floor,
                                          jint room_id) const
    -> std::optional<mansion_direction> {
    static constexpr std::array<mansion_direction, 4> horizontal{
        mansion_direction::north, mansion_direction::east,
        mansion_direction::south, mansion_direction::west};
    for (const auto direction : horizontal) {
        if (is_room_id(grid, x + mansion_step_x(direction),
                       y + mansion_step_z(direction), floor, room_id)) {
            return direction;
        }
    }
    return std::nullopt;
}

void mansion_grid::recursive_corridor(mansion_simple_grid &grid, jint x, jint y,
                                      mansion_direction heading, jint depth) {
    if (depth <= 0) {
        return;
    }
    grid.set(x, y, k_corridor);
    grid.set_if(x + mansion_step_x(heading), y + mansion_step_z(heading),
                k_clear, k_corridor);
    for (jint attempt = 0; attempt < 8; ++attempt) {
        const auto next_direction = mansion_from_2d_data(random_->next_int(4));
        if (next_direction != mansion_opposite(heading) &&
            (next_direction != mansion_direction::east ||
             !random_->next_boolean())) {
            const jint nx = x + mansion_step_x(heading);
            const jint ny = y + mansion_step_z(heading);
            if (grid.get(nx + mansion_step_x(next_direction),
                         ny + mansion_step_z(next_direction)) == k_clear &&
                grid.get(nx + mansion_step_x(next_direction) * 2,
                         ny + mansion_step_z(next_direction) * 2) == k_clear) {
                recursive_corridor(grid,
                                   x + mansion_step_x(heading) +
                                       mansion_step_x(next_direction),
                                   y + mansion_step_z(heading) +
                                       mansion_step_z(next_direction),
                                   next_direction, depth - 1);
                break;
            }
        }
    }
    const auto clockwise = mansion_clockwise(heading);
    const auto counter = mansion_counter_clockwise(heading);
    grid.set_if(x + mansion_step_x(clockwise), y + mansion_step_z(clockwise),
                k_clear, k_room);
    grid.set_if(x + mansion_step_x(counter), y + mansion_step_z(counter),
                k_clear, k_room);
    grid.set_if(x + mansion_step_x(heading) + mansion_step_x(clockwise),
                y + mansion_step_z(heading) + mansion_step_z(clockwise),
                k_clear, k_room);
    grid.set_if(x + mansion_step_x(heading) + mansion_step_x(counter),
                y + mansion_step_z(heading) + mansion_step_z(counter), k_clear,
                k_room);
    grid.set_if(x + mansion_step_x(heading) * 2,
                y + mansion_step_z(heading) * 2, k_clear, k_room);
    grid.set_if(x + mansion_step_x(clockwise) * 2,
                y + mansion_step_z(clockwise) * 2, k_clear, k_room);
    grid.set_if(x + mansion_step_x(counter) * 2,
                y + mansion_step_z(counter) * 2, k_clear, k_room);
}

auto mansion_grid::clean_edges(mansion_simple_grid &grid) -> bool {
    bool touched = false;
    for (jint y = 0; y < grid.height(); ++y) {
        for (jint x = 0; x < grid.width(); ++x) {
            if (grid.get(x, y) != k_clear) {
                continue;
            }
            jint direct = 0;
            direct += is_house(grid, x + 1, y) ? 1 : 0;
            direct += is_house(grid, x - 1, y) ? 1 : 0;
            direct += is_house(grid, x, y + 1) ? 1 : 0;
            direct += is_house(grid, x, y - 1) ? 1 : 0;
            if (direct >= 3) {
                grid.set(x, y, k_room);
                touched = true;
            } else if (direct == 2) {
                jint diagonal = 0;
                diagonal += is_house(grid, x + 1, y + 1) ? 1 : 0;
                diagonal += is_house(grid, x - 1, y + 1) ? 1 : 0;
                diagonal += is_house(grid, x + 1, y - 1) ? 1 : 0;
                diagonal += is_house(grid, x - 1, y - 1) ? 1 : 0;
                if (diagonal <= 1) {
                    grid.set(x, y, k_room);
                    touched = true;
                }
            }
        }
    }
    return touched;
}

void mansion_grid::setup_third_floor() {
    std::vector<grid_pos> potential;
    auto &floor = floor_rooms_[1];
    for (jint y = 0; y < third_floor_grid_.height(); ++y) {
        for (jint x = 0; x < third_floor_grid_.width(); ++x) {
            const jint data = floor.get(x, y);
            if ((data & k_room_type_mask) == k_room_1x2 &&
                (data & k_room_door) == k_room_door) {
                potential.push_back({x, y});
            }
        }
    }
    if (potential.empty()) {
        third_floor_grid_.set(0, 0, third_floor_grid_.width(),
                              third_floor_grid_.height(), k_blocked);
        return;
    }
    const auto room = potential[static_cast<std::size_t>(
        random_->next_int(static_cast<jint>(potential.size())))];
    const jint room_data = floor.get(room.x, room.y);
    floor.set(room.x, room.y, room_data | k_room_stairs);
    const auto room_direction = get_1x2_room_direction(
        base_grid_, room.x, room.y, 1, room_data & k_room_id_mask);
    if (!room_direction) {
        return;
    }
    const jint room_end_x = room.x + mansion_step_x(*room_direction);
    const jint room_end_y = room.y + mansion_step_z(*room_direction);
    for (jint y = 0; y < third_floor_grid_.height(); ++y) {
        for (jint x = 0; x < third_floor_grid_.width(); ++x) {
            if (!is_house(base_grid_, x, y)) {
                third_floor_grid_.set(x, y, k_blocked);
            } else if (x == room.x && y == room.y) {
                third_floor_grid_.set(x, y, k_start_room);
            } else if (x == room_end_x && y == room_end_y) {
                third_floor_grid_.set(x, y, k_start_room);
                floor_rooms_[2].set(x, y, k_room_corridor);
            }
        }
    }
    std::vector<mansion_direction> corridors;
    static constexpr std::array<mansion_direction, 4> horizontal{
        mansion_direction::north, mansion_direction::east,
        mansion_direction::south, mansion_direction::west};
    for (const auto direction : horizontal) {
        if (third_floor_grid_.get(room_end_x + mansion_step_x(direction),
                                  room_end_y + mansion_step_z(direction)) ==
            k_clear) {
            corridors.push_back(direction);
        }
    }
    if (corridors.empty()) {
        third_floor_grid_.set(0, 0, third_floor_grid_.width(),
                              third_floor_grid_.height(), k_blocked);
        floor.set(room.x, room.y, room_data);
        return;
    }
    const auto corridor = corridors[static_cast<std::size_t>(
        random_->next_int(static_cast<jint>(corridors.size())))];
    recursive_corridor(third_floor_grid_, room_end_x + mansion_step_x(corridor),
                       room_end_y + mansion_step_z(corridor), corridor, 4);
    while (clean_edges(third_floor_grid_)) {
    }
}

void mansion_grid::identify_rooms(const mansion_simple_grid &from,
                                  mansion_simple_grid &rooms) {
    std::vector<grid_pos> positions;
    for (jint y = 0; y < from.height(); ++y) {
        for (jint x = 0; x < from.width(); ++x) {
            if (from.get(x, y) == k_room) {
                positions.push_back({x, y});
            }
        }
    }
    shuffle(positions, *random_);
    jint room_id = 10;
    for (const auto &position : positions) {
        const jint xx = position.x;
        const jint y = position.y;
        if (rooms.get(xx, y) != 0) {
            continue;
        }
        jint x0 = xx;
        jint x1 = xx;
        jint y0 = y;
        jint y1 = y;
        jint type = k_room_1x1;
        if (rooms.get(xx + 1, y) == 0 && rooms.get(xx, y + 1) == 0 &&
            rooms.get(xx + 1, y + 1) == 0 && from.get(xx + 1, y) == k_room &&
            from.get(xx, y + 1) == k_room &&
            from.get(xx + 1, y + 1) == k_room) {
            x1 = xx + 1;
            y1 = y + 1;
            type = k_room_2x2;
        } else if (rooms.get(xx - 1, y) == 0 && rooms.get(xx, y + 1) == 0 &&
                   rooms.get(xx - 1, y + 1) == 0 &&
                   from.get(xx - 1, y) == k_room &&
                   from.get(xx, y + 1) == k_room &&
                   from.get(xx - 1, y + 1) == k_room) {
            x0 = xx - 1;
            y1 = y + 1;
            type = k_room_2x2;
        } else if (rooms.get(xx - 1, y) == 0 && rooms.get(xx, y - 1) == 0 &&
                   rooms.get(xx - 1, y - 1) == 0 &&
                   from.get(xx - 1, y) == k_room &&
                   from.get(xx, y - 1) == k_room &&
                   from.get(xx - 1, y - 1) == k_room) {
            x0 = xx - 1;
            y0 = y - 1;
            type = k_room_2x2;
        } else if (rooms.get(xx + 1, y) == 0 && from.get(xx + 1, y) == k_room) {
            x1 = xx + 1;
            type = k_room_1x2;
        } else if (rooms.get(xx, y + 1) == 0 && from.get(xx, y + 1) == k_room) {
            y1 = y + 1;
            type = k_room_1x2;
        } else if (rooms.get(xx - 1, y) == 0 && from.get(xx - 1, y) == k_room) {
            x0 = xx - 1;
            type = k_room_1x2;
        } else if (rooms.get(xx, y - 1) == 0 && from.get(xx, y - 1) == k_room) {
            y0 = y - 1;
            type = k_room_1x2;
        }
        jint door_x = random_->next_boolean() ? x0 : x1;
        jint door_y = random_->next_boolean() ? y0 : y1;
        jint door_flag = k_room_door;
        if (!from.edges_to(door_x, door_y, k_corridor)) {
            door_x = door_x == x0 ? x1 : x0;
            door_y = door_y == y0 ? y1 : y0;
            if (!from.edges_to(door_x, door_y, k_corridor)) {
                door_y = door_y == y0 ? y1 : y0;
                if (!from.edges_to(door_x, door_y, k_corridor)) {
                    door_x = door_x == x0 ? x1 : x0;
                    door_y = door_y == y0 ? y1 : y0;
                    if (!from.edges_to(door_x, door_y, k_corridor)) {
                        door_flag = 0;
                        door_x = x0;
                        door_y = y0;
                    }
                }
            }
        }
        for (jint ry = y0; ry <= y1; ++ry) {
            for (jint rx = x0; rx <= x1; ++rx) {
                if (rx == door_x && ry == door_y) {
                    rooms.set(rx, ry,
                              k_room_origin | door_flag | type | room_id);
                } else {
                    rooms.set(rx, ry, type | room_id);
                }
            }
        }
        ++room_id;
    }
}

} // namespace refrain::worldgen::structure

namespace refrain::worldgen::structure {

auto mansion_rotate_2d(template_rotation rotation, jint x, jint z)
    -> block_pos {
    switch (rotation) {
    case template_rotation::clockwise_90:
        return {.x = -z, .y = 0, .z = x};
    case template_rotation::clockwise_180:
        return {.x = -x, .y = 0, .z = -z};
    case template_rotation::counterclockwise_90:
        return {.x = z, .y = 0, .z = -x};
    case template_rotation::none:
    default:
        return {.x = x, .y = 0, .z = z};
    }
}

auto mansion_relative(const block_pos &origin, template_rotation rotation,
                      mansion_direction direction, jint amount) -> block_pos {
    const auto rotated = mansion_rotate_2d(rotation, mansion_step_x(direction),
                                           mansion_step_z(direction));
    return {.x = origin.x + rotated.x * amount,
            .y = origin.y,
            .z = origin.z + rotated.z * amount};
}

auto mansion_room_collection::room_1x1(worldgen_random &random) const
    -> std::string {
    if (floor_ == 0) {
        return "1x1_a" + std::to_string(random.next_int(5) + 1);
    }
    return "1x1_b" + std::to_string(random.next_int(5) + 1);
}

auto mansion_room_collection::room_1x1_secret(worldgen_random &random) const
    -> std::string {
    return "1x1_as" + std::to_string(random.next_int(4) + 1);
}

auto mansion_room_collection::room_1x2_side(worldgen_random &random,
                                            bool stairs_room) const
    -> std::string {
    if (floor_ == 0) {
        return "1x2_a" + std::to_string(random.next_int(9) + 1);
    }
    if (stairs_room) {
        return "1x2_c_stairs";
    }
    return "1x2_c" + std::to_string(random.next_int(4) + 1);
}

auto mansion_room_collection::room_1x2_front(worldgen_random &random,
                                             bool stairs_room) const
    -> std::string {
    if (floor_ == 0) {
        return "1x2_b" + std::to_string(random.next_int(5) + 1);
    }
    if (stairs_room) {
        return "1x2_d_stairs";
    }
    return "1x2_d" + std::to_string(random.next_int(5) + 1);
}

auto mansion_room_collection::room_1x2_secret(worldgen_random &random) const
    -> std::string {
    if (floor_ == 0) {
        return "1x2_s" + std::to_string(random.next_int(2) + 1);
    }
    return "1x2_se" + std::to_string(random.next_int(1) + 1);
}

auto mansion_room_collection::room_2x2(worldgen_random &random) const
    -> std::string {
    if (floor_ == 0) {
        return "2x2_a" + std::to_string(random.next_int(4) + 1);
    }
    return "2x2_b" + std::to_string(random.next_int(5) + 1);
}

auto mansion_room_collection::room_2x2_secret(
    worldgen_random & /*random*/) const -> std::string {
    return "2x2_s1";
}

} // namespace refrain::worldgen::structure
