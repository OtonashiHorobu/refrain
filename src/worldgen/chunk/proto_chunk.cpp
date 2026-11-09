// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/chunk/proto_chunk.hpp>

#include <algorithm>

namespace refrain::worldgen {

namespace {

constexpr std::array<heightmap_kind, 2> worldgen_heightmaps{
    heightmap_kind::world_surface_wg,
    heightmap_kind::ocean_floor_wg,
};

constexpr std::array<heightmap_kind, 4> final_heightmaps{
    heightmap_kind::world_surface,
    heightmap_kind::ocean_floor,
    heightmap_kind::motion_blocking,
    heightmap_kind::motion_blocking_no_leaves,
};

} // namespace

proto_chunk::proto_chunk(const block_state_table &table, jint chunk_x,
                         jint chunk_z, jint min_y, jint height)
    : table_{&table}, chunk_x_{chunk_x}, chunk_z_{chunk_z}, min_y_{min_y},
      height_{height}, section_count_{height / 16},
      states_(static_cast<std::size_t>(height) * 256U, table.air()),
      biomes_(static_cast<std::size_t>(section_count_) * 64U, 0),
      post_processing_(static_cast<std::size_t>(section_count_)) {
    for (std::size_t kind = 0; kind < heightmap_kind_count; ++kind) {
        heightmaps_[kind] =
            heightmap{static_cast<heightmap_kind>(kind), min_y, height};
    }
    source_ = heightmap_source{
        .state_at = [this](jint x, jint y,
                           jint z) { return state_at(x, y, z); },
        .traits_of =
            [this](std::uint16_t state) { return table_->traits(state); },
    };
}

auto proto_chunk::state_index(jint x, jint y, jint z) const -> std::size_t {
    return static_cast<std::size_t>(y - min_y_) * 256U +
           static_cast<std::size_t>(z) * 16U + static_cast<std::size_t>(x);
}

auto proto_chunk::biome_index(jint x, jint y, jint z) const -> std::size_t {
    const auto section = static_cast<std::size_t>((y - min_y_) >> 4);
    const auto cell = static_cast<std::size_t>(((y - min_y_) & 15) >> 2) * 16U +
                      static_cast<std::size_t>((z & 15) >> 2) * 4U +
                      static_cast<std::size_t>((x & 15) >> 2);
    return section * 64U + cell;
}

auto proto_chunk::state_at(jint x, jint y, jint z) const -> block_state_id {
    if (x < 0 || x > 15 || z < 0 || z > 15 || y < min_y_ ||
        y >= min_y_ + height_) {
        return table_->air();
    }
    return states_[state_index(x, y, z)];
}

auto proto_chunk::set_state(jint x, jint y, jint z, block_state_id state)
    -> bool {
    if (x < 0 || x > 15 || z < 0 || z > 15 || y < min_y_ ||
        y >= min_y_ + height_) {
        return false;
    }
    states_[state_index(x, y, z)] = state;
    update_heightmaps(x, y, z, state);
    return true;
}

void proto_chunk::set_status(chunk_status status) {
    if (status_at_least(status, chunk_status::terrain) &&
        !status_at_least(status_, chunk_status::terrain)) {
        prime_heightmaps(final_heightmaps);
    }
    status_ = status;
}

auto proto_chunk::first_available(heightmap_kind kind, jint x, jint z) const
    -> jint {
    return heightmap_at(kind).first_available(x, z);
}

auto proto_chunk::biome_at(jint x, jint y, jint z) const -> std::uint16_t {
    if (x < 0 || x > 15 || z < 0 || z > 15 || y < min_y_ ||
        y >= min_y_ + height_) {
        return 0;
    }
    return biomes_[biome_index(x, y, z)];
}

void proto_chunk::set_biome(jint x, jint y, jint z, std::uint16_t biome) {
    if (x < 0 || x > 15 || z < 0 || z > 15 || y < min_y_ ||
        y >= min_y_ + height_) {
        return;
    }
    biomes_[biome_index(x, y, z)] = biome;
}

void proto_chunk::set_block_entity(const block_pos &pos,
                                   data::nbt::compound entity) {
    if (pos.x < 0 || pos.x > 15 || pos.z < 0 || pos.z > 15 || pos.y < min_y_ ||
        pos.y >= min_y_ + height_) {
        return;
    }
    block_entities_[pos] = std::move(entity);
}

auto proto_chunk::block_entity_at(const block_pos &pos) const
    -> const data::nbt::compound * {
    const auto it = block_entities_.find(pos);
    return it == block_entities_.end() ? nullptr : &it->second;
}

void proto_chunk::remove_block_entity(const block_pos &pos) {
    block_entities_.erase(pos);
}

void proto_chunk::mark_post_processing(jint x, jint y, jint z) {
    if (x < 0 || x > 15 || z < 0 || z > 15 || y < min_y_ ||
        y >= min_y_ + height_) {
        return;
    }
    const auto packed =
        static_cast<std::uint16_t>(x | ((y & 15) << 4) | ((z & 15) << 8));
    post_processing_[static_cast<std::size_t>((y - min_y_) >> 4)].push_back(
        packed);
}

auto proto_chunk::source() const -> const heightmap_source & { return source_; }

void proto_chunk::update_heightmaps(jint x, jint y, jint z,
                                    block_state_id state) {
    if (status_at_least(status_, chunk_status::terrain)) {
        for (const auto kind : final_heightmaps) {
            heightmap_at(kind).update(x, y, z, state, source_);
        }
        return;
    }
    for (const auto kind : worldgen_heightmaps) {
        heightmap_at(kind).update(x, y, z, state, source_);
    }
}

void proto_chunk::prime_heightmaps(std::array<heightmap_kind, 4> kinds) {
    // One top-down pass over each column fills every map at once. The world
    // surface map is exact at this point and no final map can match above the
    // highest non-air block, so the scan starts at that block instead of the
    // level ceiling.
    std::array<heightmap *, 4> maps{};
    std::array<bool, 4> found{};
    for (std::size_t index = 0; index < kinds.size(); ++index) {
        maps[index] = &heightmap_at(kinds[index]);
    }
    const auto &surface = heightmap_at(heightmap_kind::world_surface_wg);
    for (jint z = 0; z < 16; ++z) {
        for (jint x = 0; x < 16; ++x) {
            found.fill(false);
            std::size_t remaining = maps.size();
            const jint top = surface.first_available(x, z) - 1;
            for (jint y = top; y >= min_y_ && remaining > 0; --y) {
                const auto state = states_[state_index(x, y, z)];
                const auto traits = table_->traits(state);
                for (std::size_t index = 0; index < maps.size(); ++index) {
                    if (!found[index] && matches(kinds[index], traits)) {
                        maps[index]->set_first_available(x, z, y + 1);
                        found[index] = true;
                        --remaining;
                    }
                }
            }
            for (std::size_t index = 0; index < maps.size(); ++index) {
                if (!found[index]) {
                    maps[index]->set_first_available(x, z, min_y_);
                }
            }
        }
    }
}

void proto_chunk::add_structure_start(structure_start start) {
    structure_starts_.push_back(std::move(start));
}

void proto_chunk::add_structure_reference(std::string_view structure,
                                          jint chunk_x, jint chunk_z,
                                          jint count) {
    for (auto &reference : structure_references_) {
        if (reference.structure == structure && reference.chunk_x == chunk_x &&
            reference.chunk_z == chunk_z) {
            reference.count += count;
            return;
        }
    }
    structure_references_.push_back(
        structure_reference{.structure = std::string{structure},
                            .chunk_x = chunk_x,
                            .chunk_z = chunk_z,
                            .count = count});
}

} // namespace refrain::worldgen
