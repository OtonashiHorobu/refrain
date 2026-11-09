// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/dimension.hpp>

namespace refrain::worldgen {

namespace {

constexpr dimension_profile overworld_profile{
    .kind = dimension_kind::overworld,
    .min_y = -64,
    .height = 384,
    .logical_height = 384,
    .sea_level = 63,
    .coordinate_scale = 1.0,
    .has_skylight = true,
    .has_ceiling = false,
    .has_carvers = true,
    .noise_settings = "minecraft/overworld",
    .material_rule = "minecraft/overworld",
};

constexpr dimension_profile nether_profile{
    .kind = dimension_kind::nether,
    .min_y = 0,
    .height = 256,
    .logical_height = 128,
    .sea_level = 32,
    .coordinate_scale = 8.0,
    .has_skylight = false,
    .has_ceiling = true,
    .has_carvers = true,
    .noise_settings = "minecraft/nether",
    .material_rule = "minecraft/nether",
};

constexpr dimension_profile end_profile{
    .kind = dimension_kind::end,
    .min_y = 0,
    .height = 256,
    .logical_height = 256,
    .sea_level = 0,
    .coordinate_scale = 1.0,
    .has_skylight = true,
    .has_ceiling = false,
    .has_carvers = false,
    .noise_settings = "minecraft/end",
    .material_rule = "minecraft/end",
};

} // namespace

auto dimension_profile_for(dimension_kind kind) -> const dimension_profile & {
    switch (kind) {
    case dimension_kind::nether:
        return nether_profile;
    case dimension_kind::end:
        return end_profile;
    case dimension_kind::overworld:
    default:
        return overworld_profile;
    }
}

auto dimension_name(dimension_kind kind) -> std::string_view {
    switch (kind) {
    case dimension_kind::nether:
        return "minecraft:the_nether";
    case dimension_kind::end:
        return "minecraft:the_end";
    case dimension_kind::overworld:
    default:
        return "minecraft:overworld";
    }
}

} // namespace refrain::worldgen
