// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Generated entities belong to the column that carries them: adopting or
// loading a column spawns them exactly once, unloading it removes them, and a
// reload from the store brings back the same records with fresh runtime ids.

#include <refrain/data/nbt.hpp>
#include <refrain/sim/world.hpp>
#include <refrain/world/create.hpp>
#include <refrain/world/model.hpp>
#include <refrain/world/store.hpp>

#include "support/scratch_directory.hpp"

#include <boost/ut.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
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

[[nodiscard]] auto entity_columns(sim::world &level)
    -> std::vector<std::string> {
    std::vector<std::string> ids{};
    const auto entities{level.chunk_entities(entity_column)};
    ids.reserve(entities.size());
    for (const auto &entity : entities) {
        ids.push_back(entity.record.id);
    }
    return ids;
}

} // namespace

suite<"sim/chunk-entities"> chunk_entity_tests = [] {
    "entities are spawned once per column and survive a reload"_test = [] {
        const refrain::test_support::scratch_directory scratch;
        const auto root{scratch.path() / "world"};
        expect(static_cast<bool>(
            world::create_world(root.string(), 1, "entity-columns")));
        {
            auto created{world::world_store::open(root.string())};
            expect(static_cast<bool>(created));
            if (!created) {
                return;
            }
            expect(static_cast<bool>(created->write_chunk(
                entity_column,
                make_column({
                    make_entity("minecraft:shulker", 2.5, 3.5, 3),
                    make_entity("minecraft:item_frame", 0.5, 0.5, 0),
                }))));
            expect(static_cast<bool>(created->commit()));
        }

        {
            auto opened{sim::world::open(root.string())};
            expect(static_cast<bool>(opened))
                << (opened ? "" : opened.error().message);
            if (!opened) {
                return;
            }
            auto *column{opened->find_chunk(entity_column)};
            expect(column == nullptr);
            auto loaded{opened->ensure_chunk(entity_column)};
            expect(static_cast<bool>(loaded))
                << (loaded ? "" : loaded.error().message);
            if (!loaded) {
                return;
            }
            const auto entities{opened->chunk_entities(entity_column)};
            expect(entities.size() == 2U);
            if (entities.size() != 2U) {
                return;
            }
            // Canonical order is position order.
            expect(entities[0].record.id == "minecraft:item_frame");
            expect(entities[0].record.x == 0.5);
            expect(entities[0].record.yaw == 90.0F);
            expect(entities[1].record.id == "minecraft:shulker");
            expect(entities[1].record.data == components(3));
            expect(entities[0].id != entities[1].id);
            // The generated entities join the normal entity table.
            expect(opened->entities().size() == 2U);
            for (const auto &entity : entities) {
                expect(opened->entity(entity.id) != nullptr);
            }

            // A generated entity stays where the structure placed it instead
            // of falling: the sim has no entity AI or living-entity physics.
            for (int tick{0}; tick < 5; ++tick) {
                opened->tick();
            }
            const auto after_ticks{opened->chunk_entities(entity_column)};
            expect(after_ticks.size() == 2U);
            if (after_ticks.size() == 2U) {
                expect(after_ticks[0].record.x == 0.5);
                expect(after_ticks[0].record.y == 65.0);
                expect(after_ticks[1].record.x == 2.5);
            }

            // Unloading removes the entities with the column, and loading it
            // again spawns exactly one copy from the stored record.
            const auto old_ids{
                std::vector<sim::entity_id>{entities[0].id, entities[1].id}};
            expect(static_cast<bool>(opened->unload_chunk(entity_column)));
            expect(opened->entities().empty());
            for (const auto id : old_ids) {
                expect(opened->entity(id) == nullptr);
            }
            auto reloaded{opened->ensure_chunk(entity_column)};
            expect(static_cast<bool>(reloaded));
            if (!reloaded) {
                return;
            }
            const auto reloaded_entities{opened->chunk_entities(entity_column)};
            expect(reloaded_entities.size() == 2U);
            expect(opened->entities().size() == 2U);
            if (reloaded_entities.size() == 2U) {
                expect(reloaded_entities[0].record.id ==
                       "minecraft:item_frame");
                expect(reloaded_entities[1].record.id == "minecraft:shulker");
            }
            expect(static_cast<bool>(opened->flush()));
        }

        // Closing and reopening the world reads the entities back from the
        // store, not from the entity table of the previous process state.
        auto reopened{sim::world::open(root.string())};
        expect(static_cast<bool>(reopened))
            << (reopened ? "" : reopened.error().message);
        if (!reopened) {
            return;
        }
        expect(reopened->entities().empty());
        auto loaded{reopened->ensure_chunk(entity_column)};
        expect(static_cast<bool>(loaded));
        if (!loaded) {
            return;
        }
        expect(entity_columns(*reopened) ==
               std::vector<std::string>{"minecraft:item_frame",
                                        "minecraft:shulker"});
        expect(reopened->entities().size() == 2U);
    };

    "adopting a generated column spawns its entities exactly once"_test = [] {
        const refrain::test_support::scratch_directory scratch;
        const auto root{scratch.path() / "world"};
        expect(static_cast<bool>(
            world::create_world(root.string(), 1, "adopted-entities")));
        auto opened{sim::world::open(root.string())};
        expect(static_cast<bool>(opened));
        if (!opened) {
            return;
        }
        auto inserted{opened->insert_chunk(
            entity_column,
            make_column({make_entity("minecraft:pig", 1.5, 2.5, 0)}))};
        expect(static_cast<bool>(inserted))
            << (inserted ? "" : inserted.error().message);
        if (!inserted) {
            return;
        }
        const auto entities{opened->chunk_entities(entity_column)};
        expect(entities.size() == 1U);
        expect(opened->entities().size() == 1U);
        if (entities.size() == 1U) {
            expect(entities[0].record.id == "minecraft:pig");
        }
        // Adopting the same column again is refused, so the duplicate cannot
        // spawn a second copy.
        auto duplicate{opened->insert_chunk(entity_column, make_column({}))};
        expect(!duplicate);
        if (!duplicate) {
            expect(duplicate.error().code ==
                   refrain::error_code::already_exists);
        }
        expect(opened->entities().size() == 1U);
        // Removing the generated entity from the world also takes it out of
        // the column record, so it is not spawned again after a reload.
        const auto pig_id{entities[0].id};
        expect(opened->remove_entity(pig_id));
        expect(opened->chunk_entities(entity_column).empty());
        expect(opened->entities().empty());
        expect(static_cast<bool>(opened->unload_chunk(entity_column)));
        auto reloaded{opened->ensure_chunk(entity_column)};
        expect(static_cast<bool>(reloaded))
            << (reloaded ? "" : reloaded.error().message);
        if (!reloaded) {
            return;
        }
        expect(opened->chunk_entities(entity_column).empty());
        expect(opened->entities().empty());
        expect(static_cast<bool>(opened->unload_chunk(entity_column)));
    };

    "a column without entities leaves the entity table alone"_test = [] {
        const refrain::test_support::scratch_directory scratch;
        const auto root{scratch.path() / "world"};
        expect(static_cast<bool>(
            world::create_world(root.string(), 1, "empty-columns")));
        auto opened{sim::world::open(root.string())};
        expect(static_cast<bool>(opened));
        if (!opened) {
            return;
        }
        auto inserted{opened->insert_chunk(entity_column, make_column({}))};
        expect(static_cast<bool>(inserted));
        if (!inserted) {
            return;
        }
        expect(opened->chunk_entities(entity_column).empty());
        expect(opened->entities().empty());
    };
};
