// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/chunk/heightmap.hpp>

#include <algorithm>

namespace refrain::worldgen {

auto matches(heightmap_kind kind, const state_traits &traits) -> bool {
    switch (kind) {
    case heightmap_kind::world_surface_wg:
    case heightmap_kind::world_surface:
        return !traits.air;
    case heightmap_kind::ocean_floor_wg:
    case heightmap_kind::ocean_floor:
        return traits.blocks_motion;
    case heightmap_kind::motion_blocking:
        return traits.blocks_motion || traits.fluid;
    case heightmap_kind::motion_blocking_no_leaves:
        return traits.blocks_motion_no_leaves || traits.fluid;
    }
    return false;
}

heightmap::heightmap(heightmap_kind kind, jint min_y, jint height)
    : kind_{kind}, min_y_{min_y}, height_{height} {
    first_available_.fill(min_y_);
}

auto heightmap::first_available(jint x, jint z) const -> jint {
    if (x < 0 || x > 15 || z < 0 || z > 15) {
        return min_y_;
    }
    return first_available_[index(x, z)];
}

auto heightmap::highest_taken(jint x, jint z) const -> jint {
    return first_available(x, z) - 1;
}

auto heightmap::empty() const noexcept -> bool {
    return std::all_of(first_available_.begin(), first_available_.end(),
                       [this](jint value) { return value <= min_y_; });
}

void heightmap::set_first_available(jint x, jint z, jint value) {
    if (x < 0 || x > 15 || z < 0 || z > 15) {
        return;
    }
    first_available_[index(x, z)] = std::clamp(value, min_y_, min_y_ + height_);
}

void heightmap::clear() { first_available_.fill(min_y_); }

void heightmap::update(jint x, jint y, jint z, std::uint16_t state,
                       const heightmap_source &source) {
    if (x < 0 || x > 15 || z < 0 || z > 15 || y < min_y_ ||
        y >= min_y_ + height_) {
        return;
    }

    auto &first = first_available_[index(x, z)];
    // Writes at least two blocks below the current top cannot change it.
    if (y <= first - 2) {
        return;
    }

    if (matches_state(state, source)) {
        if (y >= first) {
            first = y + 1;
        }
        return;
    }

    // The block that defined the top stopped matching: rescan downwards.
    if (first - 1 == y) {
        for (jint scan_y = y - 1; scan_y >= min_y_; --scan_y) {
            if (matches_state(source.state_at(x, scan_y, z), source)) {
                first = scan_y + 1;
                return;
            }
        }
        first = min_y_;
    }
}

void heightmap::prime(const heightmap_source &source) {
    for (jint z = 0; z < 16; ++z) {
        for (jint x = 0; x < 16; ++x) {
            auto &first = first_available_[index(x, z)];
            first = min_y_;
            for (jint y = min_y_ + height_ - 1; y >= min_y_; --y) {
                if (matches_state(source.state_at(x, y, z), source)) {
                    first = y + 1;
                    break;
                }
            }
        }
    }
}

} // namespace refrain::worldgen
