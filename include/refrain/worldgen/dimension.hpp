// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The fixed per-dimension generation parameters.

#pragma once

#include <refrain/worldgen/core/java_compat.hpp>

#include <string_view>

namespace refrain::worldgen {

enum class dimension_kind : std::uint8_t {
    overworld = 0,
    nether = 1,
    end = 2,
};

/// Values shared by the generator, the chunk representation and the
/// structure service. `sea_level` is the noise settings value; the nether
/// and end do not use a water sea.
struct dimension_profile {
    dimension_kind kind{dimension_kind::overworld};
    jint min_y{-64};
    jint height{384};
    jint logical_height{384};
    jint sea_level{63};
    double coordinate_scale{1.0};
    bool has_skylight{true};
    bool has_ceiling{false};
    std::string_view noise_settings{"minecraft/overworld"};
    std::string_view material_rule{"minecraft/overworld"};
};

[[nodiscard]] auto dimension_profile_for(dimension_kind kind)
    -> const dimension_profile &;

[[nodiscard]] auto dimension_name(dimension_kind kind) -> std::string_view;

} // namespace refrain::worldgen
