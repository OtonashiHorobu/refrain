// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/sim/world.hpp>

#include <refrain/core/io.hpp>
#include <refrain/data/nbt.hpp>
#include <refrain/platform/process.hpp>
#include <refrain/world/store.hpp>

#include <boost/ut.hpp>

#include <fmt/format.h>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>
#include <thread>

namespace {
using namespace boost::ut;

namespace fs = std::filesystem;
namespace nbt = refrain::data::nbt;
namespace storage = refrain::storage;
namespace world = refrain::world;

[[nodiscard]] auto scratch_directory() -> fs::path {
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    auto path{fs::temp_directory_path() /
              fmt::format("refrain-sim-{}-{}", refrain::platform::process_id(),
                          stamp)};
    std::error_code error{};
    fs::create_directories(path, error);
    return path;
}

[[nodiscard]] auto make_chunk() -> storage::chunk {
    storage::chunk chunk{};
    chunk.format_version = world::chunk_format_version;
    chunk.status = storage::chunk_status::full;
    chunk.y_pos = 0;
    storage::section_record section{};
    section.y = 0;
    section.kind = storage::section_kind::single_value;
    section.bits_per_entry = 0;
    section.block_count = static_cast<std::uint16_t>(world::section_volume);
    section.biome_kind = storage::section_kind::single_value;
    section.biome_bits_per_entry = 0;
    section.biome_palette.push_back("minecraft:plains");
    section.palette.push_back(storage::palette_entry{
        .key = "minecraft:stone",
        .count = static_cast<std::uint16_t>(world::section_volume),
        .state = 0,
    });
    chunk.sections.push_back(std::move(section));
    return chunk;
}

[[nodiscard]] auto make_store(const fs::path &directory,
                              std::span<const world::chunk_pos> extra = {})
    -> refrain::result<world::world_store> {
    auto store{world::world_store::create(directory.string())};
    if (!store) {
        return std::unexpected{std::move(store.error())};
    }
    const auto chunk{make_chunk()};
    auto written{store->write_chunk(world::chunk_pos{0, 0}, chunk)};
    if (!written) {
        return std::unexpected{std::move(written.error())};
    }
    for (const auto &pos : extra) {
        auto extra_written{store->write_chunk(pos, chunk)};
        if (!extra_written) {
            return std::unexpected{std::move(extra_written.error())};
        }
    }
    auto committed{store->commit()};
    if (!committed) {
        return std::unexpected{std::move(committed.error())};
    }
    return store;
}

/// Expected adaptive level for a player at the origin (test oracle for the
/// Chebyshev boundaries).
[[nodiscard]] auto expected_level_at_origin(const world::chunk_pos &pos)
    -> int {
    const auto ax{pos.x < 0 ? -pos.x : pos.x};
    const auto az{pos.z < 0 ? -pos.z : pos.z};
    const auto distance{ax > az ? ax : az};
    if (distance <= 2) {
        return refrain::sim::near_zstd_level;
    }
    if (distance <= 8) {
        return refrain::sim::mid_zstd_level;
    }
    return refrain::sim::far_zstd_level;
}

/// Register the synthetic player the console and replay scripts drive and
/// return its entity id (0 when registration failed).
[[nodiscard]] auto make_console_player(refrain::sim::world &world,
                                       double x = 0.5, double y = 0.0,
                                       double z = 0.5)
    -> refrain::sim::entity_id {
    auto added{world.add_player(refrain::sim::player_table::registration{
        .connection = refrain::sim::synthetic_connection,
        .name = "@console",
        .world = "overworld",
        .x = x,
        .y = y,
        .z = z,
        .game_mode = 1,
    })};
    return added ? *added : 0;
}

[[maybe_unused]] const suite tests = [] {
    "sim/block registry interns air and states"_test = [] {
        refrain::sim::block_registry registry{};
        expect(registry.size() == 1U);
        expect(registry.key(refrain::sim::air_block) == "minecraft:air");
        const auto stone{registry.intern("minecraft:stone")};
        const auto again{registry.intern("minecraft:stone")};
        expect(stone == again);
        expect(registry.find("minecraft:stone") == stone);
        expect(registry.find("minecraft:dirt") == std::nullopt);
    };

    "sim/block registry is safe for concurrent intern and reads"_test = [] {
        refrain::sim::block_registry registry{};
        std::atomic<bool> failed{false};
        std::thread writer{[&registry, &failed] {
            for (std::uint32_t index{0}; index < 2000U; ++index) {
                const auto key{fmt::format("minecraft:test_{}", index)};
                const auto id{registry.intern(std::string{key})};
                if (registry.key(id) != key) {
                    failed.store(true, std::memory_order_relaxed);
                }
            }
        }};
        std::thread reader{[&registry, &failed] {
            for (std::uint32_t index{0}; index < 2000U; ++index) {
                if (!registry.find("minecraft:air").has_value()) {
                    failed.store(true, std::memory_order_relaxed);
                }
                if (registry.size() == 0U) {
                    failed.store(true, std::memory_order_relaxed);
                }
            }
        }};
        writer.join();
        reader.join();
        expect(!failed.load(std::memory_order_relaxed));
        expect(registry.size() == 2001U);
        expect(registry.find("minecraft:test_1999").has_value());
    };

    "sim/chunk decode, edit and flush round-trips"_test = [] {
        const auto directory{scratch_directory()};
        {
            auto store{make_store(directory)};
            expect(store.has_value()) << (store ? "" : store.error().message);
            if (!store) {
                return;
            }
            refrain::data::digest32 runtime_hash{};
            {
                auto runtime{refrain::sim::world::open(directory.string())};
                expect(runtime.has_value());
                if (!runtime) {
                    return;
                }
                auto ensured{runtime->ensure_chunk({0, 0})};
                expect(ensured.has_value());
                if (!ensured) {
                    return;
                }
                const auto stone{runtime->registry().find("minecraft:stone")};
                expect(stone.has_value());
                if (!stone) {
                    return;
                }
                expect(runtime->block_at({0, 2, 3}) == *stone);
                expect(runtime->block_at({0, 40, 3}) ==
                       refrain::sim::air_block);

                const auto dirt{runtime->registry().intern("minecraft:dirt")};
                expect(runtime->set_block({0, 2, 3}, dirt).has_value());
                expect(runtime->block_at({0, 2, 3}) == dirt);
                expect(!runtime->set_block({0, 2000, 3}, dirt).has_value());
                expect(
                    runtime
                        ->add_ticket({0, 0}, refrain::sim::ticket_kind::forced)
                        .has_value());
                expect(
                    runtime
                        ->add_ticket({0, 0}, refrain::sim::ticket_kind::forced)
                        .has_value());
                expect(runtime->ticket_count({0, 0}) == 2U);
                expect(runtime->remove_ticket({0, 0}).has_value());
                expect(runtime->remove_ticket({0, 0}).has_value());
                expect(!runtime->remove_ticket({0, 0}).has_value());
                auto flushed{runtime->flush()};
                expect(flushed.has_value())
                    << (flushed ? "" : flushed.error().message);
                runtime_hash = runtime->state_hash();
            }
            {
                auto reopened{refrain::sim::world::open(directory.string())};
                expect(reopened.has_value());
                if (!reopened) {
                    return;
                }
                auto ensured{reopened->ensure_chunk({0, 0})};
                expect(ensured.has_value());
                if (!ensured) {
                    return;
                }
                const auto dirt{reopened->registry().find("minecraft:dirt")};
                const auto stone{reopened->registry().find("minecraft:stone")};
                expect(dirt.has_value());
                expect(stone.has_value());
                if (!dirt || !stone) {
                    return;
                }
                expect(reopened->block_at({0, 2, 3}) == *dirt);
                expect(reopened->block_at({0, 3, 3}) == *stone);
                expect(reopened->state_hash() == runtime_hash);
            }
        }
        std::error_code error;
        fs::remove_all(directory, error);
    };

    "sim/flush reuses clean section records and rewrites dirty ones"_test = [] {
        const auto directory{scratch_directory()};
        {
            auto store{world::world_store::create(directory.string())};
            expect(store.has_value());
            if (!store) {
                return;
            }
            auto chunk{make_chunk()};
            auto second{chunk.sections.front()};
            second.y = 1;
            second.palette.front().key = "minecraft:dirt";
            chunk.sections.push_back(std::move(second));
            expect(
                store->write_chunk(world::chunk_pos{0, 0}, chunk).has_value());
            expect(store->commit().has_value());
        }
        {
            auto runtime{refrain::sim::world::open(directory.string())};
            expect(runtime.has_value());
            if (!runtime) {
                return;
            }
            auto ensured{runtime->ensure_chunk({0, 0})};
            expect(ensured.has_value())
                << (ensured ? "" : ensured.error().message);
            expect(runtime->store().palette().find("minecraft:plains") !=
                   nullptr);
            auto before{runtime->store().read_chunk({0, 0})};
            expect(before.has_value())
                << (before ? "" : before.error().message);
            if (!before) {
                return;
            }
            const auto dirt{runtime->registry().intern("minecraft:dirt")};
            // Section 0 is touched; section 1 must keep its decoded record
            // (including the original palette order) instead of being rebuilt.
            expect(runtime->set_block({0, 2, 3}, dirt).has_value());
            auto flushed{runtime->flush()};
            expect(flushed.has_value())
                << (flushed ? "" : flushed.error().message);
            auto after{runtime->store().read_chunk({0, 0})};
            expect(after.has_value()) << (after ? "" : after.error().message);
            if (!after) {
                return;
            }
            const auto find_section = [](const auto &sections,
                                         std::int8_t y) -> const auto * {
                const auto found{std::find_if(
                    sections.begin(), sections.end(),
                    [y](const auto &section) { return section.y == y; })};
                return found == sections.end() ? nullptr : &*found;
            };
            const auto *clean{find_section(after->sections, 1)};
            expect(clean != nullptr);
            if (clean != nullptr) {
                expect(clean->palette.size() == 1U);
                if (clean->palette.size() == 1U) {
                    expect(clean->palette.front().key == "minecraft:dirt");
                    expect(clean->palette.front().count ==
                           world::section_volume);
                }
                expect(clean->biome_palette.size() == 1U);
                if (clean->biome_palette.size() == 1U) {
                    expect(clean->biome_palette.front() == "minecraft:plains");
                }
            }
            const auto *dirty{find_section(after->sections, 0)};
            expect(dirty != nullptr);
            if (dirty != nullptr) {
                expect(dirty->palette.size() == 2U);
            }
        }
        std::error_code error;
        fs::remove_all(directory, error);
    };

    "sim/pending snapshot detaches encoding from the tick thread"_test = [] {
        const auto directory{scratch_directory()};
        {
            auto store{make_store(directory)};
            expect(store.has_value()) << (store ? "" : store.error().message);
            if (!store) {
                return;
            }
            auto runtime{refrain::sim::world::open(directory.string())};
            expect(runtime.has_value());
            if (!runtime) {
                return;
            }
            expect(runtime->ensure_chunk({0, 0}).has_value());
            const auto dirt{runtime->registry().intern("minecraft:dirt")};
            const auto stone{runtime->registry().intern("minecraft:stone")};
            expect(runtime->set_block({0, 2, 3}, dirt).has_value());

            // The copy is taken on the sim thread; the worker may encode it
            // much later, after the tick thread has already edited the chunk.
            auto pending{runtime->snapshot_pending()};
            expect(pending.size() == 1U);
            if (pending.size() != 1U) {
                return;
            }
            expect(pending[0].revision == 1U);
            expect(runtime->set_block({0, 2, 3}, stone).has_value());

            auto written{runtime->write_pending(pending)};
            expect(written.has_value());
            if (!written) {
                return;
            }
            expect(written->size() == 1U);
            // The revision no longer matches, so the newer edit stays dirty.
            runtime->mark_clean(*written);
            auto still_dirty{runtime->snapshot_pending()};
            expect(still_dirty.size() == 1U);
            if (still_dirty.size() != 1U) {
                return;
            }
            expect(still_dirty[0].revision == 2U);
            auto second{runtime->write_pending(still_dirty)};
            expect(second.has_value());
            if (!second) {
                return;
            }
            runtime->mark_clean(*second);
            expect(runtime->snapshot_pending().empty());
        }
        {
            auto reopened{refrain::sim::world::open(directory.string())};
            expect(reopened.has_value());
            if (!reopened) {
                return;
            }
            expect(reopened->ensure_chunk({0, 0}).has_value());
            const auto stone{reopened->registry().find("minecraft:stone")};
            expect(stone.has_value());
            if (stone) {
                expect(reopened->block_at({0, 2, 3}) == *stone);
            }
        }
        std::error_code error;
        fs::remove_all(directory, error);
    };

    "sim/zstd policy is configurable and validated"_test = [] {
        const auto directory{scratch_directory()};
        {
            auto store{make_store(directory)};
            expect(store.has_value());
            if (!store) {
                return;
            }
            auto runtime{refrain::sim::world::open(directory.string())};
            expect(runtime.has_value());
            if (!runtime) {
                return;
            }
            expect(runtime
                       ->set_zstd_policy(
                           refrain::sim::zstd_policy{.near_level = 2,
                                                     .mid_level = 4,
                                                     .far_level = 7,
                                                     .near_radius = 1,
                                                     .mid_radius = 3})
                       .has_value());
            expect(runtime->compression_level_for({0, 0}) == 2);
            expect(runtime->compression_level_for({1, 0}) == 2);
            expect(runtime->compression_level_for({2, 0}) == 4);
            expect(runtime->compression_level_for({4, 0}) == 7);
            expect(!runtime
                        ->set_zstd_policy(
                            refrain::sim::zstd_policy{.near_level = 0,
                                                      .mid_level = 3,
                                                      .far_level = 6,
                                                      .near_radius = 2,
                                                      .mid_radius = 8})
                        .has_value());
            expect(!runtime
                        ->set_zstd_policy(
                            refrain::sim::zstd_policy{.near_level = 1,
                                                      .mid_level = 3,
                                                      .far_level = 6,
                                                      .near_radius = 4,
                                                      .mid_radius = 2})
                        .has_value());
            // A failed set leaves the previous policy intact.
            expect(runtime->compression_level_for({4, 0}) == 7);
        }
        std::error_code error;
        fs::remove_all(directory, error);
    };

    "sim/entity spawn, gravity tick, hash and removal"_test = [] {
        const auto directory{scratch_directory()};
        {
            auto store{make_store(directory)};
            expect(store.has_value());
            if (!store) {
                return;
            }
            auto runtime{refrain::sim::world::open(directory.string())};
            expect(runtime.has_value());
            if (!runtime) {
                return;
            }
            expect(runtime->ensure_chunk({0, 0}).has_value());
            const auto before{runtime->state_hash()};
            auto spawned{
                runtime->spawn_entity("minecraft:pig", 0.5, 20.5, 0.5)};
            expect(spawned.has_value());
            if (!spawned) {
                return;
            }
            expect(*spawned == 1U);
            expect(runtime->entities().size() == 1U);
            auto second{runtime->spawn_entity("minecraft:cow", 3.5, 30.0, 3.5)};
            expect(second.has_value());
            if (second) {
                expect(*second == 2U);
            }
            expect(runtime->spawn_entity("", 0.0, 0.0, 0.0).has_value() ==
                   false);
            expect(runtime->state_hash() != before);

            for (int tick{0}; tick < 200; ++tick) {
                runtime->tick();
            }
            const auto *pig{runtime->entity(*spawned)};
            expect(pig != nullptr);
            if (pig != nullptr) {
                expect(pig->y >= 15.999 && pig->y <= 16.001) << pig->y;
                expect(pig->on_ground);
                expect(pig->x == 0.5);
            }
            const auto grounded_hash{runtime->state_hash()};
            expect(runtime->remove_entity(*spawned));
            expect(!runtime->remove_entity(*spawned));
            expect(runtime->state_hash() != grounded_hash);
        }
        std::error_code error;
        fs::remove_all(directory, error);
    };

    "sim/inventory stacks obey the stack cap and remove across stacks"_test =
        [] {
            const auto directory{scratch_directory()};
            {
                auto store{make_store(directory)};
                expect(store.has_value());
                if (!store) {
                    return;
                }
                auto runtime{refrain::sim::world::open(directory.string())};
                expect(runtime.has_value());
                if (!runtime) {
                    return;
                }
                const auto player_id{make_console_player(*runtime)};
                auto *player{runtime->player(player_id)};
                expect(player != nullptr);
                if (player == nullptr) {
                    return;
                }
                expect(runtime->add_item(*player, "minecraft:dirt", 100)
                           .has_value());
                expect(runtime->item_count(*player, "minecraft:dirt") == 100U);
                expect(player->inventory.size() == 2U);
                if (player->inventory.size() == 2U) {
                    expect(player->inventory[0].count ==
                           refrain::sim::max_stack_size);
                    expect(player->inventory[1].count == 36U);
                }
                expect(runtime->add_item(*player, "minecraft:dirt", 30)
                           .has_value());
                expect(runtime->item_count(*player, "minecraft:dirt") == 130U);
                expect(player->inventory.size() == 3U);
                if (player->inventory.size() == 3U) {
                    expect(player->inventory[1].count ==
                           refrain::sim::max_stack_size);
                    expect(player->inventory[2].count == 2U);
                }
                expect(runtime->remove_item(*player, "minecraft:dirt", 70));
                expect(runtime->item_count(*player, "minecraft:dirt") == 60U);
                expect(player->inventory.size() == 2U);
                if (player->inventory.size() == 2U) {
                    expect(player->inventory[0].count == 58U);
                    expect(player->inventory[1].count == 2U);
                }
                expect(runtime->remove_item(*player, "minecraft:dirt", 60));
                expect(runtime->item_count(*player, "minecraft:dirt") == 0U);
                expect(player->inventory.empty());
                expect(!runtime->remove_item(*player, "minecraft:dirt", 61));
                expect(runtime->item_count(*player, "minecraft:dirt") == 0U);
            }
            std::error_code error;
            fs::remove_all(directory, error);
        };

    "sim/player gravity lands on the stone floor"_test = [] {
        const auto directory{scratch_directory()};
        {
            auto store{make_store(directory)};
            expect(store.has_value()) << (store ? "" : store.error().message);
            if (!store) {
                return;
            }
            auto runtime{refrain::sim::world::open(directory.string())};
            expect(runtime.has_value());
            if (!runtime) {
                return;
            }
            auto ensured{runtime->ensure_chunk({0, 0})};
            expect(ensured.has_value());
            const auto player_id{make_console_player(*runtime)};
            auto *player{runtime->player(player_id)};
            expect(player != nullptr);
            if (player == nullptr) {
                return;
            }
            runtime->set_player_position(*player, 0.5, 30.0, 0.5);
            for (int tick{0}; tick < 200; ++tick) {
                runtime->tick();
            }
            expect(player->y >= 15.99 && player->y <= 16.001) << player->y;
            expect(player->on_ground);
            expect(runtime->block_solid(
                *runtime->registry().find("minecraft:stone")));
            expect(!runtime->block_solid(refrain::sim::air_block));
        }
        std::error_code error;
        fs::remove_all(directory, error);
    };

    "sim/player collision stops at a wall and jump clears the ground"_test =
        [] {
            const auto directory{scratch_directory()};
            {
                auto store{make_store(directory)};
                expect(store.has_value());
                if (!store) {
                    return;
                }
                auto runtime{refrain::sim::world::open(directory.string())};
                expect(runtime.has_value());
                if (!runtime) {
                    return;
                }
                auto ensured{runtime->ensure_chunk({0, 0})};
                expect(ensured.has_value());
                if (!ensured) {
                    return;
                }
                const auto stone{*runtime->registry().find("minecraft:stone")};
                expect(runtime->set_block({1, 16, 0}, stone).has_value());
                const auto player_id{make_console_player(*runtime)};
                auto *player{runtime->player(player_id)};
                expect(player != nullptr);
                if (player == nullptr) {
                    return;
                }
                runtime->set_player_position(*player, 0.5, 16.0, 0.5);
                expect(runtime->move_player(*player, 1.0, 0.0, 0.0));
                expect(player->x < 0.71) << player->x;
                expect(player->x > 0.5) << player->x;

                runtime->tick(); // settle on the floor so jump() is legal
                expect(player->on_ground);
                runtime->jump(*player);
                expect(!player->on_ground);
                runtime->tick();
                expect(player->y > 16.0);
            }
            std::error_code error;
            fs::remove_all(directory, error);
        };

    "sim/inventory add, stack and remove"_test = [] {
        const auto directory{scratch_directory()};
        {
            auto store{make_store(directory)};
            expect(store.has_value());
            if (!store) {
                return;
            }
            auto runtime{refrain::sim::world::open(directory.string())};
            expect(runtime.has_value());
            if (!runtime) {
                return;
            }
            const auto player_id{make_console_player(*runtime, 0.0, 0.0, 0.0)};
            auto *player{runtime->player(player_id)};
            expect(player != nullptr);
            if (player == nullptr) {
                return;
            }
            expect(runtime->add_item(*player, "minecraft:dirt", 2).has_value());
            expect(runtime->item_count(*player, "minecraft:dirt") == 2U);
            expect(runtime->remove_item(*player, "minecraft:dirt", 1));
            expect(runtime->item_count(*player, "minecraft:dirt") == 1U);
            expect(!runtime->remove_item(*player, "minecraft:dirt", 2));
            expect(runtime->item_count(*player, "minecraft:stone") == 0U);
            expect(
                runtime->add_item(*player, "minecraft:stone", 3).has_value());
            expect(player->inventory.size() == 2U);
        }
        std::error_code error;
        fs::remove_all(directory, error);
    };

    "sim/runtime region compaction keeps the latest chunk"_test = [] {
        const auto directory{scratch_directory()};
        {
            auto store{make_store(directory)};
            expect(store.has_value());
            if (!store) {
                return;
            }
            auto chunk{*store->read_chunk({0, 0})};
            for (int revision{0}; revision < 8; ++revision) {
                chunk.last_update = revision;
                expect(store->write_chunk({0, 0}, chunk).has_value());
                expect(store->commit().has_value());
            }
            auto compacted{store->compact_regions(1, 0)};
            expect(compacted.has_value());
            if (compacted) {
                expect(*compacted == 1U);
            }
            auto reread{store->read_chunk({0, 0})};
            expect(reread.has_value());
            if (reread) {
                expect(reread->last_update == 7);
            }
        }
        std::error_code error;
        fs::remove_all(directory, error);
    };

    "sim/zstd level profile validates its range"_test = [] {
        const auto directory{scratch_directory()};
        {
            auto store{make_store(directory)};
            expect(store.has_value());
            if (!store) {
                return;
            }
            expect(store->set_zstd_level(1).has_value());
            expect(store->set_zstd_level(19).has_value());
            expect(!store->set_zstd_level(0).has_value());
            expect(!store->set_zstd_level(20).has_value());
        }
        std::error_code error;
        fs::remove_all(directory, error);
    };

    "sim/adaptive zstd levels follow player distance"_test = [] {
        const auto directory{scratch_directory()};
        const std::array<world::chunk_pos, 5> extra{
            world::chunk_pos{2, 0}, world::chunk_pos{3, 0},
            world::chunk_pos{8, 0}, world::chunk_pos{9, 0},
            world::chunk_pos{-16, 0}};
        const std::array<world::chunk_pos, 6> dirty_chunks{
            world::chunk_pos{0, 0}, world::chunk_pos{2, 0},
            world::chunk_pos{3, 0}, world::chunk_pos{8, 0},
            world::chunk_pos{9, 0}, world::chunk_pos{-16, 0}};
        {
            auto store{make_store(directory, extra)};
            expect(store.has_value()) << (store ? "" : store.error().message);
            if (!store) {
                std::error_code error;
                fs::remove_all(directory, error);
                return;
            }
        }
        {
            auto runtime{refrain::sim::world::open(directory.string())};
            expect(runtime.has_value());
            if (!runtime) {
                std::error_code error;
                fs::remove_all(directory, error);
                return;
            }
            const auto player_id{make_console_player(*runtime)};
            auto *player{runtime->player(player_id)};
            expect(player != nullptr);
            if (player == nullptr) {
                return;
            }
            runtime->set_player_position(*player, 0.5, 30.0, 0.5);
            // Chebyshev boundaries (player chunk is (0, 0)).
            expect(runtime->compression_level_for(world::chunk_pos{0, 0}) ==
                   refrain::sim::near_zstd_level);
            expect(runtime->compression_level_for(world::chunk_pos{2, 2}) ==
                   refrain::sim::near_zstd_level);
            expect(runtime->compression_level_for(world::chunk_pos{-2, 2}) ==
                   refrain::sim::near_zstd_level);
            expect(runtime->compression_level_for(world::chunk_pos{3, 0}) ==
                   refrain::sim::mid_zstd_level);
            expect(runtime->compression_level_for(world::chunk_pos{8, -8}) ==
                   refrain::sim::mid_zstd_level);
            expect(runtime->compression_level_for(world::chunk_pos{9, 0}) ==
                   refrain::sim::far_zstd_level);
            expect(runtime->compression_level_for(world::chunk_pos{-9, 0}) ==
                   refrain::sim::far_zstd_level);

            // Negative player coordinates must use floor, not truncation.
            runtime->set_player_position(*player, -0.5, 30.0, -0.5);
            expect(runtime->compression_level_for(world::chunk_pos{-1, -1}) ==
                   refrain::sim::near_zstd_level);
            expect(runtime->compression_level_for(world::chunk_pos{-3, -3}) ==
                   refrain::sim::near_zstd_level);
            expect(runtime->compression_level_for(world::chunk_pos{-4, 0}) ==
                   refrain::sim::mid_zstd_level);
            runtime->set_player_position(*player, 0.5, 30.0, 0.5);

            const auto dirt{runtime->registry().intern("minecraft:dirt")};
            for (const auto &pos : dirty_chunks) {
                auto ensured{runtime->ensure_chunk(pos)};
                expect(ensured.has_value()) << pos.x;
                if (!ensured) {
                    return;
                }
                expect(runtime
                           ->set_block({pos.x * world::section_edge, 0,
                                        pos.z * world::section_edge},
                                       dirt)
                           .has_value())
                    << pos.x;
            }
            const auto before_snapshot{runtime->state_hash()};
            auto snapshot{runtime->snapshot_dirty()};
            expect(snapshot.has_value());
            if (!snapshot) {
                return;
            }
            // Taking a snapshot is an encode-only step: no tick/state change.
            expect(runtime->state_hash() == before_snapshot);
            expect(runtime->ticks() == 0);
            expect(snapshot->size() == dirty_chunks.size());
            for (const auto &entry : *snapshot) {
                expect(entry.level == expected_level_at_origin(entry.pos))
                    << entry.pos.x;
            }
            // `write_snapshot()` forwards the per-entry level; an invalid one
            // is rejected by the store before any chunk is encoded.
            auto invalid{*snapshot};
            invalid.front().level = 0;
            expect(!runtime->write_snapshot(invalid).has_value());
            expect(runtime->write_snapshot(*snapshot).has_value());
            runtime->mark_clean(*snapshot);

            // The synchronous flush() shares the same policy: re-dirty one
            // near chunk and let flush() write it.
            const auto stone{*runtime->registry().find("minecraft:stone")};
            expect(runtime->set_block({0, 0, 0}, stone).has_value());
            auto flushed{runtime->flush()};
            expect(flushed.has_value())
                << (flushed ? "" : flushed.error().message);
            expect(runtime->ticks() == 0);
        }
        {
            // Fresh handle: both the snapshot batch and flush() are durable.
            auto verify{refrain::sim::world::open(directory.string())};
            expect(verify.has_value());
            if (verify) {
                for (const auto &pos : dirty_chunks) {
                    auto ensured{verify->ensure_chunk(pos)};
                    expect(ensured.has_value()) << pos.x;
                }
                const auto dirt{verify->registry().find("minecraft:dirt")};
                const auto stone{verify->registry().find("minecraft:stone")};
                expect(dirt.has_value());
                expect(stone.has_value());
                if (dirt && stone) {
                    for (const auto &pos : dirty_chunks) {
                        const auto state{
                            verify->block_at({pos.x * world::section_edge, 0,
                                              pos.z * world::section_edge})};
                        if (pos == world::chunk_pos{0, 0}) {
                            expect(state == *stone) << pos.x;
                        } else {
                            expect(state == *dirt) << pos.x;
                        }
                    }
                }
            }
        }
        std::error_code error;
        fs::remove_all(directory, error);
    };

    "sim/strict level validation names missing/wrong fields"_test = [] {
        const auto directory{scratch_directory()};
        {
            auto store{make_store(directory)};
            expect(store.has_value());
            if (!store) {
                return;
            }
            const auto make_meta = [](bool wrong_version) {
                nbt::compound level{};
                level.emplace(
                    "DataVersion",
                    nbt::value{nbt::value::storage{
                        wrong_version
                            ? nbt::value::storage{std::string{"x"}}
                            : nbt::value::storage{std::int32_t{5023}}}});
                nbt::compound spawn{};
                spawn.emplace("pos", nbt::value{nbt::value::storage{
                                         nbt::int_array{1, 2, 3}}});
                level.emplace(
                    "spawn", nbt::value{nbt::value::storage{std::move(spawn)}});
                level.emplace("Time", nbt::value{nbt::value::storage{
                                          std::int64_t{6000}}});
                level.emplace("LevelName", nbt::value{nbt::value::storage{
                                               std::string{"Strict"}}});
                nbt::compound root{};
                root.emplace("level",
                             nbt::value{nbt::value::storage{std::move(level)}});
                return nbt::write(nbt::make_compound(std::move(root)), "");
            };
            auto valid{make_meta(false)};
            expect(valid.has_value());
            if (valid) {
                expect(store->write_meta(*valid).has_value());
                auto strict{store->validate_level()};
                expect(strict.has_value());
            }
            auto invalid{make_meta(true)};
            expect(invalid.has_value());
            if (invalid) {
                expect(store->write_meta(*invalid).has_value());
                auto strict{store->validate_level()};
                expect(!strict.has_value());
                if (!strict) {
                    expect(strict.error().message.find("DataVersion") !=
                           std::string::npos);
                }
            }
        }
        std::error_code error;
        fs::remove_all(directory, error);
    };

    "sim/tick publishes a deterministic notification event"_test = [] {
        const auto directory{scratch_directory()};
        {
            auto store{make_store(directory)};
            expect(store.has_value());
            if (!store) {
                return;
            }
            auto runtime{refrain::sim::world::open(directory.string())};
            expect(runtime.has_value());
            if (!runtime) {
                return;
            }
            std::int64_t observed{0};
            const auto id{runtime->events().subscribe<refrain::sim::tick_event>(
                [&observed](const refrain::sim::tick_event &event) {
                    observed = event.tick;
                })};
            runtime->tick();
            expect(observed == 1);
            runtime->events().unsubscribe(id);
            runtime->tick();
            expect(observed == 1);
            expect(runtime->ticks() == 2);
        }
        std::error_code error;
        fs::remove_all(directory, error);
    };

    "sim/section dirty flags clear after a successful snapshot write"_test =
        [] {
            const auto directory{scratch_directory()};
            {
                auto store{make_store(directory)};
                expect(store.has_value());
                if (!store) {
                    return;
                }
                auto runtime{refrain::sim::world::open(directory.string())};
                expect(runtime.has_value());
                if (!runtime) {
                    return;
                }
                auto loaded{runtime->ensure_chunk({0, 0})};
                expect(loaded.has_value());
                if (!loaded) {
                    return;
                }
                const auto *column{*loaded};
                expect(column != nullptr);
                if (column == nullptr) {
                    return;
                }
                expect(runtime->set_block({0, 2, 3}, 0).has_value());
                bool dirty{false};
                for (const auto &section : column->sections) {
                    dirty = dirty || section.dirty;
                }
                expect(dirty);
                auto snapshot{runtime->snapshot_dirty()};
                expect(snapshot.has_value());
                if (snapshot) {
                    expect(runtime->write_snapshot(*snapshot).has_value());
                    runtime->mark_clean(*snapshot);
                    for (const auto &section : column->sections) {
                        expect(!section.dirty);
                    }
                }
            }
            std::error_code error;
            fs::remove_all(directory, error);
        };

    "sim/palette usage reports referenced table entries"_test = [] {
        const auto directory{scratch_directory()};
        {
            auto store{make_store(directory)};
            expect(store.has_value());
            if (!store) {
                return;
            }
            auto usage{store->palette_usage_stats()};
            expect(usage.has_value());
            if (usage) {
                expect(usage->chunks == 1U);
                expect(usage->table_entries >= usage->referenced_entries);
                expect(usage->referenced_entries > 0U);
            }
        }
        std::error_code error;
        fs::remove_all(directory, error);
    };

    "sim/palette rebuild preserves the runtime view and policy"_test = [] {
        const auto directory{scratch_directory()};
        {
            auto store{make_store(directory)};
            expect(store.has_value());
            if (!store) {
                return;
            }
            auto palette{store->palette()};
            static_cast<void>(
                palette.intern("minecraft:refrain_unused_palette_entry"));
            expect(store->install_palette(std::move(palette)).has_value());
            nbt::value level{nbt::make_compound()};
            expect(static_cast<bool>(
                level.set("DataVersion", nbt::make_int(5023))));
            nbt::value meta{nbt::make_compound()};
            expect(static_cast<bool>(meta.set("level", std::move(level))));
            auto bytes{nbt::write(meta, "", nbt::compression::none)};
            expect(static_cast<bool>(bytes));
            if (bytes) {
                expect(store->write_meta(*bytes).has_value());
            }
            expect(store->commit().has_value());
        }
        {
            auto runtime{refrain::sim::world::open(directory.string())};
            expect(runtime.has_value())
                << (runtime ? "" : runtime.error().message);
            if (!runtime) {
                return;
            }
            expect(runtime->ensure_chunk({0, 0}).has_value());
            expect(
                runtime->add_ticket({0, 0}, refrain::sim::ticket_kind::player)
                    .has_value());
            const auto player_id{make_console_player(*runtime)};
            auto *player{runtime->player(player_id)};
            expect(player != nullptr);
            if (player == nullptr) {
                return;
            }
            runtime->set_player_position(*player, 3.0, 64.0, 3.0);
            for (int index{0}; index < 3; ++index) {
                runtime->tick();
            }
            const auto hash_before{runtime->state_hash()};
            const auto loaded_before{runtime->loaded_chunks()};
            const auto palette_before{runtime->store().palette().size()};
            expect(runtime->store().set_zstd_level(7).has_value());
            refrain::sim::zstd_policy policy{};
            policy.near_level = 2;
            policy.mid_level = 4;
            policy.far_level = 9;
            policy.near_radius = 1;
            policy.mid_radius = 5;
            expect(runtime->set_zstd_policy(policy).has_value());

            auto report{runtime->rebuild_palette_in_place(1, 3)};
            expect(report.has_value())
                << (report ? "" : report.error().message);
            if (!report) {
                return;
            }
            expect(report->entries_after < report->entries_before);
            expect(report->entries_before == palette_before);
            expect(runtime->state_hash() == hash_before);
            expect(runtime->loaded_chunks() == loaded_before);
            expect(runtime->ticket_count({0, 0}) == 1U);
            expect(runtime->store().zstd_level() == 7);
            expect(runtime->zstd_policy_value().near_level == 2);
            expect(runtime->zstd_policy_value().mid_radius == 5);
            const auto stone{runtime->registry().find("minecraft:stone")};
            expect(stone.has_value());
            if (stone) {
                expect(runtime->block_at({0, 2, 3}) == *stone);
            }
            const auto dirt{runtime->registry().intern("minecraft:dirt")};
            expect(runtime->set_block({0, 2, 3}, dirt).has_value());
            expect(runtime->flush().has_value());
        }
        std::error_code error;
        fs::remove_all(directory, error);
    };

    "sim/palette rebuild closes the store when reopen fails"_test = [] {
        const auto directory{scratch_directory()};
        {
            auto store{make_store(directory)};
            expect(store.has_value());
            if (!store) {
                return;
            }
            nbt::value level{nbt::make_compound()};
            expect(static_cast<bool>(
                level.set("DataVersion", nbt::make_int(5023))));
            nbt::value meta{nbt::make_compound()};
            expect(static_cast<bool>(meta.set("level", std::move(level))));
            auto bytes{nbt::write(meta, "", nbt::compression::none)};
            expect(static_cast<bool>(bytes));
            if (bytes) {
                expect(store->write_meta(*bytes).has_value());
            }
            expect(store->commit().has_value());
        }
        auto runtime{refrain::sim::world::open(directory.string())};
        expect(runtime.has_value());
        if (!runtime) {
            return;
        }
        const auto marker{
            world::palette_maintenance_marker_path(directory.string())};
        expect(static_cast<bool>(
            refrain::core::io::write_text(marker, "{not json")));
        auto report{runtime->rebuild_palette_in_place()};
        expect(!report);
        if (!report) {
            expect(report.error().code == refrain::error_code::parse_error);
        }
        // Reopening the original world fails on the same corrupt marker, so the
        // store stays invalid and the caller must stop (documented contract).
        expect(!runtime->store().valid());
        std::error_code error;
        fs::remove(fs::path{marker}, error);
        fs::remove_all(directory, error);
    };

    "sim/set_blocks publishes one batch event for actual changes"_test = [] {
        const auto directory{scratch_directory()};
        {
            auto store{make_store(directory)};
            expect(store.has_value());
            if (!store) {
                return;
            }
            auto runtime{refrain::sim::world::open(directory.string())};
            expect(runtime.has_value());
            if (!runtime) {
                return;
            }
            auto loaded{runtime->ensure_chunk({0, 0})};
            expect(loaded.has_value());
            if (!loaded) {
                return;
            }
            std::size_t events{0};
            std::size_t event_changes{0};
            refrain::sim::block_change observed{};
            const auto id{
                runtime->events().subscribe<refrain::sim::block_batch_event>(
                    [&](const refrain::sim::block_batch_event &event) {
                        ++events;
                        event_changes = event.changes.size();
                        if (!event.changes.empty()) {
                            observed = event.changes.front();
                        }
                    })};
            const auto old{runtime->block_at({0, 2, 3})};
            const refrain::sim::block_change changes[]{
                {.pos = {0, 2, 3}, .old_state = old, .new_state = old},
                {.pos = {0, 2, 3}, .old_state = old, .new_state = 0}};
            auto applied{runtime->set_blocks(changes)};
            expect(applied.has_value());
            if (applied) {
                expect(*applied == 1U);
            }
            expect(events == 1U);
            expect(event_changes == 1U);
            expect(observed.new_state == 0U);
            runtime->events().unsubscribe(id);
        }
        std::error_code error;
        fs::remove_all(directory, error);
    };

    "sim/timers run by due tick and can be cancelled"_test = [] {
        const auto directory{scratch_directory()};
        {
            auto store{make_store(directory)};
            expect(store.has_value());
            if (!store) {
                return;
            }
            auto runtime{refrain::sim::world::open(directory.string())};
            expect(runtime.has_value());
            if (!runtime) {
                return;
            }
            int immediate{0};
            int delayed{0};
            int cancelled{0};
            runtime->schedule_after(0, [&immediate] { ++immediate; });
            runtime->schedule_after(2, [&delayed] { ++delayed; });
            const auto cancelled_id{
                runtime->schedule_after(1, [&cancelled] { ++cancelled; })};
            expect(runtime->cancel_timer(cancelled_id));

            runtime->tick();
            expect(immediate == 1);
            expect(delayed == 0);
            runtime->tick();
            expect(delayed == 1);
            expect(cancelled == 0);
            expect(runtime->ticks() == 2);
        }
        std::error_code error;
        fs::remove_all(directory, error);
    };
};

} // namespace
