// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Chunk generation rebinding: the production tile path captures the published
// content generation when a worker builds its generator, rebuilds it when a
// publish advances the generation, and leaves the chunks of the previous
// generation unchanged. The content-backed case needs the locally generated
// datapack snapshot and is skipped when the checkout has none.

#include <refrain/core/io.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/content_view.hpp>
#include <refrain/server/chunk_generation.hpp>
#include <refrain/server/content_reload_inputs.hpp>
#include <refrain/world/proto_bridge.hpp>
#include <refrain/worldgen/chunk_generator.hpp>

#include <boost/ut.hpp>

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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

[[nodiscard]] auto directory_source(const std::filesystem::path &root)
    -> data::content_source {
    return data::content_source{
        .kind = data::content_source_kind::directory,
        .path = root.string(),
        .label = root.string(),
    };
}

[[nodiscard]] auto snapshot_of(std::uint64_t generation,
                               std::uint64_t registry_generation,
                               std::shared_ptr<const data::content_view> view)
    -> app::content_view_snapshot {
    auto registries{view != nullptr
                        ? std::make_shared<const data::registry_generation>(
                              view->registries())
                        : std::shared_ptr<const data::registry_generation>{}};
    return app::content_view_snapshot{
        .generation = generation,
        .view = std::move(view),
        .registry_generation = registry_generation,
        .registries = std::move(registries),
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

/// Dispatch and poll until the tile of `position` produced a column or a
/// failure. Returns nothing on failure or timeout.
[[nodiscard]] auto
generate_column(server::chunk_generation &service, world::chunk_pos position,
                std::chrono::seconds timeout = std::chrono::seconds{180})
    -> std::optional<world::chunk> {
    if (!service.request(position)) {
        return std::nullopt;
    }
    const auto deadline{std::chrono::steady_clock::now() + timeout};
    while (std::chrono::steady_clock::now() < deadline) {
        static_cast<void>(service.dispatch(0));
        auto results{service.take_results()};
        for (auto &[pos, column] : results) {
            if (pos.x == position.x && pos.z == position.z) {
                return std::move(column);
            }
        }
        auto failures{service.take_failures()};
        if (!failures.empty()) {
            std::fprintf(stderr, "chunk generation failed: %s\n",
                         failures.front().message.c_str());
            return std::nullopt;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return std::nullopt;
}

/// The datapack root for the content-backed case. Prints one line and returns
/// empty when the checkout has no snapshot.
[[nodiscard]] auto content_root() -> std::string {
    if (const auto *value{std::getenv("REFRAIN_CONTENT_DATA_ROOT")};
        value != nullptr && std::string_view{value}.size() > 0) {
        return std::string{value};
    }
    const auto root{std::string{REFRAIN_SOURCE_DIR} +
                    "/content/vanilla/26.3/data"};
    if (!std::filesystem::is_directory(root + "/minecraft/worldgen")) {
        std::fprintf(stderr,
                     "chunk generation reload: datapack snapshot missing; set "
                     "REFRAIN_CONTENT_DATA_ROOT or run "
                     "scripts/fetch-content.sh <server.jar>\n");
        return {};
    }
    return root;
}

/// The view overlay that changes one document chunk generation reads: the
/// overworld default block becomes glass, so every generated column differs.
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

/// Clears the process-wide published snapshot when a test returns, including
/// the early returns that guard against a failed fixture.
struct published_view_guard {
    published_view_guard() = default;
    published_view_guard(const published_view_guard &) = delete;
    auto operator=(const published_view_guard &) = delete;
    ~published_view_guard() { server::clear_content_view(); }
};

} // namespace

const suite chunk_generation_reload_tests = [] {
    "chunk generation reload/the tile path rebinds to the published "
    "generation"_test = [] {
        const auto base{content_root()};
        if (base.empty()) {
            return;
        }
        published_view_guard view_guard{};
        scratch_directory scratch{};
        // Generation 1: a data-only view, so the first chunk is produced from
        // the installed root exactly as a server without a reload produces it.
        const auto recipe_pack{scratch.path() / "recipe-pack"};
        expect(write_file(recipe_pack, "data/test/recipe/alpha.json",
                          R"({"type":"minecraft:crafting_shapeless"})"));
        auto boot_view{build_view({directory_source(recipe_pack)})};
        expect(boot_view != nullptr);
        if (!boot_view) {
            return;
        }
        server::publish_content_view(snapshot_of(1, 1, boot_view));
        server::chunk_generation::config settings{};
        settings.data_root = base;
        settings.seed = 1;
        settings.worker_count = 1;
        settings.tile_size = 1;
        auto service{server::chunk_generation::start(settings)};
        expect(static_cast<bool>(service)) << "chunk generation did not start";
        if (!service) {
            return;
        }
        const world::chunk_pos target{.x = 0, .z = 0};
        const auto before{generate_column(**service, target)};
        expect(before.has_value());
        if (!before) {
            return;
        }
        const auto before_fingerprint{chunk_fingerprint(*before)};
        expect((*service)->bound_generation() == 1U);
        expect((*service)->generation_rebuilds() == 0U);
        // A server without a reload keeps the boot generator: the column is
        // exactly what the plain tile generator produces from the same root.
        {
            auto plain{worldgen::chunk_generator::create(
                worldgen::chunk_generator_config{
                    .data_root = base,
                    .seed = 1,
                    .kind = worldgen::dimension_kind::overworld,
                    .target = worldgen::generation_target::full,
                    .tile_size = 1,
                })};
            expect(static_cast<bool>(plain));
            if (plain) {
                auto generated{(*plain)->generate_tile(
                    worldgen::chunk_pos{.x = 0, .z = 0})};
                expect(static_cast<bool>(generated));
                if (generated && !generated->empty() &&
                    (*generated)[0] != nullptr) {
                    auto column{world::from_proto_chunk(
                        *(*generated)[0], (*plain)->biome_names())};
                    expect(static_cast<bool>(column));
                    if (column) {
                        expect(chunk_fingerprint(*column) ==
                               before_fingerprint);
                    }
                }
            }
        }
        // Generation 2: the view changes the overworld default block, so the
        // worker rebuilds its generator before the next chunk and the new
        // column uses the new document.
        const auto changed_pack{scratch.path() / "changed-pack"};
        expect(write_changed_settings(changed_pack, base));
        auto changed_view{build_view({directory_source(changed_pack)})};
        expect(changed_view != nullptr);
        if (!changed_view) {
            return;
        }
        server::publish_content_view(snapshot_of(2, 1, changed_view));
        const auto after{generate_column(**service, target)};
        expect(after.has_value());
        if (!after) {
            return;
        }
        expect(chunk_fingerprint(*after) != before_fingerprint);
        expect((*service)->bound_generation() == 2U);
        expect((*service)->generation_rebuilds() >= 1U);
        // The column produced before the publish keeps its content; the reload
        // rewrote nothing.
        expect(chunk_fingerprint(*before) == before_fingerprint);
        // Generation 3: a data-only view again, so the merged content returns
        // to the installed root and the new column is the boot content again.
        server::publish_content_view(snapshot_of(3, 2, std::move(boot_view)));
        const auto restored{generate_column(**service, target)};
        expect(restored.has_value());
        if (restored) {
            expect(chunk_fingerprint(*restored) == before_fingerprint);
        }
        expect((*service)->bound_generation() == 3U);
        expect((*service)->generation_rebuilds() >= 2U);
    };
};
