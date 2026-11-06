// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// 26.3 gradient noise: `PerlinNoise`, `NoiseStack` and `NormalNoise`.
//
// The 26.3 noise hierarchy differs from older versions: `NormalNoise` owns the
// octave layout and normalization, and `create` builds two `PerlinNoise`
// instances per octave (the second at a slightly different frequency) into a
// `NoiseStack`. `create_parity` keeps the legacy-normalised construction used
// by the nether noises.
//
// Every draw and every float expression follows the reference implementation;
// the vectors in `test/worldgen/test_noise.cpp` come from the real classes.

#pragma once

#include <refrain/worldgen/java_compat.hpp>
#include <refrain/worldgen/rng.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace refrain::worldgen {

/// Java `PerlinNoise`: a 256-entry permutation, three world offsets and the
/// 16-entry gradient table, sampled with smoothstep interpolation.
class perlin_noise {
  public:
    /// Consumes three `nextDouble` draws and then 256 `nextInt(256 - i)`
    /// draws, in that order.
    explicit perlin_noise(random_source &random);

    [[nodiscard]] auto get(double x, double y) const -> float;
    [[nodiscard]] auto get(double x, double y, double z) const -> float;

    [[nodiscard]] auto offset_x() const noexcept -> double { return offset_x_; }
    [[nodiscard]] auto offset_y() const noexcept -> double { return offset_y_; }
    [[nodiscard]] auto offset_z() const noexcept -> double { return offset_z_; }
    [[nodiscard]] auto perm(std::size_t index) const -> std::uint8_t {
        return perms_[index];
    }

  private:
    [[nodiscard]] auto sample_and_lerp(jint x, jint y, jint z, float relative_x,
                                       float relative_y, float relative_z,
                                       float original_relative_y) const
        -> float;

    std::array<std::uint8_t, 256> perms_{};
    double offset_x_{0.0};
    double offset_y_{0.0};
    double offset_z_{0.0};
};

/// Java `NoiseStack`: a sum of Perlin layers at fixed frequencies. 26.3
/// specialises the all-Perlin case, which is the only one `NormalNoise`
/// produces; `SmearedPerlinNoise` layers are not modelled yet.
class noise_stack {
  public:
    struct layer {
        perlin_noise noise;
        double frequency{0.0};
        float amplitude{0.0F};
    };

    noise_stack() = default;
    explicit noise_stack(std::vector<layer> layers)
        : layers_{std::move(layers)} {}

    [[nodiscard]] auto get(double x, double y, double z) const -> float;
    [[nodiscard]] auto get(double x, double y) const -> float;

    [[nodiscard]] auto layers() const noexcept -> const std::vector<layer> & {
        return layers_;
    }

  private:
    std::vector<layer> layers_{};
};

/// Decoded `minecraft:noise` document.
///
/// `amplitude_modifiers` is empty when the document omits the field (every
/// octave then uses 1.0), otherwise it has exactly `octave_count` entries.
struct normal_noise_parameters {
    double base_amplitude{1.0};
    jint base_octave{0};
    jint octave_count{1};
    bool normalize{true};
    std::vector<double> amplitude_modifiers{};
};

/// Java `NormalNoise`: octave layout, normalization and the two-per-octave
/// `NoiseStack` construction.
class normal_noise {
  public:
    explicit normal_noise(normal_noise_parameters parameters);

    /// Legacy-normalised construction used by the nether noises:
    /// `NormalNoise.createParity(firstOctave, amplitudes)`.
    [[nodiscard]] static auto create_parity(jint first_octave,
                                            std::span<const double> amplitudes)
        -> normal_noise;

    /// Builds the stack. The random source is the one derived from
    /// `RandomState.random.fromHashOf(noise_id)`; two positional forks are
    /// taken from it in order.
    [[nodiscard]] auto create(random_source &random) const -> noise_stack;

    [[nodiscard]] auto parameters() const noexcept
        -> const normal_noise_parameters & {
        return parameters_;
    }

    /// Symmetric bound of `NormalNoise.range()` as a positive float.
    [[nodiscard]] auto range() const noexcept -> float { return range_bound_; }

  private:
    struct octave_info {
        jint index{0};
        double frequency{0.0};
        double amplitude{0.0};
    };

    [[nodiscard]] static auto
    build_octaves(jint base_octave, double base_amplitude, jint octave_count,
                  bool normalize,
                  const std::vector<double> &amplitude_modifiers)
        -> std::vector<octave_info>;
    [[nodiscard]] static auto
    amplitude_modifier(const std::vector<double> &amplitude_modifiers,
                       jint index) -> double;
    [[nodiscard]] static auto
    estimate_deviation(const std::vector<octave_info> &octaves) -> double;
    [[nodiscard]] static auto
    compute_normalization_factor(double target_amplitude,
                                 const std::vector<octave_info> &octaves)
        -> double;
    [[nodiscard]] static auto
    compute_parity_base_amplitude(jint base_octave,
                                  std::span<const double> amplitudes) -> double;
    [[nodiscard]] static auto compute_parity_normalization_factor(
        double base_amplitude, jint octave_count,
        const std::vector<double> &amplitude_modifiers) -> double;
    [[nodiscard]] static auto parity_expected_deviation(jint octave_span)
        -> double;

    normal_noise_parameters parameters_{};
    std::vector<octave_info> octaves_{};
    double normalization_factor_{0.0};
    float range_bound_{0.0F};
};

} // namespace refrain::worldgen
