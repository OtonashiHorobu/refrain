// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Biome climate parameter tables. The tables are game data and live in the
// content snapshot; this module validates and decodes them.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/worldgen/biome/biome_source.hpp>
#include <refrain/worldgen/dimension.hpp>

#include <string_view>
#include <vector>

namespace refrain::worldgen {

/// Loads the climate parameter table of one dimension.
///
/// `data_root` is the `.../<version>/data` directory of a content snapshot.
/// The report is read from `../reports/biome_parameters/<dimension>.json`,
/// next to that directory. The document names the game version and dimension,
/// records the entry count and lists every entry in selection order. Each entry
/// carries the biome key, the six quantized climate intervals and the quantized
/// offset.
///
/// Decoding is strict. A missing report fails with `not_found` and names the
/// path; malformed JSON, an unsupported format, a dimension or entry count that
/// does not match the metadata, and a missing, mistyped, out-of-range or
/// unknown field all fail with `parse_error` and name the entry. Entries keep
/// the order of the document, because the search resolves equal-distance ties
/// by first encounter.
[[nodiscard]] auto load_biome_parameters(std::string_view data_root,
                                         dimension_kind dimension)
    -> result<std::vector<biome_parameter>>;

} // namespace refrain::worldgen
