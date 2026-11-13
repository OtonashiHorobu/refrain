// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Connection lifecycle is reported from tick(), not from the transport
// callback. The I/O side only queues the event, so a lifecycle observer that
// reads simulation-owned state never runs on a transport thread. The codec is
// scripted so the test advances a connection into play without a socket.

#include <refrain/core/config.hpp>
#include <refrain/net/protocol_codec.hpp>
#include <refrain/net/server.hpp>
#include <refrain/net/transport.hpp>
#include <refrain/net/wire.hpp>
#include <refrain/protocol/current.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace net = refrain::net;
namespace config_sb = refrain::protocol::current::configuration::serverbound;
namespace handshake_sb = refrain::protocol::current::handshake::serverbound;
namespace login_sb = refrain::protocol::current::login::serverbound;

/// Hands the server the packets a client would send next and records the
/// sessions it creates and releases.
class scripted_codec final : public net::protocol_codec {
  public:
    using packet = std::pair<std::uint32_t, std::vector<std::byte>>;

    void create_connection(std::uint64_t connection) override {
        const std::lock_guard lock{mutex_};
        ++created_[connection];
        live_.insert(connection);
    }
    void close_connection(std::uint64_t connection) override {
        const std::lock_guard lock{mutex_};
        ++released_[connection];
        live_.erase(connection);
        queues_.erase(connection);
        current_.erase(connection);
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
        // The payload must stay valid until the following call on this
        // connection, so keep it in the codec's own storage.
        auto &buffer{current_[connection]};
        buffer = std::move(next.second);
        found->second.pop_front();
        payload = buffer;
        return true;
    }
    [[nodiscard]] auto send_packet(std::uint64_t connection, std::uint32_t,
                                   std::span<const std::byte>)
        -> refrain::status override {
        const std::lock_guard lock{mutex_};
        ++sent_[connection];
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

  private:
    std::mutex mutex_{};
    std::unordered_map<std::uint64_t, int> created_{};
    std::unordered_map<std::uint64_t, int> released_{};
    std::unordered_map<std::uint64_t, int> sent_{};
    std::unordered_map<std::uint64_t, std::deque<packet>> queues_{};
    std::unordered_map<std::uint64_t, std::vector<std::byte>> current_{};
    std::set<std::uint64_t> live_{};
};

struct transport_record {
    net::transport_handler handler{};
};

/// Transport that reports a close the way a socket does.
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
        return 1;
    }
    void send(std::uint64_t, std::span<const std::byte>) override {}
    void close(std::uint64_t connection) override {
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

struct fixture {
    std::shared_ptr<transport_record> wire{
        std::make_shared<transport_record>()};
    std::shared_ptr<scripted_codec> codec{std::make_shared<scripted_codec>()};
    std::unique_ptr<net::server> running{};
    std::filesystem::path dir{};
    std::vector<std::string> events{};
    std::string error{};

    fixture() {
        dir =
            std::filesystem::temp_directory_path() /
            ("refrain-net-lifecycle-" +
             std::to_string(static_cast<unsigned long long>(
                 std::chrono::steady_clock::now().time_since_epoch().count())));
        std::filesystem::create_directories(dir);
        {
            std::ofstream stream{dir / "network.toml",
                                 std::ios::binary | std::ios::trunc};
            stream << "bind_host = \"127.0.0.1\"\nport = 0\n"
                      "online_mode = false\n";
        }
        auto store{refrain::core::config::store::empty()};
        static_cast<void>(store.mount(
            {.name = "network", .path = (dir / "network.toml").string()}));
        static_cast<void>(store.load_all());
        auto view{store.module("network")};
        if (!view) {
            error = "network.toml did not load";
            return;
        }
        net::server_inputs inputs{};
        inputs.protocol = codec;
        inputs.player_join = [this](const net::player_identity &identity) {
            expect(identity.name == "LifecycleTester");
            events.push_back("join");
        };
        inputs.play_ready = [this](const net::player_identity &) {
            events.push_back("ready");
        };
        inputs.player_leave = [this](const net::player_identity &identity) {
            expect(identity.name == "LifecycleTester");
            events.push_back("leave");
        };
        const auto wire_copy{wire};
        inputs.make_transport = [wire_copy](net::transport_handler handler,
                                            std::size_t) {
            wire_copy->handler = std::move(handler);
            return refrain::result<std::unique_ptr<net::transport>>{
                std::make_unique<recording_transport>(wire_copy)};
        };
        auto started{net::server::start(std::move(inputs), *view)};
        if (!started) {
            error = started.error().message;
            return;
        }
        running = std::move(*started);
    }

    ~fixture() {
        if (running) {
            running->stop();
        }
        std::error_code error_code;
        std::filesystem::remove_all(dir, error_code);
    }

    fixture(const fixture &) = delete;
    auto operator=(const fixture &) -> fixture & = delete;

    void open(std::uint64_t connection) { wire->handler.on_open(connection); }

    /// Abrupt drop: the transport reports the close, no disconnect packet.
    void drop(std::uint64_t connection) {
        wire->handler.on_close(connection, "connection reset by peer");
    }

    /// Hand the server the next scripted packet and drain it through the
    /// transport callback, the way `on_data` receives bytes.
    void feed(std::uint64_t connection, std::uint32_t packet_id,
              std::vector<std::byte> payload) {
        codec->enqueue(connection, packet_id, std::move(payload));
        wire->handler.on_data(connection, {});
    }

    /// Drive one connection from handshake to play with the packets a client
    /// sends on the offline path.
    void join_play(std::uint64_t connection) {
        handshake_sb::intention intention{};
        intention.protocol = 777;
        intention.host = "localhost";
        intention.port = 25565;
        intention.next = 2;
        feed(connection, handshake_sb::intention::packet_id,
             encode_payload(intention, handshake_sb::encode_intention));

        login_sb::hello hello{};
        hello.name = "LifecycleTester";
        hello.profile_high = 0;
        hello.profile_low = 0;
        feed(connection, login_sb::hello::packet_id,
             encode_payload(hello, login_sb::encode_hello));

        feed(connection, login_sb::login_acknowledged::packet_id, {});
        feed(connection, config_sb::finish_configuration::packet_id, {});
    }
};

suite<"net/lifecycle"> server_lifecycle_tests = [] {
    "net/server reports join and leave from tick, not the transport callback"_test =
        [] {
            fixture test{};
            expect(static_cast<bool>(test.running));
            if (!test.running) {
                return;
            }
            constexpr std::uint64_t connection{1};
            test.open(connection);
            test.join_play(connection);
            expect(test.codec->live(connection));
            // The transport callback only queued the event: an observer that
            // touches simulation state has not run yet.
            expect(test.events.empty());

            test.running->tick();
            expect(test.events == std::vector<std::string>{"join", "ready"});

            test.drop(connection);
            expect(test.events.size() == 2U);
            test.running->tick();
            expect(test.events ==
                   std::vector<std::string>{"join", "ready", "leave"});
        };

    "net/server orders a close that races the play transition after the join"_test =
        [] {
            fixture test{};
            expect(static_cast<bool>(test.running));
            if (!test.running) {
                return;
            }
            constexpr std::uint64_t connection{2};
            test.open(connection);
            // The drop is observed before the tick that would deliver the
            // login burst's lifecycle events, so both the join and the leave
            // are already queued when the simulation thread drains them.
            test.join_play(connection);
            test.drop(connection);
            expect(test.events.empty());

            test.running->tick();
            expect(test.events ==
                   std::vector<std::string>{"join", "ready", "leave"});
        };

    "net/server reports no leave for a connection closed before play"_test =
        [] {
            fixture test{};
            expect(static_cast<bool>(test.running));
            if (!test.running) {
                return;
            }
            constexpr std::uint64_t connection{3};
            test.open(connection);
            test.drop(connection);
            test.running->tick();
            expect(test.events.empty());
        };

    "net/server applies a bounded number of joins per tick"_test = [] {
        fixture test{};
        expect(static_cast<bool>(test.running));
        if (!test.running) {
            return;
        }
        // More joins than one tick may apply, so the bound has to carry the
        // remainder instead of dropping it.
        constexpr std::uint64_t first_connection{10};
        constexpr std::uint64_t total{2 * net::server::max_joins_per_tick + 7};
        for (std::uint64_t connection{first_connection};
             connection < first_connection + total; ++connection) {
            test.open(connection);
            test.join_play(connection);
        }
        expect(test.events.empty());

        const auto joins{[&test] {
            return static_cast<std::size_t>(
                std::count(test.events.begin(), test.events.end(), "join"));
        }};

        test.running->tick();
        expect(joins() == net::server::max_joins_per_tick);
        expect(test.events.size() == 2U * net::server::max_joins_per_tick);

        test.running->tick();
        expect(joins() == 2U * net::server::max_joins_per_tick);

        test.running->tick();
        expect(joins() == total);
        expect(test.events.size() == 2U * total);
        // The queue keeps its order across the ticks that carry it.
        for (std::size_t index{0}; index < test.events.size(); index += 2) {
            expect(test.events[index] == "join");
            expect(test.events[index + 1] == "ready");
        }
    };

    "net/server delivers the events queued before shutdown from stop"_test =
        [] {
            fixture test{};
            expect(static_cast<bool>(test.running));
            if (!test.running) {
                return;
            }
            constexpr std::uint64_t connection{4};
            test.open(connection);
            test.join_play(connection);
            test.drop(connection);
            expect(test.events.empty());

            test.running->stop();
            // The transport has stopped, so stop() delivers what no tick will.
            expect(test.events ==
                   std::vector<std::string>{"join", "ready", "leave"});
        };
};

} // namespace
