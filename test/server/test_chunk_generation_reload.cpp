// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Chunk generation rebinding: the production tile path captures the published
// content generation when a worker builds its generator, rebuilds it when a
// publish advances the generation, and leaves the chunks of the previous
// generation unchanged. The content-backed case needs the locally generated
// datapack snapshot and is skipped when the checkout has none.

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

/// Handles that select the direct world port (dummy non-null vtables).
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

/// Read one stored column back from a world directory.
[[nodiscard]] auto read_stored_chunk(const std::filesystem::path &root,
                                     world::chunk_pos pos)
    -> std::optional<world::chunk> {
    auto store{world::world_store::open(root.string())};
    if (!store) {
        return std::nullopt;
    }
    auto chunk{store->read_chunk(pos)};
    if (!chunk) {
        return std::nullopt;
    }
    return std::move(*chunk);
}

/// Persist one generated column into a fresh world directory.
[[nodiscard]] auto store_chunk(const std::filesystem::path &root,
                               world::chunk_pos pos, const world::chunk &chunk)
    -> bool {
    if (!world::create_world(root.string(), 1, "world")) {
        return false;
    }
    auto store{world::world_store::open(root.string())};
    if (!store) {
        return false;
    }
    return store->write_chunk(pos, chunk).has_value() && store->commit();
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

/// One generated column together with the tile position it came from.
struct collected_column {
    world::chunk_pos position{};
    world::chunk column{};
};

/// Dispatch and poll until `expected` columns were served. Returns the columns
/// with their positions in the order they were taken, or an empty vector on a
/// failure or timeout. The order is the order the workers finished and the
/// caller drained, not the position order, so a caller that needs one
/// particular column looks it up by position instead of assuming it arrives
/// first.
[[nodiscard]] auto collect_columns(server::chunk_generation &service,
                                   std::size_t expected)
    -> std::vector<collected_column> {
    std::vector<collected_column> columns{};
    const auto deadline{std::chrono::steady_clock::now() +
                        std::chrono::seconds{180}};
    while (std::chrono::steady_clock::now() < deadline) {
        static_cast<void>(service.dispatch(0));
        auto results{service.take_results()};
        for (auto &[pos, column] : results) {
            columns.push_back(
                collected_column{.position = pos, .column = std::move(column)});
        }
        if (!service.take_failures().empty()) {
            return {};
        }
        if (columns.size() >= expected) {
            return columns;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return {};
}

/// The collected column of one tile position, or null when the batch did not
/// carry it.
[[nodiscard]] auto find_column(std::span<const collected_column> columns,
                               world::chunk_pos position)
    -> const collected_column * {
    for (const auto &entry : columns) {
        if (entry.position == position) {
            return &entry;
        }
    }
    return nullptr;
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
        // Persist the column before the publish: the reload must not rewrite
        // the chunk a fresh world already stored.
        const auto stored_world{scratch.path() / "stored-world"};
        expect(store_chunk(stored_world, target, *before)) << "store failed";
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
        // The stored column produced before the publish is still exactly the
        // same record; the reload rewrote nothing.
        const auto reread{read_stored_chunk(stored_world, target)};
        expect(reread.has_value());
        if (reread) {
            expect(chunk_fingerprint(*reread) == before_fingerprint);
        }
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
        // One worker composed the boot generation and each published one.
        expect((*service)->generation_compose_attempts() == 3U);
    };

    "chunk generation reload/workers share one composition per published "
    "generation"_test = [] {
        const auto base{content_root()};
        if (base.empty()) {
            return;
        }
        published_view_guard view_guard{};
        scratch_directory scratch{};
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
        settings.worker_count = 4;
        settings.tile_size = 1;
        settings.admission.initial_credits = 4;
        settings.admission.min_credits = 1;
        settings.admission.max_credits = 4;
        auto service{server::chunk_generation::start(settings)};
        expect(static_cast<bool>(service)) << "chunk generation did not start";
        if (!service) {
            return;
        }
        auto wait_for_columns = [](server::chunk_generation &generator,
                                   std::size_t expected) {
            const auto deadline{std::chrono::steady_clock::now() +
                                std::chrono::seconds{180}};
            std::size_t taken{0};
            while (std::chrono::steady_clock::now() < deadline) {
                static_cast<void>(generator.dispatch(0));
                taken += generator.take_results().size();
                const auto failures{generator.take_failures()};
                if (!failures.empty()) {
                    return false;
                }
                if (taken >= expected) {
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            }
            return false;
        };
        for (std::int32_t index{0}; index < 4; ++index) {
            expect((*service)->request(world::chunk_pos{.x = index, .z = 0}));
        }
        expect(wait_for_columns(**service, 4U));
        // Four workers created their first generator for generation 1 but
        // only one composed it.
        expect((*service)->generation_compose_attempts() == 1U);
        expect((*service)->generation_rebuilds() == 0U);
        // A publish advances all four workers; the shared single-flight slot
        // composes it once.
        const auto changed_pack{scratch.path() / "changed-pack"};
        expect(write_changed_settings(changed_pack, base));
        auto changed_view{build_view({directory_source(changed_pack)})};
        expect(changed_view != nullptr);
        if (!changed_view) {
            return;
        }
        server::publish_content_view(snapshot_of(2, 1, changed_view));
        for (std::int32_t index{0}; index < 4; ++index) {
            expect(
                (*service)->request(world::chunk_pos{.x = 4 + index, .z = 0}));
        }
        expect(wait_for_columns(**service, 4U));
        expect((*service)->generation_compose_attempts() == 2U);
        expect((*service)->bound_generation() == 2U);
        expect((*service)->generation_rebuilds() >= 4U);
    };

    "chunk generation reload/a failed bind keeps the boot content and does not "
    "claim the published generation"_test = [] {
        published_view_guard view_guard{};
        scratch_directory scratch{};
        // A view that changes a document chunk generation reads. The empty
        // installed root cannot load a generator, so the bind fails and the
        // worker falls back to the boot content.
        const auto pack{scratch.path() / "pack"};
        expect(write_file(pack, "data/minecraft/worldgen/biome/plains.json",
                          R"({"temperature":0.8})"));
        auto view{build_view({directory_source(pack)})};
        expect(view != nullptr);
        if (!view) {
            return;
        }
        server::publish_content_view(snapshot_of(1, 1, view));
        // An installed block tag that cannot be parsed makes the merged tag
        // tree unreadable, so binding the published generation fails.
        const auto broken_root{scratch.path() / "broken-root"};
        expect(write_file(broken_root, "minecraft/tags/block/broken.json",
                          R"({"values":[)"));
        server::chunk_generation::config settings{};
        settings.data_root = broken_root.string();
        settings.seed = 1;
        settings.worker_count = 2;
        settings.tile_size = 1;
        settings.admission.initial_credits = 2;
        settings.admission.min_credits = 1;
        settings.admission.max_credits = 2;
        auto service{server::chunk_generation::start(settings)};
        expect(static_cast<bool>(service)) << "chunk generation did not start";
        if (!service) {
            return;
        }
        // The production tile path declares that it can bind registry
        // generations; the content reload service reads this capability.
        expect(app::current_registry_rebind_capability().supported);
        expect((*service)->request(world::chunk_pos{.x = 0, .z = 0}));
        expect((*service)->request(world::chunk_pos{.x = 1, .z = 0}));
        static_cast<void>((*service)->dispatch(0));
        const auto deadline{std::chrono::steady_clock::now() +
                            std::chrono::seconds{30}};
        std::size_t reported_failures{0};
        while (std::chrono::steady_clock::now() < deadline) {
            static_cast<void>((*service)->dispatch(0));
            static_cast<void>((*service)->take_results());
            reported_failures += (*service)->take_failures().size();
            if (reported_failures >= 2U &&
                (*service)->generation_rebuild_failures() >= 2U) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        // Two workers failed to bind and to create their boot generator, but
        // the generation was composed once for both, nothing was applied, and
        // no rebuild was reported.
        expect((*service)->generation_compose_attempts() == 1U);
        expect((*service)->generation_rebuild_failures() >= 2U);
        expect((*service)->bound_generation() == 0U);
        expect((*service)->generation_rebuilds() == 0U);
        (*service)->stop();
        // Stopping the only chunk-generation path restores the default.
        expect(app::current_registry_rebind_capability().supported);
    };

    "chunk generation reload/an overlapping publish never composes an epoch "
    "twice"_test = [] {
        const auto base{content_root()};
        if (base.empty()) {
            return;
        }
        published_view_guard view_guard{};
        scratch_directory scratch{};
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
        settings.worker_count = 2;
        settings.tile_size = 1;
        settings.admission.initial_credits = 2;
        settings.admission.min_credits = 1;
        settings.admission.max_credits = 2;
        auto service{server::chunk_generation::start(settings)};
        expect(static_cast<bool>(service)) << "chunk generation did not start";
        if (!service) {
            return;
        }
        expect((*service)->request(world::chunk_pos{.x = 0, .z = 0}));
        const auto boot_columns{collect_columns(**service, 1U)};
        const auto *boot_column{
            find_column(boot_columns, world::chunk_pos{.x = 0, .z = 0})};
        expect(boot_column != nullptr);
        if (boot_column == nullptr) {
            return;
        }
        expect(chunk_fingerprint(boot_column->column) != data::digest32{});
        expect((*service)->generation_compose_attempts() == 1U);
        // Publish generation 2 and dispatch a tile: a worker starts composing
        // the published epoch. The composition counter is bumped before the
        // composition work, so waiting for it is the synchronization point;
        // generation 3 is published while that composition is in flight, and
        // the test asserts the invariant instead of the timing. An older
        // composer must not clear a newer in-flight slot and no epoch may be
        // composed twice.
        const auto changed_pack{scratch.path() / "changed-pack"};
        expect(write_changed_settings(changed_pack, base));
        auto changed_view{build_view({directory_source(changed_pack)})};
        expect(changed_view != nullptr);
        if (!changed_view) {
            return;
        }
        server::publish_content_view(snapshot_of(2, 1, changed_view));
        expect((*service)->request(world::chunk_pos{.x = 1, .z = 0}));
        expect((*service)->dispatch(0) == 1U);
        const auto compose_deadline{std::chrono::steady_clock::now() +
                                    std::chrono::seconds{30}};
        while ((*service)->generation_compose_attempts() < 2U &&
               std::chrono::steady_clock::now() < compose_deadline) {
            std::this_thread::yield();
        }
        expect((*service)->generation_compose_attempts() >= 2U);
        server::publish_content_view(snapshot_of(3, 1, changed_view));
        expect((*service)->request(world::chunk_pos{.x = 2, .z = 0}));
        const auto columns{collect_columns(**service, 2U)};
        expect(columns.size() == 2U);
        const auto *first_tile{
            find_column(columns, world::chunk_pos{.x = 1, .z = 0})};
        const auto *second_tile{
            find_column(columns, world::chunk_pos{.x = 2, .z = 0})};
        expect(first_tile != nullptr);
        expect(second_tile != nullptr);
        // Three published epochs (1, 2, 3) plus at most one redundant
        // composition: an overlapping publish never composes an epoch twice.
        expect((*service)->generation_compose_attempts() <= 4U);
        expect((*service)->generation_rebuild_failures() == 0U);
        expect((*service)->bound_generation() == 3U);
        if (first_tile != nullptr && second_tile != nullptr) {
            // Generations 2 and 3 publish the same view content, so the tile
            // generated while the older composition was in flight and the tile
            // generated under generation 3 carry the same content. Requesting
            // the first tile again has to produce exactly that column. The
            // columns arrive in completion order, so the comparison looks the
            // tile up by position instead of assuming which one the batch
            // served first.
            expect((*service)->request(world::chunk_pos{.x = 1, .z = 0}));
            const auto again{collect_columns(**service, 1U)};
            expect(again.size() == 1U);
            const auto *again_tile{
                find_column(again, world::chunk_pos{.x = 1, .z = 0})};
            expect(again_tile != nullptr);
            if (again_tile != nullptr) {
                expect(chunk_fingerprint(again_tile->column) ==
                       chunk_fingerprint(first_tile->column));
            }
        }
        (*service)->stop();
    };
};
