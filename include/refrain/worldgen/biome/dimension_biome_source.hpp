// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The biome source of one dimension. The overworld and the nether select a
// biome from their climate parameter table; the end selects from its island
// bands; a fixed source returns one biome for every cell and backs reference
// worlds whose preset declares a fixed biome source.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/worldgen/biome/biome_source.hpp>
#include <refrain/worldgen/biome/end_biome_source.hpp>
#include <refrain/worldgen/dimension.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace refrain::worldgen {

class dimension_biome_source {
  public:
    /// Builds the source of `kind` from the content snapshot. The overworld and
    /// the nether decode their parameter report; the end has no report because
    /// its selection only needs the erosion sampler.
    [[nodiscard]] static auto load(std::string_view data_root,
                                   dimension_kind kind)
        -> result<dimension_biome_source>;

    /// A source that selects `biome` everywhere, for reference worlds.
    [[nodiscard]] static auto fixed(dimension_kind kind, std::string biome)
        -> dimension_biome_source;

    /// Selects the biome of a quart cell. The climate search is stateful: one
    /// source must be shared by everything that queries the same chunk.
    [[nodiscard]] auto noise_biome(const climate_sampler &sampler, jint quart_x,
                                   jint quart_y, jint quart_z) -> std::uint16_t;

    /// Drops the nearest-point search warm start. Selection is then
    /// independent of the chunks generated before this one.
    void reset_search();

    [[nodiscard]] auto biome(std::uint16_t id) const -> std::string_view;
    [[nodiscard]] auto biomes() const -> const std::vector<std::string> &;
    [[nodiscard]] auto kind() const noexcept -> dimension_kind { return kind_; }
    [[nodiscard]] auto is_fixed() const noexcept -> bool;

  private:
    dimension_biome_source(dimension_kind kind,
                           multi_noise_biome_source source);
    dimension_biome_source(dimension_kind kind, the_end_biome_source source);
    dimension_biome_source(dimension_kind kind, std::vector<std::string> biome);

    dimension_kind kind_{dimension_kind::overworld};
    std::variant<multi_noise_biome_source, the_end_biome_source,
                 std::vector<std::string>>
        source_;
};

} // namespace refrain::worldgen
