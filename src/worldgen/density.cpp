// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/density.hpp>

#include <refrain/worldgen/mth.hpp>

#include <memory>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <unordered_map>
#include <utility>

namespace refrain::worldgen::density {

namespace {

auto binary_search(jint from, jint to,
                   const std::function<bool(jint)> &condition) -> jint {
    jint length = to - from;
    while (length > 0) {
        const jint half = length / 2;
        const jint middle = from + half;
        if (condition(middle)) {
            length = half;
        } else {
            from = middle + 1;
            length -= half + 1;
        }
    }
    return from;
}

auto linear_extend(float input, const std::vector<float> &locations,
                   float value, const std::vector<float> &derivatives,
                   std::size_t index) -> float {
    const float derivative = derivatives[index];
    return derivative == 0.0F ? value
                              : value + derivative * (input - locations[index]);
}

} // namespace

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

class noise_sampler final : public sampler {
  public:
    noise_sampler(std::shared_ptr<const noise_stack> stack, double xz_scale,
                  double y_scale)
        : stack_{std::move(stack)}, xz_scale_{xz_scale}, y_scale_{y_scale} {}

    [[nodiscard]] auto sample_value(const sampler_context &, jint block_x,
                                    jint block_y, jint block_z) const
        -> float override {
        return stack_->get(static_cast<double>(block_x) * xz_scale_,
                           static_cast<double>(block_y) * y_scale_,
                           static_cast<double>(block_z) * xz_scale_);
    }
    void sample_volume(const sampler_context &, buffer &output,
                       const volume &vol) const override {
        output.fill(0.0F);
        stack_->add_to_volume(output, vol, xz_scale_, y_scale_, 1.0F);
    }
    [[nodiscard]] auto range() const -> interval override {
        return stack_->range();
    }

  private:
    std::shared_ptr<const noise_stack> stack_;
    double xz_scale_;
    double y_scale_;
};

class shifted_noise_xz_sampler final : public sampler {
  public:
    shifted_noise_xz_sampler(sampler_ptr shift_x, sampler_ptr shift_z,
                             double xz_scale, double y_scale,
                             std::shared_ptr<const noise_stack> stack)
        : shift_x_{std::move(shift_x)}, shift_z_{std::move(shift_z)},
          xz_scale_{xz_scale}, y_scale_{y_scale}, stack_{std::move(stack)} {}

    [[nodiscard]] auto sample_value(const sampler_context &context,
                                    jint block_x, jint block_y,
                                    jint block_z) const -> float override {
        const double x =
            static_cast<double>(block_x) * xz_scale_ +
            shift_x_->sample_value(context, block_x, block_y, block_z);
        const double y = static_cast<double>(block_y) * y_scale_;
        const double z =
            static_cast<double>(block_z) * xz_scale_ +
            shift_z_->sample_value(context, block_x, block_y, block_z);
        return stack_->get(x, y, z);
    }
    void sample_volume(const sampler_context &context, buffer &output,
                       const volume &vol) const override {
        shift_x_->sample_volume(context, output, vol);
        auto shift_z_buffer = context.acquire(vol.size());
        shift_z_->sample_volume(context, shift_z_buffer, vol);
        std::size_t index = 0;
        for (jint z = 0; z < vol.size_z(); ++z) {
            const double base_noise_z =
                static_cast<double>(vol.block_z(z)) * xz_scale_;
            for (jint x = 0; x < vol.size_x(); ++x) {
                const double base_noise_x =
                    static_cast<double>(vol.block_x(x)) * xz_scale_;
                for (jint y = 0; y < vol.size_y(); ++y) {
                    const double noise_x = base_noise_x + output.get(index);
                    const double noise_y =
                        static_cast<double>(vol.block_y(y)) * y_scale_;
                    const double noise_z =
                        base_noise_z + shift_z_buffer.get(index);
                    output.set(index, stack_->get(noise_x, noise_y, noise_z));
                    ++index;
                }
            }
        }
    }
    [[nodiscard]] auto range() const -> interval override {
        return stack_->range();
    }

  private:
    sampler_ptr shift_x_;
    sampler_ptr shift_z_;
    double xz_scale_;
    double y_scale_;
    std::shared_ptr<const noise_stack> stack_;
};

class shifted_noise_xyz_sampler final : public sampler {
  public:
    shifted_noise_xyz_sampler(sampler_ptr shift_x, sampler_ptr shift_y,
                              sampler_ptr shift_z, double xz_scale,
                              double y_scale,
                              std::shared_ptr<const noise_stack> stack)
        : shift_x_{std::move(shift_x)}, shift_y_{std::move(shift_y)},
          shift_z_{std::move(shift_z)}, xz_scale_{xz_scale}, y_scale_{y_scale},
          stack_{std::move(stack)} {}

    [[nodiscard]] auto sample_value(const sampler_context &context,
                                    jint block_x, jint block_y,
                                    jint block_z) const -> float override {
        const double x =
            static_cast<double>(block_x) * xz_scale_ +
            shift_x_->sample_value(context, block_x, block_y, block_z);
        const double y =
            static_cast<double>(block_y) * y_scale_ +
            shift_y_->sample_value(context, block_x, block_y, block_z);
        const double z =
            static_cast<double>(block_z) * xz_scale_ +
            shift_z_->sample_value(context, block_x, block_y, block_z);
        return stack_->get(x, y, z);
    }
    void sample_volume(const sampler_context &context, buffer &output,
                       const volume &vol) const override {
        shift_x_->sample_volume(context, output, vol);
        auto shift_y_buffer = context.acquire(vol.size());
        shift_y_->sample_volume(context, shift_y_buffer, vol);
        auto shift_z_buffer = context.acquire(vol.size());
        shift_z_->sample_volume(context, shift_z_buffer, vol);
        std::size_t index = 0;
        for (jint z = 0; z < vol.size_z(); ++z) {
            const double base_noise_z =
                static_cast<double>(vol.block_z(z)) * xz_scale_;
            for (jint x = 0; x < vol.size_x(); ++x) {
                const double base_noise_x =
                    static_cast<double>(vol.block_x(x)) * xz_scale_;
                for (jint y = 0; y < vol.size_y(); ++y) {
                    const double noise_x = base_noise_x + output.get(index);
                    const double noise_y =
                        static_cast<double>(vol.block_y(y)) * y_scale_ +
                        shift_y_buffer.get(index);
                    const double noise_z =
                        base_noise_z + shift_z_buffer.get(index);
                    output.set(index, stack_->get(noise_x, noise_y, noise_z));
                    ++index;
                }
            }
        }
    }
    [[nodiscard]] auto range() const -> interval override {
        return stack_->range();
    }

  private:
    sampler_ptr shift_x_;
    sampler_ptr shift_y_;
    sampler_ptr shift_z_;
    double xz_scale_;
    double y_scale_;
    std::shared_ptr<const noise_stack> stack_;
};

class shift_b_sampler final : public sampler {
  public:
    explicit shift_b_sampler(std::shared_ptr<const noise_stack> stack)
        : stack_{std::move(stack)} {}

    [[nodiscard]] auto sample_value(const sampler_context &, jint block_x,
                                    jint /*block_y*/, jint block_z) const
        -> float override {
        return stack_->get(static_cast<double>(block_z) * 0.25,
                           static_cast<double>(block_x) * 0.25, 0.0) *
               4.0F;
    }
    void sample_volume(const sampler_context &context, buffer &output,
                       const volume &vol) const override {
        const volume transposed{vol.size_z(),       vol.size_x(),       1,
                                vol.min_block_z(),  vol.min_block_x(),  0,
                                vol.step_block_z(), vol.step_block_x(), 1};
        auto transposed_buffer = context.acquire(transposed.size());
        transposed_buffer.fill(0.0F);
        stack_->add_to_volume(transposed_buffer, transposed, 0.25, 0.25, 4.0F);
        for (jint z = 0; z < vol.size_z(); ++z) {
            for (jint x = 0; x < vol.size_x(); ++x) {
                const float value =
                    transposed_buffer.get(transposed.index_unchecked(z, x, 0));
                output.set_range(vol.index_unchecked(x, 0, z), vol.size_y(),
                                 value);
            }
        }
    }
    [[nodiscard]] auto range() const -> interval override {
        return interval::mul(stack_->range(), interval::of_exact(4.0F));
    }

  private:
    std::shared_ptr<const noise_stack> stack_;
};

class interpolated_sampler final : public sampler {
  public:
    interpolated_sampler(sampler_ptr input, jint cell_size_xz, jint cell_size_y)
        : input_{std::move(input)}, cell_size_xz_{cell_size_xz},
          cell_size_y_{cell_size_y},
          cell_size_xz_inv_{1.0F / static_cast<float>(cell_size_xz)},
          cell_size_y_inv_{1.0F / static_cast<float>(cell_size_y)} {}

    [[nodiscard]] auto sample_value(const sampler_context &context,
                                    jint block_x, jint block_y,
                                    jint block_z) const -> float override {
        const jint x_in_cell = floor_mod(block_x, cell_size_xz_);
        const jint y_in_cell = floor_mod(block_y, cell_size_y_);
        const jint z_in_cell = floor_mod(block_z, cell_size_xz_);
        if (x_in_cell == 0 && y_in_cell == 0 && z_in_cell == 0) {
            return input_->sample_value(context, block_x, block_y, block_z);
        }
        const volume vol{2,
                         2,
                         2,
                         block_x - x_in_cell,
                         block_y - y_in_cell,
                         block_z - z_in_cell,
                         cell_size_xz_,
                         cell_size_y_,
                         cell_size_xz_};
        auto input_buffer = context.acquire(vol.size());
        input_->sample_volume(context, input_buffer, vol);
        return mth::lerp3(
            static_cast<float>(x_in_cell) / static_cast<float>(cell_size_xz_),
            static_cast<float>(y_in_cell) / static_cast<float>(cell_size_y_),
            static_cast<float>(z_in_cell) / static_cast<float>(cell_size_xz_),
            input_buffer.get(vol.index_unchecked(0, 0, 0)),
            input_buffer.get(vol.index_unchecked(1, 0, 0)),
            input_buffer.get(vol.index_unchecked(0, 1, 0)),
            input_buffer.get(vol.index_unchecked(1, 1, 0)),
            input_buffer.get(vol.index_unchecked(0, 0, 1)),
            input_buffer.get(vol.index_unchecked(1, 0, 1)),
            input_buffer.get(vol.index_unchecked(0, 1, 1)),
            input_buffer.get(vol.index_unchecked(1, 1, 1)));
    }

    void sample_volume(const sampler_context &context, buffer &output,
                       const volume &vol) const override {
        if ((vol.step_block_x() == cell_size_xz_ || vol.size_x() == 1) &&
            (vol.step_block_y() == cell_size_y_ || vol.size_y() == 1) &&
            (vol.step_block_z() == cell_size_xz_ || vol.size_z() == 1) &&
            floor_mod(vol.min_block_x(), cell_size_xz_) == 0 &&
            floor_mod(vol.min_block_y(), cell_size_y_) == 0 &&
            floor_mod(vol.min_block_z(), cell_size_xz_) == 0) {
            input_->sample_volume(context, output, vol);
        } else if (vol.step_block_x() == 1 && vol.step_block_y() == 1 &&
                   vol.step_block_z() == 1) {
            sample_with_block_step(context, output, vol);
        } else {
            sample_with_non_block_step(context, output, vol);
        }
    }

    [[nodiscard]] auto range() const -> interval override {
        return input_->range();
    }

  private:
    void sample_with_non_block_step(const sampler_context &context,
                                    buffer &output, const volume &vol) const {
        const volume block_volume{vol.size_x() * vol.step_block_x(),
                                  vol.size_y() * vol.step_block_y(),
                                  vol.size_z() * vol.step_block_z(),
                                  vol.min_block_x(),
                                  vol.min_block_y(),
                                  vol.min_block_z()};
        auto block_buffer = context.acquire(block_volume.size());
        sample_with_block_step(context, block_buffer, block_volume);
        for (jint z = 0; z < vol.size_z(); ++z) {
            for (jint x = 0; x < vol.size_x(); ++x) {
                for (jint y = 0; y < vol.size_y(); ++y) {
                    const float value =
                        block_buffer.get(block_volume.index_unchecked(
                            x * vol.step_block_x(), y * vol.step_block_y(),
                            z * vol.step_block_z()));
                    output.set(vol.index_unchecked(x, y, z), value);
                }
            }
        }
    }

    void sample_with_block_step(const sampler_context &context, buffer &output,
                                const volume &vol) const {
        const jint min_cell_x = floor_div(vol.min_block_x(), cell_size_xz_);
        const jint min_cell_y = floor_div(vol.min_block_y(), cell_size_y_);
        const jint min_cell_z = floor_div(vol.min_block_z(), cell_size_xz_);
        const jint max_cell_x = floor_div(vol.max_block_x(), cell_size_xz_);
        const jint max_cell_y = floor_div(vol.max_block_y(), cell_size_y_);
        const jint max_cell_z = floor_div(vol.max_block_z(), cell_size_xz_);
        const jint cell_count_x = max_cell_x - min_cell_x + 1;
        const jint cell_count_y = max_cell_y - min_cell_y + 1;
        const jint cell_count_z = max_cell_z - min_cell_z + 1;
        const volume cell_volume{
            floor_mod(vol.max_block_x(), cell_size_xz_) == 0 ? cell_count_x
                                                             : cell_count_x + 1,
            floor_mod(vol.max_block_y(), cell_size_y_) == 0 ? cell_count_y
                                                            : cell_count_y + 1,
            floor_mod(vol.max_block_z(), cell_size_xz_) == 0 ? cell_count_z
                                                             : cell_count_z + 1,
            min_cell_x * cell_size_xz_,
            min_cell_y * cell_size_y_,
            min_cell_z * cell_size_xz_,
            cell_size_xz_,
            cell_size_y_,
            cell_size_xz_};
        auto cell_buffer = context.acquire(cell_volume.size());
        input_->sample_volume(context, cell_buffer, cell_volume);

        for (jint cell_z = 0; cell_z < cell_count_z; ++cell_z) {
            const jint next_cell_z =
                std::min(cell_z + 1, cell_volume.size_z() - 1);
            for (jint cell_x = 0; cell_x < cell_count_x; ++cell_x) {
                const jint next_cell_x =
                    std::min(cell_x + 1, cell_volume.size_x() - 1);
                float v000 = cell_buffer.get(
                    cell_volume.index_unchecked(cell_x, 0, cell_z));
                float v100 = cell_buffer.get(
                    cell_volume.index_unchecked(next_cell_x, 0, cell_z));
                float v001 = cell_buffer.get(
                    cell_volume.index_unchecked(cell_x, 0, next_cell_z));
                float v101 = cell_buffer.get(
                    cell_volume.index_unchecked(next_cell_x, 0, next_cell_z));
                for (jint cell_y = 0; cell_y < cell_count_y; ++cell_y) {
                    const jint next_cell_y =
                        std::min(cell_y + 1, cell_volume.size_y() - 1);
                    const float v010 =
                        cell_buffer.get(cell_volume.index_unchecked(
                            cell_x, next_cell_y, cell_z));
                    const float v110 =
                        cell_buffer.get(cell_volume.index_unchecked(
                            next_cell_x, next_cell_y, cell_z));
                    const float v011 =
                        cell_buffer.get(cell_volume.index_unchecked(
                            cell_x, next_cell_y, next_cell_z));
                    const float v111 =
                        cell_buffer.get(cell_volume.index_unchecked(
                            next_cell_x, next_cell_y, next_cell_z));
                    fill_cell(output, vol, cell_volume, cell_x, cell_y, cell_z,
                              v000, v100, v010, v110, v001, v101, v011, v111);
                    v000 = v010;
                    v100 = v110;
                    v001 = v011;
                    v101 = v111;
                }
            }
        }
    }

    void fill_cell(buffer &output, const volume &output_volume,
                   const volume &cell_volume, jint cell_x, jint cell_y,
                   jint cell_z, float v000, float v100, float v010, float v110,
                   float v001, float v101, float v011, float v111) const {
        const jint cell_output_x =
            cell_volume.block_x(cell_x) - output_volume.min_block_x();
        const jint cell_output_y =
            cell_volume.block_y(cell_y) - output_volume.min_block_y();
        const jint cell_output_z =
            cell_volume.block_z(cell_z) - output_volume.min_block_z();
        const jint x0 = std::max(0, -cell_output_x);
        const jint y0 = std::max(0, -cell_output_y);
        const jint z0 = std::max(0, -cell_output_z);
        const jint x1 =
            std::min(cell_size_xz_, output_volume.size_x() - cell_output_x) - 1;
        const jint y1 =
            std::min(cell_size_y_, output_volume.size_y() - cell_output_y) - 1;
        const jint z1 =
            std::min(cell_size_xz_, output_volume.size_z() - cell_output_z) - 1;

        for (jint z = z0; z <= z1; ++z) {
            const jint output_z = cell_output_z + z;
            const float alpha_z = static_cast<float>(z) * cell_size_xz_inv_;
            const float v00 = mth::lerp(alpha_z, v000, v001);
            const float v01 = mth::lerp(alpha_z, v010, v011);
            const float v10 = mth::lerp(alpha_z, v100, v101);
            const float v11 = mth::lerp(alpha_z, v110, v111);
            for (jint x = x0; x <= x1; ++x) {
                const jint output_x = cell_output_x + x;
                const float alpha_x = static_cast<float>(x) * cell_size_xz_inv_;
                const float v_0 = mth::lerp(alpha_x, v00, v10);
                const float v_1 = mth::lerp(alpha_x, v01, v11);
                const float value_step = (v_1 - v_0) * cell_size_y_inv_;
                float value = v_0 + value_step * static_cast<float>(y0);
                std::size_t output_index = output_volume.index_unchecked(
                    output_x, cell_output_y + y0, output_z);
                for (jint y = y0; y <= y1; ++y) {
                    output.set(output_index, value);
                    ++output_index;
                    value += value_step;
                }
            }
        }
    }

    sampler_ptr input_;
    jint cell_size_xz_;
    jint cell_size_y_;
    float cell_size_xz_inv_;
    float cell_size_y_inv_;
};

class range_choice_sampler final : public sampler {
  public:
    range_choice_sampler(sampler_ptr input, float min_inclusive,
                         float max_exclusive, sampler_ptr when_in_range,
                         sampler_ptr when_out_of_range)
        : input_{std::move(input)}, min_inclusive_{min_inclusive},
          max_exclusive_{max_exclusive},
          when_in_range_{std::move(when_in_range)},
          when_out_of_range_{std::move(when_out_of_range)} {}

    [[nodiscard]] auto sample_value(const sampler_context &context, jint x,
                                    jint y, jint z) const -> float override {
        const float input = input_->sample_value(context, x, y, z);
        return input >= min_inclusive_ && input < max_exclusive_
                   ? when_in_range_->sample_value(context, x, y, z)
                   : when_out_of_range_->sample_value(context, x, y, z);
    }
    void sample_volume(const sampler_context &context, buffer &output,
                       const volume &vol) const override {
        when_in_range_->sample_volume(context, output, vol);
        auto input_buffer = context.acquire(vol.size());
        input_->sample_volume(context, input_buffer, vol);
        auto out_buffer = context.acquire(vol.size());
        when_out_of_range_->sample_volume(context, out_buffer, vol);
        for (std::size_t i = 0; i < output.size(); ++i) {
            const float input = input_buffer.get(i);
            if (!(input >= min_inclusive_) || !(input < max_exclusive_)) {
                output.set(i, out_buffer.get(i));
            }
        }
    }
    [[nodiscard]] auto range() const -> interval override {
        return interval::encapsulating(when_in_range_->range(),
                                       when_out_of_range_->range());
    }

  private:
    sampler_ptr input_;
    float min_inclusive_;
    float max_exclusive_;
    sampler_ptr when_in_range_;
    sampler_ptr when_out_of_range_;
};

class const_range_choice_sampler final : public sampler {
  public:
    const_range_choice_sampler(sampler_ptr input, float min_inclusive,
                               float max_exclusive, float when_in_range,
                               float when_out_of_range)
        : input_{std::move(input)}, min_inclusive_{min_inclusive},
          max_exclusive_{max_exclusive}, when_in_range_{when_in_range},
          when_out_of_range_{when_out_of_range} {}

    [[nodiscard]] auto sample_value(const sampler_context &context, jint x,
                                    jint y, jint z) const -> float override {
        return choose(input_->sample_value(context, x, y, z));
    }
    void sample_volume(const sampler_context &context, buffer &output,
                       const volume &vol) const override {
        input_->sample_volume(context, output, vol);
        for (std::size_t i = 0; i < output.size(); ++i) {
            output.set(i, choose(output.get(i)));
        }
    }
    [[nodiscard]] auto range() const -> interval override {
        return interval::encapsulating(when_in_range_, when_out_of_range_);
    }

  private:
    [[nodiscard]] auto choose(float input) const -> float {
        return input >= min_inclusive_ && input < max_exclusive_
                   ? when_in_range_
                   : when_out_of_range_;
    }

    sampler_ptr input_;
    float min_inclusive_;
    float max_exclusive_;
    float when_in_range_;
    float when_out_of_range_;
};

class lerp_sampler final : public sampler {
  public:
    lerp_sampler(sampler_ptr alpha, sampler_ptr first, sampler_ptr second)
        : alpha_{std::move(alpha)}, first_{std::move(first)},
          second_{std::move(second)} {}

    [[nodiscard]] auto sample_value(const sampler_context &context, jint x,
                                    jint y, jint z) const -> float override {
        const float alpha = alpha_->sample_value(context, x, y, z);
        if (alpha == 0.0F) {
            return first_->sample_value(context, x, y, z);
        }
        if (alpha == 1.0F) {
            return second_->sample_value(context, x, y, z);
        }
        return mth::lerp(alpha, first_->sample_value(context, x, y, z),
                         second_->sample_value(context, x, y, z));
    }
    void sample_volume(const sampler_context &context, buffer &output,
                       const volume &vol) const override {
        alpha_->sample_volume(context, output, vol);
        auto first_buffer = context.acquire(vol.size());
        first_->sample_volume(context, first_buffer, vol);
        auto second_buffer = context.acquire(vol.size());
        second_->sample_volume(context, second_buffer, vol);
        for (std::size_t i = 0; i < output.size(); ++i) {
            const float alpha = output.get(i);
            const float first = first_buffer.get(i);
            const float second = second_buffer.get(i);
            if (alpha == 0.0F) {
                output.set(i, first);
            } else if (alpha == 1.0F) {
                output.set(i, second);
            } else {
                output.set(i, mth::lerp(alpha, first, second));
            }
        }
    }
    [[nodiscard]] auto range() const -> interval override {
        return interval::lerp(alpha_->range(), first_->range(),
                              second_->range());
    }

  private:
    sampler_ptr alpha_;
    sampler_ptr first_;
    sampler_ptr second_;
};

class const_first_lerp_sampler final : public sampler {
  public:
    const_first_lerp_sampler(sampler_ptr alpha, float first, sampler_ptr second)
        : alpha_{std::move(alpha)}, first_{first}, second_{std::move(second)} {}

    [[nodiscard]] auto sample_value(const sampler_context &context, jint x,
                                    jint y, jint z) const -> float override {
        const float alpha = alpha_->sample_value(context, x, y, z);
        if (alpha == 0.0F) {
            return first_;
        }
        if (alpha == 1.0F) {
            return second_->sample_value(context, x, y, z);
        }
        return mth::lerp(alpha, first_,
                         second_->sample_value(context, x, y, z));
    }
    void sample_volume(const sampler_context &context, buffer &output,
                       const volume &vol) const override {
        alpha_->sample_volume(context, output, vol);
        auto second_buffer = context.acquire(vol.size());
        second_->sample_volume(context, second_buffer, vol);
        for (std::size_t i = 0; i < output.size(); ++i) {
            const float alpha = output.get(i);
            const float second = second_buffer.get(i);
            if (alpha == 0.0F) {
                output.set(i, first_);
            } else if (alpha == 1.0F) {
                output.set(i, second);
            } else {
                output.set(i, mth::lerp(alpha, first_, second));
            }
        }
    }
    [[nodiscard]] auto range() const -> interval override {
        return interval::lerp(alpha_->range(), interval::of_exact(first_),
                              second_->range());
    }

  private:
    sampler_ptr alpha_;
    float first_;
    sampler_ptr second_;
};

class const_second_lerp_sampler final : public sampler {
  public:
    const_second_lerp_sampler(sampler_ptr alpha, sampler_ptr first,
                              float second)
        : alpha_{std::move(alpha)}, first_{std::move(first)}, second_{second} {}

    [[nodiscard]] auto sample_value(const sampler_context &context, jint x,
                                    jint y, jint z) const -> float override {
        const float alpha = alpha_->sample_value(context, x, y, z);
        if (alpha == 0.0F) {
            return first_->sample_value(context, x, y, z);
        }
        if (alpha == 1.0F) {
            return second_;
        }
        return mth::lerp(alpha, first_->sample_value(context, x, y, z),
                         second_);
    }
    void sample_volume(const sampler_context &context, buffer &output,
                       const volume &vol) const override {
        alpha_->sample_volume(context, output, vol);
        auto first_buffer = context.acquire(vol.size());
        first_->sample_volume(context, first_buffer, vol);
        for (std::size_t i = 0; i < output.size(); ++i) {
            const float alpha = output.get(i);
            const float first = first_buffer.get(i);
            if (alpha == 0.0F) {
                output.set(i, first);
            } else if (alpha == 1.0F) {
                output.set(i, second_);
            } else {
                output.set(i, mth::lerp(alpha, first, second_));
            }
        }
    }
    [[nodiscard]] auto range() const -> interval override {
        return interval::lerp(alpha_->range(), first_->range(),
                              interval::of_exact(second_));
    }

  private:
    sampler_ptr alpha_;
    sampler_ptr first_;
    float second_;
};

class spline_sampler_impl final : public sampler {
  public:
    explicit spline_sampler_impl(std::shared_ptr<const spline> value)
        : value_{std::move(value)} {
        collect(*value_);
    }

    [[nodiscard]] auto sample_value(const sampler_context &context, jint x,
                                    jint y, jint z) const -> float override {
        std::vector<float> caches(coordinate_index_.size(),
                                  std::numeric_limits<float>::quiet_NaN());
        point_input input{context, x, y, z, caches, coordinate_index_};
        return evaluate(*value_, input);
    }

    void sample_volume(const sampler_context &context, buffer &output,
                       const volume &vol) const override {
        std::vector<buffer> buffers;
        buffers.reserve(coordinate_index_.size());
        for (std::size_t i = 0; i < coordinate_index_.size(); ++i) {
            buffers.emplace_back(vol.size());
        }
        std::vector<bool> sampled(coordinate_index_.size(), false);
        volume_input input{context, vol, buffers,
                           sampled, 0,   coordinate_index_};
        for (std::size_t i = 0; i < output.size(); ++i) {
            input.index = i;
            output.set(i, evaluate(*value_, input));
        }
    }

    [[nodiscard]] auto range() const -> interval override {
        return value_->range();
    }

  private:
    struct point_input {
        const sampler_context &context;
        jint x;
        jint y;
        jint z;
        std::vector<float> &caches;
        const std::unordered_map<const sampler *, std::size_t> &indices;

        auto coordinate(const sampler &which) -> float {
            const std::size_t index = indices.at(&which);
            float &cached = caches[index];
            if (!std::isnan(cached)) {
                return cached;
            }
            cached = which.sample_value(context, x, y, z);
            return cached;
        }
    };

    struct volume_input {
        const sampler_context &context;
        const volume &vol;
        std::vector<buffer> &buffers;
        std::vector<bool> &sampled;
        std::size_t index;
        const std::unordered_map<const sampler *, std::size_t> &indices;

        auto coordinate(const sampler &which) -> float {
            const std::size_t coordinate_index = indices.at(&which);
            if (!sampled[coordinate_index]) {
                which.sample_volume(context, buffers[coordinate_index], vol);
                sampled[coordinate_index] = true;
            }
            return buffers[coordinate_index].get(index);
        }
    };

    template <typename Input>
    static auto evaluate(const spline &value, Input &input) -> float {
        if (value.constant_) {
            return value.constant_value_;
        }
        const float input_value = input.coordinate(*value.coordinate_);
        const jint last_index = static_cast<jint>(value.locations_.size()) - 1;
        const jint start =
            binary_search(
                0, static_cast<jint>(value.locations_.size()),
                [&](jint i) {
                    return input_value <
                           value.locations_[static_cast<std::size_t>(i)];
                }) -
            1;
        if (start < 0) {
            return linear_extend(input_value, value.locations_,
                                 evaluate(*value.values_[0], input),
                                 value.derivatives_, 0);
        }
        if (start == last_index) {
            const auto index = static_cast<std::size_t>(last_index);
            return linear_extend(input_value, value.locations_,
                                 evaluate(*value.values_[index], input),
                                 value.derivatives_, index);
        }
        const auto start_index = static_cast<std::size_t>(start);
        const float x1 = value.locations_[start_index];
        const float x2 = value.locations_[start_index + 1];
        const float t = (input_value - x1) / (x2 - x1);
        const float y1 = evaluate(*value.values_[start_index], input);
        const float y2 = evaluate(*value.values_[start_index + 1], input);
        const float d1 = value.derivatives_[start_index];
        const float d2 = value.derivatives_[start_index + 1];
        const float a = d1 * (x2 - x1) - (y2 - y1);
        const float b = -d2 * (x2 - x1) + (y2 - y1);
        return mth::lerp(t, y1, y2) + t * (1.0F - t) * mth::lerp(t, a, b);
    }

    void collect(const spline &value) {
        if (value.constant_) {
            return;
        }
        if (!coordinate_index_.contains(value.coordinate_.get())) {
            const auto index = coordinate_index_.size();
            coordinate_index_.emplace(value.coordinate_.get(), index);
        }
        for (const auto &child : value.values_) {
            collect(*child);
        }
    }

    std::shared_ptr<const spline> value_;
    std::unordered_map<const sampler *, std::size_t> coordinate_index_;
};

class context_bound_sampler final : public sampler {
  public:
    context_bound_sampler(context_key key, sampler_ptr fallback, interval range)
        : key_{key}, fallback_{std::move(fallback)}, range_{range} {}

    [[nodiscard]] auto sample_value(const sampler_context &context, jint x,
                                    jint y, jint z) const -> float override {
        return resolve(context).sample_value(context, x, y, z);
    }
    void sample_volume(const sampler_context &context, buffer &output,
                       const volume &vol) const override {
        resolve(context).sample_volume(context, output, vol);
    }
    [[nodiscard]] auto range() const -> interval override { return range_; }

  private:
    [[nodiscard]] auto resolve(const sampler_context &context) const
        -> const sampler & {
        const sampler *bound = context.get_field(key_);
        return bound != nullptr ? *bound : *fallback_;
    }

    context_key key_;
    sampler_ptr fallback_;
    interval range_;
};

class range_override_sampler final : public sampler {
  public:
    range_override_sampler(sampler_ptr input, interval range)
        : input_{std::move(input)}, range_{range} {}

    [[nodiscard]] auto sample_value(const sampler_context &context, jint x,
                                    jint y, jint z) const -> float override {
        return input_->sample_value(context, x, y, z);
    }
    void sample_volume(const sampler_context &context, buffer &output,
                       const volume &vol) const override {
        input_->sample_volume(context, output, vol);
    }
    [[nodiscard]] auto range() const -> interval override { return range_; }

  private:
    sampler_ptr input_;
    interval range_;
};

class blend_density_sampler final : public sampler {
  public:
    explicit blend_density_sampler(sampler_ptr input)
        : input_{std::move(input)} {}

    [[nodiscard]] auto sample_value(const sampler_context &context, jint x,
                                    jint y, jint z) const -> float override {
        return input_->sample_value(context, x, y, z);
    }
    void sample_volume(const sampler_context &context, buffer &output,
                       const volume &vol) const override {
        input_->sample_volume(context, output, vol);
    }
    [[nodiscard]] auto range() const -> interval override {
        return input_->range();
    }

  private:
    sampler_ptr input_;
};

class slice_y_sampler final : public sampler {
  public:
    slice_y_sampler(sampler_ptr input, jint y)
        : input_{std::move(input)}, y_{y} {}

    [[nodiscard]] auto sample_value(const sampler_context &context, jint x,
                                    jint /*y*/, jint z) const
        -> float override {
        return input_->sample_value(context, x, y_, z);
    }
    void sample_volume(const sampler_context &context, buffer &output,
                       const volume &vol) const override {
        if (vol.size_y() == 1 && vol.min_block_y() == y_) {
            input_->sample_volume(context, output, vol);
            return;
        }
        const volume input_volume{vol.size_x(),
                                  1,
                                  vol.size_z(),
                                  vol.min_block_x(),
                                  y_,
                                  vol.min_block_z(),
                                  vol.step_block_x(),
                                  vol.step_block_y(),
                                  vol.step_block_z()};
        auto input_buffer = context.acquire(input_volume.size());
        input_->sample_volume(context, input_buffer, input_volume);
        for (jint z = 0; z < vol.size_z(); ++z) {
            for (jint x = 0; x < vol.size_x(); ++x) {
                const float value =
                    input_buffer.get(input_volume.index_unchecked(x, 0, z));
                output.set_range(vol.index_unchecked(x, 0, z), vol.size_y(),
                                 value);
            }
        }
    }
    [[nodiscard]] auto range() const -> interval override {
        return input_->range();
    }

  private:
    sampler_ptr input_;
    jint y_;
};

class find_top_surface_sampler final : public sampler {
  public:
    find_top_surface_sampler(sampler_ptr density, sampler_ptr upper_bound,
                             jint lower_bound, jint cell_height)
        : density_{std::move(density)}, upper_bound_{std::move(upper_bound)},
          lower_bound_{lower_bound}, cell_height_{cell_height} {}

    [[nodiscard]] auto sample_value(const sampler_context &context, jint x,
                                    jint y, jint z) const -> float override {
        const float upper = upper_bound_->sample_value(context, x, y, z);
        return static_cast<float>(find_surface_from(context, x, z, upper));
    }
    void sample_volume(const sampler_context &context, buffer &output,
                       const volume &vol) const override {
        if (vol.size_y() != 1) {
            throw std::invalid_argument(
                "find_top_surface volume must have sizeY=1");
        }
        upper_bound_->sample_volume(context, output, vol);
        std::size_t index = 0;
        for (jint z = 0; z < vol.size_z(); ++z) {
            for (jint x = 0; x < vol.size_x(); ++x) {
                const float upper = output.get(index);
                output.set(index, static_cast<float>(find_surface_from(
                                      context, vol.block_x(x), vol.block_z(z),
                                      upper)));
                ++index;
            }
        }
    }
    [[nodiscard]] auto range() const -> interval override {
        return interval::of(static_cast<float>(lower_bound_),
                            std::max(static_cast<float>(lower_bound_),
                                     upper_bound_->range().max()));
    }

  private:
    [[nodiscard]] auto find_surface_from(const sampler_context &context, jint x,
                                         jint z, float upper) const -> jint {
        const jint top_y =
            mth::floor(upper / static_cast<float>(cell_height_)) * cell_height_;
        if (top_y <= lower_bound_) {
            return lower_bound_;
        }
        for (jint probe_y = top_y; probe_y >= lower_bound_;
             probe_y -= cell_height_) {
            if (density_->sample_value(context, x, probe_y, z) > 0.0F) {
                return probe_y;
            }
        }
        return lower_bound_;
    }

    sampler_ptr density_;
    sampler_ptr upper_bound_;
    jint lower_bound_;
    jint cell_height_;
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

auto noise(std::shared_ptr<const noise_stack> stack, double xz_scale,
           double y_scale) -> sampler_ptr {
    return std::make_unique<noise_sampler>(std::move(stack), xz_scale, y_scale);
}

auto shifted_noise_xz(sampler_ptr shift_x, sampler_ptr shift_z, double xz_scale,
                      double y_scale, std::shared_ptr<const noise_stack> stack)
    -> sampler_ptr {
    return std::make_unique<shifted_noise_xz_sampler>(
        std::move(shift_x), std::move(shift_z), xz_scale, y_scale,
        std::move(stack));
}

auto shifted_noise_xyz(sampler_ptr shift_x, sampler_ptr shift_y,
                       sampler_ptr shift_z, double xz_scale, double y_scale,
                       std::shared_ptr<const noise_stack> stack)
    -> sampler_ptr {
    return std::make_unique<shifted_noise_xyz_sampler>(
        std::move(shift_x), std::move(shift_y), std::move(shift_z), xz_scale,
        y_scale, std::move(stack));
}

auto shift(std::shared_ptr<const noise_stack> stack) -> sampler_ptr {
    return mul(noise(stack, 0.25, 0.25), constant(4.0F));
}

auto shift_a(std::shared_ptr<const noise_stack> stack) -> sampler_ptr {
    return mul(noise(std::move(stack), 0.25, 0.0), constant(4.0F));
}

auto shift_b(std::shared_ptr<const noise_stack> stack) -> sampler_ptr {
    return std::make_unique<shift_b_sampler>(std::move(stack));
}

auto interpolated(sampler_ptr input, jint cell_size_xz, jint cell_size_y)
    -> sampler_ptr {
    return std::make_unique<interpolated_sampler>(std::move(input),
                                                  cell_size_xz, cell_size_y);
}

auto range_choice(sampler_ptr input, float min_inclusive, float max_exclusive,
                  sampler_ptr when_in_range, sampler_ptr when_out_of_range)
    -> sampler_ptr {
    const float *in_value = when_in_range->constant_value();
    const float *out_value = when_out_of_range->constant_value();
    if (in_value != nullptr && out_value != nullptr) {
        return std::make_unique<const_range_choice_sampler>(
            std::move(input), min_inclusive, max_exclusive, *in_value,
            *out_value);
    }
    return std::make_unique<range_choice_sampler>(
        std::move(input), min_inclusive, max_exclusive,
        std::move(when_in_range), std::move(when_out_of_range));
}

auto lerp(sampler_ptr alpha, sampler_ptr first, sampler_ptr second)
    -> sampler_ptr {
    if (const auto *value = first->constant_value(); value != nullptr) {
        return std::make_unique<const_first_lerp_sampler>(
            std::move(alpha), *value, std::move(second));
    }
    if (const auto *value = second->constant_value(); value != nullptr) {
        return std::make_unique<const_second_lerp_sampler>(
            std::move(alpha), std::move(first), *value);
    }
    return std::make_unique<lerp_sampler>(std::move(alpha), std::move(first),
                                          std::move(second));
}

auto cache(sampler_ptr input) -> sampler_ptr { return input; }

auto blend_alpha() -> sampler_ptr {
    return std::make_unique<context_bound_sampler>(
        context_key::blend_alpha, constant(1.0F), interval::of(0.0F, 1.0F));
}

auto blend_offset() -> sampler_ptr {
    return std::make_unique<context_bound_sampler>(
        context_key::blend_offset, constant(0.0F), interval::infinite());
}

auto beardifier() -> sampler_ptr {
    return std::make_unique<context_bound_sampler>(
        context_key::beardifier, constant(0.0F), interval::infinite());
}

auto blend_density(sampler_ptr input) -> sampler_ptr {
    return std::make_unique<blend_density_sampler>(std::move(input));
}

namespace {

auto create_fbm(random_source &random, jint first_octave, double smear_scale_y,
                double value_factor) -> std::vector<noise_stack::layer> {
    const jint octaves = -first_octave + 1;
    double factor = 1.0;
    value_factor /= std::pow(2.0, static_cast<double>(octaves)) - 1.0;
    std::vector<noise_stack::layer> layers;
    layers.reserve(static_cast<std::size_t>(octaves));
    for (jint i = octaves - 1; i >= 0; --i) {
        layers.push_back(noise_stack::layer{
            .source = std::make_unique<smeared_perlin_noise>(
                random, smear_scale_y * factor),
            .frequency = factor,
            .amplitude = static_cast<float>(value_factor),
        });
        factor /= 2.0;
        value_factor *= 2.0;
    }
    return layers;
}

auto compute_fbm_range(jint first_octave, double smear_scale_y,
                       double value_factor) -> interval {
    const jint octaves = -first_octave + 1;
    double factor = 1.0;
    value_factor /= std::pow(2.0, static_cast<double>(octaves)) - 1.0;
    interval range = interval::of_exact(0.0F);
    for (jint i = octaves - 1; i >= 0; --i) {
        range = interval::add(
            range, interval::mul(
                       smeared_perlin_noise::range_for(smear_scale_y * factor),
                       interval::of_exact(static_cast<float>(value_factor))));
        factor /= 2.0;
        value_factor *= 2.0;
    }
    return range;
}

} // namespace

auto slice_y(sampler_ptr input, jint y) -> sampler_ptr {
    return std::make_unique<slice_y_sampler>(std::move(input), y);
}

auto find_top_surface(sampler_ptr density, sampler_ptr upper_bound,
                      jint lower_bound, jint cell_height) -> sampler_ptr {
    return slice_y(std::make_unique<find_top_surface_sampler>(
                       std::move(density), std::move(upper_bound), lower_bound,
                       cell_height),
                   0);
}

auto blended_noise(const blended_noise_parameters &parameters,
                   random_source &random) -> sampler_ptr {
    const double xz_multiplier = 684.412 * parameters.xz_scale;
    const double y_multiplier = 684.412 * parameters.y_scale;
    const double limit_smear_scale_y =
        y_multiplier * parameters.smear_scale_multiplier;
    const double main_smear_scale_y = limit_smear_scale_y / parameters.y_factor;

    auto min_limit_noise = std::make_shared<noise_stack>(
        create_fbm(random, -15, limit_smear_scale_y, 0.99998474F));
    auto max_limit_noise = std::make_shared<noise_stack>(
        create_fbm(random, -15, limit_smear_scale_y, 0.99998474F));
    auto main_noise = std::make_shared<noise_stack>(
        create_fbm(random, -7, main_smear_scale_y, 12.75));

    sampler_ptr min_sampler =
        noise(min_limit_noise, xz_multiplier, y_multiplier);
    sampler_ptr max_sampler =
        noise(max_limit_noise, xz_multiplier, y_multiplier);
    sampler_ptr main_sampler =
        noise(main_noise, xz_multiplier / parameters.xz_factor,
              y_multiplier / parameters.y_factor);
    sampler_ptr choice =
        clamp(add(std::move(main_sampler), constant(0.5F)), 0.0F, 1.0F);
    return std::make_unique<range_override_sampler>(
        lerp(std::move(choice), std::move(min_sampler), std::move(max_sampler)),
        compute_fbm_range(-15, limit_smear_scale_y, 0.99998474F));
}
auto spline::constant(float value) -> std::shared_ptr<const spline> {
    auto result = std::shared_ptr<spline>(new spline{});
    result->constant_ = true;
    result->constant_value_ = value;
    return result;
}

auto spline::multipoint(std::shared_ptr<sampler> coordinate,
                        std::vector<spline_point> points)
    -> std::shared_ptr<const spline> {
    if (points.empty()) {
        throw std::invalid_argument("spline needs at least one point");
    }
    auto result = std::shared_ptr<spline>(new spline{});
    result->coordinate_ = std::move(coordinate);
    result->locations_.reserve(points.size());
    result->derivatives_.reserve(points.size());
    result->values_.reserve(points.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (i > 0 && points[i].location <= points[i - 1].location) {
            throw std::invalid_argument(
                "spline points must be in ascending order");
        }
        result->locations_.push_back(points[i].location);
        result->derivatives_.push_back(points[i].derivative);
        result->values_.push_back(std::move(points[i].value));
    }
    return result;
}

auto spline::range() const -> interval {
    if (constant_) {
        return interval::of_exact(constant_value_);
    }
    const std::size_t last_index = locations_.size() - 1;
    float min_value = std::numeric_limits<float>::infinity();
    float max_value = -std::numeric_limits<float>::infinity();
    const interval input_range = coordinate_->range();
    if (input_range.is_na_i()) {
        return input_range;
    }
    if (input_range.min() < locations_[0]) {
        const interval first_range = values_[0]->range();
        const float edge1 = linear_extend(input_range.min(), locations_,
                                          first_range.min(), derivatives_, 0);
        const float edge2 = linear_extend(input_range.min(), locations_,
                                          first_range.max(), derivatives_, 0);
        min_value = java_min(min_value, java_min(edge1, edge2));
        max_value = java_max(max_value, java_max(edge1, edge2));
    }
    if (input_range.max() > locations_[last_index]) {
        const interval last_range = values_[last_index]->range();
        const float edge1 =
            linear_extend(input_range.max(), locations_, last_range.min(),
                          derivatives_, last_index);
        const float edge2 =
            linear_extend(input_range.max(), locations_, last_range.max(),
                          derivatives_, last_index);
        min_value = java_min(min_value, java_min(edge1, edge2));
        max_value = java_max(max_value, java_max(edge1, edge2));
    }
    for (const auto &value : values_) {
        const interval value_range = value->range();
        min_value = java_min(min_value, value_range.min());
        max_value = java_max(max_value, value_range.max());
    }
    for (std::size_t i = 0; i < last_index; ++i) {
        const float x1 = locations_[i];
        const float x2 = locations_[i + 1];
        const float x_diff = x2 - x1;
        const interval range1 = values_[i]->range();
        const interval range2 = values_[i + 1]->range();
        const float min1 = range1.min();
        const float max1 = range1.max();
        const float min2 = range2.min();
        const float max2 = range2.max();
        const float d1 = derivatives_[i];
        const float d2 = derivatives_[i + 1];
        if (d1 != 0.0F || d2 != 0.0F) {
            const float p1 = d1 * x_diff;
            const float p2 = d2 * x_diff;
            const float min_lerp1 = java_min(min1, min2);
            const float max_lerp1 = java_max(max1, max2);
            const float min_a = p1 - max2 + min1;
            const float max_a = p1 - min2 + max1;
            const float min_b = -p2 + min2 - max1;
            const float max_b = -p2 + max2 - min1;
            const float min_lerp2 = java_min(min_a, min_b);
            const float max_lerp2 = java_max(max_a, max_b);
            min_value = java_min(min_value, min_lerp1 + 0.25F * min_lerp2);
            max_value = java_max(max_value, max_lerp1 + 0.25F * max_lerp2);
        }
    }
    return interval::of(min_value, max_value);
}

auto spline_value(std::shared_ptr<const spline> value) -> sampler_ptr {
    return std::make_unique<spline_sampler_impl>(std::move(value));
}

} // namespace refrain::worldgen::density
