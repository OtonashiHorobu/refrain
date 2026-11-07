// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The generation status chain. 26.3 merges the old noise, surface and carvers
// steps into `terrain`; the enum order is the dependency order used by the
// single-threaded generator.

#pragma once

#include <cstdint>
#include <string_view>

namespace refrain::worldgen {

enum class chunk_status : std::uint8_t {
    empty = 0,
    structure_starts = 1,
    structure_references = 2,
    biomes = 3,
    terrain = 4,
    features = 5,
    initialize_light = 6,
    light = 7,
    spawn = 8,
    full = 9,
};

inline constexpr std::size_t chunk_status_count = 10;

[[nodiscard]] auto status_name(chunk_status status) -> std::string_view;
[[nodiscard]] auto status_at_least(chunk_status value, chunk_status minimum)
    -> bool;

} // namespace refrain::worldgen
