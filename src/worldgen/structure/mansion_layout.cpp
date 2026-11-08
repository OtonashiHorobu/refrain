// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/structure/mansion_layout.hpp>

namespace refrain::worldgen::structure {

mansion_simple_grid::mansion_simple_grid(jint width, jint height,
                                         jint outside_value)
    : width_{width}, height_{height}, outside_{outside_value},
      cells_(static_cast<std::size_t>(width * height), 0) {}

auto mansion_simple_grid::index(jint x, jint y) const -> std::size_t {
    return static_cast<std::size_t>(y * width_ + x);
}

void mansion_simple_grid::set(jint x, jint y, jint value) {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) {
        return;
    }
    cells_[index(x, y)] = value;
}

void mansion_simple_grid::set(jint x0, jint y0, jint x1, jint y1, jint value) {
    for (jint y = y0; y <= y1; ++y) {
        for (jint x = x0; x <= x1; ++x) {
            set(x, y, value);
        }
    }
}

auto mansion_simple_grid::get(jint x, jint y) const -> jint {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) {
        return outside_;
    }
    return cells_[index(x, y)];
}

void mansion_simple_grid::set_if(jint x, jint y, jint if_value, jint value) {
    if (get(x, y) == if_value) {
        set(x, y, value);
    }
}

auto mansion_simple_grid::edges_to(jint x, jint y, jint target) const -> bool {
    return get(x - 1, y) == target || get(x + 1, y) == target ||
           get(x, y - 1) == target || get(x, y + 1) == target;
}

} // namespace refrain::worldgen::structure
