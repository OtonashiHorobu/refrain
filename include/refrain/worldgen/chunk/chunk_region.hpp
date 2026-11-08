// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The neighbourhood a generation stage sees. Stages write through
// `set_block_state`, which applies the step's write radius; reads see every
// attached neighbour. Chunks are owned by the generator, not the region.

#pragma once

#include <refrain/worldgen/chunk/proto_chunk.hpp>

#include <cstdint>
#include <map>
#include <utility>

namespace refrain::worldgen {

struct chunk_pos {
    jint x{0};
    jint z{0};

    friend constexpr auto operator<=>(const chunk_pos &,
                                      const chunk_pos &) = default;
};

class chunk_region {
  public:
    chunk_region(proto_chunk &center, jint write_radius);

    /// Makes `chunk` readable (and writable when inside the write radius).
    void attach(proto_chunk &chunk);

    [[nodiscard]] auto center() -> proto_chunk & { return *center_; }
    [[nodiscard]] auto center() const -> const proto_chunk & {
        return *center_;
    }
    [[nodiscard]] auto write_radius() const noexcept -> jint {
        return write_radius_;
    }
    [[nodiscard]] auto dropped_writes() const noexcept -> std::uint64_t {
        return dropped_writes_;
    }

    [[nodiscard]] auto find(const chunk_pos &pos) const -> proto_chunk *;
    [[nodiscard]] auto find(jint chunk_x, jint chunk_z) const -> proto_chunk * {
        return find(chunk_pos{.x = chunk_x, .z = chunk_z});
    }
    [[nodiscard]] auto has_chunk(const chunk_pos &pos) const -> bool {
        return find(pos) != nullptr;
    }

    /// Reads a block by world position; an unattached chunk reads as air.
    [[nodiscard]] auto state_at(jint x, jint y, jint z) const -> block_state_id;

    /// Writes a block by world position; returns false and counts the write
    /// when the target is absent or outside the write radius.
    auto set_block_state(jint x, jint y, jint z, block_state_id state) -> bool;

    /// `LevelChunk.markPosForPostProcessing`: marks a loaded chunk, independent
    /// of the active write radius.
    auto mark_post_processing(jint x, jint y, jint z) -> bool;

    /// Writes a block entity by world position with the same write-zone rule
    /// as `set_block_state`.
    auto set_block_entity(jint x, jint y, jint z, data::nbt::compound entity)
        -> bool;

    /// Height of the centre chunk's heightmap at chunk-local coordinates.
    [[nodiscard]] auto first_available(heightmap_kind kind, jint x,
                                       jint z) const -> jint;

    /// Height of the heightmap of the chunk that owns the world column. An
    /// unattached chunk reads as `min_y`.
    [[nodiscard]] auto height_at(heightmap_kind kind, jint x, jint z) const
        -> jint;

  private:
    [[nodiscard]] auto within_write_zone(const chunk_pos &pos) const -> bool;

    proto_chunk *center_{nullptr};
    jint write_radius_{0};
    std::map<chunk_pos, proto_chunk *> chunks_{};
    std::uint64_t dropped_writes_{0};
};

} // namespace refrain::worldgen
