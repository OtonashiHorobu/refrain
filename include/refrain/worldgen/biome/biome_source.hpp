// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Climate sampling: sample the six climate density functions at a quart
// position, quantize them and select the biome of the nearest parameter point.
// The parameter tables are loaded from the worldgen content snapshot rather
// than built in code.

#pragma once

#include <refrain/worldgen/biome/climate.hpp>
#include <refrain/worldgen/core/java_compat.hpp>
#include <refrain/worldgen/density/density.hpp>
#include <refrain/worldgen/density/density_buffer.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::worldgen {

/// Java `Climate.Sampler`: the six climate samplers bound to one context.
class climate_sampler {
  public:
    climate_sampler(const density::sampler &temperature,
                    const density::sampler &humidity,
                    const density::sampler &continentalness,
                    const density::sampler &erosion,
                    const density::sampler &depth,
                    const density::sampler &weirdness,
                    const density::sampler_context &context);

    /// Samples the six functions at the block position of `(quart_x, quart_y,
    /// quart_z)` and quantizes them.
    [[nodiscard]] auto sample(jint quart_x, jint quart_y, jint quart_z) const
        -> climate::target_point;

    [[nodiscard]] auto samplers() const
        -> const std::array<const density::sampler *, 6> & {
        return samplers_;
    }
    [[nodiscard]] auto context() const -> const density::sampler_context & {
        return *context_;
    }

  private:
    std::array<const density::sampler *, 6> samplers_{};
    const density::sampler_context *context_{nullptr};
};

/// One `Climate.ParameterList` entry: the climate box and the biome key.
struct biome_parameter {
    climate::parameter_point parameters{};
    std::string biome{};
};

/// Java `MultiNoiseBiomeSource` over a list of parameter points. Biome keys are
/// interned in first-seen order, so the returned ids are stable for the life of
/// the source and index `biomes()`.
class multi_noise_biome_source {
  public:
    explicit multi_noise_biome_source(std::vector<biome_parameter> parameters);

    /// `MultiNoiseBiomeSource.getNoiseBiome`; mutates the underlying search
    /// because the reference R-tree keeps the previous result as a candidate.
    [[nodiscard]] auto noise_biome(const climate_sampler &sampler, jint quart_x,
                                   jint quart_y, jint quart_z) -> std::uint16_t;

    /// `Climate.ParameterList.findValue`; mutates the search cache.
    [[nodiscard]] auto biome_at(const climate::target_point &target)
        -> std::uint16_t {
        return parameters_.find(target);
    }

    [[nodiscard]] auto biome(std::uint16_t id) const -> std::string_view {
        return biomes_[id];
    }
    [[nodiscard]] auto biomes() const -> const std::vector<std::string> & {
        return biomes_;
    }
    [[nodiscard]] auto parameters() const -> const climate::parameter_list & {
        return parameters_;
    }

  private:
    std::vector<std::string> biomes_{};
    climate::parameter_list parameters_;
};

/// Java `MultiNoiseBiomeSource.createResolverForChunk`: the six climate
/// functions are sampled once over the whole quart grid with step 4, and each
/// query only picks the precomputed target out of the buffers.
class biome_grid {
  public:
    /// `min_quart_*` are quart coordinates; the underlying volume is in block
    /// coordinates with step 4.
    biome_grid(multi_noise_biome_source &source, const climate_sampler &sampler,
               jint min_quart_x, jint min_quart_y, jint min_quart_z,
               jint size_x, jint size_y, jint size_z);

    [[nodiscard]] auto at(jint quart_x, jint quart_y, jint quart_z)
        -> std::uint16_t;

  private:
    multi_noise_biome_source *source_{nullptr};
    jint min_quart_x_{0};
    jint min_quart_y_{0};
    jint min_quart_z_{0};
    jint size_x_{0};
    jint size_y_{0};
    std::vector<buffer> values_{};
};

} // namespace refrain::worldgen
