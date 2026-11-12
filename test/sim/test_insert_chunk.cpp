// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Adopting a generated column into the runtime world. The generator needs the
// local datapack, so the suite runs only when REFRAIN_VANILLA_WORLDGEN points
// at a data root.

#include <refrain/sim/world.hpp>
#include <refrain/world/create.hpp>
#include <refrain/world/proto_bridge.hpp>
#include <refrain/worldgen/chunk_generator.hpp>

#include <boost/ut.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace sim = refrain::sim;
namespace world = refrain::world;
namespace worldgen = refrain::worldgen;

[[nodiscard]] auto scratch_directory() -> std::filesystem::path {
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    const auto path{std::filesystem::temp_directory_path() /
                    ("refrain_insert_chunk_" + std::to_string(stamp))};
    std::error_code error;
    std::filesystem::create_directories(path, error);
    return path;
}

[[nodiscard]] auto data_root() -> std::string {
    const char *root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
    return root == nullptr ? std::string{} : std::string{root};
}

/// One generated overworld column at (0, 0).
[[nodiscard]] auto generated_column() -> world::chunk {
    auto generator{
        worldgen::chunk_generator::create(worldgen::chunk_generator_config{
            .data_root = data_root(),
            .templates_root = {},
            .seed = 7,
            .kind = worldgen::dimension_kind::overworld,
            .target = worldgen::generation_target::terrain,
        })};
    expect(static_cast<bool>(generator));
    if (!generator) {
        return {};
    }
    auto proto{(*generator)->generate(worldgen::chunk_pos{0, 0})};
    expect(static_cast<bool>(proto));
    if (!proto) {
        return {};
    }
    auto storage{world::from_proto_chunk(**proto, (*generator)->biome_names())};
    expect(static_cast<bool>(storage));
    if (!storage) {
        return {};
    }
    return std::move(*storage);
}

suite insert_chunk_tests = [] {
    "sim/world adopts a generated column and persists it"_test = [] {
        const auto root{data_root()};
        if (root.empty()) {
            std::fprintf(stderr,
                         "skipping the generated chunk test: set "
                         "REFRAIN_VANILLA_WORLDGEN to a datapack data root\n");
            return;
        }
        const auto directory = scratch_directory();
        const auto path = (directory / "world").string();
        expect(static_cast<bool>(world::create_world(path, 7, "probe")));

        auto column{generated_column()};
        const auto pos{sim::chunk_pos{0, 0}};
        std::uint32_t surface{0};
        if (!column.sections.empty()) {
            auto opened{sim::world::open(path)};
            expect(static_cast<bool>(opened)) << opened.error().message;
            if (opened) {
                auto inserted{opened->insert_chunk(pos, std::move(column))};
                expect(static_cast<bool>(inserted)) << inserted.error().message;
                if (inserted) {
                    expect(opened->find_chunk(pos) != nullptr);
                    // The column starts at the world bottom; terrain is above
                    // it, so the bottom block must be solid.
                    expect(opened->block_at(sim::block_pos{
                               0, static_cast<std::int16_t>(-64), 0}) !=
                           sim::air_block);

                    // Adopting the same coordinate twice is refused.
                    auto duplicate{opened->insert_chunk(pos, world::chunk{})};
                    expect(!duplicate);
                    if (!duplicate) {
                        expect(duplicate.error().code ==
                               refrain::error_code::already_exists);
                    }

                    surface = opened->block_at(
                        sim::block_pos{0, static_cast<std::int16_t>(64), 0});
                    expect(static_cast<bool>(opened->flush()));
                }
            }
        }

        auto reopened{sim::world::open(path)};
        expect(static_cast<bool>(reopened)) << reopened.error().message;
        if (reopened && !column.sections.empty()) {
            expect(reopened->find_chunk(pos) != nullptr);
            expect(reopened->block_at(sim::block_pos{
                       0, static_cast<std::int16_t>(64), 0}) == surface);
        }

        std::error_code error;
        std::filesystem::remove_all(directory, error);
    };
};

} // namespace
