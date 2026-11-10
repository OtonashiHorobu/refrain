// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The write-only output a carver fills. The mask is the bit set the terrain
// stage applies after the surface pass; it is local to that stage.

#pragma once

#include <refrain/worldgen/core/java_compat.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace refrain::worldgen {

class carver_output {
  public:
    carver_output() = default;
    carver_output(const carver_output &) = delete;
    auto operator=(const carver_output &) -> carver_output & = delete;
    carver_output(carver_output &&) = default;
    auto operator=(carver_output &&) -> carver_output & = default;
    virtual ~carver_output() = default;

    virtual void carve(jint x, jint y, jint z) = 0;
    [[nodiscard]] virtual auto min_y() const -> jint = 0;
    [[nodiscard]] virtual auto max_y() const -> jint = 0;
};

/// One bit per block of the 16x16 column set between `min_y`
/// and `max_y` inclusive. Bits are indexed
/// `(y - min_y) + (z + (x << 4)) * height`.
class carving_mask final : public carver_output {
  public:
    carving_mask(jint min_y, jint max_y);

    void carve(jint x, jint y, jint z) override;
    [[nodiscard]] auto min_y() const -> jint override { return min_y_; }
    [[nodiscard]] auto max_y() const -> jint override { return max_y_; }

    [[nodiscard]] auto test(jint x, jint y, jint z) const -> bool;
    [[nodiscard]] auto empty() const noexcept -> bool { return count_ == 0; }
    [[nodiscard]] auto count() const noexcept -> std::size_t { return count_; }
    [[nodiscard]] auto height() const noexcept -> jint { return height_; }

    /// Visits maximal vertical runs per column, bottom to top. The callback is
    /// `(x, z, bottom_y, top_y)` with both ends inclusive.
    void visit(const std::function<void(jint x, jint z, jint bottom, jint top)>
                   &visitor) const;

  private:
    [[nodiscard]] auto index(jint x, jint y, jint z) const -> std::size_t;
    [[nodiscard]] auto bit(std::size_t index) const -> bool;
    void set_bit(std::size_t index);

    jint min_y_{0};
    jint max_y_{0};
    jint height_{0};
    std::vector<std::uint64_t> bits_{};
    std::size_t count_{0};
};

} // namespace refrain::worldgen
