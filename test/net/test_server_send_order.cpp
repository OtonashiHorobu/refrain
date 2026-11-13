// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Per-connection send ordering and close/send interleaving. The transport and
// the codec are injected so the test observes the bytes that reach the wire
// without a socket.

#include <refrain/core/config.hpp>
#include <refrain/net/protocol_codec.hpp>
#include <refrain/net/server.hpp>
#include <refrain/net/transport.hpp>

#include <boost/ut.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace net = refrain::net;

/// Payload-only codec: the frame handed to the transport is exactly the
/// payload the caller sent, so the recorder sees that payload unchanged.
class passthrough_codec final : public net::protocol_codec {
  public:
    void create_connection(std::uint64_t connection) override {
        const std::lock_guard lock{mutex_};
        channels_[connection] = channel{};
    }
    void close_connection(std::uint64_t connection) override {
        const std::lock_guard lock{mutex_};
        channels_.erase(connection);
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
    [[nodiscard]] auto feed(std::uint64_t connection,
                            std::span<const std::byte>)
        -> refrain::status override {
        const std::lock_guard lock{mutex_};
        if (poisoned_.contains(connection)) {
            return std::unexpected{refrain::make_error(
                refrain::error_code::parse_error, "poisoned frame")};
        }
        return refrain::status_ok();
    }
    [[nodiscard]] auto next_packet(std::uint64_t, std::uint32_t &,
                                   std::span<const std::byte> &)
        -> refrain::result<bool> override {
        return false;
    }
    [[nodiscard]] auto send_packet(std::uint64_t connection, std::uint32_t,
                                   std::span<const std::byte> payload)
        -> refrain::status override {
        const std::lock_guard lock{mutex_};
        const auto found{channels_.find(connection)};
        if (found == channels_.end()) {
            return std::unexpected{refrain::make_error(
                refrain::error_code::not_found, "unknown codec connection")};
        }
        found->second.frames.emplace_back(payload.begin(), payload.end());
        return refrain::status_ok();
    }
    [[nodiscard]] auto take_outbound(std::uint64_t connection,
                                     std::span<const std::byte> &data)
        -> bool override {
        const std::lock_guard lock{mutex_};
        const auto found{channels_.find(connection)};
        if (found == channels_.end() || found->second.frames.empty()) {
            return false;
        }
        auto &current{current_[connection]};
        current = std::move(found->second.frames.front());
        found->second.frames.pop_front();
        data = current;
        return true;
    }
    void notify_state(std::uint64_t, net::connection_state) override {}
    [[nodiscard]] auto transition(std::uint64_t connection, net::protocol_event)
        -> net::connection_state override {
        const std::lock_guard lock{mutex_};
        return states_[connection];
    }

    void poison(std::uint64_t connection) {
        const std::lock_guard lock{mutex_};
        poisoned_.insert(connection);
    }

  private:
    struct channel {
        std::deque<std::vector<std::byte>> frames{};
    };
    std::mutex mutex_{};
    std::unordered_map<std::uint64_t, channel> channels_{};
    std::unordered_map<std::uint64_t, std::vector<std::byte>> current_{};
    std::unordered_map<std::uint64_t, net::connection_state> states_{};
    std::set<std::uint64_t> poisoned_{};
};

struct recording {
    std::mutex mutex{};
    std::unordered_map<std::uint64_t, std::vector<std::vector<std::byte>>>
        frames{};
    net::transport_handler handler{};
};

class recording_transport final : public net::transport {
  public:
    explicit recording_transport(std::shared_ptr<recording> sink)
        : sink_{std::move(sink)} {}

    [[nodiscard]] auto listen(const net::endpoint &)
        -> refrain::status override {
        return refrain::status_ok();
    }
    [[nodiscard]] auto connect(const net::endpoint &)
        -> refrain::result<std::uint64_t> override {
        return 1;
    }
    void send(std::uint64_t connection,
              std::span<const std::byte> bytes) override {
        const std::lock_guard lock{sink_->mutex};
        sink_->frames[connection].emplace_back(bytes.begin(), bytes.end());
    }
    void close(std::uint64_t) override {}
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
    std::shared_ptr<recording> sink_;
};

[[nodiscard]] auto encode_sequence(std::uint64_t value)
    -> std::vector<std::byte> {
    std::vector<std::byte> body(9);
    body[0] = std::byte{1}; // packet id
    for (std::size_t index{0}; index < 8; ++index) {
        body[index + 1] =
            static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    }
    return body;
}

[[nodiscard]] auto decode_sequence(const std::vector<std::byte> &frame)
    -> std::uint64_t {
    std::uint64_t value{0};
    for (std::size_t index{0}; index < 8 && index < frame.size(); ++index) {
        value |= static_cast<std::uint64_t>(
                     std::to_integer<std::uint8_t>(frame[index]))
                 << (index * 8U);
    }
    return value;
}

/// Every frame in one connection's stream must be the next sequence value the
/// producer emitted; a dropped tail is allowed after a close.
[[nodiscard]] auto
strictly_ordered(const std::vector<std::vector<std::byte>> &frames) -> bool {
    for (std::size_t index{0}; index < frames.size(); ++index) {
        if (decode_sequence(frames[index]) != index) {
            return false;
        }
    }
    return true;
}

struct fixture {
    std::shared_ptr<recording> sink{std::make_shared<recording>()};
    std::shared_ptr<passthrough_codec> codec{
        std::make_shared<passthrough_codec>()};
    std::unique_ptr<net::server> running{};
    std::filesystem::path dir{};

    fixture() {
        dir =
            std::filesystem::temp_directory_path() /
            ("refrain-net-send-order-" +
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
            return;
        }
        net::server_inputs inputs{};
        inputs.protocol = codec;
        const auto sink_copy{sink};
        inputs.make_transport = [sink_copy](net::transport_handler handler,
                                            std::size_t) {
            sink_copy->handler = std::move(handler);
            return refrain::result<std::unique_ptr<net::transport>>{
                std::make_unique<recording_transport>(sink_copy)};
        };
        auto started{net::server::start(std::move(inputs), *view)};
        if (started) {
            running = std::move(*started);
        }
    }

    ~fixture() {
        if (running) {
            running->stop();
        }
        std::error_code error;
        std::filesystem::remove_all(dir, error);
    }

    fixture(const fixture &) = delete;
    auto operator=(const fixture &) -> fixture & = delete;

    void open(std::uint64_t connection) { sink->handler.on_open(connection); }

    [[nodiscard]] auto frames(std::uint64_t connection)
        -> std::vector<std::vector<std::byte>> {
        const std::lock_guard lock{sink->mutex};
        const auto found{sink->frames.find(connection)};
        if (found == sink->frames.end()) {
            return {};
        }
        return found->second;
    }
};

suite<"net/send-order"> server_send_order_tests = [] {
    "net/send preserves per-connection order across producers"_test = [] {
        fixture test{};
        expect(static_cast<bool>(test.running));
        if (!test.running) {
            return;
        }
        constexpr std::size_t connections{8};
        constexpr std::size_t per_connection{500};
        for (std::size_t index{0}; index < connections; ++index) {
            test.open(static_cast<std::uint64_t>(index + 1));
        }
        std::vector<std::thread> producers;
        producers.reserve(connections);
        for (std::size_t thread{0}; thread < connections; ++thread) {
            producers.emplace_back([&test, thread] {
                const auto connection{static_cast<std::uint64_t>(thread + 1)};
                for (std::size_t sequence{0}; sequence < per_connection;
                     ++sequence) {
                    const auto body{encode_sequence(sequence)};
                    test.running->send_raw(connection, body);
                }
            });
        }
        for (auto &producer : producers) {
            producer.join();
        }
        for (std::size_t index{0}; index < connections; ++index) {
            const auto stream{
                test.frames(static_cast<std::uint64_t>(index + 1))};
            expect(stream.size() == per_connection);
            expect(strictly_ordered(stream));
        }
    };

    "net/send many drains every recipient to the transport"_test = [] {
        // `send_raw_many` queues one shared frame per codec connection; the
        // transport must receive it without waiting for a later single send.
        fixture test{};
        expect(static_cast<bool>(test.running));
        if (!test.running) {
            return;
        }
        constexpr std::array<std::uint64_t, 4> connections{1, 2, 3, 4};
        for (const auto connection : connections) {
            test.open(connection);
        }
        const std::array payload{std::byte{0x2C}, std::byte{0x07},
                                 std::byte{0x08}, std::byte{0x09}};
        test.running->send_raw_many(connections, payload);
        const std::array expected{std::byte{0x07}, std::byte{0x08},
                                  std::byte{0x09}};
        for (const auto connection : connections) {
            const auto stream{test.frames(connection)};
            expect(stream.size() == 1U);
            if (!stream.empty()) {
                expect(stream.front() == std::vector<std::byte>{
                                             expected.begin(), expected.end()});
            }
        }
    };

    "net/send keeps its order while the transport closes the connection"_test =
        [] {
            fixture test{};
            expect(static_cast<bool>(test.running));
            if (!test.running) {
                return;
            }
            constexpr std::uint64_t connection{1};
            constexpr std::size_t per_connection{2000};
            test.open(connection);
            constexpr std::size_t seeded{3};
            for (std::size_t sequence{0}; sequence < seeded; ++sequence) {
                const auto body{encode_sequence(sequence)};
                test.running->send_raw(connection, body);
            }
            std::thread producer{[&test] {
                for (std::size_t sequence{seeded}; sequence < per_connection;
                     ++sequence) {
                    const auto body{encode_sequence(sequence)};
                    test.running->send_raw(connection, body);
                }
            }};
            test.sink->handler.on_close(connection, "test close");
            producer.join();
            // A send that raced the close may be dropped, but a frame that
            // reached the transport must still be the next produced value.
            expect(strictly_ordered(test.frames(connection)));
            const auto before{test.frames(connection).size()};
            const auto body{encode_sequence(per_connection)};
            test.running->send_raw(connection, body);
            expect(test.frames(connection).size() == before);
        };

    "net/send drops a connection the protocol closed under it"_test = [] {
        fixture test{};
        expect(static_cast<bool>(test.running));
        if (!test.running) {
            return;
        }
        constexpr std::uint64_t connection{3};
        constexpr std::size_t per_connection{2000};
        test.open(connection);
        constexpr std::size_t seeded{3};
        for (std::size_t sequence{0}; sequence < seeded; ++sequence) {
            const auto body{encode_sequence(sequence)};
            test.running->send_raw(connection, body);
        }
        test.codec->poison(connection);
        std::thread producer{[&test] {
            for (std::size_t sequence{seeded}; sequence < per_connection;
                 ++sequence) {
                const auto body{encode_sequence(sequence)};
                test.running->send_raw(connection, body);
            }
        }};
        test.sink->handler.on_data(connection, std::span<const std::byte>{});
        producer.join();
        expect(strictly_ordered(test.frames(connection)));
    };
};

} // namespace
