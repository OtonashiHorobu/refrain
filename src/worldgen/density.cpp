// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/density.hpp>

#include <refrain/worldgen/mth.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>

namespace refrain::worldgen::density {

namespace {

auto cube_value(float value) -> float { return value * value * value; }

auto squeeze_value(float value) -> float {
    const float clamped = std::clamp(value, -1.0F, 1.0F);
    return clamped / 2.0F - cube_value(clamped) / 24.0F;
}

auto leaky_relu(float value, float negative_factor) -> float {
    return value > 0.0F ? value : value * negative_factor;
}

class constant_sampler final : public sampler {
  public:
    explicit constant_sampler(float value) : value_{value} {}

    [[nodiscard]] auto sample_value(const sampler_context &, jint, jint,
                                    jint) const -> float override {
        return value_;
    }
    void sample_volume(const sampler_context &, buffer &output,
                       const volume &) const override {
        output.fill(value_);
    }
    [[nodiscard]] auto range() const -> interval override {
        return interval::of_exact(value_);
    }
    [[nodiscard]] auto constant_value() const -> const float * override {
        return &value_;
    }

  private:
    float value_;
};

class add_sampler final : public sampler {
  public:
    add_sampler(sampler_ptr left, sampler_ptr right)
        : left_{std::move(left)}, right_{std::move(right)} {}

    [[nodiscard]] auto sample_value(const sampler_context &context, jint x,
                                    jint y, jint z) const -> float override {
        return left_->sample_value(context, x, y, z) +
               right_->sample_value(context, x, y, z);
    }
    void sample_volume(const sampler_context &context, buffer &output,
                       const volume &vol) const override {
        left_->sample_volume(context, output, vol);
        auto right_buffer = context.acquire(vol.size());
        right_->sample_volume(context, right_buffer, vol);
        for (std::size_t i = 0; i < output.size(); ++i) {
            output.add_to(i, right_buffer.get(i));
        }
    }
    [[nodiscard]] auto range() const -> interval override {
        return interval::add(left_->range(), right_->range());
    }

  private:
    sampler_ptr left_;
    sampler_ptr right_;
};

class const_add_sampler final : public sampler {
  public:
    const_add_sampler(sampler_ptr left, float right)
        : left_{std::move(left)}, right_{right} {}

    [[nodiscard]] auto sample_value(const sampler_context &context, jint x,
                                    jint y, jint z) const -> float override {
        return left_->sample_value(context, x, y, z) + right_;
    }
    void sample_volume(const sampler_context &context, buffer &output,
                       const volume &vol) const override {
        left_->sample_volume(context, output, vol);
        for (std::size_t i = 0; i < output.size(); ++i) {
            output.add_to(i, right_);
        }
    }
    [[nodiscard]] auto range() const -> interval override {
        return interval::add(left_->range(), interval::of_exact(right_));
    }

  private:
    sampler_ptr left_;
    float right_;
};

class sub_sampler final : public sampler {
  public:
    sub_sampler(sampler_ptr left, sampler_ptr right)
        : left_{std::move(left)}, right_{std::move(right)} {}

    [[nodiscard]] auto sample_value(const sampler_context &context, jint x,
                                    jint y, jint z) const -> float override {
        return left_->sample_value(context, x, y, z) -
               right_->sample_value(context, x, y, z);
    }
    void sample_volume(const sampler_context &context, buffer &output,
                       const volume &vol) const override {
        left_->sample_volume(context, output, vol);
        auto right_buffer = context.acquire(vol.size());
        right_->sample_volume(context, right_buffer, vol);
        for (std::size_t i = 0; i < output.size(); ++i) {
            output.add_to(i, -right_buffer.get(i));
        }
    }
    [[nodiscard]] auto range() const -> interval override {
        return interval::sub(left_->range(), right_->range());
    }

  private:
    sampler_ptr left_;
    sampler_ptr right_;
};

class const_sub_sampler final : public sampler {
  public:
    const_sub_sampler(float left, sampler_ptr right)
        : left_{left}, right_{std::move(right)} {}

    [[nodiscard]] auto sample_value(const sampler_context &context, jint x,
                                    jint y, jint z) const -> float override {
        return left_ - right_->sample_value(context, x, y, z);
    }
    void sample_volume(const sampler_context &context, buffer &output,
                       const volume &vol) const override {
        right_->sample_volume(context, output, vol);
        for (std::size_t i = 0; i < output.size(); ++i) {
            output.set(i, left_ - output.get(i));
        }
    }
    [[nodiscard]] auto range() const -> interval override {
        return interval::sub(interval::of_exact(left_), right_->range());
    }

  private:
    float left_;
    sampler_ptr right_;
};

class mul_sampler final : public sampler {
  public:
    mul_sampler(sampler_ptr left, sampler_ptr right)
        : left_{std::move(left)}, right_{std::move(right)} {}

    [[nodiscard]] auto sample_value(const sampler_context &context, jint x,
                                    jint y, jint z) const -> float override {
        const float left = left_->sample_value(context, x, y, z);
        return left == 0.0F ? 0.0F
                            : left * right_->sample_value(context, x, y, z);
    }
    void sample_volume(const sampler_context &context, buffer &output,
                       const volume &vol) const override {
        left_->sample_volume(context, output, vol);
        auto right_buffer = context.acquire(vol.size());
        right_->sample_volume(context, right_buffer, vol);
        for (std::size_t i = 0; i < output.size(); ++i) {
            output.set(i, output.get(i) * right_buffer.get(i));
        }
    }
    [[nodiscard]] auto range() const -> interval override {
        return interval::mul(left_->range(), right_->range());
    }

  private:
    sampler_ptr left_;
    sampler_ptr right_;
};

class const_mul_sampler final : public sampler {
  public:
    const_mul_sampler(sampler_ptr left, float right)
        : left_{std::move(left)}, right_{right} {}

    [[nodiscard]] auto sample_value(const sampler_context &context, jint x,
                                    jint y, jint z) const -> float override {
        return left_->sample_value(context, x, y, z) * right_;
    }
    void sample_volume(const sampler_context &context, buffer &output,
                       const volume &vol) const override {
        left_->sample_volume(context, output, vol);
        for (std::size_t i = 0; i < output.size(); ++i) {
            output.set(i, output.get(i) * right_);
        }
    }
    [[nodiscard]] auto range() const -> interval override {
        return interval::mul(left_->range(), interval::of_exact(right_));
    }

  private:
    sampler_ptr left_;
    float right_;
};

class div_sampler final : public sampler {
  public:
    div_sampler(sampler_ptr left, sampler_ptr right)
        : left_{std::move(left)}, right_{std::move(right)} {}

    [[nodiscard]] auto sample_value(const sampler_context &context, jint x,
                                    jint y, jint z) const -> float override {
        const float left = left_->sample_value(context, x, y, z);
        return left == 0.0F ? 0.0F
                            : left / right_->sample_value(context, x, y, z);
    }
    void sample_volume(const sampler_context &context, buffer &output,
                       const volume &vol) const override {
        left_->sample_volume(context, output, vol);
        auto right_buffer = context.acquire(vol.size());
        right_->sample_volume(context, right_buffer, vol);
        for (std::size_t i = 0; i < output.size(); ++i) {
            output.set(i, output.get(i) / right_buffer.get(i));
        }
    }
    [[nodiscard]] auto range() const -> interval override {
        return interval::div(left_->range(), right_->range());
    }

  private:
    sampler_ptr left_;
    sampler_ptr right_;
};

class const_div_sampler final : public sampler {
  public:
    const_div_sampler(float left, sampler_ptr right)
        : left_{left}, right_{std::move(right)} {}

    [[nodiscard]] auto sample_value(const sampler_context &context, jint x,
                                    jint y, jint z) const -> float override {
        return left_ / right_->sample_value(context, x, y, z);
    }
    void sample_volume(const sampler_context &context, buffer &output,
                       const volume &vol) const override {
        right_->sample_volume(context, output, vol);
        for (std::size_t i = 0; i < output.size(); ++i) {
            output.set(i, left_ / output.get(i));
        }
    }
    [[nodiscard]] auto range() const -> interval override {
        return interval::div(interval::of_exact(left_), right_->range());
    }

  private:
    float left_;
    sampler_ptr right_;
};

class min_sampler final : public sampler {
  public:
    min_sampler(sampler_ptr left, sampler_ptr right, float right_min)
        : left_{std::move(left)}, right_{std::move(right)},
          right_min_{right_min} {}

    [[nodiscard]] auto sample_value(const sampler_context &context, jint x,
                                    jint y, jint z) const -> float override {
        const float left = left_->sample_value(context, x, y, z);
        return left <= right_min_
                   ? left
                   : java_min(left, right_->sample_value(context, x, y, z));
    }
    void sample_volume(const sampler_context &context, buffer &output,
                       const volume &vol) const override {
        left_->sample_volume(context, output, vol);
        auto right_buffer = context.acquire(vol.size());
        right_->sample_volume(context, right_buffer, vol);
        for (std::size_t i = 0; i < output.size(); ++i) {
            const float right = right_buffer.get(i);
            if (right < output.get(i)) {
                output.set(i, right);
            }
        }
    }
    [[nodiscard]] auto range() const -> interval override {
        return interval::min(left_->range(), right_->range());
    }

  private:
    sampler_ptr left_;
    sampler_ptr right_;
    float right_min_;
};

class const_min_sampler final : public sampler {
  public:
    const_min_sampler(sampler_ptr left, float right)
        : left_{std::move(left)}, right_{right} {}

    [[nodiscard]] auto sample_value(const sampler_context &context, jint x,
                                    jint y, jint z) const -> float override {
        return java_min(left_->sample_value(context, x, y, z), right_);
    }
    void sample_volume(const sampler_context &context, buffer &output,
                       const volume &vol) const override {
        left_->sample_volume(context, output, vol);
        for (std::size_t i = 0; i < output.size(); ++i) {
            if (right_ < output.get(i)) {
                output.set(i, right_);
            }
        }
    }
    [[nodiscard]] auto range() const -> interval override {
        return interval::min(left_->range(), interval::of_exact(right_));
    }

  private:
    sampler_ptr left_;
    float right_;
};

class max_sampler final : public sampler {
  public:
    max_sampler(sampler_ptr left, sampler_ptr right, float right_max)
        : left_{std::move(left)}, right_{std::move(right)},
          right_max_{right_max} {}

    [[nodiscard]] auto sample_value(const sampler_context &context, jint x,
                                    jint y, jint z) const -> float override {
        const float left = left_->sample_value(context, x, y, z);
        return left >= right_max_
                   ? left
                   : java_max(left, right_->sample_value(context, x, y, z));
    }
    void sample_volume(const sampler_context &context, buffer &output,
                       const volume &vol) const override {
        left_->sample_volume(context, output, vol);
        auto right_buffer = context.acquire(vol.size());
        right_->sample_volume(context, right_buffer, vol);
        for (std::size_t i = 0; i < output.size(); ++i) {
            const float right = right_buffer.get(i);
            if (right > output.get(i)) {
                output.set(i, right);
            }
        }
    }
    [[nodiscard]] auto range() const -> interval override {
        return interval::max(left_->range(), right_->range());
    }

  private:
    sampler_ptr left_;
    sampler_ptr right_;
    float right_max_;
};

class const_max_sampler final : public sampler {
  public:
    const_max_sampler(sampler_ptr left, float right)
        : left_{std::move(left)}, right_{right} {}

    [[nodiscard]] auto sample_value(const sampler_context &context, jint x,
                                    jint y, jint z) const -> float override {
        return java_max(left_->sample_value(context, x, y, z), right_);
    }
    void sample_volume(const sampler_context &context, buffer &output,
                       const volume &vol) const override {
        left_->sample_volume(context, output, vol);
        for (std::size_t i = 0; i < output.size(); ++i) {
            if (right_ > output.get(i)) {
                output.set(i, right_);
            }
        }
    }
    [[nodiscard]] auto range() const -> interval override {
        return interval::max(left_->range(), interval::of_exact(right_));
    }

  private:
    sampler_ptr left_;
    float right_;
};

class unary_sampler final : public sampler {
  public:
    unary_sampler(sampler_ptr input, interval range,
                  std::function<float(float)> op)
        : input_{std::move(input)}, range_{range}, op_{std::move(op)} {}

    [[nodiscard]] auto sample_value(const sampler_context &context, jint x,
                                    jint y, jint z) const -> float override {
        return op_(input_->sample_value(context, x, y, z));
    }
    void sample_volume(const sampler_context &context, buffer &output,
                       const volume &vol) const override {
        input_->sample_volume(context, output, vol);
        for (std::size_t i = 0; i < output.size(); ++i) {
            output.set(i, op_(output.get(i)));
        }
    }
    [[nodiscard]] auto range() const -> interval override { return range_; }

  private:
    sampler_ptr input_;
    interval range_;
    std::function<float(float)> op_;
};

class clamp_sampler final : public sampler {
  public:
    clamp_sampler(sampler_ptr input, float min_value, float max_value)
        : input_{std::move(input)}, min_{min_value}, max_{max_value} {}

    [[nodiscard]] auto sample_value(const sampler_context &context, jint x,
                                    jint y, jint z) const -> float override {
        return std::clamp(input_->sample_value(context, x, y, z), min_, max_);
    }
    void sample_volume(const sampler_context &context, buffer &output,
                       const volume &vol) const override {
        input_->sample_volume(context, output, vol);
        for (std::size_t i = 0; i < output.size(); ++i) {
            output.set(i, std::clamp(output.get(i), min_, max_));
        }
    }
    [[nodiscard]] auto range() const -> interval override {
        return interval::clamp(input_->range(), min_, max_);
    }

  private:
    sampler_ptr input_;
    float min_;
    float max_;
};

class gradient_sampler final : public sampler {
  public:
    gradient_sampler(jint from_coordinate, jint to_coordinate, float from_value,
                     float to_value)
        : from_coordinate_{from_coordinate},
          min_coordinate_{std::min(from_coordinate, to_coordinate)},
          max_coordinate_{std::max(from_coordinate, to_coordinate)},
          from_value_{from_value}, to_value_{to_value},
          coordinate_factor_{
              (to_value - from_value) /
              static_cast<float>(to_coordinate - from_coordinate)} {}

    [[nodiscard]] auto sample_value(const sampler_context &, jint, jint block_y,
                                    jint) const -> float override {
        return compute(block_y);
    }
    void sample_volume(const sampler_context &, buffer &output,
                       const volume &vol) const override {
        for (jint y = 0; y < vol.size_y(); ++y) {
            const float value = compute(vol.block_y(y));
            for (jint z = 0; z < vol.size_z(); ++z) {
                for (jint x = 0; x < vol.size_x(); ++x) {
                    output.set(vol.index_unchecked(x, y, z), value);
                }
            }
        }
    }
    [[nodiscard]] auto range() const -> interval override {
        return interval::encapsulating(from_value_, to_value_);
    }

  private:
    [[nodiscard]] auto compute(jint coordinate) const -> float {
        const jint clamped =
            coordinate < min_coordinate_
                ? min_coordinate_
                : (coordinate > max_coordinate_ ? max_coordinate_ : coordinate);
        const jint relative = clamped - from_coordinate_;
        return from_value_ + static_cast<float>(relative) * coordinate_factor_;
    }

    jint from_coordinate_;
    jint min_coordinate_;
    jint max_coordinate_;
    float from_value_;
    float to_value_;
    float coordinate_factor_;
};

} // namespace

auto constant(float value) -> sampler_ptr {
    return std::make_unique<constant_sampler>(value);
}

auto add(sampler_ptr left, sampler_ptr right) -> sampler_ptr {
    if (const auto *value = left->constant_value(); value != nullptr) {
        return std::make_unique<const_add_sampler>(std::move(right), *value);
    }
    if (const auto *value = right->constant_value(); value != nullptr) {
        return std::make_unique<const_add_sampler>(std::move(left), *value);
    }
    return std::make_unique<add_sampler>(std::move(left), std::move(right));
}

auto sub(sampler_ptr left, sampler_ptr right) -> sampler_ptr {
    if (const auto *value = left->constant_value(); value != nullptr) {
        return std::make_unique<const_sub_sampler>(*value, std::move(right));
    }
    if (const auto *value = right->constant_value(); value != nullptr) {
        return std::make_unique<const_add_sampler>(std::move(left), -*value);
    }
    return std::make_unique<sub_sampler>(std::move(left), std::move(right));
}

auto mul(sampler_ptr left, sampler_ptr right) -> sampler_ptr {
    if (const auto *value = left->constant_value(); value != nullptr) {
        return std::make_unique<const_mul_sampler>(std::move(right), *value);
    }
    if (const auto *value = right->constant_value(); value != nullptr) {
        return std::make_unique<const_mul_sampler>(std::move(left), *value);
    }
    return std::make_unique<mul_sampler>(std::move(left), std::move(right));
}

auto div(sampler_ptr left, sampler_ptr right) -> sampler_ptr {
    if (const auto *value = left->constant_value(); value != nullptr) {
        return std::make_unique<const_div_sampler>(*value, std::move(right));
    }
    if (const auto *value = right->constant_value(); value != nullptr) {
        return std::make_unique<const_mul_sampler>(std::move(left),
                                                   1.0F / *value);
    }
    return std::make_unique<div_sampler>(std::move(left), std::move(right));
}

auto min(sampler_ptr left, sampler_ptr right) -> sampler_ptr {
    const interval left_range = left->range();
    const interval right_range = right->range();
    if (left_range.max() < right_range.min()) {
        return left;
    }
    if (right_range.max() < left_range.min()) {
        return right;
    }
    if (const auto *value = left->constant_value(); value != nullptr) {
        return std::make_unique<const_min_sampler>(std::move(right), *value);
    }
    if (const auto *value = right->constant_value(); value != nullptr) {
        return std::make_unique<const_min_sampler>(std::move(left), *value);
    }
    return std::make_unique<min_sampler>(std::move(left), std::move(right),
                                         right_range.min());
}

auto max(sampler_ptr left, sampler_ptr right) -> sampler_ptr {
    const interval left_range = left->range();
    const interval right_range = right->range();
    if (left_range.min() > right_range.max()) {
        return left;
    }
    if (right_range.min() > left_range.max()) {
        return right;
    }
    if (const auto *value = left->constant_value(); value != nullptr) {
        return std::make_unique<const_max_sampler>(std::move(right), *value);
    }
    if (const auto *value = right->constant_value(); value != nullptr) {
        return std::make_unique<const_max_sampler>(std::move(left), *value);
    }
    return std::make_unique<max_sampler>(std::move(left), std::move(right),
                                         right_range.max());
}

auto abs(sampler_ptr input) -> sampler_ptr {
    const interval range = interval::abs(input->range());
    return std::make_unique<unary_sampler>(std::move(input), range,
                                           [](float x) { return std::abs(x); });
}

auto square(sampler_ptr input) -> sampler_ptr {
    const interval range = interval::square(input->range());
    return std::make_unique<unary_sampler>(std::move(input), range,
                                           [](float x) { return x * x; });
}

auto cube(sampler_ptr input) -> sampler_ptr {
    const interval range = interval::map_monotonic(
        input->range(), [](float x) { return x * x * x; });
    return std::make_unique<unary_sampler>(std::move(input), range,
                                           [](float x) { return x * x * x; });
}

auto sqrt(sampler_ptr input) -> sampler_ptr {
    const interval range =
        interval::pow(input->range(), interval::of_exact(0.5F));
    return std::make_unique<unary_sampler>(
        std::move(input), range,
        [](float x) { return static_cast<float>(std::sqrt(x)); });
}

auto half_negative(sampler_ptr input) -> sampler_ptr {
    const interval range = interval::map_monotonic(
        input->range(), [](float x) { return leaky_relu(x, 0.5F); });
    return std::make_unique<unary_sampler>(
        std::move(input), range, [](float x) { return leaky_relu(x, 0.5F); });
}

auto quarter_negative(sampler_ptr input) -> sampler_ptr {
    const interval range = interval::map_monotonic(
        input->range(), [](float x) { return leaky_relu(x, 0.25F); });
    return std::make_unique<unary_sampler>(
        std::move(input), range, [](float x) { return leaky_relu(x, 0.25F); });
}

auto reciprocal(sampler_ptr input) -> sampler_ptr {
    const interval range =
        interval::div(interval::of_exact(1.0F), input->range());
    return std::make_unique<unary_sampler>(std::move(input), range,
                                           [](float x) { return 1.0F / x; });
}

auto negate(sampler_ptr input) -> sampler_ptr {
    const interval range =
        interval::sub(interval::of_exact(0.0F), input->range());
    return std::make_unique<unary_sampler>(std::move(input), range,
                                           [](float x) { return -x; });
}

auto squeeze(sampler_ptr input) -> sampler_ptr {
    const interval range = interval::map_monotonic(
        input->range(), [](float x) { return squeeze_value(x); });
    return std::make_unique<unary_sampler>(
        std::move(input), range, [](float x) { return squeeze_value(x); });
}

auto log(sampler_ptr input) -> sampler_ptr {
    const interval range = interval::log(input->range());
    return std::make_unique<unary_sampler>(
        std::move(input), range,
        [](float x) { return static_cast<float>(std::log(x)); });
}

auto sign(sampler_ptr input) -> sampler_ptr {
    const interval range = interval::sign(input->range());
    return std::make_unique<unary_sampler>(
        std::move(input), range,
        [](float x) { return x > 0.0F ? 1.0F : (x < 0.0F ? -1.0F : x); });
}

auto clamp(sampler_ptr input, float min_value, float max_value) -> sampler_ptr {
    return std::make_unique<clamp_sampler>(std::move(input), min_value,
                                           max_value);
}

auto gradient_y(jint from_coordinate, jint to_coordinate, float from_value,
                float to_value) -> sampler_ptr {
    return std::make_unique<gradient_sampler>(from_coordinate, to_coordinate,
                                              from_value, to_value);
}

} // namespace refrain::worldgen::density
