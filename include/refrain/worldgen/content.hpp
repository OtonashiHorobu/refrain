// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Materialisation of the worldgen datapack snapshot. The repository ships the
// snapshot as one compressed archive under `content/vanilla/<version>/`; the
// existing filesystem loaders consume the extracted tree.

#pragma once

#include <refrain/core/error.hpp>

#include <string>
#include <string_view>

namespace refrain::worldgen {

/// Returns a directory that contains the `<namespace>/worldgen` and
/// `<namespace>/tags` trees. A directory is returned unchanged. A `.zip`
/// archive is extracted once under the system temporary directory; a marker
/// file makes retries idempotent.
[[nodiscard]] auto materialize_worldgen_data(std::string_view path)
    -> result<std::string>;

} // namespace refrain::worldgen
