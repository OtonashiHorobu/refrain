// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/generator.hpp>

#include <algorithm>

namespace refrain::worldgen {

generator::generator(const block_state_table &states, jint min_y, jint height,
                     generator_stages stages)
    : states_{&states}, min_y_{min_y}, height_{height},
      stages_{std::move(stages)} {}

auto generator::generate_terrain(const chunk_pos &pos)
    -> result<std::unique_ptr<proto_chunk>> {
    if (!stages_.fill) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "generator has no fill stage")};
    }

    auto chunk =
        std::make_unique<proto_chunk>(*states_, pos.x, pos.z, min_y_, height_);
    chunk->set_status(chunk_status::biomes);
    if (stages_.biomes) {
        if (const auto staged = stages_.biomes(*chunk); !staged) {
            return std::unexpected{staged.error()};
        }
    }
    stages_.fill(*chunk);

    if (stages_.surface) {
        if (const auto staged = stages_.surface(*chunk); !staged) {
            return std::unexpected{staged.error()};
        }
    }
    if (stages_.carve) {
        if (const auto staged = stages_.carve(*chunk); !staged) {
            return std::unexpected{staged.error()};
        }
    }

    // Completing terrain primes the four final heightmaps from everything the
    // stages wrote.
    chunk->set_status(chunk_status::terrain);
    return chunk;
}

auto generator::generate(const chunk_pos &pos, chunk_status target)
    -> result<std::unique_ptr<proto_chunk>> {
    if (target == chunk_status::terrain) {
        return generate_terrain(pos);
    }
    if (target != chunk_status::features && target != chunk_status::full) {
        return std::unexpected{make_error(
            error_code::unsupported, "generation status is not implemented")};
    }
    auto chunk = generate_terrain(pos);
    if (!chunk) {
        return chunk;
    }
    if (stages_.structures) {
        if (const auto staged = stages_.structures(**chunk); !staged) {
            return std::unexpected{staged.error()};
        }
    }
    if (stages_.features) {
        if (const auto staged = stages_.features(**chunk); !staged) {
            return std::unexpected{staged.error()};
        }
    }
    (*chunk)->set_status(target);
    return chunk;
}

} // namespace refrain::worldgen
