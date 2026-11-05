// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/data/rfs.hpp>

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace refrain::data {

/// Result of cooking one directory tree into an RFS1 container.
struct cook_result {
    std::vector<std::byte> bytes{};
    std::size_t files{0};
};

/// Pack every regular file below `root` into a deterministic RFS1 container
/// (sorted virtual paths, raw pass-through payloads).
///
/// This is the transport/cache slice of the content cook pipeline: it does not
/// parse or validate datapack semantics yet (that stays with
/// `datapack_set::load`), but it gives the RFS cache a producer and a stable
/// content identity. Symlinks are skipped so the snapshot cannot escape
/// `root`; unsafe virtual paths fail closed.
[[nodiscard]] auto cook_directory_rfs(std::string_view root,
                                      const rfs_write_options &options = {})
    -> result<cook_result>;

} // namespace refrain::data
