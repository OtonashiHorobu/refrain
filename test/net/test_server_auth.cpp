// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Online-mode login coverage that needs the encrypted exchange: the session
// verification runs on a bounded worker pool instead of the single network I/O
// thread, one slow login does not stall another connection, the verify token is
// compared in constant time and consumed, and repeated attempts from one
// address are throttled. The RSA keypair is injected so the test can produce
// the client's encrypted key packet without knowing the server's private key.

#include <refrain/core/config.hpp>
#include <refrain/net/auth.hpp>
#include <refrain/net/crypto.hpp>
#include <refrain/net/protocol_codec.hpp>
#include <refrain/net/server.hpp>
#include <refrain/net/transport.hpp>
#include <refrain/net/wire.hpp>
#include <refrain/protocol/current.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
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
namespace handshake_sb = refrain::protocol::current::handshake::serverbound;
namespace login_cb = refrain::protocol::current::login::clientbound;
namespace login_sb = refrain::protocol::current::login::serverbound;
namespace status_cb = refrain::protocol::current::status::clientbound;
namespace status_sb = refrain::protocol::current::status::serverbound;

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
    [[nodiscard]] auto sent(std::uint64_t connection) -> std::vector<packet> {
        const std::lock_guard lock{mutex_};
        const auto found{sent_.find(connection)};
        return found == sent_.end() ? std::vector<packet>{} : found->second;
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
    std::string peer_address{};
    net::transport_handler handler{};

    [[nodiscard]] auto close_calls() -> std::size_t {
        const std::lock_guard lock{mutex};
        return closes;
    }
};

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
    [[nodiscard]] auto peer_address(std::uint64_t) const
        -> std::string override {
        const std::lock_guard lock{record_->mutex};
        return record_->peer_address;
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

struct fixture {
    fake_clock clock{};
    std::shared_ptr<transport_record> wire{
        std::make_shared<transport_record>()};
    std::shared_ptr<scripted_codec> codec{std::make_shared<scripted_codec>()};
    std::unique_ptr<net::server> running{};
    std::filesystem::path dir{};
    std::string error{};
    std::vector<std::byte> public_key{};

    fixture(net::session_verifier verifier, std::string peer = {}) {
        static std::atomic<std::uint64_t> sequence{0};
        dir =
            std::filesystem::temp_directory_path() /
            ("refrain-net-auth-" +
             std::to_string(static_cast<unsigned long long>(
                 std::chrono::steady_clock::now().time_since_epoch().count())) +
             "-" + std::to_string(sequence.fetch_add(1)));
        std::filesystem::create_directories(dir);
        {
            std::ofstream stream{dir / "network.toml",
                                 std::ios::binary | std::ios::trunc};
            stream << "bind_host = \"127.0.0.1\"\nport = 0\n"
                      "online_mode = true\n";
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
        {
            const std::lock_guard lock{wire->mutex};
            wire->peer_address = std::move(peer);
        }
        auto keypair{net::rsa_keypair::generate()};
        if (!keypair) {
            error = "the RSA keypair could not be generated";
            return;
        }
        auto der{keypair->public_key_der()};
        if (!der) {
            error = "the RSA public key could not be encoded";
            return;
        }
        public_key = std::move(*der);
        net::server_inputs inputs{};
        inputs.protocol = codec;
        inputs.now_ns = [this] { return clock.now(); };
        inputs.verify_session = std::move(verifier);
        inputs.session_keypair = std::move(*keypair);
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

    [[nodiscard]] auto encrypt(std::span<const std::byte> plaintext)
        -> std::vector<std::byte> {
        auto encrypted{net::rsa_encrypt_pkcs1_v1_5(public_key, plaintext)};
        return encrypted ? std::move(*encrypted) : std::vector<std::byte>{};
    }

    /// Drive a login connection up to the server's `login_hello` and return the
    /// challenge it sent.
    [[nodiscard]] auto join_to_hello(std::uint64_t connection,
                                     std::string_view name)
        -> std::vector<std::byte> {
        handshake_sb::intention intention{};
        intention.protocol = 777;
        intention.host = "localhost";
        intention.port = 25565;
        intention.next = 2;
        feed(connection, handshake_sb::intention::packet_id,
             encode_payload(intention, handshake_sb::encode_intention));
        login_sb::hello hello{};
        hello.name = std::string{name};
        hello.profile_high = 0;
        hello.profile_low = 0;
        feed(connection, login_sb::hello::packet_id,
             encode_payload(hello, login_sb::encode_hello));
        const auto packets{codec->sent(connection)};
        const auto hello_packet{
            std::ranges::find_if(packets, [](const auto &packet) {
                return packet.first == login_cb::hello::packet_id;
            })};
        if (hello_packet == packets.end()) {
            return {};
        }
        net::reader in{hello_packet->second};
        login_cb::hello decoded{};
        if (!login_cb::decode_hello(in, decoded)) {
            return {};
        }
        return decoded.challenge;
    }

    /// Send the encryption response that claims `token` as the decrypted
    /// challenge and `shared_secret` as the shared key.
    void send_key(std::uint64_t connection, std::span<const std::byte> token,
                  std::span<const std::byte> shared_secret) {
        login_sb::key packet{};
        packet.keybytes = encrypt(shared_secret);
        packet.encrypted_challenge = encrypt(token);
        feed(connection, login_sb::key::packet_id,
             encode_payload(packet, login_sb::encode_key));
    }

    [[nodiscard]] auto disconnect_reason(std::uint64_t connection)
        -> std::string {
        const auto packets{codec->sent(connection)};
        const auto disconnect{
            std::ranges::find_if(packets, [](const auto &packet) {
                return packet.first == login_cb::login_disconnect::packet_id;
            })};
        if (disconnect == packets.end()) {
            return {};
        }
        net::reader in{disconnect->second};
        login_cb::login_disconnect decoded{};
        if (!login_cb::decode_login_disconnect(in, decoded)) {
            return {};
        }
        return decoded.reason;
    }

    [[nodiscard]] auto has_packet(std::uint64_t connection,
                                  std::uint32_t packet_id) -> bool {
        const auto packets{codec->sent(connection)};
        return std::ranges::any_of(packets, [packet_id](const auto &packet) {
            return packet.first == packet_id;
        });
    }
};

[[nodiscard]] auto shared_secret() -> std::vector<std::byte> {
    std::vector<std::byte> secret(net::aes128_key_bytes);
    for (std::size_t index{0}; index < secret.size(); ++index) {
        secret[index] = static_cast<std::byte>(index + 1U);
    }
    return secret;
}

[[nodiscard]] auto expected_profile() -> net::session_profile {
    net::session_profile profile{};
    profile.id = *net::uuid::parse("00112233-4455-4677-8899-aabbccddeeff");
    profile.name = "AuthTester";
    return profile;
}

suite<"net/auth"> server_auth_tests = [] {
    "net/server verifies a login off the I/O thread and a status connection is "
    "not blocked"_test = [] {
        std::mutex mutex{};
        std::condition_variable changed{};
        bool verifier_started{false};
        bool verifier_released{false};
        std::atomic<int> verifier_calls{0};
        net::session_verifier verifier = [&](std::string_view /*name*/,
                                             std::string_view /*hash*/)
            -> refrain::result<net::session_profile> {
            ++verifier_calls;
            {
                const std::lock_guard lock{mutex};
                verifier_started = true;
            }
            changed.notify_all();
            std::unique_lock lock{mutex};
            static_cast<void>(changed.wait_for(
                lock, std::chrono::milliseconds{5000},
                [&verifier_released] { return verifier_released; }));
            return expected_profile();
        };
        fixture test{verifier};
        expect(static_cast<bool>(test.running));
        if (!test.running) {
            return;
        }
        constexpr std::uint64_t login_connection{1};
        test.open(login_connection);
        const auto challenge{
            test.join_to_hello(login_connection, "AuthTester")};
        expect(challenge.size() == 4U);
        if (challenge.size() != 4U) {
            return;
        }
        const auto secret{shared_secret()};
        test.send_key(login_connection, challenge, secret);
        // The key packet returned without waiting for the session verifier.
        expect(wait_until([&] {
            const std::lock_guard lock{mutex};
            return verifier_started;
        }));
        {
            const std::lock_guard lock{mutex};
            expect(!verifier_released);
        }
        // A status request on another connection is served while the verifier
        // is still blocked.
        constexpr std::uint64_t status_connection{2};
        test.open(status_connection);
        handshake_sb::intention intention{};
        intention.protocol = 777;
        intention.host = "localhost";
        intention.port = 25565;
        intention.next = 1;
        test.feed(status_connection, handshake_sb::intention::packet_id,
                  encode_payload(intention, handshake_sb::encode_intention));
        status_sb::status_request request{};
        test.feed(status_connection, status_sb::status_request::packet_id,
                  encode_payload(request, status_sb::encode_status_request));
        expect(test.has_packet(status_connection,
                               status_cb::status_response::packet_id));

        {
            const std::lock_guard lock{mutex};
            verifier_released = true;
        }
        changed.notify_all();
        expect(wait_until([&] {
            return test.has_packet(login_connection,
                                   login_cb::login_finished::packet_id);
        }));
        expect(verifier_calls == 1);
        // The profile the verifier returned is the one sent to the client.
        const auto packets{test.codec->sent(login_connection)};
        const auto finished{
            std::ranges::find_if(packets, [](const auto &packet) {
                return packet.first == login_cb::login_finished::packet_id;
            })};
        expect(finished != packets.end());
        if (finished != packets.end()) {
            net::reader in{finished->second};
            login_cb::login_finished decoded{};
            expect(static_cast<bool>(
                login_cb::decode_login_finished(in, decoded)));
            expect(net::uuid::from_halves(decoded.profile_high,
                                          decoded.profile_low) ==
                   expected_profile().id);
        }
    };

    "net/server rejects a repeated key packet while verification is in flight"_test =
        [] {
            std::mutex mutex{};
            std::condition_variable changed{};
            bool started{false};
            bool released{false};
            std::atomic<int> calls{0};
            net::session_verifier verifier = [&](std::string_view /*name*/,
                                                 std::string_view /*hash*/)
                -> refrain::result<net::session_profile> {
                ++calls;
                {
                    const std::lock_guard lock{mutex};
                    started = true;
                }
                changed.notify_all();
                std::unique_lock lock{mutex};
                static_cast<void>(
                    changed.wait_for(lock, std::chrono::milliseconds{5000},
                                     [&released] { return released; }));
                return expected_profile();
            };
            fixture test{verifier};
            expect(static_cast<bool>(test.running));
            if (!test.running) {
                return;
            }
            constexpr std::uint64_t connection{3};
            test.open(connection);
            const auto challenge{test.join_to_hello(connection, "AuthTester")};
            expect(challenge.size() == 4U);
            if (challenge.size() != 4U) {
                return;
            }
            const auto secret{shared_secret()};
            test.send_key(connection, challenge, secret);
            expect(wait_until([&] {
                const std::lock_guard lock{mutex};
                return started;
            }));
            // The same encrypted response again must not start a second
            // session-server request.
            test.send_key(connection, challenge, secret);
            expect(!test.codec->live(connection));
            expect(calls == 1);
            expect(test.disconnect_reason(connection)
                       .find("already in progress") != std::string::npos);
            {
                const std::lock_guard lock{mutex};
                released = true;
            }
            changed.notify_all();
        };

    "net/server rejects a verify token that does not match the challenge"_test =
        [] {
            std::atomic<int> calls{0};
            net::session_verifier verifier = [&](std::string_view /*name*/,
                                                 std::string_view /*hash*/)
                -> refrain::result<net::session_profile> {
                ++calls;
                return expected_profile();
            };
            fixture test{verifier};
            expect(static_cast<bool>(test.running));
            if (!test.running) {
                return;
            }
            constexpr std::uint64_t connection{4};
            test.open(connection);
            const auto challenge{test.join_to_hello(connection, "AuthTester")};
            expect(challenge.size() == 4U);
            if (challenge.size() != 4U) {
                return;
            }
            const std::array<std::byte, 4> wrong{
                std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE},
                std::byte{0xEF}};
            const auto secret{shared_secret()};
            test.send_key(connection, wrong, secret);
            expect(!test.codec->live(connection));
            // A mismatched token is rejected before the session verifier runs.
            expect(calls == 0);
            expect(test.disconnect_reason(connection)
                       .find("Invalid verify token") != std::string::npos);
        };

    "net/server throttles repeated login attempts from one address"_test = [] {
        net::session_verifier verifier =
            [](std::string_view,
               std::string_view) -> refrain::result<net::session_profile> {
            return std::unexpected{refrain::make_error(
                refrain::error_code::not_found, "not joined")};
        };
        fixture test{verifier, "198.51.100.7"};
        expect(static_cast<bool>(test.running));
        if (!test.running) {
            return;
        }
        const std::array<std::byte, 4> wrong{std::byte{0x01}, std::byte{0x02},
                                             std::byte{0x03}, std::byte{0x04}};
        const auto secret{shared_secret()};
        for (std::uint64_t attempt{1}; attempt <= 8; ++attempt) {
            constexpr std::uint64_t base{100};
            const auto connection{base + attempt};
            test.open(connection);
            const auto challenge{
                test.join_to_hello(connection, "ThrottleTester")};
            expect(challenge.size() == 4U);
            if (challenge.size() != 4U) {
                return;
            }
            test.send_key(connection, wrong, secret);
            expect(!test.codec->live(connection));
        }
        constexpr std::uint64_t throttled{200};
        test.open(throttled);
        const auto challenge{test.join_to_hello(throttled, "ThrottleTester")};
        expect(challenge.size() == 4U);
        if (challenge.size() != 4U) {
            return;
        }
        test.send_key(throttled, wrong, secret);
        expect(!test.codec->live(throttled));
        expect(test.disconnect_reason(throttled).find("Too many") !=
               std::string::npos);
    };

    "net/server sends encryption, compression and login success in order"_test =
        [] {
            std::atomic<bool> verifier_inputs_valid{false};
            net::session_verifier verifier =
                [&verifier_inputs_valid](std::string_view name,
                                         std::string_view hash)
                -> refrain::result<net::session_profile> {
                verifier_inputs_valid.store(name == "AuthTester" &&
                                                !hash.empty(),
                                            std::memory_order_relaxed);
                return expected_profile();
            };
            fixture test{verifier};
            expect(static_cast<bool>(test.running));
            if (!test.running) {
                return;
            }
            constexpr std::uint64_t connection{300};
            test.open(connection);
            const auto challenge{test.join_to_hello(connection, "AuthTester")};
            expect(challenge.size() == 4U);
            if (challenge.size() != 4U) {
                return;
            }
            // Client-side encryption step: the shared secret is random per
            // login, and the challenge it echoes is the one the server sent.
            test.send_key(connection, challenge, shared_secret());
            expect(wait_until([&] {
                return test.has_packet(connection,
                                       login_cb::login_finished::packet_id);
            }));
            expect(test.codec->live(connection));
            const auto packets{test.codec->sent(connection)};
            const auto position_of = [&packets](std::uint32_t id) {
                std::size_t position{0};
                for (const auto &packet : packets) {
                    if (packet.first == id) {
                        return position;
                    }
                    ++position;
                }
                return position;
            };
            // The server raises encryption before it sends the compressed
            // front, and login success comes after both.
            const auto hello_position{position_of(login_cb::hello::packet_id)};
            const auto compression_position{
                position_of(login_cb::login_compression::packet_id)};
            const auto finished_position{
                position_of(login_cb::login_finished::packet_id)};
            expect(verifier_inputs_valid.load(std::memory_order_relaxed));
            expect(hello_position < compression_position);
            expect(compression_position < finished_position);
            // The verified profile, not the client-declared one, is echoed.
            const auto finished{
                std::ranges::find_if(packets, [](const auto &packet) {
                    return packet.first == login_cb::login_finished::packet_id;
                })};
            expect(finished != packets.end());
            if (finished != packets.end()) {
                net::reader in{finished->second};
                login_cb::login_finished decoded{};
                expect(static_cast<bool>(
                    login_cb::decode_login_finished(in, decoded)));
                expect(net::uuid::from_halves(decoded.profile_high,
                                              decoded.profile_low) ==
                       expected_profile().id);
            }
        };

    "net/server rejects login acknowledgement before the encryption step"_test =
        [] {
            net::session_verifier verifier =
                [](std::string_view,
                   std::string_view) -> refrain::result<net::session_profile> {
                return expected_profile();
            };
            fixture test{verifier};
            expect(static_cast<bool>(test.running));
            if (!test.running) {
                return;
            }
            constexpr std::uint64_t connection{301};
            test.open(connection);
            const auto challenge{test.join_to_hello(connection, "AuthTester")};
            expect(challenge.size() == 4U);
            if (challenge.size() != 4U) {
                return;
            }
            // Skipping the key exchange must not reach the configuration
            // stage: no login success was acknowledged yet.
            test.feed(connection, login_sb::login_acknowledged::packet_id, {});
            expect(!test.codec->live(connection));
            expect(!test.has_packet(connection,
                                    login_cb::login_finished::packet_id));
        };

    "net/server rejects a login whose session check times out or fails TLS"_test =
        [] {
            for (const auto reason :
                 {net::auth_error::timeout, net::auth_error::tls_failure}) {
                // The verifier fails synthetically: no network access is
                // involved, and the login handler must close the connection
                // instead of waiting forever.
                net::session_verifier verifier = [reason](std::string_view,
                                                          std::string_view)
                    -> refrain::result<net::session_profile> {
                    return std::unexpected{net::make_auth_error(
                        reason, "synthetic session failure")};
                };
                fixture test{verifier};
                expect(static_cast<bool>(test.running));
                if (!test.running) {
                    return;
                }
                constexpr std::uint64_t connection{302};
                test.open(connection);
                const auto challenge{
                    test.join_to_hello(connection, "AuthTester")};
                expect(challenge.size() == 4U);
                if (challenge.size() != 4U) {
                    return;
                }
                test.send_key(connection, challenge, shared_secret());
                expect(
                    wait_until([&] { return !test.codec->live(connection); }));
                expect(!test.has_packet(connection,
                                        login_cb::login_finished::packet_id));
                expect(test.disconnect_reason(connection)
                           .find("Failed to verify username") !=
                       std::string::npos);
            }
        };
};

} // namespace
