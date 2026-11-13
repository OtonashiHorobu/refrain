// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Column residency in the runtime world: a loaded column can be dropped after
// its edits reach the store, a ticket keeps it pinned, and a later read loads
// it back from disk. The tests also pin the properties the server policy
// relies on: unload order is the position order, and the state hash follows the
// resident set.

#include <refrain/sim/world.hpp>
#include <refrain/world/store.hpp>

#include <boost/ut.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "support/scratch_directory.hpp"

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace sim = refrain::sim;
namespace world = refrain::world;
namespace storage = refrain::storage;
namespace test_support = refrain::test_support;

/// A minimal valid column: one stone section at y=0 with a plains biome.
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

/// Create an RWF world holding one stone column at the origin and at every
/// position in `extra`.
[[nodiscard]] auto make_store(const std::filesystem::path &directory,
                              std::span<const world::chunk_pos> extra = {})
    -> refrain::result<world::world_store> {
    auto store{world::world_store::create(directory.string())};
    if (!store) {
        return std::unexpected{std::move(store.error())};
    }
    auto written{store->write_chunk(world::chunk_pos{0, 0}, make_chunk())};
    if (!written) {
        return std::unexpected{std::move(written.error())};
    }
    for (const auto &pos : extra) {
        auto extra_written{store->write_chunk(pos, make_chunk())};
        if (!extra_written) {
            return std::unexpected{std::move(extra_written.error())};
        }
    }
    if (auto committed{store->commit()}; !committed) {
        return std::unexpected{std::move(committed.error())};
    }
    return store;
}

[[nodiscard]] auto block(std::int32_t index, std::int32_t y) -> sim::block_pos {
    return sim::block_pos{index, static_cast<std::int16_t>(y), index};
}

suite<"sim/chunk-residency"> chunk_residency_tests = [] {
    "sim/world unload flushes a dirty column and reloads it from disk"_test =
        [] {
            test_support::scratch_directory scratch;
            const auto path{(scratch.path() / "world").string()};
            {
                auto store{make_store(scratch.path() / "world")};
                expect(static_cast<bool>(store))
                    << (store ? "" : store.error().message);
                if (!store) {
                    return;
                }
            }
            auto opened{sim::world::open(path)};
            expect(static_cast<bool>(opened));
            if (!opened) {
                return;
            }
            auto &runtime{*opened};
            expect(static_cast<bool>(runtime.ensure_chunk({0, 0})));

            const auto dirt{runtime.registry().intern("minecraft:dirt")};
            expect(static_cast<bool>(runtime.set_block(block(2, 2), dirt)));
            const auto dirty_hash{runtime.state_hash()};
            expect(runtime.loaded_chunks() == 1U);

            // Unloading writes the edit and drops the column.
            expect(static_cast<bool>(runtime.unload_chunk({0, 0})));
            expect(runtime.loaded_chunks() == 0U);
            expect(runtime.find_chunk({0, 0}) == nullptr);
            // The state hash covers the resident columns, so it moves with the
            // unload and comes back with the reload.
            expect(runtime.state_hash() != dirty_hash);

            auto reloaded{runtime.ensure_chunk({0, 0})};
            expect(static_cast<bool>(reloaded))
                << (reloaded ? "" : reloaded.error().message);
            expect(runtime.loaded_chunks() == 1U);
            expect(runtime.block_at(block(2, 2)) == dirt);
            expect(runtime.state_hash() == dirty_hash);
            expect(runtime.find_chunk({0, 0}) != nullptr);
        };

    "sim/world unload refuses a missing or ticketed column"_test = [] {
        test_support::scratch_directory scratch;
        const auto path{(scratch.path() / "world").string()};
        const std::array extras{world::chunk_pos{1, 0}};
        {
            auto store{make_store(scratch.path() / "world", extras)};
            expect(static_cast<bool>(store))
                << (store ? "" : store.error().message);
            if (!store) {
                return;
            }
        }
        auto opened{sim::world::open(path)};
        expect(static_cast<bool>(opened));
        if (!opened) {
            return;
        }
        auto &runtime{*opened};

        auto missing{runtime.unload_chunk({7, 7})};
        expect(!missing);
        if (!missing) {
            expect(missing.error().code == refrain::error_code::not_found);
        }

        expect(static_cast<bool>(runtime.ensure_chunk({0, 0})));
        expect(static_cast<bool>(
            runtime.add_ticket({0, 0}, sim::ticket_kind::forced)));
        auto pinned{runtime.unload_chunk({0, 0})};
        expect(!pinned);
        if (!pinned) {
            expect(pinned.error().code ==
                   refrain::error_code::invalid_argument);
        }
        expect(runtime.loaded_chunks() == 1U);

        expect(static_cast<bool>(runtime.remove_ticket({0, 0})));
        expect(static_cast<bool>(runtime.unload_chunk({0, 0})));
        expect(runtime.loaded_chunks() == 0U);
    };

    "sim/world a ticket pins a column that does not exist yet"_test = [] {
        test_support::scratch_directory scratch;
        const auto path{(scratch.path() / "world").string()};
        {
            auto store{make_store(scratch.path() / "world")};
            expect(static_cast<bool>(store))
                << (store ? "" : store.error().message);
            if (!store) {
                return;
            }
        }
        auto opened{sim::world::open(path)};
        expect(static_cast<bool>(opened));
        if (!opened) {
            return;
        }
        auto &runtime{*opened};
        const world::chunk_pos pinned{4, 4};
        expect(static_cast<bool>(
            runtime.hold_ticket(pinned, sim::ticket_kind::player)));
        expect(runtime.ticket_count(pinned) == 1U);

        // Holding the ticket does not require the store to hold the column.
        auto missing{runtime.ensure_chunk(pinned)};
        expect(!missing);
        expect(runtime.ticket_count(pinned) == 1U);

        // A generated column that arrives while the pin is held stays resident.
        auto inserted{runtime.insert_chunk(pinned, make_chunk())};
        expect(static_cast<bool>(inserted))
            << (inserted ? "" : inserted.error().message);
        expect(runtime.loaded_chunks() == 1U);
        expect(!runtime.unload_chunk(pinned));
        expect(static_cast<bool>(runtime.remove_ticket(pinned)));
        expect(static_cast<bool>(runtime.unload_chunk(pinned)));
        expect(runtime.loaded_chunks() == 0U);
    };

    "sim/world batch unload is all-or-nothing and persists every column"_test =
        [] {
            test_support::scratch_directory scratch;
            const auto path{(scratch.path() / "world").string()};
            const std::array extras{world::chunk_pos{1, 0},
                                    world::chunk_pos{2, 0}};
            {
                auto store{make_store(scratch.path() / "world", extras)};
                expect(static_cast<bool>(store))
                    << (store ? "" : store.error().message);
                if (!store) {
                    return;
                }
            }
            const std::array positions{world::chunk_pos{0, 0},
                                       world::chunk_pos{1, 0},
                                       world::chunk_pos{2, 0}};
            {
                auto opened{sim::world::open(path)};
                expect(static_cast<bool>(opened));
                if (!opened) {
                    return;
                }
                auto &runtime{*opened};
                const auto dirt{runtime.registry().intern("minecraft:dirt")};
                for (std::size_t index{0}; index < positions.size(); ++index) {
                    expect(static_cast<bool>(
                        runtime.ensure_chunk(positions[index])));
                    expect(static_cast<bool>(runtime.set_block(
                        block(static_cast<std::int32_t>(index), 4), dirt)));
                }

                // One pinned column rejects the whole batch: nothing is
                // written and nothing is dropped.
                expect(static_cast<bool>(
                    runtime.add_ticket({2, 0}, sim::ticket_kind::forced)));
                auto rejected{
                    runtime.unload_chunks(std::span<const world::chunk_pos>{
                        positions.data(), positions.size()})};
                expect(!rejected);
                if (!rejected) {
                    expect(rejected.error().code ==
                           refrain::error_code::invalid_argument);
                }
                expect(runtime.loaded_chunks() == positions.size());

                expect(static_cast<bool>(runtime.remove_ticket({2, 0})));
                expect(static_cast<bool>(
                    runtime.unload_chunks(std::span<const world::chunk_pos>{
                        positions.data(), positions.size()})));
                expect(runtime.loaded_chunks() == 0U);
                expect(static_cast<bool>(runtime.flush()));
            }

            // Reopen and read every edit back through a disk load.
            auto reopened{sim::world::open(path)};
            expect(static_cast<bool>(reopened));
            if (!reopened) {
                return;
            }
            for (std::size_t index{0}; index < positions.size(); ++index) {
                expect(static_cast<bool>(
                    reopened->ensure_chunk(positions[index])));
                expect(reopened->block_at(
                           block(static_cast<std::int32_t>(index), 4)) ==
                       reopened->registry().find("minecraft:dirt"));
            }
        };

    "sim/world a deferred unload is readable before the store commit"_test =
        [] {
            test_support::scratch_directory scratch;
            const auto path{(scratch.path() / "world").string()};
            {
                auto store{make_store(scratch.path() / "world")};
                expect(static_cast<bool>(store))
                    << (store ? "" : store.error().message);
                if (!store) {
                    return;
                }
            }
            {
                auto opened{sim::world::open(path)};
                expect(static_cast<bool>(opened));
                if (!opened) {
                    return;
                }
                auto &runtime{*opened};
                expect(static_cast<bool>(runtime.ensure_chunk({0, 0})));
                const auto dirt{runtime.registry().intern("minecraft:dirt")};
                expect(static_cast<bool>(runtime.set_block(block(3, 5), dirt)));
                // The runtime unload path writes the record without publishing
                // the store index; the same world still reads it back.
                expect(static_cast<bool>(runtime.unload_chunk({0, 0}, false)));
                expect(runtime.loaded_chunks() == 0U);
                auto reloaded{runtime.ensure_chunk({0, 0})};
                expect(static_cast<bool>(reloaded))
                    << (reloaded ? "" : reloaded.error().message);
                expect(runtime.block_at(block(3, 5)) == dirt);
                // The normal commit point publishes the whole store.
                expect(static_cast<bool>(runtime.store().commit()));
                expect(static_cast<bool>(runtime.unload_chunk({0, 0}, true)));
            }
            auto reopened{sim::world::open(path)};
            expect(static_cast<bool>(reopened));
            if (!reopened) {
                return;
            }
            expect(static_cast<bool>(reopened->ensure_chunk({0, 0})));
            expect(reopened->block_at(block(3, 5)) ==
                   reopened->registry().find("minecraft:dirt"));
        };

    "sim/world unticketed columns are listed with their dirty state"_test = [] {
        test_support::scratch_directory scratch;
        const auto path{(scratch.path() / "world").string()};
        const std::array extras{world::chunk_pos{1, 0},
                                world::chunk_pos{-2, 3}};
        {
            auto store{make_store(scratch.path() / "world", extras)};
            expect(static_cast<bool>(store))
                << (store ? "" : store.error().message);
            if (!store) {
                return;
            }
        }
        auto opened{sim::world::open(path)};
        expect(static_cast<bool>(opened));
        if (!opened) {
            return;
        }
        auto &runtime{*opened};
        for (const auto &pos : extras) {
            expect(static_cast<bool>(runtime.ensure_chunk(pos)));
        }
        expect(static_cast<bool>(runtime.ensure_chunk({0, 0})));
        const auto dirt{runtime.registry().intern("minecraft:dirt")};
        // The edit belongs to chunk (1, 0), not to the origin column.
        expect(static_cast<bool>(
            runtime.set_block(sim::block_pos{17, 1, 1}, dirt)));
        expect(static_cast<bool>(
            runtime.hold_ticket({-2, 3}, sim::ticket_kind::forced)));
        const auto candidates{runtime.unticketed_chunks()};
        expect(candidates.size() == 2U);
        for (std::size_t index{1}; index < candidates.size(); ++index) {
            const auto &previous{candidates[index - 1].pos};
            const auto &current{candidates[index].pos};
            const auto ordered{previous.x != current.x
                                   ? previous.x < current.x
                                   : previous.z < current.z};
            expect(ordered);
        }
        expect(candidates[0].pos == world::chunk_pos{0, 0});
        expect(!candidates[0].dirty);
        expect(candidates[1].pos == world::chunk_pos{1, 0});
        expect(candidates[1].dirty);
    };

    "sim/world resident columns are listed in position order"_test = [] {
        test_support::scratch_directory scratch;
        const auto path{(scratch.path() / "world").string()};
        const std::array extras{world::chunk_pos{1, 0},
                                world::chunk_pos{-2, 3}};
        {
            auto store{make_store(scratch.path() / "world", extras)};
            expect(static_cast<bool>(store))
                << (store ? "" : store.error().message);
            if (!store) {
                return;
            }
        }
        auto opened{sim::world::open(path)};
        expect(static_cast<bool>(opened));
        if (!opened) {
            return;
        }
        auto &runtime{*opened};
        expect(static_cast<bool>(runtime.ensure_chunk({1, 0})));
        expect(static_cast<bool>(runtime.ensure_chunk({0, 0})));
        expect(static_cast<bool>(runtime.ensure_chunk({-2, 3})));
        const auto positions{runtime.chunk_positions()};
        expect(positions.size() == 3U);
        for (std::size_t index{1}; index < positions.size(); ++index) {
            const auto &previous{positions[index - 1]};
            const auto &current{positions[index]};
            const auto ordered{previous.x != current.x
                                   ? previous.x < current.x
                                   : previous.z < current.z};
            expect(ordered);
        }

        // A sweep that drops every unpinned column leaves exactly the pinned
        // set, in the same order a replay would produce it.
        expect(static_cast<bool>(
            runtime.hold_ticket({1, 0}, sim::ticket_kind::forced)));
        std::vector<world::chunk_pos> victims{};
        for (const auto &pos : runtime.chunk_positions()) {
            if (runtime.ticket_count(pos) == 0) {
                victims.push_back(pos);
            }
        }
        expect(victims.size() == 2U);
        expect(static_cast<bool>(runtime.unload_chunks(victims)));
        expect(runtime.loaded_chunks() == 1U);
        expect(runtime.find_chunk({1, 0}) != nullptr);
        expect(runtime.find_chunk({0, 0}) == nullptr);
    };
};

} // namespace
