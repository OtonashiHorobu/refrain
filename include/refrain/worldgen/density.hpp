// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// 26.3 density-function samplers.
//
// 26.3 compiles every density-function node into a `DensitySampler` with a
// scalar (`sampleValue`) and a bulk (`sampleVolume`) path. The two paths do not
// always agree: for example `mul` short-circuits a zero left operand on the
// scalar path but not in bulk, and `sub` accumulates the right operand negated
// in bulk. Both are implemented separately, because terrain generation reads
// the bulk path.
//
// The compiler also rewrites the graph (constant specialisations, `div` by a
// constant becoming a multiply by the reciprocal, min/max range shortcuts).
// Those rewrites are part of the observable float results, so the factories
// below reproduce them.

#pragma once

#include <refrain/worldgen/density_buffer.hpp>
#include <refrain/worldgen/interval.hpp>
#include <refrain/worldgen/java_compat.hpp>
#include <refrain/worldgen/noise.hpp>

#include <cstddef>
#include <memory>

namespace refrain::worldgen::density {

class sampler_context {
  public:
    sampler_context() = default;

    /// Scratch storage for one sampling pass. The reference uses a pooled arena
    /// with scoped lifetimes; a fresh allocation has the same values.
    [[nodiscard]] auto acquire(std::size_t size) const -> buffer {
        return buffer{size};
    }
};

class sampler {
  public:
    sampler() = default;
    sampler(const sampler &) = delete;
    auto operator=(const sampler &) -> sampler & = delete;
    sampler(sampler &&) = delete;
    auto operator=(sampler &&) -> sampler & = delete;
    virtual ~sampler() = default;

    [[nodiscard]] virtual auto sample_value(const sampler_context &context,
                                            jint block_x, jint block_y,
                                            jint block_z) const -> float = 0;
    virtual void sample_volume(const sampler_context &context, buffer &output,
                               const volume &volume) const = 0;

    [[nodiscard]] virtual auto range() const -> interval = 0;

    /// Non-null when the node is a constant; the compiler uses this for the
    /// constant specialisations.
    [[nodiscard]] virtual auto constant_value() const -> const float * {
        return nullptr;
    }
};

using sampler_ptr = std::unique_ptr<sampler>;

[[nodiscard]] auto constant(float value) -> sampler_ptr;

[[nodiscard]] auto add(sampler_ptr left, sampler_ptr right) -> sampler_ptr;
[[nodiscard]] auto sub(sampler_ptr left, sampler_ptr right) -> sampler_ptr;
[[nodiscard]] auto mul(sampler_ptr left, sampler_ptr right) -> sampler_ptr;
[[nodiscard]] auto div(sampler_ptr left, sampler_ptr right) -> sampler_ptr;
[[nodiscard]] auto min(sampler_ptr left, sampler_ptr right) -> sampler_ptr;
[[nodiscard]] auto max(sampler_ptr left, sampler_ptr right) -> sampler_ptr;

[[nodiscard]] auto abs(sampler_ptr input) -> sampler_ptr;
[[nodiscard]] auto square(sampler_ptr input) -> sampler_ptr;
[[nodiscard]] auto cube(sampler_ptr input) -> sampler_ptr;
[[nodiscard]] auto sqrt(sampler_ptr input) -> sampler_ptr;
[[nodiscard]] auto half_negative(sampler_ptr input) -> sampler_ptr;
[[nodiscard]] auto quarter_negative(sampler_ptr input) -> sampler_ptr;
[[nodiscard]] auto reciprocal(sampler_ptr input) -> sampler_ptr;
[[nodiscard]] auto negate(sampler_ptr input) -> sampler_ptr;
[[nodiscard]] auto squeeze(sampler_ptr input) -> sampler_ptr;
[[nodiscard]] auto log(sampler_ptr input) -> sampler_ptr;
[[nodiscard]] auto sign(sampler_ptr input) -> sampler_ptr;

[[nodiscard]] auto clamp(sampler_ptr input, float min_value, float max_value)
    -> sampler_ptr;

/// `gradient` / `y_clamped_gradient` with `CLAMP_TO_EDGE`. Only the Y axis is
/// needed for the overworld router; the other axes follow the same formula.
[[nodiscard]] auto gradient_y(jint from_coordinate, jint to_coordinate,
                              float from_value, float to_value) -> sampler_ptr;

/// `minecraft:noise`: `noise.get(x * xz, y * y, z * xz)`.
[[nodiscard]] auto noise(std::shared_ptr<const noise_stack> stack,
                         double xz_scale, double y_scale) -> sampler_ptr;

/// `minecraft:noise` with shift functions; `y_scale` is used for the Y
/// coordinate and the X/Z shifts are evaluated per sample.
[[nodiscard]] auto shifted_noise_xz(sampler_ptr shift_x, sampler_ptr shift_z,
                                    double xz_scale, double y_scale,
                                    std::shared_ptr<const noise_stack> stack)
    -> sampler_ptr;
[[nodiscard]] auto shifted_noise_xyz(sampler_ptr shift_x, sampler_ptr shift_y,
                                     sampler_ptr shift_z, double xz_scale,
                                     double y_scale,
                                     std::shared_ptr<const noise_stack> stack)
    -> sampler_ptr;

/// `minecraft:shift`: the 3D noise at a quarter scale, times four.
[[nodiscard]] auto shift(std::shared_ptr<const noise_stack> stack)
    -> sampler_ptr;
[[nodiscard]] auto shift_a(std::shared_ptr<const noise_stack> stack)
    -> sampler_ptr;
[[nodiscard]] auto shift_b(std::shared_ptr<const noise_stack> stack)
    -> sampler_ptr;

} // namespace refrain::worldgen::density
