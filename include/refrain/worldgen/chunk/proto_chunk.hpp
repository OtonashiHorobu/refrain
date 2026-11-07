// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The mutable chunk the generation stages share before it becomes a level
// chunk: dense block states, the 4x4x4 biome grid, the six heightmaps, block
// entities and scheduled fluid post-processing. Status drives which heightmaps
// a write updates, mirroring `ChunkStatus.heightmapsAfter`.

#pragma once

#include <refrain/data/nbt.hpp>
#include <refrain/worldgen/chunk/block_state.hpp>
#include <refrain/worldgen/chunk/heightmap.hpp>
#include <refrain/worldgen/chunk/status.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::worldgen {

struct block_pos {
    jint x{0};
    jint y{0};
    jint z{0};

    friend constexpr auto operator<=>(const block_pos &,
                                      const block_pos &) = default;
};

struct structure_piece_record {
    std::string type{};
    jint min_x{0};
    jint min_y{0};
    jint min_z{0};
    jint max_x{0};
    jint max_y{0};
    jint max_z{0};
    jint orientation{-1};
    std::string template_id{};
    jint template_x{0};
    jint template_y{0};
    jint template_z{0};
    jint template_rotation{0};
    jint template_mirror{0};
    jint template_pivot_x{0};
    jint template_pivot_y{0};
    jint template_pivot_z{0};
};

struct structure_start {
    std::string structure{};
    std::string step{};
    jint chunk_x{0};
    jint chunk_z{0};
    jint min_x{0};
    jint min_y{0};
    jint min_z{0};
    jint max_x{0};
    jint max_y{0};
    jint max_z{0};
    std::vector<structure_piece_record> pieces{};
};

struct structure_reference {
    std::string structure{};
    jint chunk_x{0};
    jint chunk_z{0};
    jint count{1};
};

class proto_chunk {
  public:
    proto_chunk(const block_state_table &table, jint chunk_x, jint chunk_z,
                jint min_y, jint height);
    proto_chunk(const proto_chunk &) = delete;
    auto operator=(const proto_chunk &) -> proto_chunk & = delete;
    proto_chunk(proto_chunk &&) = delete;
    auto operator=(proto_chunk &&) -> proto_chunk & = delete;
    ~proto_chunk() = default;

    [[nodiscard]] auto chunk_x() const noexcept -> jint { return chunk_x_; }
    [[nodiscard]] auto chunk_z() const noexcept -> jint { return chunk_z_; }
    [[nodiscard]] auto min_y() const noexcept -> jint { return min_y_; }
    [[nodiscard]] auto height() const noexcept -> jint { return height_; }
    [[nodiscard]] auto section_count() const noexcept -> jint {
        return section_count_;
    }
    [[nodiscard]] auto table() const noexcept -> const block_state_table & {
        return *table_;
    }

    [[nodiscard]] auto state_at(jint x, jint y, jint z) const -> block_state_id;
    /// Writes one block and updates the heightmaps the current status keeps.
    /// Returns false when the position is outside the build height.
    auto set_state(jint x, jint y, jint z, block_state_id state) -> bool;

    void add_structure_start(structure_start start);
    void add_structure_reference(std::string_view structure, jint chunk_x,
                                 jint chunk_z, jint count = 1);
    [[nodiscard]] auto structure_starts() const
        -> const std::vector<structure_start> & {
        return structure_starts_;
    }
    [[nodiscard]] auto structure_references() const
        -> const std::vector<structure_reference> & {
        return structure_references_;
    }

    [[nodiscard]] auto status() const noexcept -> chunk_status {
        return status_;
    }
    /// Completing `terrain` primes the four final heightmaps from the blocks
    /// `doFill`, `buildSurface` and the carvers produced.
    void set_status(chunk_status status);

    [[nodiscard]] auto heightmap_at(heightmap_kind kind) -> heightmap & {
        return heightmaps_[static_cast<std::size_t>(kind)];
    }
    [[nodiscard]] auto heightmap_at(heightmap_kind kind) const
        -> const heightmap & {
        return heightmaps_[static_cast<std::size_t>(kind)];
    }
    [[nodiscard]] auto first_available(heightmap_kind kind, jint x,
                                       jint z) const -> jint;

    /// Biome id of the 4x4x4 cell containing the block position.
    [[nodiscard]] auto biome_at(jint x, jint y, jint z) const -> std::uint16_t;
    void set_biome(jint x, jint y, jint z, std::uint16_t biome);

    void set_block_entity(const block_pos &pos, data::nbt::compound entity);
    [[nodiscard]] auto block_entity_at(const block_pos &pos) const
        -> const data::nbt::compound *;
    void remove_block_entity(const block_pos &pos);
    [[nodiscard]] auto block_entities() const
        -> const std::map<block_pos, data::nbt::compound> & {
        return block_entities_;
    }

    /// `ChunkAccess.markPosForPostProcessing`: packed `x | y<<4 | z<<8`.
    void mark_post_processing(jint x, jint y, jint z);
    [[nodiscard]] auto post_processing() const
        -> const std::vector<std::vector<std::uint16_t>> & {
        return post_processing_;
    }

  private:
    [[nodiscard]] auto state_index(jint x, jint y, jint z) const -> std::size_t;
    [[nodiscard]] auto biome_index(jint x, jint y, jint z) const -> std::size_t;
    [[nodiscard]] auto source() const -> const heightmap_source &;
    void update_heightmaps(jint x, jint y, jint z, block_state_id state);
    void prime_heightmaps(std::array<heightmap_kind, 4> kinds);

    const block_state_table *table_{nullptr};
    heightmap_source source_{};
    std::vector<structure_start> structure_starts_{};
    std::vector<structure_reference> structure_references_{};
    jint chunk_x_{0};
    jint chunk_z_{0};
    jint min_y_{0};
    jint height_{0};
    jint section_count_{0};
    std::vector<block_state_id> states_{};
    std::vector<std::uint16_t> biomes_{};
    std::array<heightmap, heightmap_kind_count> heightmaps_{};
    std::map<block_pos, data::nbt::compound> block_entities_{};
    std::vector<std::vector<std::uint16_t>> post_processing_{};
    chunk_status status_{chunk_status::empty};
};

} // namespace refrain::worldgen
