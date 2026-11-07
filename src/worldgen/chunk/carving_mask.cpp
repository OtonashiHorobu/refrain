// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/chunk/carving_mask.hpp>

namespace refrain::worldgen {

carving_mask::carving_mask(jint min_y, jint max_y)
    : min_y_{min_y}, max_y_{max_y}, height_{max_y - min_y + 1},
      bits_(static_cast<std::size_t>(256) *
                static_cast<std::size_t>(height_ > 0 ? height_ : 0) / 64U +
            1U) {}

auto carving_mask::index(jint x, jint y, jint z) const -> std::size_t {
    return static_cast<std::size_t>(y - min_y_) +
           (static_cast<std::size_t>(z) + (static_cast<std::size_t>(x) << 4U)) *
               static_cast<std::size_t>(height_);
}

auto carving_mask::bit(std::size_t index) const -> bool {
    return (bits_[index / 64U] >> (index % 64U)) & 1U;
}

void carving_mask::set_bit(std::size_t index) {
    bits_[index / 64U] |= std::uint64_t{1} << (index % 64U);
}

auto carving_mask::test(jint x, jint y, jint z) const -> bool {
    if (x < 0 || x > 15 || z < 0 || z > 15 || y < min_y_ || y > max_y_) {
        return false;
    }
    return bit(index(x, y, z));
}

void carving_mask::carve(jint x, jint y, jint z) {
    if (x < 0 || x > 15 || z < 0 || z > 15 || y < min_y_ || y > max_y_) {
        return;
    }
    const auto position = index(x, y, z);
    if (!bit(position)) {
        set_bit(position);
        ++count_;
    }
}

void carving_mask::visit(
    const std::function<void(jint, jint, jint, jint)> &visitor) const {
    for (jint x = 0; x < 16; ++x) {
        for (jint z = 0; z < 16; ++z) {
            jint y = min_y_;
            while (y <= max_y_) {
                if (!test(x, y, z)) {
                    ++y;
                    continue;
                }
                const jint bottom = y;
                while (y + 1 <= max_y_ && test(x, y + 1, z)) {
                    ++y;
                }
                visitor(x, z, bottom, y);
                ++y;
            }
        }
    }
}

} // namespace refrain::worldgen
