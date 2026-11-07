// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Loads the worldgen density-function/noise documents and assembles the noise
// router of a `noise_settings` document.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/worldgen/core/rng.hpp>
#include <refrain/worldgen/density/density.hpp>
#include <refrain/worldgen/density/density_json.hpp>
#include <refrain/worldgen/terrain/aquifer.hpp>

#include <string>
#include <string_view>

namespace refrain::worldgen::density {

using aquifer_config = terrain::aquifer_config;

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
    aquifer_config aquifer{}; ///< set when `aquifers` is present
    /// The loaded documents, so callers can decode further functions (ore vein
    /// densities, for example) with the same seed and references.
    density_documents documents{};
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
