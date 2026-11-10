// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Nether fortress piece payload: the piece kind plus the random choices its
// constructor made that its placement reads. The bridge and castle sections
// are procedural, so the payload carries the individual bridge-end seed and
// whether a corridor turn contains a chest.

#pragma once

#include <refrain/worldgen/core/java_compat.hpp>

#include <cstdint>

namespace refrain::worldgen::structure {

enum class fortress_piece_kind : std::uint8_t {
    bridge_crossing,
    bridge_end_filler,
    bridge_straight,
    castle_corridor_stairs,
    castle_corridor_t_balcony,
    castle_entrance,
    castle_small_corridor,
    castle_small_corridor_crossing,
    castle_small_corridor_left_turn,
    castle_small_corridor_right_turn,
    castle_stalk_room,
    monster_throne,
    room_crossing,
    stairs_room,
};

struct fortress_piece_data {
    fortress_piece_kind kind{fortress_piece_kind::bridge_crossing};
    /// The bridge-end filler draws a private seed for its ragged end.
    jlong self_seed{0};
    /// Corridor turn pieces place a chest for one third of their starts.
    bool chest{false};
};

} // namespace refrain::worldgen::structure
