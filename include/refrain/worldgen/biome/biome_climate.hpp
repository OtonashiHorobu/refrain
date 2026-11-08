// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Biome climate settings and the temperature function the surface stage uses
// for frozen-ocean icebergs. The noises are the per-biome static noises
// (legacy seeds 1234/3456/2345).

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/worldgen/core/java_compat.hpp>
#include <refrain/worldgen/noise/noise.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace refrain::worldgen {

/// How a biome temperature is modified.
enum class temperature_modifier : std::uint8_t { none, frozen };

/// The climate fields world generation reads from a biome document.
struct biome_climate {
    bool has_precipitation{true};
    float temperature{0.5F};
    float downfall{0.5F};
    temperature_modifier modifier{temperature_modifier::none};
};

/// Climate settings of the datapack biomes.
class biome_climates {
  public:
    void add(std::string id, biome_climate climate);
    [[nodiscard]] auto get(std::string_view id) const -> const biome_climate *;
    [[nodiscard]] auto load(std::string_view data_root) -> status;
    [[nodiscard]] auto size() const noexcept -> std::size_t {
        return climates_.size();
    }

  private:
    std::unordered_map<std::string, biome_climate> climates_{};
};

/// Per-position biome temperature and frozen-ocean iceberg melting.
class biome_temperatures {
  public:
    biome_temperatures();
    biome_temperatures(const biome_temperatures &) = delete;
    auto operator=(const biome_temperatures &) -> biome_temperatures & = delete;
    biome_temperatures(biome_temperatures &&) = delete;
    auto operator=(biome_temperatures &&) -> biome_temperatures & = delete;
    ~biome_temperatures() = default;

    [[nodiscard]] auto temperature_at(const biome_climate &climate, jint x,
                                      jint y, jint z, jint sea_level) const
        -> float;
    [[nodiscard]] auto should_melt_iceberg(const biome_climate &climate, jint x,
                                           jint y, jint z, jint sea_level) const
        -> bool;

  private:
    [[nodiscard]] auto modify_temperature(const biome_climate &climate, jint x,
                                          jint z) const -> float;

    std::unique_ptr<noise> temperature_noise_{};
    std::unique_ptr<noise> frozen_temperature_noise_{};
    std::unique_ptr<noise> biome_info_noise_{};
};

} // namespace refrain::worldgen
