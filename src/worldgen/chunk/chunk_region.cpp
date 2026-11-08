// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/chunk/chunk_region.hpp>

#include <cstdlib>

namespace refrain::worldgen {

namespace {

constexpr auto floor_div16(jint value) -> jint {
    return value >= 0 ? value / 16 : -(((-value) + 15) / 16);
}

} // namespace

chunk_region::chunk_region(proto_chunk &center, jint write_radius)
    : center_{&center}, write_radius_{write_radius} {
    chunks_[chunk_pos{.x = center.chunk_x(), .z = center.chunk_z()}] = &center;
}

void chunk_region::attach(proto_chunk &chunk) {
    chunks_[chunk_pos{.x = chunk.chunk_x(), .z = chunk.chunk_z()}] = &chunk;
}

auto chunk_region::find(const chunk_pos &pos) const -> proto_chunk * {
    const auto it = chunks_.find(pos);
    return it == chunks_.end() ? nullptr : it->second;
}

auto chunk_region::within_write_zone(const chunk_pos &pos) const -> bool {
    return std::abs(pos.x - center_->chunk_x()) <= write_radius_ &&
           std::abs(pos.z - center_->chunk_z()) <= write_radius_;
}

auto chunk_region::state_at(jint x, jint y, jint z) const -> block_state_id {
    const chunk_pos pos{.x = floor_div16(x), .z = floor_div16(z)};
    const auto *chunk = find(pos);
    if (chunk == nullptr) {
        return center_->table().air();
    }
    return chunk->state_at(x - pos.x * 16, y, z - pos.z * 16);
}

auto chunk_region::set_block_state(jint x, jint y, jint z, block_state_id state)
    -> bool {
    const chunk_pos pos{.x = floor_div16(x), .z = floor_div16(z)};
    auto *chunk = find(pos);
    if (chunk == nullptr || !within_write_zone(pos)) {
        ++dropped_writes_;
        return false;
    }
    return chunk->set_state(x - pos.x * 16, y, z - pos.z * 16, state);
}

auto chunk_region::mark_post_processing(jint x, jint y, jint z) -> bool {
    const chunk_pos pos{.x = floor_div16(x), .z = floor_div16(z)};
    auto *chunk = find(pos);
    if (chunk == nullptr) {
        return false;
    }
    chunk->mark_post_processing(x - pos.x * 16, y, z - pos.z * 16);
    return true;
}

auto chunk_region::set_block_entity(jint x, jint y, jint z,
                                    data::nbt::compound entity) -> bool {
    const chunk_pos pos{.x = floor_div16(x), .z = floor_div16(z)};
    auto *chunk = find(pos);
    if (chunk == nullptr || !within_write_zone(pos)) {
        ++dropped_writes_;
        return false;
    }
    chunk->set_block_entity(
        block_pos{.x = x - pos.x * 16, .y = y, .z = z - pos.z * 16},
        std::move(entity));
    return true;
}

auto chunk_region::first_available(heightmap_kind kind, jint x, jint z) const
    -> jint {
    return center_->first_available(kind, x, z);
}

auto chunk_region::height_at(heightmap_kind kind, jint x, jint z) const
    -> jint {
    const chunk_pos pos{.x = floor_div16(x), .z = floor_div16(z)};
    const auto *chunk = find(pos);
    if (chunk == nullptr) {
        return center_->min_y();
    }
    return chunk->first_available(kind, x - pos.x * 16, z - pos.z * 16);
}

} // namespace refrain::worldgen
