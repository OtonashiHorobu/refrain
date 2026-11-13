// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/world_bootstrap.hpp>

#include <refrain/world/create.hpp>
#include <refrain/world/proto_bridge.hpp>
#include <refrain/worldgen/chunk_generator.hpp>

#include <fmt/format.h>

#include <string>
#include <vector>

namespace refrain::server {

auto generate_spawn_area(world_port &world,
                         const world_bootstrap_config &config,
                         refrain::world::chunk_pos center)
    -> result<std::size_t> {
    if (config.data_root.empty()) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "spawn generation needs the datapack data root")};
    }
    auto generator{
        worldgen::chunk_generator::create(worldgen::chunk_generator_config{
            .data_root = config.data_root,
            .templates_root = config.templates_root,
            .seed = config.seed,
            .kind = worldgen::dimension_kind::overworld,
            .target = worldgen::generation_target::full,
        })};
    if (!generator) {
        return std::unexpected{generator.error()};
    }
    const auto radius{config.radius < 0 ? 0 : config.radius};
    const auto side{static_cast<std::int32_t>(radius * 2 + 1)};
    const auto origin{
        refrain::world::chunk_pos{center.x - radius, center.z - radius}};
    // The spawn square is generated through whole tiles, so every written
    // chunk holds the feature writes its neighbours made across its border.
    auto generated{
        (*generator)
            ->generate_area_tiled(worldgen::chunk_pos{origin.x, origin.z}, side,
                                  side, config.workers)};
    if (!generated) {
        return std::unexpected{generated.error()};
    }
    const auto &biomes{(*generator)->biome_names()};
    std::size_t written{0};
    std::size_t index{0};
    for (const auto &proto : *generated) {
        if (proto == nullptr) {
            return std::unexpected{make_error(
                error_code::unknown, "spawn generator returned no chunk")};
        }
        // `generate_area_tiled` returns the requested square in the same
        // x-outer, z-inner order.
        const auto x{origin.x + static_cast<std::int32_t>(index / side)};
        const auto z{origin.z + static_cast<std::int32_t>(index % side)};
        ++index;
        const refrain::world::chunk_pos pos{x, z};
        auto storage{refrain::world::from_proto_chunk(*proto, biomes)};
        if (!storage) {
            return std::unexpected{storage.error()};
        }
        if (auto stored{world.write_chunk(pos, *storage)}; !stored) {
            return std::unexpected{stored.error()};
        }
        ++written;
    }
    if (auto committed{world.commit()}; !committed) {
        return std::unexpected{committed.error()};
    }
    return written;
}

} // namespace refrain::server
