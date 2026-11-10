// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Loads the `worldgen/carver` documents and each biome's ordered carver list
// from a data pack root.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/worldgen/carver/carver.hpp>
#include <refrain/worldgen/carver/carver_stage.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace refrain::worldgen::carver {

struct loaded_carvers {
    std::unordered_map<std::string, std::unique_ptr<world_carver>> carvers{};
    carver_biomes biomes{};
};

/// Reads `<data_root>/<namespace>/worldgen/carver/*.json` and
/// `<data_root>/<namespace>/worldgen/biome/*.json` for every namespace present.
[[nodiscard]] auto load_carvers(std::string_view data_root)
    -> result<loaded_carvers>;

} // namespace refrain::worldgen::carver
