// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Block face support used by predicates and features that ask whether a block
// offers a full collision face in one direction. Generation has no collision
// geometry, so the face model combines the motion-blocking block tags with
// the shape families a state names through its block name and properties.

#pragma once

#include <refrain/worldgen/chunk/block_state.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace refrain::worldgen::feature {

/// The six block faces in the order the reference enumerates them.
enum class block_face : std::uint8_t {
    down = 0,
    up = 1,
    north = 2,
    south = 3,
    west = 4,
    east = 5
};

[[nodiscard]] auto parse_block_face(std::string_view name)
    -> std::optional<block_face>;
[[nodiscard]] auto block_face_name(block_face face) -> std::string_view;
[[nodiscard]] auto opposite_block_face(block_face face) -> block_face;

/// True when `state` offers a full face toward `face`, matching the reference
/// collision-shape test. States whose collision shape is empty or smaller than
/// a full face never qualify; shape families derive the answer from their
/// facing, half and type properties.
[[nodiscard]] auto is_sturdy_face(const block_state_table &states,
                                  block_state_id state, block_face face)
    -> bool;

} // namespace refrain::worldgen::feature
