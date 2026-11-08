// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The five-point nether climate table used by `MultiNoiseBiomeSource` with
// the `minecraft:nether` preset.

#pragma once

#include <refrain/worldgen/biome/biome_source.hpp>

#include <vector>

namespace refrain::worldgen {

[[nodiscard]] auto nether_biome_parameters() -> std::vector<biome_parameter>;

} // namespace refrain::worldgen
