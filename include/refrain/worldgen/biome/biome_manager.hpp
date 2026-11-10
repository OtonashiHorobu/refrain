// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The fuzzy biome lookup used by surface material rules and feature
// placement. It picks one of the eight quart corners around a block
// position by the smallest "fiddled" distance, then resolves that quart with
// the biome source.

#pragma once

#include <refrain/worldgen/biome/biome_source.hpp>
#include <refrain/worldgen/core/java_compat.hpp>

#include <cstdint>

namespace refrain::worldgen {

/// Maps quart coordinates to a biome id.
class biome_resolver {
  public:
    biome_resolver() = default;
    biome_resolver(const biome_resolver &) = delete;
    auto operator=(const biome_resolver &) -> biome_resolver & = delete;
    biome_resolver(biome_resolver &&) = delete;
    auto operator=(biome_resolver &&) -> biome_resolver & = delete;
    virtual ~biome_resolver() = default;

    [[nodiscard]] virtual auto get_noise_biome(jint quart_x, jint quart_y,
                                               jint quart_z)
        -> std::uint16_t = 0;
};

/// Binds a multi-noise source and a climate sampler into a resolver. The
/// source must outlive the resolver; `noise_biome` mutates the source's search
/// cache, hence the non-const interface.
class climate_biome_resolver final : public biome_resolver {
  public:
    climate_biome_resolver(multi_noise_biome_source &source,
                           const climate_sampler &sampler)
        : source_{&source}, sampler_{&sampler} {}

    [[nodiscard]] auto get_noise_biome(jint quart_x, jint quart_y, jint quart_z)
        -> std::uint16_t override {
        return source_->noise_biome(*sampler_, quart_x, quart_y, quart_z);
    }

  private:
    multi_noise_biome_source *source_{nullptr};
    const climate_sampler *sampler_{nullptr};
};

/// Fuzzy biome lookup around a block position.
class biome_manager {
  public:
    /// `biome_zoom_seed` is already obfuscated; use `obfuscate_seed(seed)`.
    biome_manager(biome_resolver &resolver, jlong biome_zoom_seed)
        : resolver_{&resolver}, biome_zoom_seed_{biome_zoom_seed} {}

    /// Obfuscates the zoom seed: SHA-256 of the little-endian seed, first
    /// eight bytes read as a big-endian long.
    [[nodiscard]] static auto obfuscate_seed(jlong seed) -> jlong;

    [[nodiscard]] auto get_biome(jint x, jint y, jint z) -> std::uint16_t;

  private:
    biome_resolver *resolver_{nullptr};
    jlong biome_zoom_seed_{0};
};

} // namespace refrain::worldgen
