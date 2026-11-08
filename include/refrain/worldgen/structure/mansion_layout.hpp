// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// The room grid used by woodland mansion generation. The grid encodes a room
// type and id, a door flag and the optional third-floor stair marker in the
// same integer the reference uses.

#pragma once

#include <refrain/worldgen/core/java_compat.hpp>

#include <vector>

namespace refrain::worldgen::structure {

class mansion_simple_grid {
  public:
    mansion_simple_grid(jint width, jint height, jint outside_value);

    void set(jint x, jint y, jint value);
    void set(jint x0, jint y0, jint x1, jint y1, jint value);
    [[nodiscard]] auto get(jint x, jint y) const -> jint;
    void set_if(jint x, jint y, jint if_value, jint value);
    [[nodiscard]] auto edges_to(jint x, jint y, jint target) const -> bool;

    [[nodiscard]] auto width() const -> jint { return width_; }
    [[nodiscard]] auto height() const -> jint { return height_; }

  private:
    [[nodiscard]] auto index(jint x, jint y) const -> std::size_t;

    jint width_{0};
    jint height_{0};
    jint outside_{0};
    std::vector<jint> cells_{};
};

} // namespace refrain::worldgen::structure
