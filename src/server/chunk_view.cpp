// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/chunk_view.hpp>

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace refrain::server {

namespace {

[[nodiscard]] auto distance(std::int32_t left, std::int32_t right)
    -> std::int32_t {
    const auto delta{static_cast<std::int64_t>(left) - right};
    const auto magnitude{std::llabs(delta)};
    if (magnitude > std::numeric_limits<std::int32_t>::max()) {
        return std::numeric_limits<std::int32_t>::max();
    }
    return static_cast<std::int32_t>(magnitude);
}

} // namespace

auto chunk_distance(chunk_coordinate left, chunk_coordinate right)
    -> std::int32_t {
    return std::max(distance(left.x, right.x), distance(left.z, right.z));
}

auto chunks_outside_view(std::span<const chunk_coordinate> tracked,
                         chunk_coordinate center, std::int32_t radius)
    -> std::vector<chunk_coordinate> {
    const auto limit{std::max(0, radius)};
    std::vector<chunk_coordinate> outside{};
    for (const auto &position : tracked) {
        if (chunk_distance(position, center) > limit) {
            outside.push_back(position);
        }
    }
    std::ranges::sort(outside, [](const chunk_coordinate &left,
                                  const chunk_coordinate &right) {
        return left.x != right.x ? left.x < right.x : left.z < right.z;
    });
    return outside;
}

} // namespace refrain::server
