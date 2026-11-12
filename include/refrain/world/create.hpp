// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>

#include <cstdint>
#include <string_view>

namespace refrain::world {

/// Create an empty RWF world at `root`.
///
/// Writes the directory layout, the level metadata (`meta.rfw`) and the
/// generator settings object, so `world_store::open` accepts the result and
/// `validate_level` reports the seed. The world contains no chunks: the caller
/// fills the spawn area before serving joins.
///
/// Fails with `already_exists` when `root` already holds a world, and with
/// `io_error` when a directory or file cannot be written.
[[nodiscard]] auto create_world(std::string_view root, std::int64_t seed,
                                std::string_view level_name) -> status;

} // namespace refrain::world
