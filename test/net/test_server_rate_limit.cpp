// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Inbound rate-limit enforcement without a socket or a sleep. The codec is
// scripted so the server advances from handshake to play, and the injected
// clock makes the bucket refill on demand. The codec records session creation
// and release, so the test observes that a closed overflow reaches the same
// single removal step as every other local close.

#include <refrain/core/config.hpp>
#include <refrain/net/protocol_codec.hpp>
#include <refrain/net/server.hpp>
#include <refrain/net/transport.hpp>
#include <refrain/net/wire.hpp>
#include <refrain/protocol/current.hpp>

#include <boost/ut.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
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
namespace play_sb = refrain::protocol::current::play::serverbound;

/// Injected monotonic clock. The server reads it from the I/O thread and the
/// simulation thread, so every operation is atomic.
class fake_clock {
  public:
    [[nodiscard]] auto now() const -> std::int64_t {
        return nanoseconds_.load(std::memory_order_relaxed);
    }

    void advance(std::chrono::milliseconds duration) {
        nanoseconds_.fetch_add(
            std::chrono::duration_cast<std::chrono::nanoseconds>(duration)
                .count(),
            std::memory_order_relaxed);
    }

  private:
    std::atomic<std::int64_t> nanoseconds_{0};
};

/// Records the sessions the server creates and releases, and hands the server
/// the packets a client would send next. `close_connection` is the release the
/// provider is owed exactly once per connection.
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
    [[nodiscard]] auto send_packet(std::uint64_t, std::uint32_t,
                                   std::span<const std::byte>)
        -> refrain::status override {
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

    [[nodiscard]] auto released(std::uint64_t connection) -> int {
        const std::lock_guard lock{mutex_};
        const auto found{released_.find(connection)};
        return found == released_.end() ? 0 : found->second;
    }
    [[nodiscard]] auto live(std::uint64_t connection) -> bool {
        const std::lock_guard lock{mutex_};
        return live_.contains(connection);
    }

  private:
    std::mutex mutex_{};
    std::unordered_map<std::uint64_t, int> released_{};
    std::unordered_map<std::uint64_t, std::deque<packet>> queues_{};
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
        return 1;
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

struct fixture {
    fake_clock clock{};
    std::shared_ptr<transport_record> wire{
        std::make_shared<transport_record>()};
    std::shared_ptr<scripted_codec> codec{std::make_shared<scripted_codec>()};
    std::unique_ptr<net::server> running{};
    std::filesystem::path dir{};
    std::string error{};

    explicit fixture(std::optional<std::int64_t> rate_limit) {
        static std::atomic<std::uint64_t> sequence{0};
        dir =
            std::filesystem::temp_directory_path() /
            ("refrain-net-rate-" +
             std::to_string(static_cast<unsigned long long>(
                 std::chrono::steady_clock::now().time_since_epoch().count())) +
             "-" + std::to_string(sequence.fetch_add(1)));
        std::filesystem::create_directories(dir);
        {
            std::ofstream stream{dir / "network.toml",
                                 std::ios::binary | std::ios::trunc};
            stream << "bind_host = \"127.0.0.1\"\nport = 0\n"
                      "online_mode = false\n";
            if (rate_limit) {
                stream << "rate_limit = " << *rate_limit << "\n";
            }
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
        inputs.now_ns = [this] { return clock.now(); };
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

    /// One `on_data` call carrying `count` play packets, the way a single read
    /// can hold a burst.
    void feed_play(std::uint64_t connection, std::size_t count) {
        for (std::size_t index{0}; index < count; ++index) {
            codec->enqueue(connection, play_sb::client_tick_end::packet_id, {});
        }
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
        hello.name = "RateTester";
        hello.profile_high = 0;
        hello.profile_low = 0;
        feed(connection, login_sb::hello::packet_id,
             encode_payload(hello, login_sb::encode_hello));

        feed(connection, login_sb::login_acknowledged::packet_id, {});
        feed(connection, config_sb::finish_configuration::packet_id, {});
    }
};

suite<"net/rate_limit"> server_rate_limit_tests = [] {
    "net/server closes a play connection over the configured packet rate"_test =
        [] {
            fixture test{std::int64_t{1}};
            expect(static_cast<bool>(test.running));
            if (!test.running) {
                return;
            }
            constexpr std::uint64_t connection{1};
            test.open(connection);
            test.join_play(connection);

            // Handshake, login and configuration traffic is not play traffic,
            // so a one-packet-per-second limit leaves the session open.
            expect(test.codec->live(connection));
            expect(test.running->active_connections().size() == 1U);
            expect(test.wire->close_calls() == 0U);

            // The first play packet spends the one-second burst; the next has
            // no token and closes the connection.
            test.feed_play(connection, 1);
            expect(test.codec->live(connection));
            expect(test.wire->close_calls() == 0U);

            test.feed_play(connection, 1);
            expect(!test.codec->live(connection));
            expect(test.codec->released(connection) == 1);
            expect(test.running->active_connections().empty());
            expect(test.wire->close_calls() == 1U);

            // Re-delivering the transport report the socket would send
            // describes the same disconnect: the session is released once.
            test.drop(connection);
            expect(test.codec->released(connection) == 1);
            expect(test.wire->close_calls() == 1U);
        };

    "net/server refills the configured rate from the injected clock"_test = [] {
        fixture test{std::int64_t{2}};
        expect(static_cast<bool>(test.running));
        if (!test.running) {
            return;
        }
        constexpr std::uint64_t connection{2};
        test.open(connection);
        test.join_play(connection);

        // The burst is two packets; a seconds reading of the key would stop
        // after the first one.
        test.feed_play(connection, 2);
        expect(test.codec->live(connection));
        expect(test.wire->close_calls() == 0U);

        // Half a second refills one token at two packets per second, not two.
        test.clock.advance(std::chrono::milliseconds{500});
        test.feed_play(connection, 1);
        expect(test.codec->live(connection));
        expect(test.wire->close_calls() == 0U);

        test.feed_play(connection, 1);
        expect(!test.codec->live(connection));
        expect(test.codec->released(connection) == 1);
        expect(test.wire->close_calls() == 1U);
    };

    "net/server disables the rate limit when the key is zero"_test = [] {
        fixture test{std::int64_t{0}};
        expect(static_cast<bool>(test.running));
        if (!test.running) {
            return;
        }
        constexpr std::uint64_t connection{3};
        test.open(connection);
        test.join_play(connection);

        test.feed_play(connection, 1000);
        expect(test.codec->live(connection));
        expect(test.running->active_connections().size() == 1U);
        expect(test.wire->close_calls() == 0U);
    };

    "net/server disables the rate limit when the key is absent"_test = [] {
        fixture test{std::nullopt};
        expect(static_cast<bool>(test.running));
        if (!test.running) {
            return;
        }
        constexpr std::uint64_t connection{4};
        test.open(connection);
        test.join_play(connection);

        test.feed_play(connection, 1000);
        expect(test.codec->live(connection));
        expect(test.running->active_connections().size() == 1U);
        expect(test.wire->close_calls() == 0U);
    };

    "net/server rejects a negative or oversized rate limit"_test = [] {
        fixture negative{std::int64_t{-1}};
        expect(!negative.running);
        expect(negative.error.find("rate_limit") != std::string::npos);

        fixture oversized{static_cast<std::int64_t>(
                              std::numeric_limits<std::int32_t>::max()) +
                          1};
        expect(!oversized.running);
        expect(oversized.error.find("rate_limit") != std::string::npos);
    };
};

} // namespace
