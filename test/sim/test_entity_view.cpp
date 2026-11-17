// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Runtime entities live in an EnTT registry and are read through an ascending
// logical-id view. These tests pin what that view guarantees to the state hash,
// the save path and the clientbound entity stream: a spawn appends, a despawn
// leaves the relative order alone, a column load appends its records in
// canonical order, and a reload re-derives the same ids from the stored
// records.

#include <refrain/data/checksum.hpp>
#include <refrain/data/nbt.hpp>
#include <refrain/sim/world.hpp>
#include <refrain/world/create.hpp>
#include <refrain/world/model.hpp>
#include <refrain/world/store.hpp>

#include "support/scratch_directory.hpp"

#include <boost/ut.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace sim = refrain::sim;
namespace world = refrain::world;

constexpr world::chunk_pos entity_column{.x = 0, .z = 0};

[[nodiscard]] auto components(int color) -> std::vector<std::byte> {
    refrain::data::nbt::compound tag{};
    tag["Color"] = refrain::data::nbt::value{std::int32_t{color}};
    auto bytes{
        refrain::data::nbt::write(refrain::data::nbt::value{std::move(tag)}, "",
                                  refrain::data::nbt::compression::none)};
    expect(static_cast<bool>(bytes));
    return bytes ? std::move(*bytes) : std::vector<std::byte>{};
}

[[nodiscard]] auto make_entity(std::string id, double x, double z, int color)
    -> world::entity_record {
    world::entity_record record{};
    record.id = std::move(id);
    record.x = x;
    record.y = 65.0;
    record.z = z;
    record.yaw = 90.0F;
    record.pitch = 0.0F;
    record.pos = world::block_pos{
        .x = static_cast<std::int32_t>(x),
        .y = 65,
        .z = static_cast<std::int32_t>(z),
    };
    record.data = components(color);
    return record;
}

/// One valid column with a single bedrock section and the given entities.
[[nodiscard]] auto make_column(std::vector<world::entity_record> entities)
    -> world::chunk {
    world::chunk value{};
    value.format_version = world::chunk_format_version;
    value.status = world::chunk_status::full;
    value.y_pos = 0;
    world::section_record section{};
    section.y = 0;
    section.kind = world::section_kind::single_value;
    section.bits_per_entry = 0;
    section.palette.push_back(world::palette_entry{
        .key = "minecraft:stone", .count = 4096, .state = 0});
    section.biome_kind = world::section_kind::single_value;
    section.biome_bits_per_entry = 0;
    section.biome_palette.push_back("minecraft:plains");
    value.sections.push_back(std::move(section));
    world::canonicalize_entities(entities);
    value.entities = std::move(entities);
    return value;
}

void write_column(const std::filesystem::path &root, world::chunk_pos pos,
                  std::vector<world::entity_record> entities) {
    auto store{world::world_store::open(root.string())};
    expect(static_cast<bool>(store));
    if (!store) {
        return;
    }
    expect(static_cast<bool>(
        store->write_chunk(pos, make_column(std::move(entities)))));
    expect(static_cast<bool>(store->commit()));
}

/// Logical ids of the view in iteration order.
[[nodiscard]] auto view_ids(const sim::world &level)
    -> std::vector<sim::entity_id> {
    std::vector<sim::entity_id> ids{};
    level.entities().each(
        [&ids](const sim::entity_state &state) { ids.push_back(state.id); });
    return ids;
}

} // namespace

suite<"sim/entity-view"> entity_view_tests = [] {
    "spawns append and removals keep the remaining order in ascending id"_test =
        [] {
            const refrain::test_support::scratch_directory scratch;
            const auto root{scratch.path() / "world"};
            expect(static_cast<bool>(
                world::create_world(root.string(), 1, "entity-view")));
            auto level{sim::world::open(root.string())};
            expect(static_cast<bool>(level));
            if (!level) {
                return;
            }

            const auto pig{
                level->spawn_entity("minecraft:pig", 0.5, 20.5, 0.5)};
            const auto cow{
                level->spawn_entity("minecraft:cow", 3.5, 30.0, 3.5)};
            const auto sheep{
                level->spawn_entity("minecraft:sheep", 1.5, 10.0, 1.5)};
            expect(pig.has_value() && *pig == 1U);
            expect(cow.has_value() && *cow == 2U);
            expect(sheep.has_value() && *sheep == 3U);
            expect(level->entities().size() == 3U);
            expect(!level->entities().empty());
            expect(view_ids(*level) == std::vector<sim::entity_id>{1U, 2U, 3U});

            // The projection carries every component value of the spawned
            // entity.
            const auto second{level->entities().at(1)};
            expect(second.has_value());
            if (second) {
                expect(second->id == 2U);
                expect(second->type == "minecraft:cow");
                expect(second->x == 3.5);
                expect(second->y == 30.0);
                expect(second->z == 3.5);
                expect(second->vx == 0.0 && second->vy == 0.0 &&
                       second->vz == 0.0);
                expect(second->yaw == 0.0F && second->pitch == 0.0F);
                expect(!second->on_ground);
                expect(!second->pinned);
            }
            expect(!level->entities().at(3).has_value());
            const auto found{level->entities().find(3U)};
            expect(found.has_value());
            if (found) {
                expect(found->type == "minecraft:sheep");
            }
            expect(level->entity(2U).has_value());
            expect(!level->entity(9U).has_value());

            expect(level->remove_entity(2U));
            expect(!level->remove_entity(2U));
            expect(level->entities().size() == 2U);
            expect(view_ids(*level) == std::vector<sim::entity_id>{1U, 3U});
            expect(!level->entities().find(2U).has_value());

            // The next spawn takes a fresh id above every live and dead one, so
            // it appends instead of reusing the removed slot.
            const auto wolf{
                level->spawn_entity("minecraft:wolf", 2.5, 40.0, 2.5)};
            expect(wolf.has_value() && *wolf == 4U);
            expect(view_ids(*level) == std::vector<sim::entity_id>{1U, 3U, 4U});
            const auto last{level->entities().at(2)};
            expect(last.has_value());
            if (last) {
                expect(last->id == 4U);
                expect(last->type == "minecraft:wolf");
            }
        };

    "a column load appends its generated entities in record order"_test = [] {
        const refrain::test_support::scratch_directory scratch;
        const auto root{scratch.path() / "world"};
        expect(static_cast<bool>(
            world::create_world(root.string(), 1, "entity-column-load")));
        write_column(root, entity_column,
                     {make_entity("minecraft:shulker", 2.5, 3.5, 3),
                      make_entity("minecraft:item_frame", 0.5, 0.5, 0)});

        auto level{sim::world::open(root.string())};
        expect(static_cast<bool>(level));
        if (!level) {
            return;
        }
        expect(static_cast<bool>(level->ensure_chunk(entity_column)));
        expect(view_ids(*level) == std::vector<sim::entity_id>{1U, 2U});

        const auto column{level->chunk_entities(entity_column)};
        expect(column.size() == 2U);
        if (column.size() != 2U) {
            return;
        }
        expect(column[0].record.id == "minecraft:item_frame");
        expect(column[0].id == 1U);
        expect(column[1].record.id == "minecraft:shulker");
        expect(column[1].id == 2U);
        // The column record order and the logical id order agree, which is what
        // the clientbound announcement order follows.
        expect(column[0].id < column[1].id);

        const auto generated{level->entities().find(1U)};
        expect(generated.has_value());
        if (generated) {
            expect(generated->type == "minecraft:item_frame");
            expect(generated->x == 0.5);
            expect(generated->y == 65.0);
            expect(generated->yaw == 90.0F);
            expect(generated->pinned);
        }

        // A generated entity keeps the position the structure placed: the
        // point-entity tick is skipped for pinned entities.
        for (int tick{0}; tick < 5; ++tick) {
            level->tick();
        }
        const auto after_ticks{level->entities().find(2U)};
        expect(after_ticks.has_value());
        if (after_ticks) {
            expect(after_ticks->x == 2.5);
            expect(after_ticks->y == 65.0);
        }

        // Unloading the column removes its entities from the view; reloading
        // it spawns them again under fresh ids, never reusing the old ones.
        expect(static_cast<bool>(level->unload_chunk(entity_column)));
        expect(level->entities().empty());
        expect(static_cast<bool>(level->ensure_chunk(entity_column)));
        expect(view_ids(*level) == std::vector<sim::entity_id>{3U, 4U});
        const auto reloaded{level->chunk_entities(entity_column)};
        expect(reloaded.size() == 2U);
        if (reloaded.size() == 2U) {
            expect(reloaded[0].id == 3U);
            expect(reloaded[1].id == 4U);
        }
    };

    "columns loaded in position order append their entities in id order"_test =
        [] {
            const refrain::test_support::scratch_directory scratch;
            const auto root{scratch.path() / "world"};
            expect(static_cast<bool>(
                world::create_world(root.string(), 1, "entity-column-order")));
            constexpr world::chunk_pos second_column{.x = 1, .z = 0};
            write_column(root, entity_column,
                         {make_entity("minecraft:shulker", 2.5, 3.5, 3)});
            write_column(root, second_column,
                         {make_entity("minecraft:item_frame", 0.5, 0.5, 0)});

            auto level{sim::world::open(root.string())};
            expect(static_cast<bool>(level));
            if (!level) {
                return;
            }
            // The server applies generated columns in position order, so the
            // ids a load hands out follow that order and the view stays
            // ascending.
            expect(static_cast<bool>(level->ensure_chunk(entity_column)));
            expect(static_cast<bool>(level->ensure_chunk(second_column)));
            expect(view_ids(*level) == std::vector<sim::entity_id>{1U, 2U});
            const auto first{level->chunk_entities(entity_column)};
            const auto second{level->chunk_entities(second_column)};
            expect(first.size() == 1U && second.size() == 1U);
            if (first.size() == 1U && second.size() == 1U) {
                expect(first[0].id == 1U);
                expect(first[0].record.id == "minecraft:shulker");
                expect(second[0].id == 2U);
                expect(second[0].record.id == "minecraft:item_frame");
            }

            // Unloading one column removes only its entry; the relative order
            // of the survivors and the monotonic id sequence are unchanged.
            expect(static_cast<bool>(level->unload_chunk(entity_column)));
            expect(view_ids(*level) == std::vector<sim::entity_id>{2U});
            expect(static_cast<bool>(level->ensure_chunk(entity_column)));
            expect(view_ids(*level) == std::vector<sim::entity_id>{2U, 3U});
        };

    "a save and reload preserve the entity state hash"_test = [] {
        const refrain::test_support::scratch_directory scratch;
        const auto root{scratch.path() / "world"};
        expect(static_cast<bool>(
            world::create_world(root.string(), 1, "entity-hash-round-trip")));
        write_column(root, entity_column,
                     {make_entity("minecraft:shulker", 2.5, 3.5, 3),
                      make_entity("minecraft:item_frame", 0.5, 0.5, 0)});

        refrain::data::digest32 before{};
        {
            auto level{sim::world::open(root.string())};
            expect(static_cast<bool>(level));
            if (!level) {
                return;
            }
            expect(static_cast<bool>(level->ensure_chunk(entity_column)));
            before = level->state_hash();
            expect(static_cast<bool>(level->flush()));
        }

        auto reopened{sim::world::open(root.string())};
        expect(static_cast<bool>(reopened));
        if (!reopened) {
            return;
        }
        expect(static_cast<bool>(reopened->ensure_chunk(entity_column)));
        expect(reopened->state_hash() == before);
        expect(view_ids(*reopened) == std::vector<sim::entity_id>{1U, 2U});
    };

    "the state hash of the entity fixture is frozen"_test = [] {
        const refrain::test_support::scratch_directory scratch;
        const auto root{scratch.path() / "world"};
        expect(static_cast<bool>(
            world::create_world(root.string(), 1, "entity-hash-frozen")));
        write_column(root, entity_column,
                     {make_entity("minecraft:shulker", 2.5, 3.5, 3),
                      make_entity("minecraft:item_frame", 0.5, 0.5, 0)});

        auto level{sim::world::open(root.string())};
        expect(static_cast<bool>(level));
        if (!level) {
            return;
        }
        expect(static_cast<bool>(level->ensure_chunk(entity_column)));
        const auto pig{level->spawn_entity("minecraft:pig", 0.5, 20.5, 0.5)};
        const auto cow{level->spawn_entity("minecraft:cow", 3.5, 30.0, 3.5)};
        expect(pig.has_value() && *pig == 3U);
        expect(cow.has_value() && *cow == 4U);
        for (int tick{0}; tick < 5; ++tick) {
            level->tick();
        }
        // The hash covers the chunk, the tick counter, the RNG state and every
        // entity's projected component values in ascending logical-id order. A
        // change here means the simulation contract changed, not just the
        // storage layout.
        expect(
            refrain::data::to_hex(level->state_hash()) ==
            "e0a95a7987599e7e13a556840acddb15baf219d3254e7547ea1f9c510342d644");
    };

    "a large entity set ticks deterministically in ascending id order"_test =
        [] {
            const refrain::test_support::scratch_directory scratch;
            const auto hash_of = [&scratch](std::string_view name) {
                const auto root{scratch.path() / std::string{name}};
                expect(static_cast<bool>(
                    world::create_world(root.string(), 1, name)));
                auto level{sim::world::open(root.string())};
                expect(static_cast<bool>(level));
                if (!level) {
                    return refrain::data::digest32{};
                }
                for (sim::entity_id id{1}; id <= 1000U; ++id) {
                    const auto spawned{level->spawn_entity(
                        "minecraft:pig", static_cast<double>(id) * 0.125, 40.0,
                        static_cast<double>(id) * 0.25)};
                    expect(spawned.has_value() && *spawned == id);
                }
                for (int tick{0}; tick < 10; ++tick) {
                    level->tick();
                }
                // Removing every second entity must leave the remaining order
                // ascending and dependent only on the ids that survived.
                for (sim::entity_id id{2}; id <= 1000U; id += 2U) {
                    expect(level->remove_entity(id));
                }
                for (int tick{0}; tick < 5; ++tick) {
                    level->tick();
                }
                const auto ids{view_ids(*level)};
                expect(ids.size() == 500U);
                if (ids.size() == 500U) {
                    expect(ids.front() == 1U);
                    expect(ids.back() == 999U);
                    for (std::size_t index{1}; index < ids.size(); ++index) {
                        expect(ids[index - 1] < ids[index]);
                    }
                }
                return level->state_hash();
            };

            const auto first{hash_of("entity-scale-a")};
            const auto second{hash_of("entity-scale-b")};
            expect(first == second);
        };
};
