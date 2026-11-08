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
#include <refrain/worldgen/structure/structure_data.hpp>
#include <refrain/worldgen/structure/template.hpp>
#include <unordered_map>

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
    jlong seed{0};
    jint sea_level{63};
    /// The chunk's `worldgen_region_random` stream, consumed by pieces that
    /// read the level random stream.
    worldgen_random *level_random{nullptr};
    /// Per-piece adjusted heights shared by the owner and neighbour sections.
    std::unordered_map<std::string, jint> *adjusted_heights{nullptr};
    /// Processor-list documents for jigsaw pieces; null leaves the element
    /// processor lists empty.
    const structure_data_set *data{nullptr};
    /// Places a `feature_pool_element` by placed-feature id. Null skips the
    /// feature piece.
    std::function<bool(std::string_view, worldgen_random &, const block_pos &)>
        place_feature{};
};

/// Places one already generated piece into `context.chunk`. Template and
/// procedural piece types share the same clipping and state resolution.
void place_structure_piece(const structure_place_context &context,
                           const structure_piece_record &piece,
                           worldgen_random &random);

/// Places one ocean monument piece. A building also places the child pieces
/// its bounding box overlaps, matching the reference placement order.
void place_monument_piece(const structure_place_context &context,
                          const structure_piece_record &piece,
                          worldgen_random &random);

/// Places one stronghold piece.
void place_stronghold_piece(const structure_place_context &context,
                            const structure_piece_record &piece,
                            worldgen_random &random);

/// Places one nether fortress piece.
void place_fortress_piece(const structure_place_context &context,
                          const structure_piece_record &piece,
                          worldgen_random &random);

/// Places one end city template piece and applies its data markers.
void place_end_city_piece(const structure_place_context &context,
                          const structure_piece_record &piece,
                          worldgen_random &random);

/// Places one nether fossil template: air and structure blocks are ignored,
/// and the piece may add a dried ghast at its floor.
void place_nether_fossil_piece(const structure_place_context &context,
                               const structure_piece_record &piece,
                               worldgen_random &random);

/// Places one `minecraft:jigsaw` piece: every element in the piece's element
/// tree is placed at the piece origin with its processors and projection.
void place_jigsaw_piece(const structure_place_context &context,
                        const structure_piece_record &piece,
                        worldgen_random &random);

/// Places a chest with an explicit loot table. Returns false when the position
/// is occupied by a chest or the write is clipped.
[[nodiscard]] auto place_loot_chest(const structure_place_context &context,
                                    jint x, jint y, jint z,
                                    worldgen_random &random,
                                    std::string_view loot_table) -> bool;

/// Maps a procedural piece's local coordinates to world coordinates using
/// the piece orientation and `base_y` as the shifted bounding box floor.
[[nodiscard]] auto piece_local_to_world(const structure_piece_record &piece,
                                        jint base_y, jint local_x, jint local_y,
                                        jint local_z) -> block_pos;

/// Recomputes fence, wall and stair connections in the chunk after all
/// neighbouring pieces have been placed.
void refresh_chunk_shapes(proto_chunk &chunk, const chunk_region &region,
                          block_state_registry &states);

/// Places every referenced start of `chunk` whose `step` matches, clipping
/// pieces to the chunk. The random source is the decoration stream, already
/// seeded by the caller for structure `structure_index`.
void place_structures(proto_chunk &chunk,
                      const structure_place_context &context,
                      std::string_view step, worldgen_random &random);

} // namespace refrain::worldgen::structure
