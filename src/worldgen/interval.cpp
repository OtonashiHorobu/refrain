// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/interval.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace refrain::worldgen {

namespace {

constexpr float infinity = std::numeric_limits<float>::infinity();

auto java_pow(double base, double exponent) -> float {
    return static_cast<float>(std::pow(base, exponent));
}

} // namespace

auto interval::na_i() -> interval {
    return interval{std::numeric_limits<float>::quiet_NaN(),
                    std::numeric_limits<float>::quiet_NaN()};
}

auto interval::infinite() -> interval { return interval{-infinity, infinity}; }

auto interval::is_na_i() const -> bool {
    return std::isnan(min_) && std::isnan(max_);
}

auto interval::of(float min, float max) -> interval {
    if (max < min) {
        throw std::invalid_argument("max < min");
    }
    if (std::isnan(min) || std::isnan(max)) {
        throw std::invalid_argument("bounds cannot include NaN");
    }
    return interval{min, max};
}

auto interval::of_symmetric(float range) -> interval {
    return of(-range, range);
}

auto interval::of_exact(float value) -> interval { return of(value, value); }

auto interval::encapsulating(std::span<const interval> values) -> interval {
    if (values.empty()) {
        throw std::invalid_argument("at least one interval required");
    }
    float min = infinity;
    float max = -infinity;
    for (const auto &value : values) {
        if (!value.is_na_i()) {
            min = std::min(value.min_, min);
            max = std::max(value.max_, max);
        }
    }
    return max < min ? na_i() : of(min, max);
}

auto interval::encapsulating(float first, float second) -> interval {
    if (std::isnan(first) && std::isnan(second)) {
        return na_i();
    }
    if (std::isnan(first)) {
        return of_exact(second);
    }
    if (std::isnan(second)) {
        return of_exact(first);
    }
    return of(std::min(first, second), std::max(first, second));
}

auto interval::encapsulating(const interval &first, const interval &second)
    -> interval {
    return encapsulating({first, second});
}

auto interval::encapsulating(const interval &first, float second) -> interval {
    if (std::isnan(second)) {
        return first;
    }
    return first.is_na_i()
               ? of_exact(second)
               : of(std::min(first.min_, second), std::max(first.max_, second));
}

auto interval::add(const interval &left, const interval &right) -> interval {
    const float min = left.min_ + right.min_;
    const float max = left.max_ + right.max_;
    return !std::isnan(min) && !std::isnan(max) ? of(min, max) : na_i();
}

auto interval::sub(const interval &left, const interval &right) -> interval {
    const float min = left.min_ - right.max_;
    const float max = left.max_ - right.min_;
    return !std::isnan(min) && !std::isnan(max) ? of(min, max) : na_i();
}

auto interval::mul_bound(float left, float right) -> float {
    return left != 0.0F && right != 0.0F ? left * right : 0.0F;
}

auto interval::mul(const interval &left, const interval &right) -> interval {
    if (left.is_na_i() || right.is_na_i()) {
        return na_i();
    }
    const float min_min = mul_bound(left.min_, right.min_);
    const float min_max = mul_bound(left.min_, right.max_);
    const float max_min = mul_bound(left.max_, right.min_);
    const float max_max = mul_bound(left.max_, right.max_);
    return of(std::min({min_min, min_max, max_min, max_max}),
              std::max({min_min, min_max, max_min, max_max}));
}

auto interval::reciprocal(const interval &input) -> interval {
    if (input.is_na_i() || (input.min_ == 0.0F && input.max_ == 0.0F)) {
        return na_i();
    }
    if (!input.contains(0.0F)) {
        return of(1.0F / input.max_, 1.0F / input.min_);
    }
    if (input.max_ == 0.0F) {
        return of(-infinity, 1.0F / input.min_);
    }
    if (input.min_ == 0.0F) {
        return of(1.0F / input.max_, infinity);
    }
    return infinite();
}

auto interval::div(const interval &left, const interval &right) -> interval {
    return mul(left, reciprocal(right));
}

auto interval::min(const interval &left, const interval &right) -> interval {
    if (left.is_na_i() || right.is_na_i()) {
        return na_i();
    }
    return of(std::min(left.min_, right.min_), std::min(left.max_, right.max_));
}

auto interval::max(const interval &left, const interval &right) -> interval {
    if (left.is_na_i() || right.is_na_i()) {
        return na_i();
    }
    return of(std::max(left.min_, right.min_), std::max(left.max_, right.max_));
}

auto interval::clamp(const interval &input, float min, float max) -> interval {
    if (min > max) {
        throw std::invalid_argument("min > max");
    }
    if (input.is_na_i()) {
        return na_i();
    }
    if (input.min_ >= max) {
        return of(max, max);
    }
    if (input.max_ <= min) {
        return of(min, min);
    }
    return of(std::max(input.min_, min), std::min(input.max_, max));
}

auto interval::abs(const interval &input) -> interval {
    if (input.is_na_i()) {
        return na_i();
    }
    const float max = std::max(std::abs(input.min_), std::abs(input.max_));
    return input.contains(0.0F)
               ? of(0.0F, max)
               : of(std::min(std::abs(input.min_), std::abs(input.max_)), max);
}

auto interval::square(const interval &input) -> interval {
    if (input.is_na_i()) {
        return na_i();
    }
    const float min_square = input.min_ * input.min_;
    const float max_square = input.max_ * input.max_;
    const float max = std::max(min_square, max_square);
    return input.contains(0.0F) ? of(0.0F, max)
                                : of(std::min(min_square, max_square), max);
}

auto interval::pow(float base, const interval &exponent) -> interval {
    if (std::isnan(base) || exponent.is_na_i()) {
        return na_i();
    }
    if (exponent.min_ == exponent.max_) {
        const float value = java_pow(base, exponent.min_);
        return std::isnan(value) ? na_i() : of_exact(value);
    }
    if (base == 0.0F) {
        return mul(pow_zero_base(exponent),
                   of_exact(std::copysign(1.0F, base)));
    }
    if (base == 1.0F) {
        return of_exact(1.0F);
    }
    return base > 0.0F ? pow_positive_base(base, exponent)
                       : pow_negative_base(base, exponent);
}

auto interval::pow(const interval &base, const interval &exponent) -> interval {
    if (base.is_na_i() || exponent.is_na_i()) {
        return na_i();
    }
    if (base.min_ == base.max_) {
        return pow(base.min_, exponent);
    }
    interval result =
        encapsulating(pow(base.min_, exponent), pow(base.max_, exponent));
    if (base.contains(0.0F)) {
        if (base.max_ > 0.0F) {
            result = encapsulating(result, pow(0.0F, exponent));
        }
        if (base.min_ < 0.0F) {
            result = encapsulating(result, pow(-0.0F, exponent));
        }
    }
    return result;
}

auto interval::pow_positive_base(float base, const interval &exponent)
    -> interval {
    if (std::isfinite(exponent.min_) && std::isfinite(exponent.max_)) {
        return encapsulating(java_pow(base, exponent.min_),
                             java_pow(base, exponent.max_));
    }
    return pow_infinite_exponent(base, exponent);
}

auto interval::pow_zero_base(const interval &exponent) -> interval {
    if (exponent.contains(0.0F)) {
        if (exponent.max_ == 0.0F) {
            return of(1.0F, infinity);
        }
        return exponent.min_ == 0.0F ? of(0.0F, 1.0F) : of(0.0F, infinity);
    }
    return exponent.max_ < 0.0F ? of_exact(infinity) : of_exact(0.0F);
}

auto interval::pow_infinite_exponent(float base, const interval &exponent)
    -> interval {
    if (std::isinf(exponent.min_) && std::isinf(exponent.max_)) {
        return of(0.0F, infinity);
    }
    if (std::isinf(exponent.min_)) {
        return base < 1.0F ? of(java_pow(base, exponent.max_), infinity)
                           : of(0.0F, java_pow(base, exponent.max_));
    }
    return base < 1.0F ? of(0.0F, java_pow(base, exponent.min_))
                       : of(java_pow(base, exponent.min_), infinity);
}

auto interval::pow_negative_base(float base, const interval &exponent)
    -> interval {
    const float exponent_min_int = std::ceil(exponent.min_);
    const float exponent_max_int = std::floor(exponent.max_);
    if (exponent_max_int < exponent_min_int) {
        return na_i();
    }
    const float base_to_min_int = java_pow(base, exponent_min_int);
    const float base_to_max_int = java_pow(base, exponent_max_int);
    interval result = encapsulating(base_to_min_int, base_to_max_int);
    if (std::isinf(exponent_min_int)) {
        result = encapsulating(result, -base_to_min_int);
    } else if (exponent_min_int + 1.0F < exponent_max_int) {
        result = encapsulating(result, java_pow(base, exponent_min_int + 1.0F));
    }
    if (std::isinf(exponent_max_int)) {
        result = encapsulating(result, -base_to_max_int);
    } else if (exponent_max_int - 1.0F > exponent_min_int) {
        result = encapsulating(result, java_pow(base, exponent_max_int - 1.0F));
    }
    return result;
}

auto interval::log(const interval &input) -> interval {
    if (input.max_ < 0.0F) {
        return na_i();
    }
    const interval clipped = max(input, of_exact(0.0F));
    return map_monotonic(clipped, [](float value) {
        return static_cast<float>(std::log(value));
    });
}

auto interval::map_monotonic(const interval &input, unary_op op) -> interval {
    if (input.is_na_i()) {
        return na_i();
    }
    const float mapped_min = op(input.min_);
    const float mapped_max = op(input.max_);
    if (std::isnan(mapped_min) || std::isnan(mapped_max)) {
        throw std::logic_error("monotonic operator produced NaN");
    }
    return of(std::min(mapped_min, mapped_max),
              std::max(mapped_min, mapped_max));
}

auto interval::lerp(const interval &alpha, const interval &first,
                    const interval &second) -> interval {
    if (alpha.is_na_i() || first.is_na_i() || second.is_na_i()) {
        return na_i();
    }
    return encapsulating({lerp(alpha, first.min_, second.min_),
                          lerp(alpha, first.max_, second.min_),
                          lerp(alpha, first.min_, second.max_),
                          lerp(alpha, first.max_, second.max_)});
}

auto interval::lerp(const interval &alpha, float first, float second)
    -> interval {
    if (alpha.is_na_i() || std::isnan(first) || std::isnan(second)) {
        return na_i();
    }
    return std::isfinite(first) && std::isfinite(second)
               ? lerp_finite_bounds(alpha, first, second)
               : lerp_infinite_bounds(alpha, first, second);
}

auto interval::lerp_finite_bounds(const interval &alpha, float first,
                                  float second) -> interval {
    return encapsulating(lerp_finite_bound(alpha.min_, first, second),
                         lerp_finite_bound(alpha.max_, first, second));
}

auto interval::lerp_finite_bound(float alpha, float first, float second)
    -> float {
    return first + mul_bound(alpha, second - first);
}

auto interval::lerp_infinite_bounds(const interval &alpha, float first,
                                    float second) -> interval {
    if (first == second) {
        return of_exact(first);
    }
    const float new_min = lerp_infinite_bound(alpha.min_, first, second);
    const float new_max = lerp_infinite_bound(alpha.max_, first, second);
    return !std::isnan(new_min) && !std::isnan(new_max)
               ? encapsulating(new_min, new_max)
               : na_i();
}

auto interval::lerp_infinite_bound(float alpha, float first, float second)
    -> float {
    const float first_part = mul_bound(1.0F - alpha, first);
    const float second_part = mul_bound(alpha, second);
    if (!std::isinf(first_part) || !std::isinf(second_part)) {
        return first_part + second_part;
    }
    if (alpha <= 0.0F) {
        return second > first ? -infinity : infinity;
    }
    if (alpha >= 1.0F) {
        return second > first ? infinity : -infinity;
    }
    return std::numeric_limits<float>::quiet_NaN();
}

auto interval::sign(const interval &input) -> interval {
    if (input.is_na_i()) {
        return na_i();
    }
    if (input.min_ == input.max_) {
        return of_exact(static_cast<float>(
            (input.min_ > 0.0F) ? 1.0F : ((input.min_ < 0.0F) ? -1.0F : 0.0F)));
    }
    if (input.contains(0.0F)) {
        if (input.min_ == 0.0F) {
            return of(0.0F, 1.0F);
        }
        return input.max_ == 0.0F ? of(-1.0F, 0.0F) : of(-1.0F, 1.0F);
    }
    return of_exact(input.min_ > 0.0F ? 1.0F : -1.0F);
}

} // namespace refrain::worldgen
