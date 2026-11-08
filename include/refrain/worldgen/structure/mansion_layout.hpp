// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// The room grid used by woodland mansion generation. The grid encodes a room
// type and id, a door flag and the optional third-floor stair marker in the
// same integer the reference uses.

#pragma once

#include <refrain/worldgen/core/java_compat.hpp>
#include <refrain/worldgen/core/rng.hpp>

#include <array>
#include <optional>
#include <vector>

namespace refrain::worldgen::structure {

enum class mansion_direction : std::uint8_t { north, east, south, west };

[[nodiscard]] auto mansion_step_x(mansion_direction direction) -> jint;
[[nodiscard]] auto mansion_step_z(mansion_direction direction) -> jint;
[[nodiscard]] auto mansion_opposite(mansion_direction direction)
    -> mansion_direction;
[[nodiscard]] auto mansion_clockwise(mansion_direction direction)
    -> mansion_direction;
[[nodiscard]] auto mansion_counter_clockwise(mansion_direction direction)
    -> mansion_direction;
[[nodiscard]] auto mansion_from_2d_data(jint data) -> mansion_direction;

class mansion_simple_grid {
  public:
    mansion_simple_grid(jint width, jint height, jint outside_value);

    void set(jint x, jint y, jint value);
    void set(jint x0, jint y0, jint x1, jint y1, jint value);
    [[nodiscard]] auto get(jint x, jint y) const -> jint;
    void set_if(jint x, jint y, jint if_value, jint value);
    [[nodiscard]] auto edges_to(jint x, jint y, jint target) const -> bool;

    [[nodiscard]] auto width() const -> jint { return width_; }
    [[nodiscard]] auto height() const -> jint { return height_; }

  private:
    [[nodiscard]] auto index(jint x, jint y) const -> std::size_t;

    jint width_{0};
    jint height_{0};
    jint outside_{0};
    std::vector<jint> cells_{};
};

class mansion_grid {
  public:
    explicit mansion_grid(worldgen_random &random);

    [[nodiscard]] static auto is_house(const mansion_simple_grid &grid, jint x,
                                       jint y) -> bool;
    [[nodiscard]] auto is_room_id(const mansion_simple_grid &grid, jint x,
                                  jint y, jint floor, jint room_id) const
        -> bool;
    [[nodiscard]] auto get_1x2_room_direction(const mansion_simple_grid &grid,
                                              jint x, jint y, jint floor,
                                              jint room_id) const
        -> std::optional<mansion_direction>;

    [[nodiscard]] auto base_grid() const -> const mansion_simple_grid & {
        return base_grid_;
    }
    [[nodiscard]] auto third_floor_grid() const -> const mansion_simple_grid & {
        return third_floor_grid_;
    }
    [[nodiscard]] auto floor_rooms(jint floor) const
        -> const mansion_simple_grid & {
        return floor_rooms_[static_cast<std::size_t>(floor)];
    }
    [[nodiscard]] auto entrance_x() const -> jint { return entrance_x_; }
    [[nodiscard]] auto entrance_y() const -> jint { return entrance_y_; }

  private:
    void recursive_corridor(mansion_simple_grid &grid, jint x, jint y,
                            mansion_direction heading, jint depth);
    [[nodiscard]] auto clean_edges(mansion_simple_grid &grid) -> bool;
    void setup_third_floor();
    void identify_rooms(const mansion_simple_grid &from,
                        mansion_simple_grid &rooms);

    worldgen_random *random_{nullptr};
    mansion_simple_grid base_grid_{11, 11, 5};
    mansion_simple_grid third_floor_grid_{11, 11, 5};
    std::array<mansion_simple_grid, 3> floor_rooms_{
        mansion_simple_grid{11, 11, 5}, mansion_simple_grid{11, 11, 5},
        mansion_simple_grid{11, 11, 5}};
    jint entrance_x_{7};
    jint entrance_y_{4};
};

} // namespace refrain::worldgen::structure
