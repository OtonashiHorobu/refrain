// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Codec between the generation structure records and the `structures` NBT
// compound the vanilla chunk schema carries. `world::proto_bridge` uses it to
// persist a generated column and to restore a stored one for generation.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/worldgen/chunk/proto_chunk.hpp>

#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace refrain::world::detail {

/// Serialises the starts and references a generated column carries into the
/// `structures` compound of the chunk schema: a `starts` compound keyed by
/// structure id and a `References` compound of packed chunk positions, with
/// one vanilla piece tag per piece. Produces an empty payload when there is
/// neither a start nor a reference.
///
/// The encoding is bounded by per-column counts and by the RWF payload limit.
/// A record the vanilla schema cannot represent (an empty structure id, an
/// unknown piece type, a piece field outside the format's shape) fails with
/// `parse_error` instead of being dropped.
///
/// `chunk_x`/`chunk_z` name the column the records are stored in. The
/// reference writes the saving chunk's coordinates into every start tag, so
/// an owned start must name that chunk.
[[nodiscard]] auto
encode_structures(const std::vector<worldgen::structure_start> &starts,
                  const std::vector<worldgen::structure_reference> &references,
                  std::int32_t chunk_x, std::int32_t chunk_z)
    -> result<std::vector<std::byte>>;

/// Restores the records `payload` carries into `target`. An empty payload
/// restores nothing. A payload that is not the compound `encode_structures`
/// writes fails with `parse_error`; so does a record outside the format's
/// limits.
///
/// The reference derives a start's generation step from its structure registry
/// entry instead of storing it, so the reverse bridge takes the same lookup
/// from the caller; an empty lookup restores the starts with an empty step.
[[nodiscard]] auto decode_structures(
    std::span<const std::byte> payload, worldgen::proto_chunk &target,
    const std::function<std::string(std::string_view)> &step_for) -> status;

} // namespace refrain::world::detail
