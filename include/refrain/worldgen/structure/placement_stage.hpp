// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Structure pieces are written during the features stage: for each decoration
// step the referenced starts place their pieces before that step's features.

#pragma once

#include <refrain/worldgen/chunk/chunk_region.hpp>
#include <refrain/worldgen/chunk/proto_chunk.hpp>
#include <refrain/worldgen/core/rng.hpp>
#include <refrain/worldgen/structure/template.hpp>

#include <functional>
#include <string_view>

namespace refrain::worldgen::structure {

struct structure_place_context {
    chunk_region *region{nullptr};
    proto_chunk *chunk{nullptr};
    const block_state_table *states{nullptr};
    /// Resolves a canonical state key through the generation registry.
    std::function<block_state_id(std::string_view)> state{};
    /// Template source and state registry for template pieces; both must be
    /// present for `minecraft:igl` to place anything.
    const structure_template_manager *templates{nullptr};
    block_state_registry *registry{nullptr};
};

/// Maps a procedural piece's local coordinates to world coordinates using
/// the piece orientation and `base_y` as the shifted bounding box floor.
[[nodiscard]] auto piece_local_to_world(const structure_piece_record &piece,
                                        jint base_y, jint local_x, jint local_y,
                                        jint local_z) -> block_pos;

/// Places every referenced start of `chunk` whose `step` matches, clipping
/// pieces to the chunk. The random source is the decoration stream, already
/// seeded by the caller for structure `structure_index`.
void place_structures(proto_chunk &chunk,
                      const structure_place_context &context,
                      std::string_view step, worldgen_random &random);

} // namespace refrain::worldgen::structure
