// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Datapack state-key resolution shared by the configured-feature and state
// provider decoders. A key names a state the way the data pack spells it; the
// registry keys states by their canonical spelling.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>

#include <string_view>

namespace refrain::worldgen::feature {

/// Resolves a datapack state key to a registry state. A key that spells out
/// only default property values folds onto the block's bare default state,
/// which is how a chunk palette abbreviates it; a fluid key that names the
/// default level folds onto the plain fluid name.
[[nodiscard]] auto resolve_state(block_state_registry &states,
                                 std::string_view key)
    -> result<block_state_id>;

} // namespace refrain::worldgen::feature
