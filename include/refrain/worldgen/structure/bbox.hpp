// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Axis-aligned structure bounds shared by start records and placement payloads.

#pragma once

#include <refrain/worldgen/core/java_compat.hpp>

namespace refrain::worldgen {
struct chunk_pos;
}

namespace refrain::worldgen::structure {

struct structure_bbox {
    jint min_x{0};
    jint min_y{0};
    jint min_z{0};
    jint max_x{0};
    jint max_y{0};
    jint max_z{0};

    [[nodiscard]] auto intersects(const structure_bbox &other) const -> bool;
    [[nodiscard]] auto is_close_to(const chunk_pos &chunk, jint radius) const
        -> bool;
    [[nodiscard]] auto intersects_chunk(const chunk_pos &chunk) const -> bool;
};

} // namespace refrain::worldgen::structure
