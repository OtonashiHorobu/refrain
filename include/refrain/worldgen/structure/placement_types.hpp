// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Template placement vocabulary shared by the template manager and the pool
// description. The values match `Mirror`, `Rotation` and `LiquidSettings`
// ordinal order, which the piece payloads store as integers.

#pragma once

#include <cstdint>

namespace refrain::worldgen::structure {

enum class template_mirror : std::uint8_t {
    none = 0,
    left_right = 1,
    front_back = 2
};

enum class template_rotation : std::uint8_t {
    none = 0,
    clockwise_90 = 1,
    clockwise_180 = 2,
    counterclockwise_90 = 3
};

enum class liquid_settings : std::uint8_t {
    apply_waterlogging = 0,
    ignore_waterlogging = 1
};

} // namespace refrain::worldgen::structure
