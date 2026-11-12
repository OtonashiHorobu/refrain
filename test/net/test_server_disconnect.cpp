// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Protocol-session lifetime across disconnect paths. The codec records session
// creation and release, and the transport reports drops the way a socket does,
// so the test observes the session without a real transport.

#include <refrain/core/config.hpp>
#include <refrain/net/protocol_codec.hpp>
#include <refrain/net/server.hpp>
#include <refrain/net/transport.hpp>

#include <boost/ut.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace net = refrain::net;

/// Records the sessions the server creates and releases. `close_connection` is
/// the release the provider is owed exactly once per connection.
class session_codec final : public net::protocol_codec {
  public:
    void create_connection(std::uint64_t connection) override {
        const std::lock_guard lock{mutex_};
        ++created_[connection];
        live_.insert(connection);
    }
    void close_connection(std::uint64_t connection) override {
        const std::lock_guard lock{mutex_};
        ++released_[connection];
        live_.erase(connection);
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
    [[nodiscard]] auto transition(std::uint64_t, net::protocol_event)
        -> net::connection_state override {
        return net::connection_state::handshake;
    }

    /// Make `feed` fail for one connection, which drives the server's decode
    /// failure path.
    void poison(std::uint64_t connection) {
        const std::lock_guard lock{mutex_};
        poisoned_.insert(connection);
    }

    [[nodiscard]] auto created(std::uint64_t connection) -> int {
        const std::lock_guard lock{mutex_};
        const auto found{created_.find(connection)};
        return found == created_.end() ? 0 : found->second;
    }
    [[nodiscard]] auto released(std::uint64_t connection) -> int {
        const std::lock_guard lock{mutex_};
        const auto found{released_.find(connection)};
        return found == released_.end() ? 0 : found->second;
    }
    [[nodiscard]] auto sent(std::uint64_t connection) -> int {
        const std::lock_guard lock{mutex_};
        const auto found{sent_.find(connection)};
        return found == sent_.end() ? 0 : found->second;
    }
    [[nodiscard]] auto live(std::uint64_t connection) -> bool {
        const std::lock_guard lock{mutex_};
        return live_.contains(connection);
    }
    [[nodiscard]] auto live_count() -> std::size_t {
        const std::lock_guard lock{mutex_};
        return live_.size();
    }

  private:
    std::mutex mutex_{};
    std::unordered_map<std::uint64_t, int> created_{};
    std::unordered_map<std::uint64_t, int> released_{};
    std::unordered_map<std::uint64_t, int> sent_{};
    std::set<std::uint64_t> live_{};
    std::set<std::uint64_t> poisoned_{};
};

struct transport_record {
    std::mutex mutex{};
    std::size_t closes{0};
    std::size_t stops{0};
    net::transport_handler handler{};

    [[nodiscard]] auto close_calls() -> std::size_t {
        const std::lock_guard lock{mutex};
        return closes;
    }
};

/// Transport with the real close contract: `close` reports `on_close` once for
/// the connection. `stop` drops every callback, the way a stopped I/O context
/// does, so shutdown has to release whatever is left itself.
class dropping_transport final : public net::transport {
  public:
    explicit dropping_transport(std::shared_ptr<transport_record> record)
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
    void stop() noexcept override {
        const std::lock_guard lock{record_->mutex};
        ++record_->stops;
    }
    [[nodiscard]] auto connection_count() const -> std::size_t override {
        return 0;
    }
    [[nodiscard]] auto local_port() const -> std::uint16_t override {
        return 25565;
    }
    [[nodiscard]] auto descriptor() const
        -> net::transport_descriptor override {
        return net::transport_descriptor{.provider = "dropping"};
    }

  private:
    std::shared_ptr<transport_record> record_;
};

struct fixture {
    std::shared_ptr<transport_record> wire{
        std::make_shared<transport_record>()};
    std::shared_ptr<session_codec> codec{std::make_shared<session_codec>()};
    std::unique_ptr<net::server> running{};
    std::filesystem::path dir{};

    fixture() {
        dir =
            std::filesystem::temp_directory_path() /
            ("refrain-net-disconnect-" +
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
        const auto wire_copy{wire};
        inputs.make_transport = [wire_copy](net::transport_handler handler,
                                            std::size_t) {
            wire_copy->handler = std::move(handler);
            return refrain::result<std::unique_ptr<net::transport>>{
                std::make_unique<dropping_transport>(wire_copy)};
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

    void open(std::uint64_t connection) { wire->handler.on_open(connection); }
    /// Abrupt drop: the transport reports the close, no disconnect packet.
    void drop(std::uint64_t connection) {
        wire->handler.on_close(connection, "connection reset by peer");
    }
    void send_probe(std::uint64_t connection) {
        const std::array<std::byte, 1> body{std::byte{0x00}};
        running->send_raw(connection, body);
    }
};

suite<"net/disconnect"> server_disconnect_tests = [] {
    "net/server releases the protocol session when the transport drops"_test =
        [] {
            fixture test{};
            expect(static_cast<bool>(test.running));
            if (!test.running) {
                return;
            }
            constexpr std::uint64_t connection{1};
            test.open(connection);
            expect(test.codec->created(connection) == 1);
            expect(test.codec->released(connection) == 0);
            test.send_probe(connection);
            expect(test.codec->sent(connection) == 1);

            test.drop(connection);
            expect(test.codec->released(connection) == 1);
            expect(!test.codec->live(connection));
            // The entry left the table with the session: a send no longer
            // reaches the codec.
            test.send_probe(connection);
            expect(test.codec->sent(connection) == 1);
        };

    "net/server releases the protocol session once when the drop repeats"_test =
        [] {
            fixture test{};
            expect(static_cast<bool>(test.running));
            if (!test.running) {
                return;
            }
            constexpr std::uint64_t connection{2};
            test.open(connection);
            test.drop(connection);
            expect(test.codec->released(connection) == 1);

            // A second report of the same close is a no-op, not a second
            // release.
            test.drop(connection);
            expect(test.codec->released(connection) == 1);
            expect(!test.codec->live(connection));
        };

    "net/server releases the protocol session once on a decode failure"_test =
        [] {
            fixture test{};
            expect(static_cast<bool>(test.running));
            if (!test.running) {
                return;
            }
            constexpr std::uint64_t connection{3};
            test.open(connection);
            test.codec->poison(connection);
            const std::span<const std::byte> garbage{};
            test.wire->handler.on_data(connection, garbage);
            expect(test.codec->released(connection) == 1);
            expect(test.codec->live(connection) == false);
            expect(test.wire->close_calls() == 1);

            // The local close and the transport report describe one
            // disconnect: the second one releases nothing.
            test.drop(connection);
            expect(test.codec->released(connection) == 1);
            expect(test.wire->close_calls() == 1);
        };

    "net/server releases the protocol session for every connection it stops"_test =
        [] {
            fixture test{};
            expect(static_cast<bool>(test.running));
            if (!test.running) {
                return;
            }
            constexpr std::array<std::uint64_t, 3> connections{4, 5, 6};
            for (const auto connection : connections) {
                test.open(connection);
            }
            expect(test.codec->live_count() == connections.size());

            // The transport drops its close callbacks when it stops, so the
            // server has to release these sessions itself.
            test.running->stop();
            expect(test.codec->live_count() == 0);
            for (const auto connection : connections) {
                expect(test.codec->released(connection) == 1);
            }
            // A report that arrives after shutdown removes nothing.
            test.drop(connections.front());
            expect(test.codec->released(connections.front()) == 1);
        };

    "net/server ignores a drop it never opened"_test = [] {
        fixture test{};
        expect(static_cast<bool>(test.running));
        if (!test.running) {
            return;
        }
        test.drop(9);
        expect(test.codec->released(9) == 0);
        expect(test.codec->created(9) == 0);
    };

    "net/server releases the protocol session once under a repeated drop"_test =
        [] {
            fixture test{};
            expect(static_cast<bool>(test.running));
            if (!test.running) {
                return;
            }
            constexpr std::uint64_t connection{7};
            constexpr std::size_t threads{8};
            test.open(connection);
            std::vector<std::thread> droppers;
            droppers.reserve(threads);
            for (std::size_t index{0}; index < threads; ++index) {
                droppers.emplace_back([&test] { test.drop(connection); });
            }
            for (auto &dropper : droppers) {
                dropper.join();
            }
            expect(test.codec->released(connection) == 1);
            expect(!test.codec->live(connection));
        };

    "net/server releases the protocol session once when a drop races a local close"_test =
        [] {
            fixture test{};
            expect(static_cast<bool>(test.running));
            if (!test.running) {
                return;
            }
            constexpr std::uint64_t connection{8};
            test.open(connection);
            test.codec->poison(connection);
            std::thread dropper{[&test] { test.drop(connection); }};
            std::thread failure{[&test] {
                const std::span<const std::byte> garbage{};
                test.wire->handler.on_data(connection, garbage);
            }};
            dropper.join();
            failure.join();
            expect(test.codec->released(connection) == 1);
            expect(!test.codec->live(connection));
        };
};

} // namespace
