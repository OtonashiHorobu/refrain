// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Structure-set placement: which chunk grid cells own a structure start.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/worldgen/chunk/chunk_region.hpp>
#include <refrain/worldgen/core/java_compat.hpp>

namespace refrain::data::json {
class value;
} // namespace refrain::data::json

namespace refrain::worldgen::structure {

enum class spread_type : std::uint8_t { linear, triangular };

struct random_spread_placement {
    jint spacing{0};
    jint separation{0};
    jint salt{0};
    spread_type spread{spread_type::linear};

    /// `RandomSpreadStructurePlacement.getPotentialStructureChunk`.
    [[nodiscard]] auto potential_chunk(jlong seed, jint chunk_x,
                                       jint chunk_z) const -> chunk_pos;

    /// `StructurePlacement.isStructureChunk` for the random-spread kind, with
    /// the default frequency reduction.
    [[nodiscard]] auto is_structure_chunk(jlong seed, jint chunk_x,
                                          jint chunk_z) const -> bool;
};

[[nodiscard]] auto decode_random_spread(const data::json::value &document)
    -> result<random_spread_placement>;

} // namespace refrain::worldgen::structure
