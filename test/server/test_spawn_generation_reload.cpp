// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Spawn prefill and the chunk-generation workers must read one content
// generation: a fresh world is filled before the runtime publishes the boot
// view, so the prefill takes the composition-root handoff and binds the same
// generation the workers will use. The case needs the locally generated
// datapack snapshot and prints one skip line without it.

#include <refrain/contracts/abi.hpp>
#include <refrain/core/io.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/content_view.hpp>
#include <refrain/server/chunk_generation.hpp>
#include <refrain/server/content_reload_inputs.hpp>
#include <refrain/server/world_bootstrap.hpp>
#include <refrain/server/world_port.hpp>
#include <refrain/world/create.hpp>
#include <refrain/world/proto_bridge.hpp>
#include <refrain/world/store.hpp>
#include <refrain/worldgen/chunk_generator.hpp>

#include <boost/ut.hpp>

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "support/scratch_directory.hpp"

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace app = refrain::app;
namespace data = refrain::data;
namespace io = refrain::core::io;
namespace server = refrain::server;
namespace world = refrain::world;
namespace worldgen = refrain::worldgen;
using refrain::test_support::scratch_directory;

[[nodiscard]] auto write_file(const std::filesystem::path &root,
                              std::string_view relative, std::string_view text)
    -> bool {
    const auto target{root / std::filesystem::path{std::string{relative}}};
    std::error_code error;
    std::filesystem::create_directories(target.parent_path(), error);
    if (error) {
        return false;
    }
    return io::write_text(target.string(), text).has_value();
}

[[nodiscard]] auto directory_source(const std::filesystem::path &root)
    -> data::content_source {
    return data::content_source{
        .kind = data::content_source_kind::directory,
        .path = root.string(),
        .label = root.string(),
    };
}

[[nodiscard]] auto
build_view(std::initializer_list<data::content_source> sources)
    -> std::shared_ptr<const data::content_view> {
    const std::vector<data::content_source> layers{sources};
    auto built{data::content_view::build(layers)};
    expect(static_cast<bool>(built)) << "content view build failed";
    if (!built) {
        return nullptr;
    }
    return std::make_shared<const data::content_view>(std::move(*built));
}

[[nodiscard]] auto snapshot_of(std::uint64_t generation,
                               std::shared_ptr<const data::content_view> view)
    -> app::content_view_snapshot {
    return app::content_view_snapshot{
        .generation = generation,
        .view = std::move(view),
        .registry_generation = 1,
        .registries = std::make_shared<const data::registry_generation>(
            data::registry_generation{}),
    };
}

[[nodiscard]] auto chunk_fingerprint(const world::chunk &chunk)
    -> data::digest32 {
    std::string text{fmt::format("y={} status={}", chunk.y_pos,
                                 static_cast<int>(chunk.status))};
    for (const auto &section : chunk.sections) {
        text += fmt::format("|s{} k{} b{} bc{}", section.y,
                            static_cast<int>(section.kind),
                            section.bits_per_entry, section.block_count);
        for (const auto &entry : section.palette) {
            text += ",";
            text += entry.key;
        }
        for (const auto word : section.data) {
            text += fmt::format(":{:x}", word);
        }
        text += fmt::format("|bk{} bb{}", static_cast<int>(section.biome_kind),
                            section.biome_bits_per_entry);
        for (const auto &biome : section.biome_palette) {
            text += ",";
            text += biome;
        }
        for (const auto word : section.biome_data) {
            text += fmt::format(":{:x}", word);
        }
    }
    return data::blake3(std::as_bytes(std::span{text.data(), text.size()}));
}

struct direct_handles {
    int dummy{0};
    server::server_service_handles handles{
        .world =
            reinterpret_cast<const refrain_services_world_vtable *>(&dummy),
        .world_self = &dummy,
        .sim = reinterpret_cast<const refrain_services_sim_vtable *>(&dummy),
        .sim_self = &dummy,
        .storage =
            reinterpret_cast<const refrain_services_storage_vtable *>(&dummy),
        .storage_self = &dummy,
        .world_provider = "refrain.world.vanilla",
        .sim_provider = "refrain.sim.vanilla",
        .storage_provider = "refrain.storage.rwf",
    };
};

struct published_view_guard {
    published_view_guard() = default;
    published_view_guard(const published_view_guard &) = delete;
    auto operator=(const published_view_guard &) = delete;
    ~published_view_guard() {
        server::clear_content_view();
        server::clear_content_reload_inputs();
    }
};

[[nodiscard]] auto content_root() -> std::string {
    if (const auto *value{std::getenv("REFRAIN_CONTENT_DATA_ROOT")};
        value != nullptr && std::string_view{value}.size() > 0) {
        return std::string{value};
    }
    const auto root{std::string{REFRAIN_SOURCE_DIR} +
                    "/content/vanilla/26.3/data"};
    if (!std::filesystem::is_directory(root + "/minecraft/worldgen")) {
        std::fprintf(stderr,
                     "spawn generation reload: datapack snapshot missing; set "
                     "REFRAIN_CONTENT_DATA_ROOT or run "
                     "scripts/fetch-content.sh <server.jar>\n");
        return {};
    }
    return root;
}

/// A view overlay that changes the overworld default block to glass, so both
/// the prefill and the streamed column differ from the installed content.
[[nodiscard]] auto write_changed_settings(const std::filesystem::path &pack,
                                          const std::string &base) -> bool {
    auto settings{
        io::read_text((std::filesystem::path{base} /
                       "minecraft/worldgen/noise_settings/overworld.json")
                          .string())};
    if (!settings) {
        return false;
    }
    constexpr std::string_view stone{R"("default_block": "minecraft:stone")"};
    constexpr std::string_view glass{R"("default_block": "minecraft:glass")"};
    const auto found{settings->find(stone)};
    if (found == std::string::npos) {
        return false;
    }
    settings->replace(found, stone.size(), glass);
    return write_file(pack,
                      "data/minecraft/worldgen/noise_settings/overworld.json",
                      *settings);
}

[[nodiscard]] auto wait_for_column(server::chunk_generation &service,
                                   world::chunk_pos position)
    -> std::optional<world::chunk> {
    if (!service.request(position)) {
        return std::nullopt;
    }
    const auto deadline{std::chrono::steady_clock::now() +
                        std::chrono::seconds{180}};
    while (std::chrono::steady_clock::now() < deadline) {
        static_cast<void>(service.dispatch(0));
        auto results{service.take_results()};
        for (auto &[pos, column] : results) {
            if (pos.x == position.x && pos.z == position.z) {
                return std::move(column);
            }
        }
        if (!service.take_failures().empty()) {
            return std::nullopt;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return std::nullopt;
}

} // namespace

const suite spawn_generation_reload_tests = [] {
    "spawn generation reload/the spawn prefill and the workers use one "
    "generation"_test = [] {
        const auto base{content_root()};
        if (base.empty()) {
            return;
        }
        published_view_guard view_guard{};
        scratch_directory scratch{};
        const auto changed_pack{scratch.path() / "changed-pack"};
        expect(write_changed_settings(changed_pack, base));
        auto changed_view{build_view({directory_source(changed_pack)})};
        expect(changed_view != nullptr);
        if (!changed_view) {
            return;
        }
        // The composition root publishes the boot view before the runtime
        // starts; the prefill takes that handoff and puts it back.
        server::publish_content_reload_inputs(server::content_reload_inputs{
            .sources = {directory_source(changed_pack)},
            .initial = changed_view,
        });
        const auto world_path{scratch.path() / "world"};
        expect(static_cast<bool>(
            world::create_world(world_path.string(), 1, "world")))
            << "world creation failed";
        direct_handles handles{};
        auto port{server::make_world_port(handles.handles)};
        expect(port != nullptr);
        if (!port) {
            return;
        }
        expect(static_cast<bool>(port->open(world_path.string())))
            << "world open failed";
        const auto generated{
            server::generate_spawn_area(*port,
                                        server::world_bootstrap_config{
                                            .data_root = base,
                                            .templates_root = "",
                                            .seed = 1,
                                            .radius = 0,
                                            .workers = 1,
                                        },
                                        world::chunk_pos{.x = 0, .z = 0})};
        expect(static_cast<bool>(generated)) << "spawn generation failed";
        if (!generated) {
            return;
        }
        expect(*generated == 1U);
        port.reset();
        // The handoff survived the prefill for the runtime to consume.
        expect(server::take_content_reload_inputs().has_value());
        server::clear_content_reload_inputs();
        // The runtime publishes the same boot view as generation 1, and the
        // tile workers stream from it.
        server::publish_content_view(snapshot_of(1, changed_view));
        server::chunk_generation::config settings{};
        settings.data_root = base;
        settings.seed = 1;
        settings.worker_count = 1;
        auto service{server::chunk_generation::start(settings)};
        expect(static_cast<bool>(service)) << "chunk generation did not start";
        if (!service) {
            return;
        }
        const world::chunk_pos target{.x = 0, .z = 0};
        const auto streamed{wait_for_column(**service, target)};
        expect(streamed.has_value());
        if (!streamed) {
            return;
        }
        auto store{world::world_store::open(world_path.string())};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        auto prefilled{store->read_chunk(world::chunk_pos{0, 0})};
        expect(static_cast<bool>(prefilled));
        if (!prefilled) {
            return;
        }
        // The spawn chunk and the streamed chunk come from one generation.
        expect(chunk_fingerprint(*prefilled) == chunk_fingerprint(*streamed));
        // Both use the view's default block, not the installed one.
        auto boot{
            worldgen::chunk_generator::create(worldgen::chunk_generator_config{
                .data_root = base,
                .seed = 1,
                .kind = worldgen::dimension_kind::overworld,
                .target = worldgen::generation_target::full,
            })};
        expect(static_cast<bool>(boot));
        if (boot) {
            auto boot_tile{(*boot)->generate_tile(worldgen::chunk_pos{0, 0})};
            expect(static_cast<bool>(boot_tile));
            if (boot_tile && !boot_tile->empty() &&
                (*boot_tile)[0] != nullptr) {
                auto boot_column{world::from_proto_chunk(
                    *(*boot_tile)[0], (*boot)->biome_names())};
                expect(static_cast<bool>(boot_column));
                if (boot_column) {
                    expect(chunk_fingerprint(*boot_column) !=
                           chunk_fingerprint(*streamed));
                }
            }
        }
        (*service)->stop();
    };
};
