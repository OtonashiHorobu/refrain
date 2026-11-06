// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Density sampling grid: the `DensityVolume` rectangle and the flat float
// `DensityBuffer` the samplers fill. Index order is Java's
// `indexY + (indexX + indexZ * sizeX) * sizeY`, i.e. Y is contiguous.

#pragma once

#include <refrain/worldgen/java_compat.hpp>

#include <cstddef>
#include <vector>

namespace refrain::worldgen {

/// Java `DensityVolume`: an axis-aligned grid of block coordinates with
/// per-axis steps. Sizes and steps are positive.
class volume {
  public:
    volume(jint size_x, jint size_y, jint size_z, jint min_block_x,
           jint min_block_y, jint min_block_z, jint step_block_x = 1,
           jint step_block_y = 1, jint step_block_z = 1);

    [[nodiscard]] auto size_x() const noexcept -> jint { return size_x_; }
    [[nodiscard]] auto size_y() const noexcept -> jint { return size_y_; }
    [[nodiscard]] auto size_z() const noexcept -> jint { return size_z_; }
    [[nodiscard]] auto min_block_x() const noexcept -> jint {
        return min_block_x_;
    }
    [[nodiscard]] auto min_block_y() const noexcept -> jint {
        return min_block_y_;
    }
    [[nodiscard]] auto min_block_z() const noexcept -> jint {
        return min_block_z_;
    }
    [[nodiscard]] auto step_block_x() const noexcept -> jint {
        return step_block_x_;
    }
    [[nodiscard]] auto step_block_y() const noexcept -> jint {
        return step_block_y_;
    }
    [[nodiscard]] auto step_block_z() const noexcept -> jint {
        return step_block_z_;
    }

    [[nodiscard]] auto index_unchecked(jint x, jint y, jint z) const noexcept
        -> std::size_t {
        return static_cast<std::size_t>(y) +
               (static_cast<std::size_t>(x) +
                static_cast<std::size_t>(z) *
                    static_cast<std::size_t>(size_x_)) *
                   static_cast<std::size_t>(size_y_);
    }
    [[nodiscard]] auto block_x(jint x) const noexcept -> jint {
        return min_block_x_ + x * step_block_x_;
    }
    [[nodiscard]] auto block_y(jint y) const noexcept -> jint {
        return min_block_y_ + y * step_block_y_;
    }
    [[nodiscard]] auto block_z(jint z) const noexcept -> jint {
        return min_block_z_ + z * step_block_z_;
    }

    [[nodiscard]] auto max_block_x() const noexcept -> jint {
        return min_block_x_ + size_x_ * step_block_x_ - 1;
    }
    [[nodiscard]] auto max_block_y() const noexcept -> jint {
        return min_block_y_ + size_y_ * step_block_y_ - 1;
    }
    [[nodiscard]] auto max_block_z() const noexcept -> jint {
        return min_block_z_ + size_z_ * step_block_z_ - 1;
    }

    [[nodiscard]] auto size() const noexcept -> std::size_t {
        return static_cast<std::size_t>(size_x_) *
               static_cast<std::size_t>(size_y_) *
               static_cast<std::size_t>(size_z_);
    }

    /// `DensityVolume.indexOfBlock`: the buffer index of a block coordinate, or
    /// `npos` when the block is outside the grid or not on a sample.
    [[nodiscard]] auto index_of_block(jint block_x, jint block_y,
                                      jint block_z) const noexcept
        -> std::size_t;

    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

  private:
    jint size_x_{0};
    jint size_y_{0};
    jint size_z_{0};
    jint min_block_x_{0};
    jint min_block_y_{0};
    jint min_block_z_{0};
    jint step_block_x_{1};
    jint step_block_y_{1};
    jint step_block_z_{1};
};

/// Java `DensityBuffer`: a flat float array with the sampling helpers the
/// samplers use (`fill`, `addTo`, `setRange`, `copyFrom`).
class buffer {
  public:
    explicit buffer(std::size_t size) : values_(size, 0.0F) {}

    void set(std::size_t index, float value) { values_[index] = value; }
    [[nodiscard]] auto get(std::size_t index) const -> float {
        return values_[index];
    }
    void add_to(std::size_t index, float value) {
        values_[index] = values_[index] + value;
    }
    void fill(float value) {
        for (auto &slot : values_) {
            slot = value;
        }
    }
    void set_range(std::size_t index, std::size_t count, float value) {
        for (std::size_t i = 0; i < count; ++i) {
            values_[index + i] = value;
        }
    }
    void copy_from(const buffer &other) { values_ = other.values_; }

    [[nodiscard]] auto size() const noexcept -> std::size_t {
        return values_.size();
    }
    [[nodiscard]] auto data() noexcept -> float * { return values_.data(); }
    [[nodiscard]] auto data() const noexcept -> const float * {
        return values_.data();
    }

  private:
    std::vector<float> values_;
};

} // namespace refrain::worldgen
