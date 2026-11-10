// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Stronghold piece payload: the piece type plus the random choices its
// constructor made that its `postProcess` reads. All stronghold pieces are
// flat pieces of the start; there is no nested child list.

#pragma once

#include <refrain/worldgen/core/java_compat.hpp>

#include <cstdint>
#include <optional>

namespace refrain::worldgen::structure {

enum class stronghold_piece_kind : std::uint8_t {
    start,
    filler_corridor,
    chest_corridor,
    five_crossing,
    left_turn,
    right_turn,
    library,
    portal_room,
    prison_hall,
    room_crossing,
    stairs_down,
    straight,
    straight_stairs_down,
};

/// Door kinds, in declaration order.
enum class stronghold_door : std::uint8_t {
    opening = 0,
    wood_door = 1,
    grates = 2,
    iron_door = 3,
};

struct stronghold_piece_data {
    stronghold_piece_kind kind{stronghold_piece_kind::start};
    stronghold_door door{stronghold_door::opening};
    bool left{false};  ///< Straight's left child
    bool right{false}; ///< Straight's right child
    bool left_low{false};
    bool left_high{false};
    bool right_low{false};
    bool right_high{false};
    bool tall{false};
    bool source{false};
    jint variant{0};   ///< RoomCrossing design
    jint steps{0};     ///< FillerCorridor length
    jint gen_depth{0}; ///< recursive fit depth
};

} // namespace refrain::worldgen::structure
