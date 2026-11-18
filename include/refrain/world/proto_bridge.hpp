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
/// falls back to `minecraft:plains`.
///
/// The structure records the generation chunk carries become the stored
/// `structures` compound, in the same encoding the Anvil import/export path
/// carries: a `starts` compound keyed by structure id and a `References`
/// compound of packed chunk positions. A jigsaw pool element keeps its
/// processor list in the reference's shape: a named list as its id, an inline
/// list as the direct codec's object form. A column without a start and
/// without a reference keeps the field empty, so an unchanged column keeps its
/// semantic hash. A record the compound cannot represent (an empty structure
/// id, an unknown piece type, an inline processor document the tag model
/// cannot carry, two starts of one structure in the same column) fails the
/// conversion with `parse_error` instead of being dropped silently.
///
/// The entities the generation chunk carries become `chunk::entities`, sorted
/// into the canonical position order. A generated entity whose type or
/// position cannot be read fails the conversion with `parse_error` naming the
/// entity: a template entity record the server cannot persist must not be
/// dropped silently.
[[nodiscard]] auto from_proto_chunk(const worldgen::proto_chunk &source,
                                    const std::vector<std::string> &biome_names,
                                    chunk_status status = chunk_status::full)
    -> result<chunk>;

} // namespace refrain::world
