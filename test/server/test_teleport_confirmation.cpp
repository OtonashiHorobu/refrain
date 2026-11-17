// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// End-to-end coverage for the teleport confirmation: a client reports the
// position it adopted after the server started the teleport handshake. The
// network boundary refuses a confirmation the server cannot represent before
// the runtime writes it into the authoritative player record, and the refused
// connection is disconnected; a normal confirmation is adopted as before.

#include <refrain/core/config.hpp>
#include <refrain/net/protocol_codec.hpp>
#include <refrain/net/server.hpp>
#include <refrain/net/transport.hpp>
#include <refrain/net/wire.hpp>
#include <refrain/protocol/current.hpp>
#include <refrain/server/server_runtime.hpp>
#include <refrain/server/world_port.hpp>
#include <refrain/sim/player.hpp>
#include <refrain/sim/world.hpp>
#include <refrain/world/create.hpp>
#include <refrain/world/store.hpp>

#include <boost/ut.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace config = refrain::core::config;
namespace net = refrain::net;
namespace config_sb = refrain::protocol::current::configuration::serverbound;
namespace handshake_sb = refrain::protocol::current::handshake::serverbound;
namespace login_sb = refrain::protocol::current::login::serverbound;
namespace play_cb = refrain::protocol::current::play::clientbound;
namespace play_sb = refrain::protocol::current::play::serverbound;
namespace server = refrain::server;
namespace sim = refrain::sim;
namespace storage = refrain::storage;
namespace world_ns = refrain::world;

/// Scratch directory removed when the test leaves, including on a failed
/// expectation.
struct scratch_directory {
    std::filesystem::path path{};

    scratch_directory() {
        const auto stamp{
            std::chrono::steady_clock::now().time_since_epoch().count()};
        path = std::filesystem::temp_directory_path() /
               ("refrain-teleport-confirm-" + std::to_string(stamp));
        std::error_code error{};
        std::filesystem::create_directories(path, error);
    }

    scratch_directory(const scratch_directory &) = delete;
    auto operator=(const scratch_directory &) -> scratch_directory & = delete;

    ~scratch_directory() {
        std::error_code error{};
        std::filesystem::remove_all(path, error);
    }
};

/// Stone from y=0 to y=15 in the single spawn column. The spawn position is
/// y=64, so a joining player starts in the air above the modeled floor.
[[nodiscard]] auto flat_chunk() -> storage::chunk {
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
    return chunk;
}

/// Create the level record and one committed column so the runtime boots
/// without generating terrain.
[[nodiscard]] auto create_flat_world(const std::filesystem::path &root)
    -> bool {
    if (!world_ns::create_world(root.string(), 0, "world")) {
        return false;
    }
    auto store{world_ns::world_store::open(root.string())};
    if (!store) {
        return false;
    }
    return static_cast<bool>(store->write_chunk({0, 0}, flat_chunk())) &&
           static_cast<bool>(store->commit());
}

/// Handles that select the first-party world, simulation and storage
/// implementations. The runtime never routes a call through the vtables on
/// that path, so a dummy pointer carries the selection.
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

/// Test double for the protocol provider: the test queues the packets a client
/// would send and records the packets the server sends, without framing or
/// sockets.
class scripted_codec final : public net::protocol_codec {
  public:
    using packet = std::pair<std::uint32_t, std::vector<std::byte>>;

    void create_connection(std::uint64_t connection) override {
        const std::lock_guard lock{mutex_};
        live_.insert(connection);
    }
    void close_connection(std::uint64_t connection) override {
        const std::lock_guard lock{mutex_};
        ++released_[connection];
        live_.erase(connection);
        queues_.erase(connection);
    }
    [[nodiscard]] auto set_compression(std::uint64_t, std::int32_t)
        -> refrain::status override {
        return refrain::status_ok();
    }
    [[nodiscard]] auto set_cipher(std::uint64_t, std::span<const std::byte>,
                                  std::span<const std::byte>)
        -> refrain::status override {
        return refrain::status_ok();
    }
    [[nodiscard]] auto feed(std::uint64_t, std::span<const std::byte>)
        -> refrain::status override {
        return refrain::status_ok();
    }
    [[nodiscard]] auto next_packet(std::uint64_t connection,
                                   std::uint32_t &packet_id,
                                   std::span<const std::byte> &payload)
        -> refrain::result<bool> override {
        const std::lock_guard lock{mutex_};
        const auto found{queues_.find(connection)};
        if (found == queues_.end() || found->second.empty()) {
            return false;
        }
        auto &next{found->second.front()};
        packet_id = next.first;
        auto &buffer{current_[connection]};
        buffer = std::move(next.second);
        found->second.pop_front();
        payload = buffer;
        return true;
    }
    [[nodiscard]] auto send_packet(std::uint64_t connection,
                                   std::uint32_t packet_id,
                                   std::span<const std::byte> payload)
        -> refrain::status override {
        const std::lock_guard lock{mutex_};
        sent_[connection].emplace_back(
            packet_id, std::vector<std::byte>{payload.begin(), payload.end()});
        return refrain::status_ok();
    }
    [[nodiscard]] auto take_outbound(std::uint64_t,
                                     std::span<const std::byte> &)
        -> bool override {
        return false;
    }
    void notify_state(std::uint64_t, net::connection_state) override {}
    [[nodiscard]] auto transition(std::uint64_t, net::protocol_event event)
        -> net::connection_state override {
        switch (event) {
        case net::protocol_event::status_request:
            return net::connection_state::status;
        case net::protocol_event::login_request:
            return net::connection_state::login;
        case net::protocol_event::login_acknowledged:
            return net::connection_state::configuration;
        case net::protocol_event::configuration_acknowledged:
            return net::connection_state::play;
        }
        return net::connection_state::handshake;
    }

    void enqueue(std::uint64_t connection, std::uint32_t packet_id,
                 std::vector<std::byte> payload) {
        const std::lock_guard lock{mutex_};
        queues_[connection].emplace_back(packet_id, std::move(payload));
    }

    [[nodiscard]] auto live(std::uint64_t connection) -> bool {
        const std::lock_guard lock{mutex_};
        return live_.contains(connection);
    }

    [[nodiscard]] auto has_sent(std::uint64_t connection,
                                std::uint32_t packet_id) -> bool {
        const std::lock_guard lock{mutex_};
        const auto found{sent_.find(connection)};
        if (found == sent_.end()) {
            return false;
        }
        for (const auto &entry : found->second) {
            if (entry.first == packet_id) {
                return true;
            }
        }
        return false;
    }

  private:
    std::mutex mutex_{};
    std::unordered_map<std::uint64_t, int> released_{};
    std::unordered_map<std::uint64_t, std::deque<packet>> queues_{};
    std::unordered_map<std::uint64_t, std::vector<packet>> sent_{};
    std::unordered_map<std::uint64_t, std::vector<std::byte>> current_{};
    std::set<std::uint64_t> live_{};
};

struct transport_record {
    std::mutex mutex{};
    std::size_t closes{0};
    net::transport_handler handler{};

    [[nodiscard]] auto close_calls() -> std::size_t {
        const std::lock_guard lock{mutex};
        return closes;
    }
};

/// Transport with the real close contract: `close` reports `on_close` once for
/// the connection, the way a socket does.
class recording_transport final : public net::transport {
  public:
    explicit recording_transport(std::shared_ptr<transport_record> record)
        : record_{std::move(record)} {}

    [[nodiscard]] auto listen(const net::endpoint &)
        -> refrain::status override {
        return refrain::status_ok();
    }
    [[nodiscard]] auto connect(const net::endpoint &)
        -> refrain::result<std::uint64_t> override {
        return std::uint64_t{1};
    }
    void send(std::uint64_t, std::span<const std::byte>) override {}
    void close(std::uint64_t connection) override {
        {
            const std::lock_guard lock{record_->mutex};
            ++record_->closes;
        }
        if (record_->handler.on_close) {
            record_->handler.on_close(connection, "closed locally");
        }
    }
    void stop() noexcept override {}
    [[nodiscard]] auto connection_count() const -> std::size_t override {
        return 0;
    }
    [[nodiscard]] auto local_port() const -> std::uint16_t override {
        return 25565;
    }
    [[nodiscard]] auto descriptor() const
        -> net::transport_descriptor override {
        return net::transport_descriptor{.provider = "recording"};
    }

  private:
    std::shared_ptr<transport_record> record_;
};

template <typename Packet, typename Encoder>
[[nodiscard]] auto encode_payload(const Packet &packet, Encoder encoder)
    -> std::vector<std::byte> {
    std::vector<std::byte> payload;
    net::writer out{payload};
    static_cast<void>(encoder(out, packet));
    return payload;
}

struct client_wire {
    std::shared_ptr<scripted_codec> codec{};
    std::shared_ptr<transport_record> record{};
};

void feed(const client_wire &wire, std::uint64_t connection,
          std::uint32_t packet_id, std::vector<std::byte> payload) {
    wire.codec->enqueue(connection, packet_id, std::move(payload));
    wire.record->handler.on_data(connection, {});
}

/// The four packets a client sends to enter play on an offline-mode server.
void join_play(const client_wire &wire, std::uint64_t connection,
               std::string_view name) {
    handshake_sb::intention intention{};
    intention.protocol = 777;
    intention.host = "localhost";
    intention.port = 25565;
    intention.next = 2;
    feed(wire, connection, handshake_sb::intention::packet_id,
         encode_payload(intention, handshake_sb::encode_intention));

    login_sb::hello hello{};
    hello.name = std::string{name};
    feed(wire, connection, login_sb::hello::packet_id,
         encode_payload(hello, login_sb::encode_hello));

    feed(wire, connection, login_sb::login_acknowledged::packet_id, {});
    feed(wire, connection, config_sb::finish_configuration::packet_id, {});
}

void confirm_teleport(const client_wire &wire, std::uint64_t connection,
                      double x, double y, double z, float yaw, float pitch) {
    play_sb::accept_teleportation ack{};
    ack.id = 0;
    ack.x = x;
    ack.y = y;
    ack.z = z;
    ack.y_rot = yaw;
    ack.x_rot = pitch;
    feed(wire, connection, play_sb::accept_teleportation::packet_id,
         encode_payload(ack, play_sb::encode_accept_teleportation));
}

} // namespace

suite<"server/teleport-confirmation"> teleport_confirmation_tests = [] {
    "server/a teleport confirmation the server cannot represent is refused and the record keeps the server position"_test =
        [] {
            const scratch_directory scratch{};
            const auto world_root{scratch.path / "world"};
            expect(create_flat_world(world_root));
            if (!std::filesystem::exists(world_root)) {
                return;
            }
            {
                std::ofstream stream{scratch.path / "network.toml",
                                     std::ios::binary | std::ios::trunc};
                stream << "bind_host = \"127.0.0.1\"\nport = 0\n"
                          "online_mode = false\nview_distance = 3\n"
                          "simulation_distance = 3\n";
            }
            auto store{config::store::empty()};
            static_cast<void>(store.mount(
                {.name = "network",
                 .path = (scratch.path / "network.toml").string()}));
            static_cast<void>(store.load_all());
            auto view{store.module("network")};
            expect(view.has_value());
            if (!view) {
                return;
            }
            direct_handles handles{};
            auto port{server::make_world_port(handles.handles)};
            expect(port != nullptr);
            if (port == nullptr) {
                return;
            }
            auto wire{
                client_wire{.codec = std::make_shared<scripted_codec>(),
                            .record = std::make_shared<transport_record>()}};
            const auto wire_copy{wire.record};
            const auto codec{wire.codec};
            // The runtime writes the authoritative record on its tick thread;
            // the test only reads the last copy after the run returned.
            std::map<std::uint64_t, sim::player_state> last_record{};
            const auto capture = [&port,
                                  &last_record](std::uint64_t connection) {
                auto *level{port->sim_world()};
                if (level == nullptr) {
                    return;
                }
                const auto *record{
                    level->players().find_by_connection(connection)};
                if (record != nullptr) {
                    last_record.insert_or_assign(connection, *record);
                }
            };
            // The runtime resolves its data paths from the process working
            // directory by default; the test names the checkout snapshot so it
            // behaves the same from the build directory and from the source
            // root. The flat world already holds its column, so the datapack
            // under `data/` is not needed for generation.
            const std::string data_root{std::string{REFRAIN_SOURCE_DIR} +
                                        "/content/vanilla/26.3"};
            server::server_run_options options{};
            options.world_path = world_root.string();
            options.data_dir = data_root;
            options.tables_path = data_root + "/tables.rvtb";
            options.protocol_registry_path =
                data_root + "/protocol_registry.json";
            options.ticks = 20;
            options.network = *view;
            options.protocol = wire.codec;
            options.make_transport = [wire_copy](net::transport_handler handler,
                                                 std::size_t) {
                wire_copy->handler = std::move(handler);
                return refrain::result<std::unique_ptr<net::transport>>{
                    std::make_unique<recording_transport>(wire_copy)};
            };
            std::atomic<bool> listening{false};
            std::atomic<bool> finished{false};
            options.on_listening = [&listening](std::uint16_t) {
                listening.store(true, std::memory_order_release);
            };
            options.on_tick = [&capture](std::uint64_t) {
                capture(1);
                capture(2);
                capture(3);
                capture(4);
            };
            int exit_code{1};
            std::thread runtime{[&] {
                exit_code = server::run_server(options, handles.handles, *port);
                finished.store(true, std::memory_order_release);
            }};
            const auto deadline{std::chrono::steady_clock::now() +
                                std::chrono::seconds{30}};
            while (!listening.load(std::memory_order_acquire) &&
                   !finished.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds{2});
            }
            expect(listening.load(std::memory_order_acquire));
            if (listening.load(std::memory_order_acquire)) {
                constexpr std::uint64_t non_finite{1};
                constexpr std::uint64_t infinite{2};
                constexpr std::uint64_t out_of_range{3};
                constexpr std::uint64_t valid{4};
                for (const auto connection : std::array<std::uint64_t, 4>{
                         non_finite, infinite, out_of_range, valid}) {
                    wire.record->handler.on_open(connection);
                }
                join_play(wire, non_finite, "NonFinite");
                join_play(wire, infinite, "Infinite");
                join_play(wire, out_of_range, "OutOfRange");
                join_play(wire, valid, "Valid");
                const auto nan{std::numeric_limits<double>::quiet_NaN()};
                const auto inf{std::numeric_limits<double>::infinity()};
                confirm_teleport(wire, non_finite, nan, 64.0, 0.5, 0.0F, 0.0F);
                confirm_teleport(wire, infinite, 0.5, inf, 0.5, 0.0F, 0.0F);
                confirm_teleport(wire, out_of_range, 1.0e300, 64.0, 0.5, 0.0F,
                                 0.0F);
                // The server started the handshake at this position, so the
                // normal confirmation has to be adopted, including the view
                // angle the client reports with it.
                confirm_teleport(wire, valid, 0.5, 64.0, 0.5, 90.0F, 0.0F);
            }
            runtime.join();
            expect(exit_code == 0) << exit_code;
            // The two non-finite confirmations are refused at the network
            // boundary with a disconnect before the runtime can write them.
            for (const auto connection : std::array<std::uint64_t, 2>{
                     std::uint64_t{1}, std::uint64_t{2}}) {
                expect(
                    codec->has_sent(connection, play_cb::disconnect::packet_id))
                    << "connection " << connection;
                const auto found{last_record.find(connection)};
                expect(found != last_record.end()) << connection;
                if (found == last_record.end()) {
                    continue;
                }
                const auto &record{found->second};
                expect(std::isfinite(record.x));
                expect(std::isfinite(record.y));
                expect(std::isfinite(record.z));
                expect(std::isfinite(record.yaw));
                expect(std::isfinite(record.pitch));
                expect(std::fabs(record.x - 0.5) < 1.0e-9) << record.x;
                expect(std::fabs(record.z - 0.5) < 1.0e-9) << record.z;
                // The record keeps the position the server assigned; it has
                // only fallen for the ticks between the join and the leave.
                expect(std::fabs(record.y - 64.0) < 1.0) << record.y;
                expect(record.yaw == 0.0F);
                expect(record.pitch == 0.0F);
            }
            // A finite position outside the addressable world is refused by
            // the gameplay consumer without touching the record; the network
            // layer only rejects the non-finite report, which is the report
            // the reference disconnects for.
            expect(!codec->has_sent(std::uint64_t{3},
                                    play_cb::disconnect::packet_id));
            const auto out_of_range_record{last_record.find(std::uint64_t{3})};
            expect(out_of_range_record != last_record.end());
            if (out_of_range_record != last_record.end()) {
                const auto &record{out_of_range_record->second};
                expect(std::isfinite(record.x));
                expect(std::isfinite(record.y));
                expect(std::isfinite(record.z));
                expect(std::fabs(record.x - 0.5) < 1.0e-9) << record.x;
                expect(std::fabs(record.z - 0.5) < 1.0e-9) << record.z;
            }
            // The normal confirmation is accepted and its view angle reaches
            // the record.
            expect(!codec->has_sent(std::uint64_t{4},
                                    play_cb::disconnect::packet_id));
            const auto valid_record{last_record.find(std::uint64_t{4})};
            expect(valid_record != last_record.end());
            if (valid_record != last_record.end()) {
                expect(std::isfinite(valid_record->second.x));
                expect(std::fabs(valid_record->second.x - 0.5) < 1.0e-9);
                expect(std::fabs(valid_record->second.z - 0.5) < 1.0e-9);
                expect(std::fabs(valid_record->second.yaw - 90.0F) < 1.0e-9)
                    << valid_record->second.yaw;
            }
        };
};
