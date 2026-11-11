// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace refrain::server {

/// Chunk coordinate, in chunks.
struct chunk_coordinate {
    std::int32_t x{0};
    std::int32_t z{0};

    friend auto operator==(const chunk_coordinate &, const chunk_coordinate &)
        -> bool = default;
};

/// Chebyshev distance between two chunk coordinates.
[[nodiscard]] auto chunk_distance(chunk_coordinate left, chunk_coordinate right)
    -> std::int32_t;

/// Chunks of `tracked` that fall outside the square viewport centred on
/// `center` with `radius`, ordered by x then z.
///
/// The server streams every chunk inside the viewport and tells the client to
/// forget the ones that leave it, so a moving player keeps a bounded chunk
/// cache.
[[nodiscard]] auto
chunks_outside_view(std::span<const chunk_coordinate> tracked,
                    chunk_coordinate center, std::int32_t radius)
    -> std::vector<chunk_coordinate>;

} // namespace refrain::server
