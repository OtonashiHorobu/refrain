// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Conversion between the reference chunk entity compound and
// `world::entity_record`.
//
// World generation and the Anvil interop path both read the reference chunk
// `entities` list, one compound per entity with `id`, `Pos`, `Rotation`,
// `block_pos` and the entity's data components. The storage record keeps the
// fields the engine interprets apart from the remaining components, which stay
// opaque NBT; this header is the one place that maps between the two shapes so
// generation, Anvil import and Anvil export agree on the tag names and the
// accepted tag types.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/data/nbt.hpp>
#include <refrain/world/model.hpp>

namespace refrain::world {

/// Convert one reference entity compound into the storage record.
///
/// Required: `id` (a non-empty string) and `Pos` (a list of three numbers).
/// `Rotation` (a list or a float array of two numbers) and `block_pos` (a list
/// or an int array of three integers) are optional; a missing `block_pos` is
/// derived from the position the way the reference derives an entity's block
/// position. A malformed present field fails with `parse_error` naming the
/// entity: the caller must not persist a record it cannot interpret. Every
/// component outside the modelled fields is preserved in `record.data`.
[[nodiscard]] auto entity_from_nbt(const data::nbt::compound &tag)
    -> result<entity_record>;

/// Convert the storage record back into a reference entity compound. The
/// components in `record.data` are restored first and the modelled fields are
/// written over them, in the tag shapes the reference entity serialization
/// uses: `Pos` as a double list, `Rotation` as a float array and `block_pos` as
/// an int array. An empty or non-compound `data` payload fails with
/// `parse_error`.
[[nodiscard]] auto entity_to_nbt(const entity_record &record)
    -> result<data::nbt::compound>;

} // namespace refrain::world
