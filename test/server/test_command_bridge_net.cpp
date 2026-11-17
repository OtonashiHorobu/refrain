// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The in-game command frontend end to end: a real network server runs the
// command bridge, a bot types a command and receives the resulting teleport
// packet and whisper lines. The unit suites cover the world mutations; this
// suite proves the network-facing effect of the same executors.

#include <refrain/command/command.hpp>
#include <refrain/core/config.hpp>
#include <refrain/net/bot.hpp>
#include <refrain/net/server.hpp>
#include <refrain/protocol/current.hpp>
#include <refrain/server/command_bridge.hpp>
#include <refrain/sim/player.hpp>
#include <refrain/sim/world.hpp>
#include <refrain/world/store.hpp>

#include <boost/ut.hpp>

#include "net/test_protocol.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace command = refrain::command;
namespace config = refrain::core::config;
namespace fs = std::filesystem;
namespace net = refrain::net;
namespace server = refrain::server;
namespace sim = refrain::sim;
namespace world_ns = refrain::world;

[[nodiscard]] auto make_temp_dir(std::string_view name) -> fs::path {
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    const auto path{fs::temp_directory_path() /
                    ("refrain-command-net-" + std::to_string(stamp) + "-" +
                     std::string{name})};
    std::error_code error{};
    fs::create_directories(path, error);
    return path;
}

[[nodiscard]] auto open_empty_world(const fs::path &directory)
    -> std::optional<sim::world> {
    auto store{world_ns::world_store::create(directory.string())};
    if (!store) {
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

/// One dummy chunk body: the bot only waits for the chunk-batch packet, and
/// the server sends `spawn_chunks` verbatim.
[[nodiscard]] auto dummy_chunk_body() -> std::vector<std::byte> {
    std::vector<std::byte> body;
    net::writer out{body};
    out.write_varint(0x2E); // level_chunk_with_light
    return body;
}

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

[[nodiscard]] auto has_message(const net::bot_report &report,
                               std::string_view needle) -> bool {
    for (const auto &message : report.messages) {
        if (message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

struct client_fixture {
    fs::path directory{};
    config::store store{};
    std::optional<config::view> view{};
};

[[nodiscard]] auto make_client_fixture(std::string_view name)
    -> std::optional<client_fixture> {
    client_fixture fixture{};
    fixture.directory = make_temp_dir(name);
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
        std::error_code error{};
        fs::remove_all(fixture.directory, error);
        return std::nullopt;
    }
    return std::optional<client_fixture>{std::move(fixture)};
}

/// The world, the port and the command environment the runtime would compose,
/// plus a server that forwards its command callbacks to the bridge.
struct harness {
    sim::world level;
    std::unique_ptr<server::world_port> port{};
    std::shared_ptr<server::command_environment> environment{};
    net::command_bridge bridge{};

    explicit harness(sim::world world) : level{std::move(world)} {}

    void wire() {
        port = server::make_borrowed_world_port(level);
        environment = std::make_shared<server::command_environment>();
        environment->world = port.get();
        // No permission provider: the development fallback level decides, and
        // the owner level keeps every command in this batch runnable for the
        // bot.
        environment->permission = command::level::owner;
        bridge = server::make_command_bridge(environment);
    }

    void fill_network_inputs(net::server_inputs &inputs) {
        inputs.protocol = refrain::test::make_test_protocol();
        inputs.spawn_x = 0.5;
        inputs.spawn_y = 64.0;
        inputs.spawn_z = 0.5;
        inputs.spawn_chunks =
            std::make_shared<const std::vector<std::vector<std::byte>>>(
                std::vector<std::vector<std::byte>>{dummy_chunk_body()});
        inputs.commands.tree = [this](const net::player_identity &identity) {
            return bridge.tree(identity);
        };
        inputs.commands.execute =
            [this](const net::player_identity &identity,
                   std::string_view input) -> net::command_feedback {
            return bridge.execute(identity, input);
        };
        inputs.commands.suggest = [this](const net::player_identity &identity,
                                         std::string_view input,
                                         std::int32_t cursor) {
            auto result{bridge.suggest(identity, input, cursor)};
            net::suggestion_result out{};
            out.start = result.start;
            out.length = result.length;
            out.entries = std::move(result.entries);
            return out;
        };
        inputs.commands.revision = [this] { return bridge.revision(); };
        inputs.player_join = [this](const net::player_identity &identity) {
            static_cast<void>(level.add_player(sim::player_table::registration{
                .connection = identity.connection,
                .profile_id = identity.id.bytes,
                .name = std::string{identity.name},
                .world = std::string{identity.world},
                .x = 0.5,
                .y = 64.0,
                .z = 0.5,
                .game_mode = sim::game_mode_creative,
                .inventory_slots = 36,
            }));
        };
        inputs.player_leave = [this](const net::player_identity &identity) {
            static_cast<void>(level.players().remove(identity.connection));
        };
    }
};

/// Start a server over `harness`, tick it on this thread until the returned
/// callable is invoked.
template <typename Body>
void run_server(client_fixture &fixture, harness &state, Body body) {
    net::server_inputs inputs{};
    state.fill_network_inputs(inputs);
    auto server{net::server::start(std::move(inputs), *fixture.view)};
    expect(server.has_value());
    if (!server) {
        return;
    }
    state.environment->listener = server->get();
    std::atomic<bool> ticking{true};
    std::thread ticker{[&] {
        while (ticking.load(std::memory_order_relaxed)) {
            (*server)->tick();
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
    }};
    body(**server);
    ticking.store(false, std::memory_order_relaxed);
    ticker.join();
    state.environment->listener = nullptr;
    (*server)->stop();
}

} // namespace

const suite command_bridge_net_tests = [] {
    "commands/tp sends the teleport packet and moves the player record"_test =
        [] {
            auto fixture{make_client_fixture("tp")};
            expect(fixture.has_value());
            if (!fixture) {
                return;
            }
            auto level{open_empty_world(fixture->directory / "world")};
            expect(level.has_value());
            if (!level) {
                std::error_code error{};
                fs::remove_all(fixture->directory, error);
                return;
            }
            harness state{std::move(*level)};
            state.wire();
            run_server(*fixture, state, [](net::server &server) {
                auto report{net::run_bot(net::bot_options{
                    .target = {.host = "127.0.0.1",
                               .port = server.local_port()},
                    .username = "Teleporter",
                    .timeout = std::chrono::milliseconds{5000},
                    .wait_chunks = 1,
                    .linger = std::chrono::milliseconds{700},
                    .command = "tp 10 64 20",
                    .capture = true,
                })};
                expect(report.has_value());
                if (!report) {
                    return;
                }
                // The play login burst sends one position; the command adds a
                // second one with the resolved destination.
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
                    // Integer absolute X/Z name the block centre.
                    expect(last->x == 10.5) << last->x;
                    expect(last->y == 64.0) << last->y;
                    expect(last->z == 20.5) << last->z;
                }
            });
            std::error_code error{};
            fs::remove_all(fixture->directory, error);
        };

    "commands/me broadcasts the emote to the players in play"_test = [] {
        auto fixture{make_client_fixture("me")};
        expect(fixture.has_value());
        if (!fixture) {
            return;
        }
        auto level{open_empty_world(fixture->directory / "world")};
        expect(level.has_value());
        if (!level) {
            std::error_code error{};
            fs::remove_all(fixture->directory, error);
            return;
        }
        harness state{std::move(*level)};
        state.wire();
        run_server(*fixture, state, [](net::server &server) {
            auto report{net::run_bot(net::bot_options{
                .target = {.host = "127.0.0.1", .port = server.local_port()},
                .username = "Emoter",
                .timeout = std::chrono::milliseconds{5000},
                .wait_chunks = 1,
                .command = "me waves",
                .wait_messages = 1,
            })};
            expect(report.has_value());
            if (report) {
                expect(has_message(*report, "* Emoter waves"))
                    << (report->messages.empty() ? std::string{"no message"}
                                                 : report->messages.front());
            }
        });
        std::error_code error{};
        fs::remove_all(fixture->directory, error);
    };

    "commands/msg delivers the whisper to the target and the sender"_test = [] {
        auto fixture{make_client_fixture("msg")};
        expect(fixture.has_value());
        if (!fixture) {
            return;
        }
        auto level{open_empty_world(fixture->directory / "world")};
        expect(level.has_value());
        if (!level) {
            std::error_code error{};
            fs::remove_all(fixture->directory, error);
            return;
        }
        harness state{std::move(*level)};
        state.wire();
        run_server(*fixture, state, [](net::server &server) {
            auto report{net::run_bot(net::bot_options{
                .target = {.host = "127.0.0.1", .port = server.local_port()},
                .username = "Whisperer",
                .timeout = std::chrono::milliseconds{5000},
                .wait_chunks = 1,
                .command = "msg Whisperer hello there",
                .wait_messages = 2,
            })};
            expect(report.has_value());
            if (report) {
                expect(has_message(*report, "Whisperer whispers to you: hello "
                                            "there"))
                    << (report->messages.empty() ? std::string{"no message"}
                                                 : report->messages.front());
                expect(has_message(*report,
                                   "You whisper to Whisperer: hello there"));
            }
        });
        std::error_code error{};
        fs::remove_all(fixture->directory, error);
    };
};
