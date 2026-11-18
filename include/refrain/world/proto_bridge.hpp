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

#include <functional>
#include <string>
#include <string_view>
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
/// compound of packed chunk positions. A column without a start and without a
/// reference keeps the field empty, so an unchanged column keeps its semantic
/// hash. A record the compound cannot represent (an empty structure id, an
/// unknown piece type, a jigsaw element with an inline processor list, two
/// starts of one structure in the same column) fails the conversion with
/// `parse_error` instead of being dropped silently.
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

/// Resolves the generation step of the structure a stored start names. The
/// chunk compound carries the start's structure id and pieces but not the
/// step: the reference derives it from the structure registry, and so does
/// the bridge caller that has the structure data set.
using structure_step_lookup = std::function<std::string(std::string_view)>;

/// Restores the structure starts and references a stored column carries into
/// a generation chunk, so a chunk loaded from storage can take part in
/// structure continuation while its neighbours are generated.
///
/// Only the structure records are restored; blocks, biomes and heightmaps
/// belong to the terrain representation the generation cache owns. The caller
/// supplies `step_for` because the stored compound does not carry the step.
/// A payload that is not the compound `from_proto_chunk` writes fails with
/// `parse_error` instead of being dropped; an empty payload restores nothing.
///
/// The compound carries the reference's own fields only. Values the reference
/// reconstructs from the structure registry or the template assets (the step,
/// a template piece's rotation pivot, a ruined portal's pivot and an ocean
/// monument's room graph) are restored from the reference defaults, so a
/// column written by this bridge round-trips exactly and a foreign column
/// keeps the geometry the tag records.
[[nodiscard]] auto restore_structures(const chunk &source,
                                      worldgen::proto_chunk &target,
                                      const structure_step_lookup &step_for)
    -> status;

} // namespace refrain::world
