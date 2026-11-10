// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/session/chunk_tracker.hpp>

#include <algorithm>
#include <utility>

namespace refrain::session {

auto chunk_tracker::update(std::uint64_t connection, std::int32_t chunk_x,
                           std::int32_t chunk_z, std::int32_t radius) -> bool {
    if (connection == 0 || radius < 0) {
        return false;
    }
    const std::lock_guard lock{mutex_};
    auto &entry{views_[connection]};
    const auto seed_radius{entry.initialized ? 0 : 3};
    if (entry.initialized && entry.chunk_x == chunk_x &&
        entry.chunk_z == chunk_z && entry.radius == radius) {
        return false;
    }
    entry.chunk_x = chunk_x;
    entry.chunk_z = chunk_z;
    entry.radius = radius;
    entry.initialized = true;
    pending_.push_back({.connection = connection,
                        .chunk_x = chunk_x,
                        .chunk_z = chunk_z,
                        .radius = radius,
                        .seed_radius = seed_radius});
    return true;
}

auto chunk_tracker::take_requests() -> std::vector<chunk_view_request> {
    const std::lock_guard lock{mutex_};
    return std::exchange(pending_, {});
}

auto chunk_tracker::remove(std::uint64_t connection) -> bool {
    const std::lock_guard lock{mutex_};
    return views_.erase(connection) != 0;
}

} // namespace refrain::session
