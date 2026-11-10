// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Ocean monument piece payload: the room definition a piece was fitted to and
// the neighbouring room states its interior reads. The room graph itself is
// rebuilt from the start random stream; only the parts a piece consults after
// generation travel with the piece.

#pragma once

#include <refrain/worldgen/core/java_compat.hpp>

#include <array>
#include <cstdint>
#include <optional>

namespace refrain::worldgen::structure {

/// Face order: down, up, north, south, west, east.
enum class monument_direction : std::uint8_t {
    down = 0,
    up = 1,
    north = 2,
    south = 3,
    west = 4,
    east = 5,
};

enum class monument_piece_kind : std::uint8_t {
    building,
    entry_room,
    core_room,
    double_x_room,
    double_xy_room,
    double_y_room,
    double_yz_room,
    double_z_room,
    simple_room,
    simple_top_room,
    wing_room,
    penthouse,
};

/// The room subset a placed piece reads: the room index (its floor and grid
/// position), whether each face has an opening, and whether the room above
/// exists.
struct monument_room_state {
    jint index{-1};
    std::array<bool, 6> openings{};
    bool up_connected{false};

    [[nodiscard]] auto count_openings() const -> jint {
        jint count = 0;
        for (const bool open : openings) {
            if (open) {
                ++count;
            }
        }
        return count;
    }
};

/// A monument piece carries up to four room states. Slot 0 is the piece's own
/// room; the remaining slots are the rooms its interior consults, in the order
/// documented by each piece kind.
struct monument_piece_data {
    monument_piece_kind kind{monument_piece_kind::building};
    std::array<monument_room_state, 4> rooms{};
    jint main_design{0};
};

} // namespace refrain::worldgen::structure
