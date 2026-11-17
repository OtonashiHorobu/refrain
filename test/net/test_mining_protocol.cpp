// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Block breaking over the real protocol: a bot reports the selected hotbar
// slot and the three player actions, the server's mining service owns the
// progress, a second bot observes the destroy-stage packets and the world ends
// up with air where the block was. The listener is the production
// `net::server`; only the runtime's wiring of the mining service is repeated
// here.

#include <refrain/core/config.hpp>
#include <refrain/net/bot.hpp>
#include <refrain/net/primitives.hpp>
#include <refrain/net/server.hpp>
#include <refrain/net/wire.hpp>
#include <refrain/protocol/current.hpp>
#include <refrain/server/mining_service.hpp>
#include <refrain/server/world_port.hpp>
#include <refrain/sim/item.hpp>
#include <refrain/sim/player.hpp>
#include <refrain/sim/world.hpp>
#include <refrain/world/model.hpp>
#include <refrain/world/store.hpp>

#include <boost/ut.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "support/scratch_directory.hpp"
#include "test_protocol.hpp"

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace config = refrain::core::config;
namespace data = refrain::data;
namespace net = refrain::net;
namespace play_sb = refrain::protocol::current::play::serverbound;
namespace play_cb = refrain::protocol::current::play::clientbound;
namespace server = refrain::server;
namespace sim = refrain::sim;
namespace storage = refrain::storage;
namespace world_ns = refrain::world;
namespace test_support = refrain::test_support;

/// One destroy-stage packet an observer received.
struct destruction_packet {
    std::int32_t entity_id{0};
    sim::block_pos pos{};
    std::int32_t progress{0};
};

/// One play connection the server accepted, keyed by the bot name.
struct joined_player {
    std::uint64_t connection{0};
    sim::entity_id entity{0};
};

/// Everything one integration test drives: a real simulation world, the
/// production network listener and the mining service, wired together the way
/// the runtime wires them.
struct harness {
    test_support::scratch_directory directory{};
    config::store configuration{};
    std::optional<config::view> view{};
    std::optional<sim::world> level{};
    std::unique_ptr<server::world_port> port{};
    std::unique_ptr<net::server> listener{};
    data::block_mining_table blocks{};
    data::item_tool_table tools{};
    std::unique_ptr<server::mining_service> mining{};
    std::atomic<int> joins{0};
    std::map<std::string, joined_player> players{};
    std::atomic<std::uint8_t> selected_slot{0};
    std::atomic<int> slot_updates{0};

    harness() = default;
    harness(const harness &) = delete;
    auto operator=(const harness &) -> harness & = delete;
    ~harness() {
        // The listener owns I/O threads that invoke player_leave; stop it
        // before the mining service (declared after the listener) is destroyed.
        if (listener) {
            listener->stop();
            listener.reset();
        }
        mining.reset();
    }

    [[nodiscard]] auto open() -> bool {
        {
            std::ofstream stream{directory.path() / "network.toml",
                                 std::ios::binary | std::ios::trunc};
            stream << "bind_host = \"127.0.0.1\"\nport = 0\n"
                      "online_mode = false\nview_distance = 3\n";
        }
        configuration = config::store::empty();
        if (!configuration.mount(
                {.name = "network",
                 .path = (directory.path() / "network.toml").string()})) {
            return false;
        }
        static_cast<void>(configuration.load_all());
        auto loaded{configuration.module("network")};
        if (!loaded) {
            return false;
        }
        view = std::move(*loaded);

        auto store{world_ns::world_store::create(
            (directory.path() / "world").string())};
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
        auto opened{sim::world::open((directory.path() / "world").string())};
        if (!opened) {
            return false;
        }
        level = std::move(*opened);
        if (!level->ensure_chunk({0, 0})) {
            return false;
        }
        // The slow target an abort keeps in place.
        const auto obsidian{level->registry().intern("minecraft:obsidian")};
        if (!level->set_block(sim::block_pos{2, 15, 0}, obsidian)) {
            return false;
        }

        blocks.insert("minecraft:stone", stone_record());
        blocks.insert("minecraft:obsidian", obsidian_record());
        tools.insert("minecraft:iron_pickaxe", iron_pickaxe_record());
        sim::item_table items{};
        items.insert(
            "minecraft:iron_pickaxe",
            sim::item_definition{.max_stack_size = 1, .max_damage = 250});
        level->set_item_table(items);
        port = server::make_borrowed_world_port(*level);
        if (!port) {
            return false;
        }

        net::server_inputs inputs{};
        inputs.protocol = refrain::test::make_test_protocol();
        inputs.spawn_x = 0.5;
        inputs.spawn_y = 16.0;
        inputs.spawn_z = 0.5;
        inputs.spawn_chunks =
            std::make_shared<const std::vector<std::vector<std::byte>>>(
                std::vector<std::vector<std::byte>>{dummy_chunk_body()});
        auto tree{std::make_shared<net::command_tree>()};
        tree->nodes.push_back(net::command_node{});
        tree->root_index = 0;
        inputs.commands.tree = [tree](const net::player_identity &) {
            return tree;
        };
        inputs.join_state = [](const net::player_identity &identity) {
            return net::join_state{.mode = identity.name == "Creative"
                                               ? net::game_mode::creative
                                               : net::game_mode::survival};
        };
        inputs.player_join = [this](const net::player_identity &identity) {
            auto *level_world{&*level};
            const auto mode{identity.name == "Creative"
                                ? sim::game_mode_creative
                                : sim::game_mode_survival};
            const auto added{
                level_world->add_player(sim::player_table::registration{
                    .connection = identity.connection,
                    .profile_id = identity.id.bytes,
                    .name = std::string{identity.name},
                    .world = std::string{identity.world},
                    .x = 0.5,
                    .y = 16.0,
                    .z = 0.5,
                    .game_mode = mode,
                    .inventory_slots = 36,
                })};
            if (added) {
                players[std::string{identity.name}] = joined_player{
                    .connection = identity.connection, .entity = *added};
                // The tool sits in the fourth hotbar slot; the client selects
                // that slot before it mines.
                auto *record{level_world->players().find_by_connection(
                    identity.connection)};
                if (record != nullptr && record->inventory.size() > 3) {
                    record->inventory[3] = sim::item_stack{
                        .item = "minecraft:iron_pickaxe", .count = 1};
                }
                ++joins;
            }
        };
        inputs.player_leave = [this](const net::player_identity &identity) {
            if (mining) {
                mining->reset(identity.connection);
            }
            static_cast<void>(level->players().remove(identity.connection));
        };
        inputs.held_slot = [this](std::uint64_t connection, std::uint8_t slot) {
            selected_slot.store(slot, std::memory_order_relaxed);
            ++slot_updates;
            if (auto *record{level->players().find_by_connection(connection)};
                record != nullptr) {
                record->selected_slot = slot;
            }
        };
        inputs.start_destroy_block =
            [this](const net::break_action_event &event) {
                if (mining) {
                    mining->start(event.connection, position_of(event));
                }
            };
        inputs.stop_destroy_block =
            [this](const net::break_action_event &event) {
                if (mining) {
                    mining->stop(event.connection, position_of(event));
                }
            };
        inputs.abort_destroy_block =
            [this](const net::break_action_event &event) {
                if (mining) {
                    mining->abort(event.connection, position_of(event));
                }
            };
        auto started{net::server::start(std::move(inputs), *view)};
        if (!started) {
            return false;
        }
        listener = std::move(*started);
        mining = std::make_unique<server::mining_service>(
            blocks, tools, *port, level->players(),
            server::mining_service::outbound{
                .destroy_stage =
                    [this](std::uint64_t connection, std::int32_t entity_id,
                           const sim::block_pos &pos, std::int32_t stage) {
                        listener->send_block_destroy_stage(
                            connection, entity_id, pos.x, pos.y, pos.z, stage);
                    },
                .block_state = {},
            });
        return true;
    }

    [[nodiscard]] auto port_number() const -> std::uint16_t {
        return listener->local_port();
    }

    [[nodiscard]] auto position_of(const net::break_action_event &event)
        -> sim::block_pos {
        return sim::block_pos{event.x, static_cast<std::int16_t>(event.y),
                              event.z};
    }

    [[nodiscard]] static auto stone_record() -> data::block_mining {
        data::block_mining record{};
        record.hardness = 1.5F;
        record.flags = data::block_mining_requires_correct_tool;
        record.tool_mask = data::tool_type_bit(data::tool_type::pickaxe);
        record.needed_tier = 1;
        return record;
    }

    [[nodiscard]] static auto obsidian_record() -> data::block_mining {
        data::block_mining record{};
        record.hardness = 50.0F;
        record.flags = data::block_mining_requires_correct_tool;
        record.tool_mask = data::tool_type_bit(data::tool_type::pickaxe);
        record.needed_tier = 3;
        return record;
    }

    [[nodiscard]] static auto iron_pickaxe_record() -> data::tool_mining {
        data::tool_mining tool{};
        tool.name = "minecraft:iron_pickaxe";
        tool.type = data::tool_type::pickaxe;
        tool.material = data::tool_material::iron;
        tool.harvest_level = 2;
        tool.tier_speed = 6.0F;
        tool.default_mining_speed = 1.0F;
        tool.damage_per_block = 1;
        tool.max_stack_size = 1;
        tool.max_damage = 250;
        tool.can_destroy_blocks_in_creative = true;
        return tool;
    }

    [[nodiscard]] static auto dummy_chunk_body() -> std::vector<std::byte> {
        std::vector<std::byte> body;
        net::writer out{body};
        out.write_varint(0x2E); // level_chunk_with_light
        return body;
    }
};

/// The body of one `player_action` packet, packet id included.
[[nodiscard]] auto player_action_body(std::uint32_t action, std::int32_t x,
                                      std::int32_t y, std::int32_t z,
                                      std::uint32_t sequence)
    -> std::vector<std::byte> {
    std::vector<std::byte> body;
    net::writer out{body};
    out.write_varint(play_sb::player_action::packet_id);
    play_sb::player_action packet{};
    packet.action = action;
    packet.position = net::pack_block_position(x, y, z);
    packet.direction = 1;
    packet.sequence = sequence;
    static_cast<void>(play_sb::encode_player_action(out, packet));
    return body;
}

/// The body of one `set_carried_item` packet, packet id included.
[[nodiscard]] auto carried_slot_body(std::int16_t slot)
    -> std::vector<std::byte> {
    std::vector<std::byte> body;
    net::writer out{body};
    out.write_varint(play_sb::set_carried_item::packet_id);
    play_sb::set_carried_item packet{.slot = slot};
    static_cast<void>(play_sb::encode_set_carried_item(out, packet));
    return body;
}

[[nodiscard]] auto bot_options(std::uint16_t port, std::string name,
                               std::size_t wait_chunks,
                               std::chrono::milliseconds linger)
    -> net::bot_options {
    return net::bot_options{
        .target = {.host = "127.0.0.1", .port = port},
        .username = std::move(name),
        .timeout = std::chrono::milliseconds{5000},
        .wait_chunks = wait_chunks,
        .linger = linger,
        .capture = true,
    };
}

[[nodiscard]] auto count_packets(const net::bot_report &report,
                                 std::uint32_t packet_id) -> std::size_t {
    std::size_t count{0};
    for (const auto &packet : report.capture) {
        if (packet.state == 2U && packet.id == packet_id) {
            ++count;
        }
    }
    return count;
}

[[nodiscard]] auto decode_destruction(std::span<const std::byte> body)
    -> std::optional<destruction_packet> {
    net::reader in{body};
    if (auto id{in.read_varint()};
        !id || *id != play_cb::block_destruction::packet_id) {
        return std::nullopt;
    }
    play_cb::block_destruction packet{};
    if (auto decoded{play_cb::decode_block_destruction(in, packet)}; !decoded) {
        return std::nullopt;
    }
    const auto position{net::unpack_block_position(packet.pos)};
    return destruction_packet{
        .entity_id = packet.id,
        .pos = sim::block_pos{position.x, static_cast<std::int16_t>(position.y),
                              position.z},
        .progress = packet.progress,
    };
}

[[nodiscard]] auto same_pos(const sim::block_pos &left,
                            const sim::block_pos &right) -> bool {
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

} // namespace

suite<"net/mining"> mining_protocol_tests = [] {
    "net/a survival break reports stages, air and tool wear"_test = [] {
        auto state{std::make_unique<harness>()};
        expect(state->open());
        if (!state->view) {
            return;
        }
        std::atomic<bool> ticking{true};
        std::thread ticker{[&state, &ticking] {
            while (ticking.load(std::memory_order_relaxed)) {
                state->listener->tick();
                state->mining->tick();
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
        }};

        const auto port{state->port_number()};
        expect(port != 0);
        // The observer connects first so the miner's stage packets have a
        // recipient from the first tick of the break.
        std::optional<net::bot_report> observer{};
        std::thread observer_thread{[&] {
            if (auto report{net::run_bot(bot_options(
                    port, "Observer", 1, std::chrono::milliseconds{2500}))};
                report) {
                observer = std::move(*report);
            }
        }};
        for (int wait{0}; wait < 1000 && state->joins.load() < 1; ++wait) {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        expect(state->joins.load() >= 1);

        // Select slot 3, mine the stone under the spawn, then abort a break on
        // the obsidian so the server keeps that block in place.
        auto miner_options{
            bot_options(port, "Miner", 1, std::chrono::milliseconds{500})};
        miner_options.play_actions = {
            net::bot_options::play_action{.delay =
                                              std::chrono::milliseconds{50},
                                          .body = carried_slot_body(3)},
            net::bot_options::play_action{
                .delay = std::chrono::milliseconds{50},
                .body = player_action_body(0, 0, 15, 0, 1)},
            net::bot_options::play_action{
                .delay = std::chrono::milliseconds{300},
                .body = player_action_body(3, 0, 15, 0, 2)},
            net::bot_options::play_action{
                .delay = std::chrono::milliseconds{50},
                .body = player_action_body(0, 2, 15, 0, 3)},
            net::bot_options::play_action{
                .delay = std::chrono::milliseconds{150},
                .body = player_action_body(2, 2, 15, 0, 4)},
        };
        auto miner{net::run_bot(miner_options)};
        expect(miner.has_value());
        ticking.store(false, std::memory_order_relaxed);
        ticker.join();
        observer_thread.join();

        expect(observer.has_value());
        if (!miner || !observer) {
            return;
        }
        // The stone under the spawn is air; the aborted obsidian is untouched.
        expect(state->level->block_at(sim::block_pos{0, 15, 0}) ==
               sim::air_block);
        const auto obsidian{
            state->level->registry().intern("minecraft:obsidian")};
        expect(state->level->block_at(sim::block_pos{2, 15, 0}) == obsidian);
        // The client's selected slot reached the player record and the break
        // used the tool in that slot.
        const auto miner_player{state->players.find("Miner")};
        expect(miner_player != state->players.end());
        if (miner_player == state->players.end()) {
            return;
        }
        const auto *record{state->level->players().find_by_connection(
            miner_player->second.connection)};
        expect(record != nullptr);
        if (record == nullptr) {
            return;
        }
        expect(record->selected_slot == 3U);
        expect(state->slot_updates.load() >= 1);
        expect(sim::stack_damage(record->inventory[3]) == 1U);
        expect(!sim::stack_empty(record->inventory[3]));
        // Every action packet is acknowledged, including the stop that arrived
        // after the server had already broken the block.
        expect(count_packets(*miner, play_cb::block_changed_ack::packet_id) >=
               4U);
        // The miner does not receive its own destroy-stage packets; the
        // observer does, and the stages follow the reference formula before
        // the animation is cleared.
        expect(count_packets(*miner, play_cb::block_destruction::packet_id) ==
               0U);
        std::vector<destruction_packet> destruction{};
        for (const auto &packet : observer->capture) {
            if (packet.state != 2U ||
                packet.id != play_cb::block_destruction::packet_id) {
                continue;
            }
            auto decoded{decode_destruction(packet.body)};
            expect(decoded.has_value());
            if (decoded) {
                destruction.push_back(*decoded);
            }
        }
        // Stone with an iron pickaxe: stages 1, 2, 4, 5, 6, 8, 9, 10 and the
        // clear. The obsidian break that follows starts at stage 0 and is
        // aborted after a handful of milliseconds; its remaining stages depend
        // on how many server ticks passed before the abort, so only the start
        // and the clear are fixed.
        const std::vector<std::int32_t> stone_stages{1, 2, 4,  5, 6,
                                                     8, 9, 10, -1};
        expect(destruction.size() >= stone_stages.size() + 2U);
        for (std::size_t index{0};
             index < stone_stages.size() && index < destruction.size();
             ++index) {
            expect(destruction[index].entity_id ==
                   static_cast<std::int32_t>(miner_player->second.entity));
            expect(destruction[index].progress == stone_stages[index]);
            expect(same_pos(destruction[index].pos, sim::block_pos{0, 15, 0}));
        }
        for (std::size_t index{stone_stages.size()}; index < destruction.size();
             ++index) {
            expect(destruction[index].entity_id ==
                   static_cast<std::int32_t>(miner_player->second.entity));
            expect(same_pos(destruction[index].pos, sim::block_pos{2, 15, 0}));
        }
        if (destruction.size() > stone_stages.size()) {
            expect(destruction[stone_stages.size()].progress == 0);
            expect(destruction.back().progress == -1);
        }
        // The stop of the finished block was ignored rather than breaking a
        // second time, and the aborted break left no state behind.
        expect(state->mining->active_breaks() == 0U);
    };

    "net/creative breaks on start and rejects out-of-reach and unloaded "
    "targets"_test = [] {
        auto state{std::make_unique<harness>()};
        expect(state->open());
        if (!state->view) {
            return;
        }
        std::atomic<bool> ticking{true};
        std::thread ticker{[&state, &ticking] {
            while (ticking.load(std::memory_order_relaxed)) {
                state->listener->tick();
                state->mining->tick();
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
        }};
        const auto port{state->port_number()};
        expect(port != 0);
        auto options{
            bot_options(port, "Creative", 1, std::chrono::milliseconds{400})};
        options.play_actions = {
            net::bot_options::play_action{
                .delay = std::chrono::milliseconds{50},
                .body = player_action_body(0, 0, 15, 0, 1)},
            // Seven blocks away: outside the creative interaction range.
            net::bot_options::play_action{
                .delay = std::chrono::milliseconds{100},
                .body = player_action_body(0, 0, 15, 7, 2)},
            // A column the server has not loaded.
            net::bot_options::play_action{
                .delay = std::chrono::milliseconds{100},
                .body = player_action_body(0, 100, 15, 100, 3)},
        };
        auto report{net::run_bot(options)};
        expect(report.has_value());
        ticking.store(false, std::memory_order_relaxed);
        ticker.join();
        if (!report) {
            return;
        }
        // Creative broke the block under the spawn immediately.
        expect(state->level->block_at(sim::block_pos{0, 15, 0}) ==
               sim::air_block);
        // The far block and the unloaded column are untouched and produced no
        // break state.
        expect(state->level->block_at(sim::block_pos{0, 15, 7}) ==
               state->level->registry().intern("minecraft:stone"));
        expect(!state->port->column_loaded(sim::chunk_pos{6, 6}));
        expect(state->mining->active_breaks() == 0U);
        // Every action was acknowledged.
        expect(count_packets(*report, play_cb::block_changed_ack::packet_id) >=
               3U);
        // Creative breaks do not animate for anyone, so the actor receives no
        // destruction packet either.
        expect(count_packets(*report, play_cb::block_destruction::packet_id) ==
               0U);
    };
};
