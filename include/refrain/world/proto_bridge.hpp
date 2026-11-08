// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Converts a world generation chunk into the storage chunk model so the
// generated column can be persisted through `world::store` or Anvil export.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/world/model.hpp>
#include <refrain/worldgen/chunk/proto_chunk.hpp>

#include <string>
#include <vector>

namespace refrain::world {

/// One generated column as a storage chunk.
///
/// `biome_names` maps the generation biome ids to keys; an id outside the list
/// falls back to `minecraft:plains`. Structures, light and ticks are left
/// empty because generation does not produce them in the storage model.
[[nodiscard]] auto from_proto_chunk(const worldgen::proto_chunk &source,
                                    const std::vector<std::string> &biome_names,
                                    chunk_status status = chunk_status::full)
    -> result<chunk>;

} // namespace refrain::world
