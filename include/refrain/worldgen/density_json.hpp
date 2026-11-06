// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Decodes datapack density-function documents (`worldgen/density_function`)
// and noise parameter documents into the sampler graph.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/worldgen/density.hpp>
#include <refrain/worldgen/rng.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::worldgen::density {

/// Documents the decoder resolves references against. Function ids are the
/// full identifiers (`minecraft:overworld/erosion`); noise ids likewise.
struct density_documents {
    std::vector<std::pair<std::string, std::string>> functions{};
    std::vector<std::pair<std::string, std::string>> noises{};
    algorithm random_algorithm{algorithm::xoroshiro};
    jlong seed{0};
};

/// Decodes one density-function document. Bare numbers are constants and
/// strings are references into `documents.functions`.
[[nodiscard]] auto decode_density(std::string_view json,
                                  const density_documents &documents)
    -> result<sampler_ptr>;

} // namespace refrain::worldgen::density
