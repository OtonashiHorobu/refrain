// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// End city starts: the fixed tower pieces and the recursive section graph that
// selects tower, bridge, house and fat-tower sections with its own rotation and
// collision rules.

#pragma once

#include <refrain/worldgen/core/rng.hpp>
#include <refrain/worldgen/structure/start.hpp>
#include <refrain/worldgen/structure/template.hpp>

#include <optional>

namespace refrain::worldgen::structure {

/// Generates the end city whose start anchor is `origin`. `random` must be the
/// structure-start stream already seeded for the source chunk; the caller has
/// drawn the rotation from it. Returns nothing when no piece survives.
[[nodiscard]] auto
end_city_start(structure_template_manager &templates, const block_pos &origin,
               template_rotation rotation, worldgen_random &random)
    -> std::optional<structure_start>;

} // namespace refrain::worldgen::structure
