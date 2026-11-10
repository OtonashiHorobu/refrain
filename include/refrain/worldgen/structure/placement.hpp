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

enum class frequency_reduction_method : std::uint8_t {
    default_method,
    legacy_type_1,
    legacy_type_2,
    legacy_type_3,
};

struct random_spread_placement {
    jint spacing{0};
    jint separation{0};
    jint salt{0};
    spread_type spread{spread_type::linear};
    frequency_reduction_method frequency_method{
        frequency_reduction_method::default_method};
    float frequency{1.0F};

    /// Returns the potential structure chunk of the grid cell.
    [[nodiscard]] auto potential_chunk(jlong seed, jint chunk_x,
                                       jint chunk_z) const -> chunk_pos;

    /// Whether the chunk owns a start under the default frequency reduction.
    [[nodiscard]] auto is_structure_chunk(jlong seed, jint chunk_x,
                                          jint chunk_z) const -> bool;

    /// Applies the additional frequency restriction to the candidate chunk.
    [[nodiscard]] auto passes_frequency(jlong seed, jint chunk_x,
                                        jint chunk_z) const -> bool;
};

[[nodiscard]] auto decode_random_spread(const data::json::value &document)
    -> result<random_spread_placement>;

} // namespace refrain::worldgen::structure
