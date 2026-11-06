// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Loads the worldgen density-function/noise documents and assembles the noise
// router of a `noise_settings` document.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/worldgen/density.hpp>
#include <refrain/worldgen/density_json.hpp>
#include <refrain/worldgen/rng.hpp>

#include <string>
#include <string_view>

namespace refrain::worldgen::density {

/// The samplers `NoiseRouter` exposes. `final_density` drives terrain fill.
struct noise_router {
    sampler_ptr temperature{};
    sampler_ptr vegetation{};
    sampler_ptr continents{};
    sampler_ptr erosion{};
    sampler_ptr depth{};
    sampler_ptr ridges{};
    sampler_ptr final_density{};
    sampler_ptr chunk_surface_level{};
};

/// Decoded `noise_settings` document (the subset terrain generation needs).
struct noise_settings {
    noise_router router{};
    std::string default_block{}; ///< `minecraft:stone`
    std::string default_fluid{}; ///< `minecraft:water`
    jint sea_level{63};
    bool aquifers{false};
};

/// Loads every density-function and noise document under the datapack's
/// `data` root (`<data>/<namespace>/worldgen/...`), then decodes
/// `<data>/<namespace>/worldgen/noise_settings/<path>.json`. `settings_id` is
/// `<namespace>/<path>`, e.g. `minecraft/overworld`.
[[nodiscard]] auto load_noise_settings(std::string_view worldgen_root,
                                       std::string_view settings_id,
                                       algorithm random_algorithm, jlong seed)
    -> result<noise_settings>;

} // namespace refrain::worldgen::density
