// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// A world is a movable value: the server and the test harness open it and
// then hand it to the object that owns it. Every configuration and piece of
// runtime state has to survive that move, so a world that was configured
// before the move still behaves as configured afterwards.

#include <refrain/sim/world.hpp>

#include <refrain/world/store.hpp>

#include "support/scratch_directory.hpp"

#include <boost/ut.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace {
using namespace boost::ut;

namespace fs = std::filesystem;
namespace storage = refrain::storage;
namespace world = refrain::world;
namespace sim = refrain::sim;

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

[[nodiscard]] auto make_world(const fs::path &directory)
    -> std::optional<sim::world> {
    std::error_code error{};
    fs::create_directories(directory, error);
    auto store{world::world_store::create(directory.string())};
    if (!store) {
        return std::nullopt;
    }
    if (!store->write_chunk(world::chunk_pos{0, 0}, make_chunk())) {
        return std::nullopt;
    }
    if (!store->commit()) {
        return std::nullopt;
    }
    auto opened{sim::world::open(directory.string())};
    if (!opened) {
        return std::nullopt;
    }
    return std::optional<sim::world>{std::move(*opened)};
}

/// Configure every field a move has to carry over.
void configure(sim::world &level) {
    sim::item_table items{};
    items.insert("minecraft:test_block",
                 sim::item_definition{.max_stack_size = 16, .max_damage = 7});
    level.set_item_table(std::move(items));
    level.set_spawn_point(sim::world::spawn_point{
        .x = 12.5, .y = 70.0, .z = -3.5, .yaw = 45.0F, .pitch = 10.0F});
    level.set_difficulty(3);
    level.set_hardcore(true);
    expect(level
               .set_zstd_policy(sim::zstd_policy{.near_level = 2,
                                                 .mid_level = 4,
                                                 .far_level = 9,
                                                 .near_radius = 1,
                                                 .mid_radius = 5})
               .has_value());
}

void expect_configuration(const sim::world &level) {
    expect(level.items().stack_limit("minecraft:test_block") == 16U);
    expect(level.items().max_damage("minecraft:test_block") == 7U);
    const auto &spawn{level.spawn_point_value()};
    expect(spawn.x == 12.5);
    expect(spawn.y == 70.0);
    expect(spawn.z == -3.5);
    expect(spawn.yaw == 45.0F);
    expect(spawn.pitch == 10.0F);
    expect(level.difficulty() == 3U);
    expect(level.hardcore());
    const auto &policy{level.zstd_policy_value()};
    expect(policy.near_level == 2);
    expect(policy.mid_level == 4);
    expect(policy.far_level == 9);
    expect(policy.near_radius == 1);
    expect(policy.mid_radius == 5);
}

} // namespace

suite<"sim/world-move"> world_move_tests = [] {
    "a move-constructed world keeps its configuration and subscriptions"_test =
        [] {
            const refrain::test_support::scratch_directory scratch;
            auto source{make_world(scratch.path() / "source")};
            expect(source.has_value());
            if (!source) {
                return;
            }
            configure(*source);
            int tick_events{0};
            const auto subscription{source->events().subscribe<sim::tick_event>(
                [&tick_events](const sim::tick_event &) { ++tick_events; })};
            bool timer_fired{false};
            const auto timer{source->schedule_after(
                1, [&timer_fired] { timer_fired = true; })};
            expect(timer == 1U);

            sim::world moved{std::move(*source)};
            expect_configuration(moved);
            expect(moved.events().listener_count(
                       refrain::core::events::type_of<sim::tick_event>()) ==
                   1U);
            expect(subscription != 0U);
            moved.tick();
            expect(tick_events == 1);
            expect(timer_fired);
            // The next timer id continues from the source; the move did not
            // restart the sequence and collide with an existing id.
            const auto next{timer + 1U};
            expect(moved.schedule_after(1, [] {}) == next);
        };

    "a move-assigned world keeps the source configuration and subscriptions"_test =
        [] {
            const refrain::test_support::scratch_directory scratch;
            auto source{make_world(scratch.path() / "source")};
            auto target{make_world(scratch.path() / "target")};
            expect(source.has_value());
            expect(target.has_value());
            if (!source || !target) {
                return;
            }
            configure(*source);
            int tick_events{0};
            static_cast<void>(source->events().subscribe<sim::tick_event>(
                [&tick_events](const sim::tick_event &) { ++tick_events; }));
            expect(source->schedule_after(1, [] {}) == 1U);

            *target = std::move(*source);
            expect_configuration(*target);
            expect(target->events().listener_count(
                       refrain::core::events::type_of<sim::tick_event>()) ==
                   1U);
            target->tick();
            expect(tick_events == 1);
            expect(target->schedule_after(1, [] {}) == 2U);
        };

    "a moved world keeps its players and entities and their id sequence"_test =
        [] {
            const refrain::test_support::scratch_directory scratch;
            auto source{make_world(scratch.path() / "source")};
            expect(source.has_value());
            if (!source) {
                return;
            }
            const auto alice{source->add_player(sim::player_table::registration{
                .connection = 1,
                .name = "Alice",
                .world = "overworld",
                .x = 1.0,
                .y = 65.0,
                .z = 2.0,
            })};
            const auto pig{
                source->spawn_entity("minecraft:pig", 4.5, 65.0, 4.5)};
            expect(alice.has_value() && *alice == 1U);
            expect(pig.has_value() && *pig == 2U);
            auto *record{source->player(1)};
            expect(record != nullptr);
            if (record != nullptr) {
                record->health = 11.0F;
            }

            sim::world moved{std::move(*source)};
            // The player table lives in the entity store the world owns, so a
            // move must carry both the table and the entity components.
            expect(moved.player(1) != nullptr);
            expect(moved.player(1)->name == "Alice");
            expect(moved.player(1)->health == 11.0F);
            expect(moved.player(2) == nullptr);
            expect(moved.entity(2).has_value());
            expect(moved.entities().size() == 1U);
            // The shared sequence continues from the source, so the moved-to
            // world never reissues a live id.
            const auto cow{moved.spawn_entity("minecraft:cow", 8.5, 65.0, 8.5)};
            expect(cow.has_value() && *cow == 3U);
            const auto bob{moved.add_player(sim::player_table::registration{
                .connection = 2,
                .name = "Bob",
                .world = "overworld",
                .x = 9.0,
                .y = 65.0,
                .z = 9.0,
            })};
            expect(bob.has_value() && *bob == 4U);
        };
};
