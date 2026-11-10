// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// A closed float interval with the arithmetic the density-function compiler
// uses for its range shortcuts and the spline/range nodes use for validation.

#pragma once

#include <array>
#include <cstddef>
#include <initializer_list>
#include <span>

namespace refrain::worldgen {

class interval {
  public:
    /// Validates ordering and rejects NaN bounds.
    [[nodiscard]] static auto of(float min, float max) -> interval;
    [[nodiscard]] static auto of_symmetric(float range) -> interval;
    [[nodiscard]] static auto of_exact(float value) -> interval;
    [[nodiscard]] static auto na_i() -> interval;
    [[nodiscard]] static auto infinite() -> interval;

    [[nodiscard]] static auto encapsulating(std::span<const interval> values)
        -> interval;
    [[nodiscard]] static auto
    encapsulating(std::initializer_list<interval> values) -> interval {
        return encapsulating(
            std::span<const interval>{values.begin(), values.size()});
    }
    [[nodiscard]] static auto encapsulating(float first, float second)
        -> interval;
    [[nodiscard]] static auto encapsulating(const interval &first,
                                            const interval &second) -> interval;

    [[nodiscard]] static auto add(const interval &left, const interval &right)
        -> interval;
    [[nodiscard]] static auto sub(const interval &left, const interval &right)
        -> interval;
    [[nodiscard]] static auto mul(const interval &left, const interval &right)
        -> interval;
    [[nodiscard]] static auto div(const interval &left, const interval &right)
        -> interval;
    [[nodiscard]] static auto min(const interval &left, const interval &right)
        -> interval;
    [[nodiscard]] static auto max(const interval &left, const interval &right)
        -> interval;
    [[nodiscard]] static auto clamp(const interval &input, float min, float max)
        -> interval;
    [[nodiscard]] static auto abs(const interval &input) -> interval;
    [[nodiscard]] static auto square(const interval &input) -> interval;
    [[nodiscard]] static auto pow(const interval &base,
                                  const interval &exponent) -> interval;
    [[nodiscard]] static auto log(const interval &input) -> interval;
    [[nodiscard]] static auto sign(const interval &input) -> interval;

    using unary_op = float (*)(float);
    [[nodiscard]] static auto map_monotonic(const interval &input, unary_op op)
        -> interval;

    [[nodiscard]] static auto lerp(const interval &alpha, const interval &first,
                                   const interval &second) -> interval;
    [[nodiscard]] static auto lerp(const interval &alpha, float first,
                                   float second) -> interval;

    [[nodiscard]] auto contains(float value) const -> bool {
        return value >= min_ && value <= max_;
    }
    [[nodiscard]] auto intersects(const interval &other) const -> bool {
        return min_ <= other.max_ && max_ >= other.min_;
    }
    [[nodiscard]] auto is_na_i() const -> bool;
    [[nodiscard]] auto min() const -> float { return min_; }
    [[nodiscard]] auto max() const -> float { return max_; }

    [[nodiscard]] auto operator==(const interval &other) const -> bool {
        return min_ == other.min_ && max_ == other.max_;
    }

  private:
    constexpr interval(float min, float max) : min_{min}, max_{max} {}

    [[nodiscard]] static auto encapsulating(const interval &first, float second)
        -> interval;
    [[nodiscard]] static auto mul_bound(float left, float right) -> float;
    [[nodiscard]] static auto reciprocal(const interval &input) -> interval;
    [[nodiscard]] static auto pow(float base, const interval &exponent)
        -> interval;
    [[nodiscard]] static auto pow_positive_base(float base,
                                                const interval &exponent)
        -> interval;
    [[nodiscard]] static auto pow_zero_base(const interval &exponent)
        -> interval;
    [[nodiscard]] static auto pow_infinite_exponent(float base,
                                                    const interval &exponent)
        -> interval;
    [[nodiscard]] static auto pow_negative_base(float base,
                                                const interval &exponent)
        -> interval;
    [[nodiscard]] static auto lerp_finite_bounds(const interval &alpha,
                                                 float first, float second)
        -> interval;
    [[nodiscard]] static auto lerp_finite_bound(float alpha, float first,
                                                float second) -> float;
    [[nodiscard]] static auto lerp_infinite_bounds(const interval &alpha,
                                                   float first, float second)
        -> interval;
    [[nodiscard]] static auto lerp_infinite_bound(float alpha, float first,
                                                  float second) -> float;

    float min_{0.0F};
    float max_{0.0F};
};

} // namespace refrain::worldgen
