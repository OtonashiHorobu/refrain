// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Block breaking through the real tick loop: the runtime binds a listener on
// an ephemeral port, a bot joins, selects a hotbar slot and starts a break, and
// the runtime's own wiring (mining service tick, held-slot input, chunk update
// broadcast) has to break the block and publish the block update. This is the
// end-to-end complement to the service and listener tests.

#include <refrain/core/config.hpp>
#include <refrain/net/bot.hpp>
#include <refrain/net/primitives.hpp>
#include <refrain/net/wire.hpp>
#include <refrain/protocol/current.hpp>
#include <refrain/server/server_runtime.hpp>
#include <refrain/server/world_port.hpp>
#include <refrain/sim/world.hpp>
#include <refrain/world/create.hpp>
#include <refrain/world/model.hpp>
#include <refrain/world/store.hpp>

#include <boost/ut.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "net/test_protocol.hpp"
#include "support/scratch_directory.hpp"

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace config = refrain::core::config;
namespace net = refrain::net;
namespace play_sb = refrain::protocol::current::play::serverbound;
namespace play_cb = refrain::protocol::current::play::clientbound;
namespace server = refrain::server;
namespace sim = refrain::sim;
namespace storage = refrain::storage;
namespace world_ns = refrain::world;
namespace test_support = refrain::test_support;

/// One dummy block-update body as the bot captures it.
struct block_update_packet {
    sim::block_pos pos{};
    std::uint32_t state{0};
};

/// Handles that select the direct world port; the runtime never routes a call
/// through the vtables on that path, so non-null dummy pointers carry the
/// selection and the provider ids name the first-party implementations.
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

/// One dirt section at y=48..63, so a player spawned at the default y=64 stands
/// on the block the test mines.
[[nodiscard]] auto dirt_chunk() -> storage::chunk {
    storage::chunk chunk{};
    chunk.format_version = world_ns::chunk_format_version;
    chunk.status = storage::chunk_status::full;
    chunk.y_pos = 0;
    storage::section_record section{};
    section.y = 3;
    section.kind = storage::section_kind::single_value;
    section.bits_per_entry = 0;
    section.block_count = static_cast<std::uint16_t>(world_ns::section_volume);
    section.biome_kind = storage::section_kind::single_value;
    section.biome_bits_per_entry = 0;
    section.biome_palette.push_back("minecraft:plains");
    section.palette.push_back(storage::palette_entry{
        .key = "minecraft:dirt",
        .count = static_cast<std::uint16_t>(world_ns::section_volume),
        .state = 0,
    });
    chunk.sections.push_back(std::move(section));
    return chunk;
}

/// Create the world the runtime opens: a level record and the 5x5 columns of
/// the view distance, all carrying the dirt floor. Writing them up front keeps
/// the run free of generation work (and therefore short and deterministic).
[[nodiscard]] auto create_flat_world(const std::filesystem::path &root)
    -> bool {
    if (!world_ns::create_world(root.string(), 0, "world")) {
        return false;
    }
    auto store{world_ns::world_store::open(root.string())};
    if (!store) {
        return false;
    }
    const auto chunk{dirt_chunk()};
    for (std::int32_t x{-2}; x <= 2; ++x) {
        for (std::int32_t z{-2}; z <= 2; ++z) {
            if (!store->write_chunk({x, z}, chunk)) {
                return false;
            }
        }
    }
    return store->commit().has_value();
}

[[nodiscard]] auto carried_slot_body(std::int16_t slot)
    -> std::vector<std::byte> {
    std::vector<std::byte> body;
    net::writer out{body};
    out.write_varint(play_sb::set_carried_item::packet_id);
    play_sb::set_carried_item packet{.slot = slot};
    static_cast<void>(play_sb::encode_set_carried_item(out, packet));
    return body;
}

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

[[nodiscard]] auto block_updates(const net::bot_report &report)
    -> std::vector<block_update_packet> {
    std::vector<block_update_packet> updates{};
    for (const auto &captured : report.capture) {
        if (captured.state != 2U ||
            captured.id != play_cb::block_update::packet_id) {
            continue;
        }
        net::reader in{captured.body};
        if (auto id{in.read_varint()};
            !id || *id != play_cb::block_update::packet_id) {
            continue;
        }
        play_cb::block_update packet{};
        if (auto decoded{play_cb::decode_block_update(in, packet)}; !decoded) {
            continue;
        }
        const auto position{net::unpack_block_position(packet.position)};
        updates.push_back(block_update_packet{
            .pos = sim::block_pos{position.x,
                                  static_cast<std::int16_t>(position.y),
                                  position.z},
            .state = packet.state,
        });
    }
    return updates;
}

} // namespace

suite<"server/mining-runtime"> mining_runtime_tests = [] {
    "server/a break in the tick loop writes air and publishes the block "
    "update"_test = [] {
        test_support::scratch_directory directory{};
        const auto world_path{directory.path() / "world"};
        if (!create_flat_world(world_path)) {
            expect(false) << "failed to create the flat world";
            return;
        }
        {
            std::ofstream stream{directory.path() / "network.toml",
                                 std::ios::binary | std::ios::trunc};
            stream << "bind_host = \"127.0.0.1\"\nport = 0\n"
                      "online_mode = false\nview_distance = 2\n"
                      "simulation_distance = 2\n";
        }
        auto store{config::store::empty()};
        expect(static_cast<bool>(store.mount(
            {.name = "network",
             .path = (directory.path() / "network.toml").string()})));
        static_cast<void>(store.load_all());
        auto network_view{store.module("network")};
        expect(static_cast<bool>(network_view));
        if (!network_view) {
            return;
        }

        const std::string data_dir{std::string{REFRAIN_SOURCE_DIR} +
                                   "/content/vanilla/26.3"};
        direct_handles handles{};
        auto world{server::make_world_port(handles.handles)};
        expect(world != nullptr);
        if (!world) {
            return;
        }
        server::server_run_options options{};
        options.world_path = world_path.string();
        options.data_dir = data_dir;
        options.datapack_root = data_dir + "/data";
        options.tables_path = data_dir + "/tables.rvtb";
        options.protocol_registry_path = data_dir + "/protocol_registry.json";
        options.network = *network_view;
        options.protocol = refrain::test::make_test_protocol();
        options.ticks = 80;
        options.watchdog_timeout_ms = 0;
        options.autosave_interval_ticks = 0;
        std::mutex port_mutex{};
        std::condition_variable port_ready{};
        std::uint16_t port{0};
        std::atomic<int> exit_code{-1};
        std::atomic<std::int64_t> runtime_ticks{-1};
        options.on_tick = [&runtime_ticks](std::uint64_t tick) {
            runtime_ticks.store(static_cast<std::int64_t>(tick),
                                std::memory_order_release);
        };
        options.on_listening = [&port_mutex, &port_ready,
                                &port](std::uint16_t local_port) {
            {
                const std::lock_guard lock{port_mutex};
                port = local_port;
            }
            port_ready.notify_all();
        };
        std::thread runtime_thread{[&] {
            exit_code.store(
                server::run_server(options, handles.handles, *world),
                std::memory_order_release);
        }};
        {
            std::unique_lock lock{port_mutex};
            if (!port_ready.wait_for(lock, std::chrono::seconds{20},
                                     [&port] { return port != 0; })) {
                std::fprintf(stderr,
                             "mining-runtime: no port, exit %d ticks %lld\n",
                             exit_code.load(std::memory_order_acquire),
                             static_cast<long long>(runtime_ticks.load(
                                 std::memory_order_acquire)));
                expect(false) << "the runtime did not bind a port";
                runtime_thread.join();
                return;
            }
        }
        const net::bot_options bot{
            .target = {.host = "127.0.0.1", .port = port},
            .username = "Miner",
            .timeout = std::chrono::milliseconds{5000},
            .wait_chunks = 1,
            .linger = std::chrono::milliseconds{1500},
            .play_actions =
                {
                    net::bot_options::play_action{
                        .delay = std::chrono::milliseconds{400},
                        .body = carried_slot_body(2)},
                    net::bot_options::play_action{
                        .delay = std::chrono::milliseconds{100},
                        .body = player_action_body(0, 0, 63, 0, 1)},
                },
            .capture = true,
        };
        auto report{net::run_bot(bot)};
        runtime_thread.join();
        expect(exit_code.load(std::memory_order_acquire) == 0)
            << exit_code.load(std::memory_order_acquire);
        expect(report.has_value());
        if (!report) {
            return;
        }
        // The break finished on the server's clock: the world is air where the
        // dirt block was.
        auto *level{world->sim_world()};
        expect(level != nullptr);
        if (level == nullptr) {
            return;
        }
        expect(level->block_at(sim::block_pos{0, 63, 0}) == sim::air_block);
        // The client received the change and the acknowledgement of the action.
        const auto updates{block_updates(*report)};
        bool saw_air{false};
        for (const auto &update : updates) {
            if (update.state == 0 && update.pos.x == 0 && update.pos.y == 63 &&
                update.pos.z == 0) {
                saw_air = true;
            }
        }
        expect(saw_air) << updates.size();
        expect(count_packets(*report, play_cb::block_changed_ack::packet_id) >=
               1U);
        // The bare-handed break took the full destroy time: no tool was held
        // and no item packet was sent.
        expect(count_packets(*report, play_cb::block_destruction::packet_id) ==
               0U);
    };
};
