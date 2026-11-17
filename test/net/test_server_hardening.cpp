// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Negative coverage for the network hardening limits: the accept-time
// connection cap, the pre-play deadline and buffer budget, the inbound play
// queue budgets, the login-time view distance normalisation, the creative
// slot gate and the offline identity derivation. The scripted codec lets a
// test drive connections without sockets, and the injected clock advances
// deadlines without sleeping.

#include <refrain/core/config.hpp>
#include <refrain/net/auth.hpp>
#include <refrain/net/protocol_codec.hpp>
#include <refrain/net/server.hpp>
#include <refrain/net/transport.hpp>
#include <refrain/net/wire.hpp>
#include <refrain/protocol/current.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <atomic>
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
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace net = refrain::net;
namespace config_cb = refrain::protocol::current::configuration::clientbound;
namespace config_sb = refrain::protocol::current::configuration::serverbound;
namespace handshake_sb = refrain::protocol::current::handshake::serverbound;
namespace login_cb = refrain::protocol::current::login::clientbound;
namespace login_sb = refrain::protocol::current::login::serverbound;
namespace play_cb = refrain::protocol::current::play::clientbound;
namespace play_sb = refrain::protocol::current::play::serverbound;
namespace status_cb = refrain::protocol::current::status::clientbound;
namespace status_sb = refrain::protocol::current::status::serverbound;

/// Injected monotonic clock shared by the I/O and simulation paths.
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

/// Hands the server the packets a client would send next, records the packets
/// the server sends, and records the sessions it creates and releases.
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
    [[nodiscard]] auto live_count() -> std::size_t {
        const std::lock_guard lock{mutex_};
        return live_.size();
    }
    [[nodiscard]] auto released(std::uint64_t connection) -> int {
        const std::lock_guard lock{mutex_};
        const auto found{released_.find(connection)};
        return found == released_.end() ? 0 : found->second;
    }
    [[nodiscard]] auto sent(std::uint64_t connection) -> std::vector<packet> {
        const std::lock_guard lock{mutex_};
        const auto found{sent_.find(connection)};
        return found == sent_.end() ? std::vector<packet>{} : found->second;
    }
    [[nodiscard]] auto has_sent(std::uint64_t connection,
                                std::uint32_t packet_id) -> bool {
        const auto packets{sent(connection)};
        return std::ranges::any_of(packets, [packet_id](const auto &entry) {
            return entry.first == packet_id;
        });
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
    bool inject_clock{true};
    std::shared_ptr<transport_record> wire{
        std::make_shared<transport_record>()};
    std::shared_ptr<scripted_codec> codec{std::make_shared<scripted_codec>()};
    std::unique_ptr<net::server> running{};
    std::filesystem::path dir{};
    std::string error{};
    net::join_state_provider join_state{};
    std::vector<net::view_distance_event> view_events{};

    explicit fixture(std::string extra_config = {},
                     bool inject_clock_value = true,
                     net::join_state_provider state = {})
        : inject_clock{inject_clock_value}, join_state{std::move(state)} {
        static std::atomic<std::uint64_t> sequence{0};
        dir =
            std::filesystem::temp_directory_path() /
            ("refrain-net-hardening-" +
             std::to_string(static_cast<unsigned long long>(
                 std::chrono::steady_clock::now().time_since_epoch().count())) +
             "-" + std::to_string(sequence.fetch_add(1)));
        std::filesystem::create_directories(dir);
        {
            std::ofstream stream{dir / "network.toml",
                                 std::ios::binary | std::ios::trunc};
            stream << "bind_host = \"127.0.0.1\"\nport = 0\n"
                      "online_mode = false\n";
            stream << extra_config;
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
        if (inject_clock) {
            inputs.now_ns = [this] { return clock.now(); };
        }
        inputs.join_state = join_state;
        inputs.view_distance = [this](const net::view_distance_event &event) {
            view_events.push_back(event);
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

    void feed(std::uint64_t connection, std::uint32_t packet_id,
              std::vector<std::byte> payload) {
        codec->enqueue(connection, packet_id, std::move(payload));
        wire->handler.on_data(connection, {});
    }

    /// One `on_data` call carrying `count` play packets, the way a single read
    /// can hold a burst.
    void feed_play(std::uint64_t connection, std::size_t count,
                   std::size_t payload_bytes = 0) {
        std::vector<std::byte> payload(payload_bytes, std::byte{0x5A});
        for (std::size_t index{0}; index < count; ++index) {
            codec->enqueue(connection, play_sb::client_tick_end::packet_id,
                           payload);
        }
        wire->handler.on_data(connection, {});
    }

    void join_play(std::uint64_t connection,
                   std::string_view name = "Hardening",
                   std::int64_t profile_high = 0,
                   std::int64_t profile_low = 0) {
        handshake_sb::intention intention{};
        intention.protocol = 777;
        intention.host = "localhost";
        intention.port = 25565;
        intention.next = 2;
        feed(connection, handshake_sb::intention::packet_id,
             encode_payload(intention, handshake_sb::encode_intention));

        login_sb::hello hello{};
        hello.name = std::string{name};
        hello.profile_high = profile_high;
        hello.profile_low = profile_low;
        feed(connection, login_sb::hello::packet_id,
             encode_payload(hello, login_sb::encode_hello));

        feed(connection, login_sb::login_acknowledged::packet_id, {});
        feed(connection, config_sb::finish_configuration::packet_id, {});
    }
};

/// Poll `predicate` until it returns true or the deadline passes.
template <typename Predicate>
[[nodiscard]] auto
wait_until(Predicate predicate,
           std::chrono::milliseconds timeout = std::chrono::milliseconds{4000})
    -> bool {
    const auto deadline{std::chrono::steady_clock::now() + timeout};
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return predicate();
}

suite<"net/hardening"> server_hardening_tests = [] {
    "net/server enforces the default connection cap at accept"_test = [] {
        // The default ceiling is [status].max_players plus the margin, so a
        // two-player configuration admits ten sockets and refuses the rest.
        fixture test{"[status]\nmax_players = 2\n"};
        expect(static_cast<bool>(test.running));
        if (!test.running) {
            return;
        }
        for (std::uint64_t connection{1}; connection <= 10; ++connection) {
            test.open(connection);
            expect(test.codec->live(connection));
        }
        test.open(11);
        expect(!test.codec->live(11));
        expect(test.codec->live_count() == 10U);
        expect(test.wire->close_calls() == 1U);
        // A refused socket still releases its transport id through the normal
        // close path, without creating a session.
        expect(test.codec->released(11) == 0);
    };

    "net/server enforces a configured connection cap"_test = [] {
        fixture test{"max_connections = 3\n"};
        expect(static_cast<bool>(test.running));
        if (!test.running) {
            return;
        }
        for (std::uint64_t connection{20}; connection <= 23; ++connection) {
            test.open(connection);
        }
        expect(test.codec->live(20));
        expect(test.codec->live(21));
        expect(test.codec->live(22));
        expect(!test.codec->live(23));
    };

    "net/server refuses an invalid connection cap at startup"_test = [] {
        fixture zero{"max_connections = 0\n"};
        expect(!zero.running);
        expect(zero.error.find("max_connections") != std::string::npos);
    };

    "net/server closes a connection whose pre-play deadline passed"_test = [] {
        fixture test{"login_timeout_ms = 1000\n"};
        expect(static_cast<bool>(test.running));
        if (!test.running) {
            return;
        }
        constexpr std::uint64_t connection{31};
        test.open(connection);
        expect(test.codec->live(connection));
        test.clock.advance(std::chrono::milliseconds{2000});
        // The next read observes the deadline even if the sweeper has not run.
        test.wire->handler.on_data(connection, {});
        expect(!test.codec->live(connection));
        expect(test.codec->released(connection) == 1);
    };

    "net/server sweeper closes a silent pre-play connection after the deadline"_test =
        [] {
            // No injected clock: the deadline runs on the real one, and the
            // sweeper thread is the only thing that can close a peer that
            // sends nothing after its handshake.
            fixture test{"login_timeout_ms = 1000\n", false};
            expect(static_cast<bool>(test.running));
            if (!test.running) {
                return;
            }
            constexpr std::uint64_t connection{32};
            test.open(connection);
            expect(test.codec->live(connection));
            expect(
                wait_until([&test] { return !test.codec->live(connection); }));
            expect(test.codec->released(connection) == 1);
        };

    "net/server closes a connection that overruns the pre-play buffer budget"_test =
        [] {
            fixture test{"pre_play_buffer_bytes = 1024\n"};
            expect(static_cast<bool>(test.running));
            if (!test.running) {
                return;
            }
            constexpr std::uint64_t connection{33};
            test.open(connection);
            expect(test.codec->live(connection));
            std::vector<std::byte> trickle(2048, std::byte{0x00});
            test.wire->handler.on_data(connection, trickle);
            expect(!test.codec->live(connection));
            expect(test.codec->released(connection) == 1);
        };

    "net/server closes a play connection that overruns its packet budget"_test =
        [] {
            fixture test{"max_inbound_packets = 4\n"};
            expect(static_cast<bool>(test.running));
            if (!test.running) {
                return;
            }
            constexpr std::uint64_t connection{34};
            test.open(connection);
            test.join_play(connection);
            test.feed_play(connection, 4);
            expect(test.codec->live(connection));
            test.feed_play(connection, 1);
            expect(!test.codec->live(connection));
            expect(test.codec->released(connection) == 1);
        };

    "net/server closes a play connection that overruns its byte budget"_test =
        [] {
            fixture test{"max_inbound_bytes = 1024\n"};
            expect(static_cast<bool>(test.running));
            if (!test.running) {
                return;
            }
            constexpr std::uint64_t connection{35};
            test.open(connection);
            test.join_play(connection);
            test.feed_play(connection, 1, 600);
            expect(test.codec->live(connection));
            test.feed_play(connection, 1, 600);
            expect(!test.codec->live(connection));
        };

    "net/server applies the global inbound budget across connections"_test =
        [] {
            fixture test{"max_inbound_bytes_total = 1024\n"};
            expect(static_cast<bool>(test.running));
            if (!test.running) {
                return;
            }
            constexpr std::uint64_t first{36};
            constexpr std::uint64_t second{37};
            test.open(first);
            test.open(second);
            test.join_play(first, "BudgetOne");
            test.join_play(second, "BudgetTwo");
            test.feed_play(first, 1, 600);
            expect(test.codec->live(first));
            test.feed_play(second, 1, 600);
            expect(!test.codec->live(second));
            expect(test.codec->live(first));
        };

    "net/server rate limits a play connection by default"_test = [] {
        // With the key absent the limiter uses its non-zero default, so a
        // single read that holds more than the one-second burst is refused
        // even though no operator configured a limit.
        fixture test{"max_inbound_packets = 1000\n"};
        expect(static_cast<bool>(test.running));
        if (!test.running) {
            return;
        }
        constexpr std::uint64_t connection{38};
        test.open(connection);
        test.join_play(connection);
        test.feed_play(connection, 501);
        expect(!test.codec->live(connection));
    };

    "net/server still honours a disabled rate limit"_test = [] {
        fixture test{"rate_limit = 0\nmax_inbound_packets = 2000\n"};
        expect(static_cast<bool>(test.running));
        if (!test.running) {
            return;
        }
        constexpr std::uint64_t connection{39};
        test.open(connection);
        test.join_play(connection);
        test.feed_play(connection, 1000);
        expect(test.codec->live(connection));
    };

    "net/server ignores a creative slot packet from a survival player"_test =
        [] {
            fixture test{"", true, [](const net::player_identity &) {
                             return net::join_state{
                                 .mode = net::game_mode::survival};
                         }};
            expect(static_cast<bool>(test.running));
            if (!test.running) {
                return;
            }
            constexpr std::uint64_t connection{40};
            test.open(connection);
            test.join_play(connection);
            play_sb::set_creative_mode_slot packet{};
            packet.slot_num = 36;
            packet.item_stack = net::item_stack{.count = 1, .item_id = 1};
            test.feed(
                connection, play_sb::set_creative_mode_slot::packet_id,
                encode_payload(packet, play_sb::encode_set_creative_mode_slot));
            test.running->tick();
            expect(test.running->take_creative_slots().empty());
        };

    "net/server accepts a creative slot in the modeled menu only"_test = [] {
        fixture test{"", true, [](const net::player_identity &) {
                         return net::join_state{.mode =
                                                    net::game_mode::creative};
                     }};
        expect(static_cast<bool>(test.running));
        if (!test.running) {
            return;
        }
        constexpr std::uint64_t connection{41};
        test.open(connection);
        test.join_play(connection);
        play_sb::set_creative_mode_slot packet{};
        packet.slot_num = 36;
        packet.item_stack = net::item_stack{.count = 1, .item_id = 1};
        test.feed(
            connection, play_sb::set_creative_mode_slot::packet_id,
            encode_payload(packet, play_sb::encode_set_creative_mode_slot));
        // Slot 0 is the crafting result, not a player inventory slot.
        packet.slot_num = 0;
        test.feed(
            connection, play_sb::set_creative_mode_slot::packet_id,
            encode_payload(packet, play_sb::encode_set_creative_mode_slot));
        // Slot 46 is outside the 1..45 menu.
        packet.slot_num = 46;
        test.feed(
            connection, play_sb::set_creative_mode_slot::packet_id,
            encode_payload(packet, play_sb::encode_set_creative_mode_slot));
        test.running->tick();
        const auto slots{test.running->take_creative_slots()};
        expect(slots.size() == 1U);
        if (!slots.empty()) {
            expect(slots.front().slot == 36);
        }
    };

    "net/server clamps the client view distance into the configured range"_test =
        [] {
            fixture test{"view_distance = 4\n"};
            expect(static_cast<bool>(test.running));
            if (!test.running) {
                return;
            }
            constexpr std::uint64_t connection{42};
            test.open(connection);
            test.join_play(connection);
            play_sb::client_information packet{};
            packet.information.view_distance = 0;
            test.feed(
                connection, play_sb::client_information::packet_id,
                encode_payload(packet, play_sb::encode_client_information));
            packet.information.view_distance = 100;
            test.feed(
                connection, play_sb::client_information::packet_id,
                encode_payload(packet, play_sb::encode_client_information));
            test.running->tick();
            expect(test.view_events.size() == 2U);
            if (test.view_events.size() == 2U) {
                expect(test.view_events[0].distance == 2);
                expect(test.view_events[1].distance == 4);
            }
        };

    "net/server rejects an out-of-range view or simulation distance"_test = [] {
        for (const auto *config :
             {"view_distance = 0\n", "view_distance = 33\n",
              "simulation_distance = 1\n", "simulation_distance = 64\n"}) {
            fixture test{config};
            expect(!test.running);
            expect(test.error.find("out of range") != std::string::npos);
        }
        fixture low{"view_distance = 2\nsimulation_distance = 2\n"};
        expect(static_cast<bool>(low.running));
        fixture high{"view_distance = 32\nsimulation_distance = 32\n"};
        expect(static_cast<bool>(high.running));
    };

    "net/server enforces the custom_payload direction ceilings"_test = [] {
        const auto channel{
            refrain::data::resource_location::parse("minecraft:brand")};
        expect(channel.has_value());
        if (!channel) {
            return;
        }
        fixture test{};
        expect(static_cast<bool>(test.running));
        if (!test.running) {
            return;
        }
        const auto join_configuration = [&test](std::uint64_t connection,
                                                std::string_view name) {
            handshake_sb::intention intention{};
            intention.protocol = 777;
            intention.host = "localhost";
            intention.port = 25565;
            intention.next = 2;
            test.feed(
                connection, handshake_sb::intention::packet_id,
                encode_payload(intention, handshake_sb::encode_intention));
            login_sb::hello hello{};
            hello.name = std::string{name};
            test.feed(connection, login_sb::hello::packet_id,
                      encode_payload(hello, login_sb::encode_hello));
            test.feed(connection, login_sb::login_acknowledged::packet_id, {});
        };

        // The serverbound ceiling is 32,767 bytes after the channel id; the
        // boundary is accepted and one byte past it closes the connection
        // before the decoder copies the payload.
        constexpr std::uint64_t boundary{80};
        test.open(boundary);
        join_configuration(boundary, "PayloadLimit");
        config_sb::custom_payload accepted{};
        accepted.channel = *channel;
        accepted.data.assign(32'767, std::byte{0x42});
        test.feed(boundary, config_sb::custom_payload::packet_id,
                  encode_payload(accepted, config_sb::encode_custom_payload));
        expect(test.codec->live(boundary));

        constexpr std::uint64_t oversized{81};
        test.open(oversized);
        join_configuration(oversized, "PayloadLimit");
        config_sb::custom_payload too_big{};
        too_big.channel = *channel;
        too_big.data.assign(32'768, std::byte{0x42});
        test.feed(oversized, config_sb::custom_payload::packet_id,
                  encode_payload(too_big, config_sb::encode_custom_payload));
        expect(!test.codec->live(oversized));

        // The play-stage serverbound packet enforces the same ceiling.
        constexpr std::uint64_t play_connection{82};
        test.open(play_connection);
        test.join_play(play_connection, "PayloadLimit");
        expect(test.codec->live(play_connection));
        play_sb::custom_payload play_oversized{};
        play_oversized.channel = *channel;
        play_oversized.data.assign(32'768, std::byte{0x42});
        test.feed(
            play_connection, play_sb::custom_payload::packet_id,
            encode_payload(play_oversized, play_sb::encode_custom_payload));
        // Play packets are handled on the simulation thread.
        test.running->tick();
        expect(!test.codec->live(play_connection));

        // The clientbound ceiling is 1 MiB. The only outbound path is raw
        // packet sending; an oversized payload is dropped before encoding.
        constexpr std::uint64_t outbound{83};
        test.open(outbound);
        test.join_play(outbound, "PayloadLimit");
        expect(test.codec->live(outbound));
        std::vector<std::byte> body;
        net::writer body_writer{body};
        body_writer.write_varint(play_cb::custom_payload::packet_id);
        expect(body_writer.write_identifier(*channel).has_value());
        body_writer.write_bytes(
            std::vector<std::byte>(1U << 20U, std::byte{0x42}));
        body_writer.write_bytes(std::vector<std::byte>(1, std::byte{0x42}));
        test.running->send_raw(outbound, body);
        expect(!test.codec->has_sent(outbound,
                                     play_cb::custom_payload::packet_id));

        body.clear();
        net::writer small_writer{body};
        small_writer.write_varint(play_cb::custom_payload::packet_id);
        expect(small_writer.write_identifier(*channel).has_value());
        small_writer.write_bytes(std::vector<std::byte>(16, std::byte{0x42}));
        test.running->send_raw(outbound, body);
        expect(
            test.codec->has_sent(outbound, play_cb::custom_payload::packet_id));
    };

    "net/server kicks the older session of a duplicate login"_test = [] {
        fixture test{};
        expect(static_cast<bool>(test.running));
        if (!test.running) {
            return;
        }
        constexpr std::uint64_t first{90};
        test.open(first);
        test.join_play(first, "DupPlayer");
        expect(test.codec->live(first));

        // Offline identity is derived from the name, so the same name is the
        // same profile; the newest login keeps the session and the older one
        // receives the duplicate-login disconnect.
        constexpr std::uint64_t second{91};
        test.open(second);
        test.join_play(second, "DupPlayer");
        expect(test.codec->live(second));
        expect(!test.codec->live(first));
        expect(test.codec->released(first) == 1);
        expect(test.codec->has_sent(first, play_cb::disconnect::packet_id));

        // A different name is a different identity and both sessions stay.
        constexpr std::uint64_t third{92};
        test.open(third);
        test.join_play(third, "OtherPlayer");
        expect(test.codec->live(second));
        expect(test.codec->live(third));
    };

    "net/server answers a status request only when status replies are enabled"_test =
        [] {
            fixture enabled{};
            expect(static_cast<bool>(enabled.running));
            if (enabled.running) {
                constexpr std::uint64_t connection{70};
                enabled.open(connection);
                handshake_sb::intention intention{};
                intention.protocol = 777;
                intention.host = "localhost";
                intention.port = 25565;
                intention.next = 1;
                enabled.feed(
                    connection, handshake_sb::intention::packet_id,
                    encode_payload(intention, handshake_sb::encode_intention));
                status_sb::status_request request{};
                enabled.feed(
                    connection, status_sb::status_request::packet_id,
                    encode_payload(request, status_sb::encode_status_request));
                expect(enabled.codec->live(connection));
                expect(enabled.codec->has_sent(
                    connection, status_cb::status_response::packet_id));
            }

            fixture disabled{"[status]\nenabled = false\n"};
            expect(static_cast<bool>(disabled.running));
            if (!disabled.running) {
                return;
            }
            constexpr std::uint64_t connection{71};
            disabled.open(connection);
            handshake_sb::intention intention{};
            intention.protocol = 777;
            intention.host = "localhost";
            intention.port = 25565;
            intention.next = 1;
            disabled.feed(
                connection, handshake_sb::intention::packet_id,
                encode_payload(intention, handshake_sb::encode_intention));
            status_sb::status_request request{};
            disabled.feed(
                connection, status_sb::status_request::packet_id,
                encode_payload(request, status_sb::encode_status_request));
            // No response of any kind, and the connection is closed: the
            // MOTD, version and player count are not disclosed.
            expect(!disabled.codec->live(connection));
            for (const auto packet_id : {status_cb::status_response::packet_id,
                                         status_cb::pong_response::packet_id}) {
                expect(!disabled.codec->has_sent(connection, packet_id));
            }
        };

    "net/server rejects out-of-range auth timeout and player ceiling"_test =
        [] {
            // Both values are narrowed into smaller integer types, so a value
            // outside the documented range must stop startup instead of
            // wrapping.
            for (const auto *config :
                 {"auth_timeout_ms = 0\n", "auth_timeout_ms = 60001\n",
                  "[status]\nmax_players = -1\n",
                  "[status]\nmax_players = 1000001\n"}) {
                fixture test{config};
                expect(!test.running);
                expect(test.error.find("out of range") != std::string::npos);
            }
            fixture low{"auth_timeout_ms = 1\n[status]\nmax_players = 0\n"};
            expect(static_cast<bool>(low.running));
            fixture high{
                "auth_timeout_ms = 60000\n[status]\nmax_players = 1000000\n"};
            expect(static_cast<bool>(high.running));
        };

    "net/server derives the offline identity from the name"_test = [] {
        fixture test{};
        expect(static_cast<bool>(test.running));
        if (!test.running) {
            return;
        }
        const auto expected{net::offline_profile("OfflineAlice")};
        expect(static_cast<bool>(expected));
        if (!expected) {
            return;
        }
        // The client claims an unrelated profile id; the server must ignore it.
        const auto other{net::offline_profile("SomeoneElse")};
        expect(static_cast<bool>(other));
        if (!other) {
            return;
        }
        const auto [other_high, other_low] = other->id.halves();
        constexpr std::uint64_t connection{43};
        test.open(connection);
        test.join_play(connection, "OfflineAlice", other_high, other_low);
        const auto packets{test.codec->sent(connection)};
        const auto finished{
            std::ranges::find_if(packets, [](const auto &packet) {
                return packet.first == login_cb::login_finished::packet_id;
            })};
        expect(finished != packets.end());
        if (finished == packets.end()) {
            return;
        }
        net::reader in{finished->second};
        login_cb::login_finished decoded{};
        expect(static_cast<bool>(login_cb::decode_login_finished(in, decoded)));
        const auto profile{
            net::uuid::from_halves(decoded.profile_high, decoded.profile_low)};
        expect(profile == expected->id);
        expect(!(profile == other->id));
    };

    "net/server gives two offline logins of one name the same profile and a "
    "fresh "
    "session"_test = [] {
        fixture test{};
        expect(static_cast<bool>(test.running));
        if (!test.running) {
            return;
        }
        const auto expected{net::offline_profile("OfflineBob")};
        expect(static_cast<bool>(expected));
        if (!expected) {
            return;
        }
        const auto capture = [&test](std::uint64_t connection,
                                     std::int64_t profile_high,
                                     std::int64_t profile_low) {
            test.open(connection);
            test.join_play(connection, "OfflineBob", profile_high, profile_low);
            const auto packets{test.codec->sent(connection)};
            const auto finished{
                std::ranges::find_if(packets, [](const auto &packet) {
                    return packet.first == login_cb::login_finished::packet_id;
                })};
            if (finished == packets.end()) {
                return std::pair<net::uuid, net::uuid>{};
            }
            net::reader in{finished->second};
            login_cb::login_finished decoded{};
            if (!login_cb::decode_login_finished(in, decoded)) {
                return std::pair<net::uuid, net::uuid>{};
            }
            return std::pair{net::uuid::from_halves(decoded.profile_high,
                                                    decoded.profile_low),
                             net::uuid::from_halves(decoded.session_high,
                                                    decoded.session_low)};
        };
        const auto [first_profile, first_session] = capture(44, 7, 11);
        const auto [second_profile, second_session] = capture(45, 0, 0);
        expect(first_profile == expected->id);
        expect(second_profile == expected->id);
        expect(!(first_session == second_session));
    };
};

} // namespace
