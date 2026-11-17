// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Server-authoritative block breaking at the service seam: the reference's
// destroy-progress formula and stage sequence, the completion and abort rules,
// tool wear, the creative and game-mode branches and the rejections. The world
// is a real simulation world so a break is observed as the block-state change
// the runtime would publish, not as a mock call.

#include <refrain/data/mining.hpp>
#include <refrain/server/mining_service.hpp>
#include <refrain/server/world_port.hpp>
#include <refrain/sim/item.hpp>
#include <refrain/sim/player.hpp>
#include <refrain/sim/world.hpp>
#include <refrain/world/model.hpp>
#include <refrain/world/store.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "support/scratch_directory.hpp"

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace data = refrain::data;
namespace server = refrain::server;
namespace sim = refrain::sim;
namespace storage = refrain::storage;
namespace world_ns = refrain::world;
namespace test_support = refrain::test_support;

/// One destroy-stage packet the service produced for an observer.
struct stage_packet {
    std::uint64_t connection{0};
    std::int32_t entity_id{0};
    sim::block_pos pos{};
    std::int32_t stage{0};
};

/// One authoritative block state the service sent back to a client.
struct state_packet {
    std::uint64_t connection{0};
    sim::block_pos pos{};
    sim::block_state_id state{sim::air_block};
};

[[nodiscard]] auto same_pos(const sim::block_pos &left,
                            const sim::block_pos &right) -> bool {
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

[[nodiscard]] auto nearly(float actual, float expected) -> bool {
    return std::abs(actual - expected) <= 1e-6F * std::max(1.0F, expected);
}

/// The reported mining record of an ordinary block. Tests that need a special
/// branch adjust the fields after the call.
[[nodiscard]] auto block_record(float hardness, std::uint8_t tool_mask = 0,
                                std::uint8_t needed_tier = 0,
                                bool requires_correct_tool = true)
    -> data::block_mining {
    data::block_mining record{};
    record.hardness = hardness;
    record.tool_mask = tool_mask;
    record.needed_tier = needed_tier;
    if (requires_correct_tool) {
        record.flags = data::block_mining_requires_correct_tool;
    }
    return record;
}

[[nodiscard]] auto pickaxe_record(std::uint8_t harvest_level, float speed,
                                  bool can_destroy_in_creative = true)
    -> data::tool_mining {
    data::tool_mining tool{};
    tool.name = "minecraft:iron_pickaxe";
    tool.type = data::tool_type::pickaxe;
    tool.material = data::tool_material::iron;
    tool.harvest_level = harvest_level;
    tool.tier_speed = speed;
    tool.default_mining_speed = 1.0F;
    tool.damage_per_block = 1;
    tool.max_stack_size = 1;
    tool.max_damage = 250;
    tool.can_destroy_blocks_in_creative = can_destroy_in_creative;
    return tool;
}

/// A world whose chunk (0, 0) is solid stone in section 0, plus the blocks the
/// break tests need at y=15 and the two players they drive.
struct fixture {
    test_support::scratch_directory directory{};
    std::optional<sim::world> level{};
    std::unique_ptr<server::world_port> port{};
    data::block_mining_table blocks{};
    data::item_tool_table tools{};
    sim::item_table items{};
    std::vector<stage_packet> stages{};
    std::vector<state_packet> states{};
    std::unique_ptr<server::mining_service> service{};
    sim::block_state_id stone{sim::air_block};
    sim::block_state_id dirt{sim::air_block};
    sim::block_state_id obsidian{sim::air_block};
    sim::block_state_id bedrock{sim::air_block};
    sim::block_state_id torch{sim::air_block};

    fixture() = default;
    fixture(const fixture &) = delete;
    auto operator=(const fixture &) -> fixture & = delete;
    ~fixture() = default;

    [[nodiscard]] auto open() -> bool {
        auto store{world_ns::world_store::create(directory.string())};
        if (!store) {
            return false;
        }
        storage::chunk chunk{};
        chunk.format_version = world_ns::chunk_format_version;
        chunk.status = storage::chunk_status::full;
        chunk.y_pos = 0;
        storage::section_record section{};
        section.y = 0;
        section.kind = storage::section_kind::single_value;
        section.bits_per_entry = 0;
        section.block_count =
            static_cast<std::uint16_t>(world_ns::section_volume);
        section.biome_kind = storage::section_kind::single_value;
        section.biome_bits_per_entry = 0;
        section.biome_palette.push_back("minecraft:plains");
        section.palette.push_back(storage::palette_entry{
            .key = "minecraft:stone",
            .count = static_cast<std::uint16_t>(world_ns::section_volume),
            .state = 0,
        });
        chunk.sections.push_back(std::move(section));
        if (!store->write_chunk({0, 0}, chunk) || !store->commit()) {
            return false;
        }
        auto opened{sim::world::open(directory.string())};
        if (!opened) {
            return false;
        }
        level = std::move(*opened);
        if (!level->ensure_chunk({0, 0})) {
            return false;
        }

        blocks.insert(
            "minecraft:stone",
            block_record(1.5F, data::tool_type_bit(data::tool_type::pickaxe),
                         1));
        blocks.insert("minecraft:dirt",
                      block_record(0.5F,
                                   data::tool_type_bit(data::tool_type::shovel),
                                   0, false));
        blocks.insert(
            "minecraft:obsidian",
            block_record(50.0F, data::tool_type_bit(data::tool_type::pickaxe),
                         3));
        blocks.insert("minecraft:bedrock", block_record(-1.0F));
        blocks.insert("minecraft:torch", block_record(0.0F));
        tools.insert("minecraft:iron_pickaxe", pickaxe_record(2, 6.0F));
        tools.insert("minecraft:diamond_pickaxe", [] {
            auto tool{pickaxe_record(3, 8.0F)};
            tool.name = "minecraft:diamond_pickaxe";
            tool.material = data::tool_material::diamond;
            return tool;
        }());
        tools.insert("minecraft:wooden_sword", [] {
            data::tool_mining tool{};
            tool.name = "minecraft:wooden_sword";
            tool.type = data::tool_type::sword;
            tool.material = data::tool_material::wood;
            tool.default_mining_speed = 1.0F;
            tool.damage_per_block = 2;
            tool.max_stack_size = 1;
            tool.max_damage = 59;
            tool.can_destroy_blocks_in_creative = false;
            return tool;
        }());
        items.insert(
            "minecraft:iron_pickaxe",
            sim::item_definition{.max_stack_size = 1, .max_damage = 250});
        items.insert(
            "minecraft:diamond_pickaxe",
            sim::item_definition{.max_stack_size = 1, .max_damage = 1561});
        items.insert(
            "minecraft:wooden_sword",
            sim::item_definition{.max_stack_size = 1, .max_damage = 59});
        level->set_item_table(items);

        const auto set_block = [this](std::int32_t x, std::int32_t y,
                                      std::int32_t z, std::string_view key) {
            return level
                ->set_block(sim::block_pos{x, static_cast<std::int16_t>(y), z},
                            level->registry().intern(std::string{key}))
                .has_value();
        };
        if (!set_block(1, 15, 0, "minecraft:dirt") ||
            !set_block(2, 15, 0, "minecraft:obsidian") ||
            !set_block(3, 15, 0, "minecraft:bedrock") ||
            !set_block(4, 15, 0, "minecraft:torch") ||
            !set_block(5, 15, 0, "minecraft:air")) {
            return false;
        }
        stone = level->registry().intern("minecraft:stone");
        dirt = level->registry().intern("minecraft:dirt");
        obsidian = level->registry().intern("minecraft:obsidian");
        bedrock = level->registry().intern("minecraft:bedrock");
        torch = level->registry().intern("minecraft:torch");

        port = server::make_borrowed_world_port(*level);
        if (!port) {
            return false;
        }
        const auto add = [this](std::uint64_t connection, std::string name,
                                double z) {
            return level
                ->add_player(sim::player_table::registration{
                    .connection = connection,
                    .name = std::move(name),
                    .world = "overworld",
                    .x = 0.5,
                    .y = 16.0,
                    .z = z,
                    .game_mode = sim::game_mode_survival,
                    .inventory_slots = 36,
                })
                .has_value();
        };
        if (!add(1, "Miner", 0.5) || !add(2, "Observer", 6.5)) {
            return false;
        }
        auto *actor{level->players().find_by_connection(1)};
        if (actor == nullptr) {
            return false;
        }
        actor->inventory[0] =
            sim::item_stack{.item = "minecraft:iron_pickaxe", .count = 1};
        service = std::make_unique<server::mining_service>(
            blocks, tools, *port, level->players(),
            server::mining_service::outbound{
                .destroy_stage =
                    [this](std::uint64_t connection, std::int32_t entity_id,
                           const sim::block_pos &pos, std::int32_t stage) {
                        stages.push_back(stage_packet{
                            .connection = connection,
                            .entity_id = entity_id,
                            .pos = pos,
                            .stage = stage,
                        });
                    },
                .block_state =
                    [this](std::uint64_t connection, const sim::block_pos &pos,
                           sim::block_state_id state) {
                        states.push_back(state_packet{
                            .connection = connection,
                            .pos = pos,
                            .state = state,
                        });
                    },
            });
        return true;
    }

    [[nodiscard]] auto actor() -> sim::player_state * {
        return level->players().find_by_connection(1);
    }
    [[nodiscard]] auto observer() -> sim::player_state * {
        return level->players().find_by_connection(2);
    }
};

[[nodiscard]] auto make_fixture() -> std::unique_ptr<fixture> {
    auto created{std::make_unique<fixture>()};
    if (!created->open()) {
        return nullptr;
    }
    return created;
}

/// The last stage the observer received, or -1 when it received none.
[[nodiscard]] auto last_stage(const fixture &state) -> std::int32_t {
    return state.stages.empty() ? -1 : state.stages.back().stage;
}

} // namespace

suite<"mining/rules"> mining_rules_tests = [] {
    "mining/the destroy progress follows the reference formula"_test = [] {
        const auto stone{block_record(
            1.5F, data::tool_type_bit(data::tool_type::pickaxe), 1)};
        const auto dirt{block_record(
            0.5F, data::tool_type_bit(data::tool_type::shovel), 0, false)};
        const auto obsidian{block_record(
            50.0F, data::tool_type_bit(data::tool_type::pickaxe), 3)};
        const auto bedrock{block_record(-1.0F)};
        const auto endless{block_record(0.0F)};
        const auto iron{pickaxe_record(2, 6.0F)};
        auto wooden{iron};
        wooden.harvest_level = 0;
        wooden.tier_speed = 2.0F;
        auto diamond{iron};
        diamond.harvest_level = 3;
        diamond.tier_speed = 8.0F;

        // A correct tool divides by 30, a wrong one by 100.
        expect(nearly(server::mining::per_tick_progress(stone, &iron),
                      6.0F / 1.5F / 30.0F));
        expect(nearly(server::mining::per_tick_progress(stone, nullptr),
                      1.0F / 1.5F / 100.0F));
        // Dirt does not require a correct tool, so bare hands keep the 30
        // divisor.
        expect(nearly(server::mining::per_tick_progress(dirt, nullptr),
                      1.0F / 0.5F / 30.0F));
        // Obsidian needs a diamond-tier tool: an iron pickaxe mines it without
        // the bonus, a diamond pickaxe with it.
        expect(nearly(server::mining::per_tick_progress(obsidian, &iron),
                      6.0F / 50.0F / 100.0F));
        expect(nearly(server::mining::per_tick_progress(obsidian, &diamond),
                      8.0F / 50.0F / 30.0F));
        // A zero harvest level is below stone's tier.
        expect(nearly(server::mining::per_tick_progress(stone, &wooden),
                      2.0F / 1.5F / 100.0F));
        // Unbreakable never progresses; a zero destroy time is infinite.
        expect(server::mining::per_tick_progress(bedrock, &iron) == 0.0F);
        expect(std::isinf(server::mining::per_tick_progress(endless, nullptr)));

        // The reference counts the tick a break starts on.
        expect(nearly(server::mining::accumulated_progress(0.1F, 0), 0.1F));
        expect(nearly(server::mining::accumulated_progress(0.1F, 7), 0.8F));
        // The stage value truncates to a tenth; ten clears the animation.
        expect(server::mining::stage_of(0.4F) == 4);
        expect(server::mining::stage_of(0.93F) == 9);
        expect(server::mining::stage_of(1.07F) == 10);

        expect(server::mining::correct_tool(stone, nullptr) == false);
        expect(server::mining::correct_tool(stone, &iron));
        expect(server::mining::correct_tool(stone, &wooden) == false);
        expect(server::mining::correct_tool(dirt, nullptr));
    };

    "mining/the interaction range is the eye-to-block distance"_test = [] {
        // Standing eyes are 1.62 above the feet; a block under the player is
        // inside the reference's 4.5 + 1.0 range and one ten blocks away is
        // not.
        const auto eye_y{16.0 + server::mining::eye_height};
        expect(server::mining::within_reach(0.5, eye_y, 0.5,
                                            sim::block_pos{0, 15, 0},
                                            server::mining::interaction_range));
        expect(server::mining::within_reach(0.5, eye_y, 0.5,
                                            sim::block_pos{5, 16, 0},
                                            server::mining::interaction_range));
        expect(!server::mining::within_reach(
            0.5, eye_y, 0.5, sim::block_pos{10, 15, 0},
            server::mining::interaction_range));
        // Creative mode adds half a block: the same far cell is in range.
        expect(!server::mining::within_reach(
                   0.5, eye_y, 0.5, sim::block_pos{6, 16, 0},
                   server::mining::interaction_range) ||
               server::mining::within_reach(
                   0.5, eye_y, 0.5, sim::block_pos{6, 16, 0},
                   server::mining::creative_interaction_range));
        expect(server::mining::block_name("minecraft:oak_log[axis=y]") ==
               "minecraft:oak_log");
        expect(server::mining::block_name("minecraft:stone") ==
               "minecraft:stone");
    };
};

suite<"mining/service"> mining_service_tests = [] {
    "mining/stone with an iron pickaxe breaks after the reference ticks and "
    "costs "
    "one durability"_test = [] {
        auto state{make_fixture()};
        expect(state != nullptr);
        if (!state) {
            return;
        }
        auto *actor{state->actor()};
        expect(actor != nullptr && state->observer() != nullptr);
        if (actor == nullptr) {
            return;
        }
        state->service->start(1, sim::block_pos{0, 15, 0});
        const auto *progress{state->service->progress_of(1)};
        expect(progress != nullptr);
        if (progress == nullptr) {
            return;
        }
        expect(progress->destroying);
        expect(progress->tool_slot == 0U);
        // Six tenths of the first tick: stage one.
        expect(progress->last_stage == 1);
        // The stage goes to the observer, not to the miner: it renders its own
        // prediction.
        expect(state->stages.size() == 1U);
        expect(state->stages.front().connection == 2U);
        expect(state->stages.front().stage == 1);

        for (int tick{0}; tick < 7; ++tick) {
            state->service->tick();
        }
        expect(state->service->progress_of(1) == nullptr);
        expect(state->level->block_at(sim::block_pos{0, 15, 0}) ==
               sim::air_block);
        expect(sim::stack_damage(actor->inventory[0]) == 1U);
        // Stage sequence of the reference formula, then the animation clear.
        const std::vector<std::int32_t> expected{1, 2, 4, 5, 6, 8, 9, 10, -1};
        expect(state->stages.size() == expected.size());
        for (std::size_t index{0}; index < state->stages.size(); ++index) {
            expect(state->stages[index].connection == 2U);
            expect(state->stages[index].entity_id ==
                   static_cast<std::int32_t>(actor->id));
            expect(
                same_pos(state->stages[index].pos, sim::block_pos{0, 15, 0}));
            expect(state->stages[index].stage == expected[index]);
        }
    };

    "mining/an abort drops the progress and leaves the block in place"_test =
        [] {
            auto state{make_fixture()};
            expect(state != nullptr);
            if (!state) {
                return;
            }
            state->service->start(1, sim::block_pos{0, 15, 0});
            state->service->tick();
            state->service->tick();
            state->service->abort(1, sim::block_pos{0, 15, 0});
            expect(state->service->progress_of(1) == nullptr);
            expect(state->level->block_at(sim::block_pos{0, 15, 0}) ==
                   state->stone);
            expect(last_stage(*state) == -1);
            // An abort for a block that is not being mined clears the animation
            // of the packet position and has no other effect.
            const auto stages_before{state->stages.size()};
            state->service->abort(1, sim::block_pos{1, 15, 0});
            expect(state->service->progress_of(1) == nullptr);
            expect(state->stages.size() == stages_before + 1U);
            expect(state->level->block_at(sim::block_pos{1, 15, 0}) ==
                   state->dirt);
        };

    "mining/a stop above the threshold completes the client-reported break"_test =
        [] {
            auto state{make_fixture()};
            expect(state != nullptr);
            if (!state) {
                return;
            }
            auto *actor{state->actor()};
            expect(actor != nullptr);
            if (actor == nullptr) {
                return;
            }
            state->service->start(1, sim::block_pos{0, 15, 0});
            for (int tick{0}; tick < 6; ++tick) {
                state->service->tick();
            }
            state->service->stop(1, sim::block_pos{0, 15, 0});
            expect(state->service->progress_of(1) == nullptr);
            expect(state->level->block_at(sim::block_pos{0, 15, 0}) ==
                   sim::air_block);
            expect(sim::stack_damage(actor->inventory[0]) == 1U);
            // A stop for a block that is not being mined is ignored.
            state->service->start(1, sim::block_pos{1, 15, 0});
            state->service->stop(1, sim::block_pos{2, 15, 0});
            expect(state->service->progress_of(1) != nullptr);
        };

    "mining/a stop below the threshold keeps the accumulated progress"_test =
        [] {
            auto state{make_fixture()};
            expect(state != nullptr);
            if (!state) {
                return;
            }
            auto *actor{state->actor()};
            expect(actor != nullptr);
            if (actor == nullptr) {
                return;
            }
            // Bare hands on stone mine at 1/150 per tick, so ten ticks are far
            // below the reference's 0.7 threshold.
            actor->inventory[0] = sim::item_stack{};
            state->service->start(1, sim::block_pos{0, 15, 0});
            for (int tick{0}; tick < 10; ++tick) {
                state->service->tick();
            }
            state->service->stop(1, sim::block_pos{0, 15, 0});
            const auto *progress{state->service->progress_of(1)};
            expect(progress != nullptr);
            if (progress == nullptr) {
                return;
            }
            expect(!progress->destroying);
            expect(progress->delayed);
            expect(state->level->block_at(sim::block_pos{0, 15, 0}) ==
                   state->stone);
            // The break keeps accumulating on the server's clock and completes
            // at the same tick continuous mining would have.
            std::size_t ticks{0};
            while (state->service->progress_of(1) != nullptr && ticks < 400) {
                state->service->tick();
                ++ticks;
            }
            expect(ticks == 139U);
            expect(state->level->block_at(sim::block_pos{0, 15, 0}) ==
                   sim::air_block);
        };

    "mining/creative breaks any hardness immediately and pays no durability"_test =
        [] {
            auto state{make_fixture()};
            expect(state != nullptr);
            if (!state) {
                return;
            }
            auto *actor{state->actor()};
            expect(actor != nullptr);
            if (actor == nullptr) {
                return;
            }
            actor->game_mode = sim::game_mode_creative;
            state->service->start(1, sim::block_pos{0, 15, 0});
            expect(state->level->block_at(sim::block_pos{0, 15, 0}) ==
                   sim::air_block);
            expect(state->service->progress_of(1) == nullptr);
            expect(sim::stack_damage(actor->inventory[0]) == 0U);
            expect(state->stages.empty());
            // Creative breaks the unbreakable block the survival rules refuse.
            state->service->start(1, sim::block_pos{3, 15, 0});
            expect(state->level->block_at(sim::block_pos{3, 15, 0}) ==
                   sim::air_block);
        };

    "mining/an unbreakable block never progresses and never breaks"_test = [] {
        auto state{make_fixture()};
        expect(state != nullptr);
        if (!state) {
            return;
        }
        auto *actor{state->actor()};
        expect(actor != nullptr);
        if (actor == nullptr) {
            return;
        }
        // The reference does not refuse the action; it records a break whose
        // accumulated progress is always zero, so the block can never reach the
        // completion threshold.
        state->service->start(1, sim::block_pos{3, 15, 0});
        const auto *progress{state->service->progress_of(1)};
        expect(progress != nullptr);
        if (progress == nullptr) {
            return;
        }
        expect(progress->destroying);
        expect(progress->last_stage == 0);
        for (int tick{0}; tick < 200; ++tick) {
            state->service->tick();
        }
        expect(state->level->block_at(sim::block_pos{3, 15, 0}) ==
               state->bedrock);
        expect(state->service->progress_of(1) != nullptr);
        expect(sim::stack_damage(actor->inventory[0]) == 0U);
        state->service->abort(1, sim::block_pos{3, 15, 0});
        expect(state->service->progress_of(1) == nullptr);
    };

    "mining/a tool that forbids creative destruction cannot break"_test = [] {
        auto state{make_fixture()};
        expect(state != nullptr);
        if (!state) {
            return;
        }
        auto *actor{state->actor()};
        expect(actor != nullptr);
        if (actor == nullptr) {
            return;
        }
        actor->game_mode = sim::game_mode_creative;
        actor->inventory[0] =
            sim::item_stack{.item = "minecraft:wooden_sword", .count = 1};
        state->service->start(1, sim::block_pos{1, 15, 0});
        expect(state->level->block_at(sim::block_pos{1, 15, 0}) == state->dirt);
        expect(state->states.size() == 1U);
        expect(state->states.front().connection == 1U);
        expect(same_pos(state->states.front().pos, sim::block_pos{1, 15, 0}));
        expect(state->states.front().state == state->dirt);
    };

    "mining/adventure and spectator cannot break"_test = [] {
        for (const auto mode :
             {sim::game_mode_adventure, sim::game_mode_spectator}) {
            auto state{make_fixture()};
            expect(state != nullptr);
            if (!state) {
                return;
            }
            auto *actor{state->actor()};
            expect(actor != nullptr);
            if (actor == nullptr) {
                return;
            }
            actor->game_mode = mode;
            state->service->start(1, sim::block_pos{0, 15, 0});
            expect(state->level->block_at(sim::block_pos{0, 15, 0}) ==
                   state->stone);
            expect(state->service->progress_of(1) == nullptr);
            expect(state->states.size() == 1U);
            expect(state->states.front().state == state->stone);
        }
    };

    "mining/out-of-reach, unloaded and air targets change nothing"_test = [] {
        auto state{make_fixture()};
        expect(state != nullptr);
        if (!state) {
            return;
        }
        auto *actor{state->actor()};
        expect(actor != nullptr);
        if (actor == nullptr) {
            return;
        }
        // Too far: the reference ignores the action without a correction.
        actor->x = 100.0;
        state->service->start(1, sim::block_pos{0, 15, 0});
        expect(state->service->progress_of(1) == nullptr);
        expect(state->level->block_at(sim::block_pos{0, 15, 0}) ==
               state->stone);
        expect(state->states.empty());
        // An unloaded column has no authoritative block to mine.
        actor->x = 0.5;
        state->service->start(1, sim::block_pos{100, 15, 100});
        expect(state->service->progress_of(1) == nullptr);
        expect(state->states.empty());
        // Air needs no break and gets no correction.
        state->service->start(1, sim::block_pos{5, 15, 0});
        expect(state->service->progress_of(1) == nullptr);
        expect(state->states.empty());
    };

    "mining/a game mode change drops the break"_test = [] {
        auto state{make_fixture()};
        expect(state != nullptr);
        if (!state) {
            return;
        }
        auto *actor{state->actor()};
        expect(actor != nullptr);
        if (actor == nullptr) {
            return;
        }
        state->service->start(1, sim::block_pos{0, 15, 0});
        state->service->tick();
        state->service->tick();
        actor->game_mode = sim::game_mode_creative;
        state->service->tick();
        expect(state->service->progress_of(1) == nullptr);
        expect(state->level->block_at(sim::block_pos{0, 15, 0}) ==
               state->stone);
        expect(last_stage(*state) == -1);
    };

    "mining/a disconnect clears the animation of the break in flight"_test =
        [] {
            auto state{make_fixture()};
            expect(state != nullptr);
            if (!state) {
                return;
            }
            state->service->start(1, sim::block_pos{0, 15, 0});
            state->service->tick();
            state->service->tick();
            // The record leaves the table first: the service still knows the
            // miner identity it recorded when the break started.
            static_cast<void>(state->level->players().remove(1));
            state->service->tick();
            expect(state->service->progress_of(1) == nullptr);
            expect(state->service->active_breaks() == 0U);
            expect(last_stage(*state) == -1);
        };

    "mining/a reset drops the break and clears its animation"_test = [] {
        auto state{make_fixture()};
        expect(state != nullptr);
        if (!state) {
            return;
        }
        state->service->start(1, sim::block_pos{0, 15, 0});
        state->service->tick();
        state->service->reset(1);
        expect(state->service->progress_of(1) == nullptr);
        expect(last_stage(*state) == -1);
    };

    "mining/observers outside the animation radius and other levels get no "
    "stage"_test = [] {
        auto state{make_fixture()};
        expect(state != nullptr);
        if (!state) {
            return;
        }
        auto *observer{state->observer()};
        expect(observer != nullptr);
        if (observer == nullptr) {
            return;
        }
        // 32 blocks from the block; the reference's squared distance bound is
        // exclusive, so a player at exactly 32 is outside.
        observer->x = 32.0;
        state->service->start(1, sim::block_pos{0, 15, 0});
        state->service->tick();
        expect(state->stages.empty());
        observer->x = 0.5;
        observer->world = "other";
        state->service->tick();
        expect(state->stages.empty());
        observer->world = "overworld";
        state->service->tick();
        expect(!state->stages.empty());
    };

    "mining/a broken tool leaves the bare hand and the next break is slower"_test =
        [] {
            auto state{make_fixture()};
            expect(state != nullptr);
            if (!state) {
                return;
            }
            auto *actor{state->actor()};
            expect(actor != nullptr);
            if (actor == nullptr) {
                return;
            }
            sim::set_stack_damage(actor->inventory[0], 249);
            state->service->start(1, sim::block_pos{0, 15, 0});
            for (int tick{0}; tick < 7; ++tick) {
                state->service->tick();
            }
            expect(state->level->block_at(sim::block_pos{0, 15, 0}) ==
                   sim::air_block);
            expect(sim::stack_empty(actor->inventory[0]));
            // The slot is empty now: bare hands on dirt use the 30 divisor and
            // the first stage is zero rather than one.
            state->service->start(1, sim::block_pos{1, 15, 0});
            const auto *progress{state->service->progress_of(1)};
            expect(progress != nullptr);
            if (progress == nullptr) {
                return;
            }
            expect(progress->tool_slot == 0U);
            expect(progress->last_stage == 0);
            expect(state->level->block_at(sim::block_pos{1, 15, 0}) ==
                   state->dirt);
        };

    "mining/a zero destroy time block breaks on start without tool wear"_test =
        [] {
            auto state{make_fixture()};
            expect(state != nullptr);
            if (!state) {
                return;
            }
            auto *actor{state->actor()};
            expect(actor != nullptr);
            if (actor == nullptr) {
                return;
            }
            state->service->start(1, sim::block_pos{4, 15, 0});
            expect(state->level->block_at(sim::block_pos{4, 15, 0}) ==
                   sim::air_block);
            expect(state->service->progress_of(1) == nullptr);
            expect(sim::stack_damage(actor->inventory[0]) == 0U);
        };

    "mining/a new target replaces the previous break"_test = [] {
        auto state{make_fixture()};
        expect(state != nullptr);
        if (!state) {
            return;
        }
        state->service->start(1, sim::block_pos{0, 15, 0});
        state->service->tick();
        state->service->start(1, sim::block_pos{2, 15, 0});
        const auto *progress{state->service->progress_of(1)};
        expect(progress != nullptr);
        if (progress == nullptr) {
            return;
        }
        expect(same_pos(progress->pos, sim::block_pos{2, 15, 0}));
        // The abandoned target got its authoritative state back.
        expect(!state->states.empty());
        expect(same_pos(state->states.back().pos, sim::block_pos{0, 15, 0}));
        expect(state->states.back().state == state->stone);
        // The abandoned animation is cleared and the new target starts at the
        // obsidian speed, a stage of zero.
        expect(last_stage(*state) == 0);
    };
};
