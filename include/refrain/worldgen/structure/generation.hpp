// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The first structure-start generators. Each entry emits the same piece
// geometry and reference cap as the reference structure type.

#pragma once

#include <refrain/worldgen/structure/start.hpp>

namespace refrain::worldgen::structure {

/// `BuriedTreasureStructure.findGenerationPoint`: a single 1x1x1 piece at the
/// chunk centre column, y=90.
[[nodiscard]] auto buried_treasure_start(const chunk_pos &chunk)
    -> structure_start;

} // namespace refrain::worldgen::structure
