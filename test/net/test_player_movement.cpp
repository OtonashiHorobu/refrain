// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/config.hpp>
#include <refrain/net/bot.hpp>
#include <refrain/net/server.hpp>
#include <refrain/platform/process.hpp>
#include <refrain/protocol/current.hpp>
#include <refrain/sim/player_physics.hpp>
#include <refrain/sim/world.hpp>
#include <refrain/world/store.hpp>

#include <boost/ut.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "test_protocol.hpp"

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace config = refrain::core::config;
namespace net = refrain::net;
namespace sim = refrain::sim;
namespace storage = refrain::storage;
namespace world_ns = refrain::world;

[[nodiscard]] auto make_temp_dir() -> std::filesystem::path {
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    const auto path{std::filesystem::temp_directory_path() /
                    ("refrain_move_" +
                     std::to_string(refrain::platform::process_id()) + "_" +
                     std::to_string(stamp))};
    std::error_code error;
    std::filesystem::create_directories(path, error);
    return path;
}

/// A world whose section zero is stone, so the floor every player stands on is
/// y=16. The network layer never reads the blocks; the simulation validator
/// does.
[[nodiscard]] auto open_test_world(const std::filesystem::path &directory)
    -> std::optional<sim::world> {
    auto store{world_ns::world_store::create(directory.string())};
    if (!store) {
        return std::nullopt;
    }
    storage::chunk chunk{};
    chunk.format_version = world_ns::chunk_format_version;
    chunk.status = storage::chunk_status::full;
    chunk.y_pos = 0;
    storage::section_record section{};
    section.y = 0;
    section.kind = storage::section_kind::single_value;
    section.bits_per_entry = 0;
    section.block_count = static_cast<std::uint16_t>(world_ns::section_volume);
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
        return std::nullopt;
    }
    auto opened{sim::world::open(directory.string())};
    if (!opened) {
        return std::nullopt;
    }
    if (!opened->ensure_chunk({0, 0})) {
        return std::nullopt;
    }
    opened->set_spawn_point(sim::world::spawn_point{
        .x = 0.5, .y = 16.0, .z = 0.5, .yaw = 0.0F, .pitch = 0.0F});
    return std::optional<sim::world>{std::move(*opened)};
}

/// One dummy chunk body: the bot only waits for the chunk-batch packet, and
/// the server sends `spawn_chunks` verbatim.
[[nodiscard]] auto dummy_chunk_body() -> std::vector<std::byte> {
    std::vector<std::byte> body;
    net::writer out{body};
    out.write_varint(0x2E); // level_chunk_with_light
    return body;
}

/// The network inputs the runtime builds for a first-party simulation: every
/// movement request goes through the simulation validator, and a rejected
/// request is answered with the authoritative position.
struct harness {
    sim::world level;
    net::server *listener{nullptr};
    std::atomic<int> corrections{0};
    std::atomic<int> accepted{0};
};

[[nodiscard]] auto movement_handler(harness &state) {
    return [&state](const net::movement_event &event) {
        auto *record{
            state.level.players().find_by_connection(event.connection)};
        if (record == nullptr) {
            return;
        }
        const auto verdict{state.level.submit_player_move(
            *record, sim::player_move_request{
                         .x = event.x,
                         .y = event.y,
                         .z = event.z,
                         .yaw = event.yaw,
                         .pitch = event.pitch,
                         .on_ground = event.on_ground,
                     })};
        if (verdict == sim::player_move_verdict::accepted) {
            ++state.accepted;
            return;
        }
        ++state.corrections;
        if (state.listener != nullptr) {
            state.listener->teleport_player(event.connection, record->x,
                                            record->y, record->z, record->yaw,
                                            record->pitch);
        }
    };
}

/// Decode the position fields of one captured `player_position` body. The
/// capture keeps the VarInt packet id, so the reader consumes it first.
[[nodiscard]] auto decode_position(std::span<const std::byte> body)
    -> std::optional<
        refrain::protocol::current::play::clientbound::player_position> {
    net::reader in{body};
    if (auto id{in.read_varint()};
        !id || *id != refrain::protocol::current::play::clientbound::
                          player_position::packet_id) {
        return std::nullopt;
    }
    refrain::protocol::current::play::clientbound::player_position packet{};
    if (auto decoded{refrain::protocol::current::play::clientbound::
                         decode_player_position(in, packet)};
        !decoded) {
        return std::nullopt;
    }
    return packet;
}

[[nodiscard]] auto count_packets(const net::bot_report &report,
                                 std::uint32_t id) -> std::size_t {
    std::size_t count{0};
    for (const auto &packet : report.capture) {
        if (packet.state == 2U && packet.id == id) {
            ++count;
        }
    }
    return count;
}

struct client_fixture {
    std::filesystem::path directory{};
    config::store store{};
    std::optional<config::view> view{};
};

[[nodiscard]] auto make_client_fixture() -> std::optional<client_fixture> {
    client_fixture fixture{};
    fixture.directory = make_temp_dir();
    {
        std::ofstream stream{fixture.directory / "network.toml",
                             std::ios::binary | std::ios::trunc};
        stream << "bind_host = \"127.0.0.1\"\nport = 0\n"
                  "online_mode = false\nview_distance = 3\n";
    }
    fixture.store = config::store::empty();
    if (static_cast<bool>(fixture.store.mount(
            {.name = "network",
             .path = (fixture.directory / "network.toml").string()}))) {
        static_cast<void>(fixture.store.load_all());
        auto view{fixture.store.module("network")};
        if (view) {
            fixture.view = std::move(*view);
        }
    }
    if (!static_cast<bool>(fixture.view)) {
        std::error_code error;
        std::filesystem::remove_all(fixture.directory, error);
        return std::nullopt;
    }
    return std::optional<client_fixture>{std::move(fixture)};
}

} // namespace

suite player_movement_net_tests = [] {
    "net/a modified teleport is rejected and corrected to the spawn point"_test =
        [] {
            auto fixture{make_client_fixture()};
            expect(fixture.has_value());
            if (!fixture) {
                return;
            }
            auto level{open_test_world(fixture->directory / "world")};
            expect(level.has_value());
            if (!level) {
                std::error_code error;
                std::filesystem::remove_all(fixture->directory, error);
                return;
            }
            harness state{.level = std::move(*level)};

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
            inputs.player_join =
                [&state](const net::player_identity &identity) {
                    static_cast<void>(
                        state.level.add_player(sim::player_table::registration{
                            .connection = identity.connection,
                            .profile_id = identity.id.bytes,
                            .name = std::string{identity.name},
                            .world = std::string{identity.world},
                            .x = 0.5,
                            .y = 16.0,
                            .z = 0.5,
                            .game_mode = sim::game_mode_survival,
                            .inventory_slots = 36,
                        }));
                };
            inputs.player_leave =
                [&state](const net::player_identity &identity) {
                    static_cast<void>(
                        state.level.players().remove(identity.connection));
                };
            inputs.movement = movement_handler(state);
            auto server{net::server::start(std::move(inputs), *fixture->view)};
            expect(server.has_value());
            if (!server) {
                std::error_code error;
                std::filesystem::remove_all(fixture->directory, error);
                return;
            }
            state.listener = server->get();
            const auto port{(*server)->local_port()};
            expect(port != 0);
            std::atomic<bool> ticking{true};
            std::thread ticker{[&] {
                while (ticking.load(std::memory_order_relaxed)) {
                    (*server)->tick();
                    std::this_thread::sleep_for(std::chrono::milliseconds{5});
                }
            }};
            auto report{net::run_bot(net::bot_options{
                .target = {.host = "127.0.0.1", .port = port},
                .username = "Cheater",
                .timeout = std::chrono::milliseconds{5000},
                .wait_chunks = 1,
                .linger = std::chrono::milliseconds{700},
                .move_dx = 0.0,
                .move_dy = 0.0,
                .move_dz = 50.0,
                .move_delay = std::chrono::milliseconds{300},
                .capture = true,
            })};
            expect(report.has_value());
            if (report) {
                expect(state.corrections.load() >= 1)
                    << state.corrections.load();
                // The login burst sends one position; the rejected request
                // adds the correction, so two positions arrive.
                expect(count_packets(
                           *report,
                           refrain::protocol::current::play::clientbound::
                               player_position::packet_id) >= 2U);
                std::optional<refrain::protocol::current::play::clientbound::
                                  player_position>
                    last{};
                for (const auto &packet : report->capture) {
                    if (auto decoded{decode_position(packet.body)}; decoded) {
                        last = decoded;
                    }
                }
                expect(last.has_value());
                if (last) {
                    expect(std::abs(last->x - 0.5) < 1.0e-9) << last->x;
                    expect(std::abs(last->y - 16.0) < 1.0e-9) << last->y;
                    expect(std::abs(last->z - 0.5) < 1.0e-9) << last->z;
                }
            }
            ticking.store(false, std::memory_order_relaxed);
            ticker.join();
            (*server)->stop();
            {
                std::error_code error;
                std::filesystem::remove_all(fixture->directory / "world",
                                            error);
            }
            std::error_code error;
            std::filesystem::remove_all(fixture->directory, error);
        };

    "net/a walking step is accepted without a correction"_test = [] {
        auto fixture{make_client_fixture()};
        expect(fixture.has_value());
        if (!fixture) {
            return;
        }
        auto level{open_test_world(fixture->directory / "world")};
        expect(level.has_value());
        if (!level) {
            std::error_code error;
            std::filesystem::remove_all(fixture->directory, error);
            return;
        }
        harness state{.level = std::move(*level)};
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
        inputs.player_join = [&state](const net::player_identity &identity) {
            static_cast<void>(
                state.level.add_player(sim::player_table::registration{
                    .connection = identity.connection,
                    .profile_id = identity.id.bytes,
                    .name = std::string{identity.name},
                    .world = std::string{identity.world},
                    .x = 0.5,
                    .y = 16.0,
                    .z = 0.5,
                    .game_mode = sim::game_mode_survival,
                    .inventory_slots = 36,
                }));
        };
        inputs.player_leave = [&state](const net::player_identity &identity) {
            static_cast<void>(
                state.level.players().remove(identity.connection));
        };
        inputs.movement = movement_handler(state);
        auto server{net::server::start(std::move(inputs), *fixture->view)};
        expect(server.has_value());
        if (!server) {
            std::error_code error;
            std::filesystem::remove_all(fixture->directory, error);
            return;
        }
        state.listener = server->get();
        const auto port{(*server)->local_port()};
        std::atomic<bool> ticking{true};
        std::thread ticker{[&] {
            while (ticking.load(std::memory_order_relaxed)) {
                (*server)->tick();
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            }
        }};
        auto report{net::run_bot(net::bot_options{
            .target = {.host = "127.0.0.1", .port = port},
            .username = "Walker",
            .timeout = std::chrono::milliseconds{5000},
            .wait_chunks = 1,
            .linger = std::chrono::milliseconds{600},
            .move_dx = 0.1,
            .move_delay = std::chrono::milliseconds{300},
            .capture = true,
        })};
        expect(report.has_value());
        if (report) {
            expect(state.accepted.load() >= 1);
            expect(state.corrections.load() == 0);
            expect(count_packets(*report,
                                 refrain::protocol::current::play::clientbound::
                                     player_position::packet_id) == 1U);
        }
        ticking.store(false, std::memory_order_relaxed);
        ticker.join();
        (*server)->stop();
        std::error_code error;
        std::filesystem::remove_all(fixture->directory, error);
    };

    "net/respawn returns a dead player to the spawn point"_test = [] {
        auto fixture{make_client_fixture()};
        expect(fixture.has_value());
        if (!fixture) {
            return;
        }
        auto level{open_test_world(fixture->directory / "world")};
        expect(level.has_value());
        if (!level) {
            std::error_code error;
            std::filesystem::remove_all(fixture->directory, error);
            return;
        }
        harness state{.level = std::move(*level)};
        net::server_inputs inputs{};
        inputs.protocol = refrain::test::make_test_protocol();
        inputs.spawn_x = 0.5;
        inputs.spawn_y = 16.0;
        inputs.spawn_z = 0.5;
        auto tree{std::make_shared<net::command_tree>()};
        tree->nodes.push_back(net::command_node{});
        tree->root_index = 0;
        inputs.commands.tree = [tree](const net::player_identity &) {
            return tree;
        };
        inputs.player_join = [&state](const net::player_identity &identity) {
            const auto added{
                state.level.add_player(sim::player_table::registration{
                    .connection = identity.connection,
                    .profile_id = identity.id.bytes,
                    .name = std::string{identity.name},
                    .world = std::string{identity.world},
                    .x = 0.5,
                    .y = 16.0,
                    .z = 0.5,
                    .game_mode = sim::game_mode_survival,
                    .inventory_slots = 36,
                })};
            if (!added) {
                return;
            }
            auto *record{state.level.player(*added)};
            if (record == nullptr) {
                return;
            }
            // The player died before the respawn request arrives.
            record->health = 0.0F;
            record->dead = true;
            record->death_time = 0;
        };
        inputs.player_leave = [&state](const net::player_identity &identity) {
            static_cast<void>(
                state.level.players().remove(identity.connection));
        };
        inputs.respawn_request = [&state](std::uint64_t connection) {
            const auto *record{
                state.level.players().find_by_connection(connection)};
            return record != nullptr && record->dead;
        };
        inputs.respawn = [&state](std::uint64_t connection) {
            if (auto *record{
                    state.level.players().find_by_connection(connection)};
                record != nullptr) {
                state.level.respawn_player(*record, 0.5, 16.0, 0.5, 0.0F, 0.0F);
            }
        };
        auto server{net::server::start(std::move(inputs), *fixture->view)};
        expect(server.has_value());
        if (!server) {
            std::error_code error;
            std::filesystem::remove_all(fixture->directory, error);
            return;
        }
        const auto port{(*server)->local_port()};
        std::atomic<bool> ticking{true};
        std::thread ticker{[&] {
            while (ticking.load(std::memory_order_relaxed)) {
                (*server)->tick();
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            }
        }};
        auto report{net::run_bot(net::bot_options{
            .target = {.host = "127.0.0.1", .port = port},
            .username = "Dead",
            .timeout = std::chrono::milliseconds{5000},
            .linger = std::chrono::milliseconds{700},
            .command_delay = std::chrono::milliseconds{300},
            .request_respawn = true,
            .capture = true,
        })};
        expect(report.has_value());
        if (report) {
            expect(count_packets(*report,
                                 refrain::protocol::current::play::clientbound::
                                     respawn::packet_id) == 1U);
            std::optional<
                refrain::protocol::current::play::clientbound::player_position>
                respawn_position{};
            for (const auto &packet : report->capture) {
                if (auto decoded{decode_position(packet.body)}; decoded) {
                    respawn_position = decoded;
                }
            }
            expect(respawn_position.has_value());
            if (respawn_position) {
                expect(std::abs(respawn_position->x - 0.5) < 1.0e-9);
                expect(std::abs(respawn_position->y - 16.0) < 1.0e-9);
                expect(std::abs(respawn_position->z - 0.5) < 1.0e-9);
            }
        }
        ticking.store(false, std::memory_order_relaxed);
        ticker.join();
        (*server)->stop();
        std::error_code error;
        std::filesystem::remove_all(fixture->directory, error);
    };

    "net/a respawn request is refused while the player is alive"_test = [] {
        auto fixture{make_client_fixture()};
        expect(fixture.has_value());
        if (!fixture) {
            return;
        }
        auto level{open_test_world(fixture->directory / "world")};
        expect(level.has_value());
        if (!level) {
            std::error_code error;
            std::filesystem::remove_all(fixture->directory, error);
            return;
        }
        harness state{.level = std::move(*level)};
        net::server_inputs inputs{};
        inputs.protocol = refrain::test::make_test_protocol();
        inputs.spawn_x = 0.5;
        inputs.spawn_y = 16.0;
        inputs.spawn_z = 0.5;
        auto tree{std::make_shared<net::command_tree>()};
        tree->nodes.push_back(net::command_node{});
        tree->root_index = 0;
        inputs.commands.tree = [tree](const net::player_identity &) {
            return tree;
        };
        inputs.player_join = [&state](const net::player_identity &identity) {
            static_cast<void>(
                state.level.add_player(sim::player_table::registration{
                    .connection = identity.connection,
                    .profile_id = identity.id.bytes,
                    .name = std::string{identity.name},
                    .world = std::string{identity.world},
                    .x = 0.5,
                    .y = 16.0,
                    .z = 0.5,
                    .game_mode = sim::game_mode_survival,
                    .inventory_slots = 36,
                }));
        };
        inputs.player_leave = [&state](const net::player_identity &identity) {
            static_cast<void>(
                state.level.players().remove(identity.connection));
        };
        inputs.respawn_request = [](std::uint64_t) { return false; };
        auto server{net::server::start(std::move(inputs), *fixture->view)};
        expect(server.has_value());
        if (!server) {
            std::error_code error;
            std::filesystem::remove_all(fixture->directory, error);
            return;
        }
        const auto port{(*server)->local_port()};
        std::atomic<bool> ticking{true};
        std::thread ticker{[&] {
            while (ticking.load(std::memory_order_relaxed)) {
                (*server)->tick();
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            }
        }};
        auto report{net::run_bot(net::bot_options{
            .target = {.host = "127.0.0.1", .port = port},
            .username = "Alive",
            .timeout = std::chrono::milliseconds{5000},
            .linger = std::chrono::milliseconds{500},
            .command_delay = std::chrono::milliseconds{300},
            .request_respawn = true,
            .capture = true,
        })};
        expect(report.has_value());
        if (report) {
            expect(count_packets(*report,
                                 refrain::protocol::current::play::clientbound::
                                     respawn::packet_id) == 0U);
        }
        ticking.store(false, std::memory_order_relaxed);
        ticker.join();
        (*server)->stop();
        std::error_code error;
        std::filesystem::remove_all(fixture->directory, error);
    };
};
