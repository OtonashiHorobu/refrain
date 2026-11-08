// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Java `TheEndBiomeSource`: the central island, the outer island bands and the
// small island ring, selected from the chunk radius and an erosion sample.

#pragma once

#include <refrain/worldgen/biome/biome_source.hpp>
#include <refrain/worldgen/core/java_compat.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::worldgen {

class the_end_biome_source {
  public:
    /// Biome ids follow `collectPossibleBiomes` order: the_end, end_highlands,
    /// end_midlands, small_end_islands, end_barrens.
    the_end_biome_source();

    [[nodiscard]] auto noise_biome(const climate_sampler &sampler, jint quart_x,
                                   jint quart_y, jint quart_z) const
        -> std::uint16_t;

    [[nodiscard]] auto biome(std::uint16_t id) const -> std::string_view {
        return biomes_[id];
    }
    [[nodiscard]] auto biomes() const -> const std::vector<std::string> & {
        return biomes_;
    }

  private:
    std::vector<std::string> biomes_{};
};

} // namespace refrain::worldgen
