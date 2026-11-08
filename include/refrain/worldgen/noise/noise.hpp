// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// 26.3 gradient noise: Perlin noise, its smeared variant, noise stacks and the
// octave-layout normal-noise builder.
//
// The 26.3 noise hierarchy differs from older versions: normal noise owns the
// octave layout and normalization, and builds two Perlin instances per octave
// (the second at a slightly different frequency) into a noise stack. Blended
// noise builds stacks of smeared Perlin noise instead.
//
// Every draw and every float expression follows the reference implementation;
// the vectors in `test/worldgen/test_noise.cpp` come from the reference
// implementation.

#pragma once

#include <refrain/worldgen/core/java_compat.hpp>
#include <refrain/worldgen/core/rng.hpp>
#include <refrain/worldgen/density/density_buffer.hpp>
#include <refrain/worldgen/density/interval.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace refrain::worldgen {

/// Common surface of the Perlin variants and noise stacks.
class noise {
  public:
    noise() = default;
    noise(const noise &) = delete;
    auto operator=(const noise &) -> noise & = delete;
    noise(noise &&) = default;
    auto operator=(noise &&) -> noise & = default;
    virtual ~noise() = default;

    [[nodiscard]] virtual auto get(double x, double y) const -> float = 0;
    [[nodiscard]] virtual auto get(double x, double y, double z) const
        -> float = 0;

    /// Bulk sampling. The arithmetic differs from `get` (it accumulates the XZ
    /// and Y terms separately), so this is the path terrain generation
    /// observes and it must stay bit-identical to Java's.
    virtual void add_to_volume(buffer &output, const volume &volume,
                               double xz_scale, double y_scale,
                               float amplitude) const = 0;

    [[nodiscard]] virtual auto range() const -> interval = 0;
};

/// Perlin noise: a 256-entry permutation, three world offsets and the
/// 16-entry gradient table, sampled with smoothstep interpolation.
class perlin_noise : public noise {
  public:
    /// Consumes three `nextDouble` draws and then 256 `nextInt(256 - i)`
    /// draws, in that order.
    explicit perlin_noise(random_source &random);

    [[nodiscard]] auto get(double x, double y) const -> float override;
    [[nodiscard]] auto get(double x, double y, double z) const
        -> float override;

    void add_to_volume(buffer &output, const volume &volume, double xz_scale,
                       double y_scale, float amplitude) const override;

    [[nodiscard]] auto range() const -> interval override {
        return interval::of_symmetric(2.0F);
    }

    [[nodiscard]] auto offset_x() const noexcept -> double { return offset_x_; }
    [[nodiscard]] auto offset_y() const noexcept -> double { return offset_y_; }
    [[nodiscard]] auto offset_z() const noexcept -> double { return offset_z_; }
    [[nodiscard]] auto perm(std::size_t index) const -> std::uint8_t {
        return perms_[index];
    }

  protected:
    [[nodiscard]] auto sample_and_lerp(jint x, jint y, jint z, float relative_x,
                                       float relative_y, float relative_z,
                                       float original_relative_y) const
        -> float;

    std::array<std::uint8_t, 256> perms_{};
    double offset_x_{0.0};
    double offset_y_{0.0};
    double offset_z_{0.0};
};

/// Smeared Perlin noise: a Perlin variant whose Y coordinate is snapped to a
/// smear grid before interpolation. Used by blended noise.
class smeared_perlin_noise final : public perlin_noise {
  public:
    smeared_perlin_noise(random_source &random, double fudge_y_scale);

    [[nodiscard]] auto get(double x, double y, double z) const
        -> float override;
    using perlin_noise::get;

    void add_to_volume(buffer &output, const volume &volume, double xz_scale,
                       double y_scale, float amplitude) const override;

    [[nodiscard]] auto range() const -> interval override;

    [[nodiscard]] static auto range_for(double fudge_y_scale) -> interval;

  private:
    [[nodiscard]] auto compute_fudge_y(double original_y,
                                       double relative_y) const -> double;

    double fudge_y_scale_{0.0};
};

/// Legacy simplex noise still used by biome temperature and a few structure
/// helpers.
class simplex_noise final : public noise {
  public:
    /// Consumes three `nextDouble` draws and then 256 `nextInt(256 - i)` draws.
    /// `discard_noise_offset` zeroes the three offsets.
    simplex_noise(random_source &random, bool discard_noise_offset);

    [[nodiscard]] auto get(double x, double y) const -> float override;
    [[nodiscard]] auto get(double x, double y, double z) const
        -> float override;

    void add_to_volume(buffer &output, const volume &volume, double xz_scale,
                       double y_scale, float amplitude) const override;

    [[nodiscard]] auto range() const -> interval override;

  private:
    [[nodiscard]] auto permute_value(jint value) const -> jint;
    [[nodiscard]] auto corner_noise3d(jint index, double x, double y, double z,
                                      double base) const -> double;

    std::array<std::uint8_t, 256> perms_{};
    double offset_x_{0.0};
    double offset_y_{0.0};
    double offset_z_{0.0};
};

/// A sum of layers at fixed frequencies.
class noise_stack final : public noise {
  public:
    struct layer {
        std::unique_ptr<noise> source;
        double frequency{0.0};
        float amplitude{0.0F};
    };

    noise_stack() = default;
    noise_stack(noise_stack &&) = default;
    auto operator=(noise_stack &&) -> noise_stack & = default;
    explicit noise_stack(std::vector<layer> layers);

    [[nodiscard]] auto get(double x, double y, double z) const
        -> float override;
    [[nodiscard]] auto get(double x, double y) const -> float override;

    void add_to_volume(buffer &output, const volume &volume, double xz_scale,
                       double y_scale, float amplitude) const override;

    [[nodiscard]] auto range() const -> interval override { return range_; }

    [[nodiscard]] auto layers() const noexcept -> const std::vector<layer> & {
        return layers_;
    }

  private:
    std::vector<layer> layers_{};
    interval range_{interval::of_exact(0.0F)};
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

/// Octave layout, normalization and the two-per-octave noise-stack
/// construction.
class normal_noise {
  public:
    explicit normal_noise(normal_noise_parameters parameters);

    /// Legacy-normalised construction used by the nether noises: one noise per
    /// octave with the given amplitudes.
    [[nodiscard]] static auto create_parity(jint first_octave,
                                            std::span<const double> amplitudes)
        -> normal_noise;

    /// Builds the stack. The random source is the one derived from
    /// Derives the stream from the noise id hash; two positional forks are
    /// taken from it in order.
    [[nodiscard]] auto create(random_source &random) const -> noise_stack;

    [[nodiscard]] auto parameters() const noexcept
        -> const normal_noise_parameters & {
        return parameters_;
    }

    /// Symmetric bound of the noise range as a positive float.
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
