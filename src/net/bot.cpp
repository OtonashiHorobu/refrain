// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/net/bot.hpp>

#include <refrain/net/base.hpp>
#include <refrain/net/crypto.hpp>
#include <refrain/net/frame.hpp>
#include <refrain/net/wire.hpp>
#include <refrain/schema/protocol/777/common.hpp>
#include <refrain/schema/protocol/777/configuration_clientbound.hpp>
#include <refrain/schema/protocol/777/configuration_serverbound.hpp>
#include <refrain/schema/protocol/777/handshake_serverbound.hpp>
#include <refrain/schema/protocol/777/login_clientbound.hpp>
#include <refrain/schema/protocol/777/login_serverbound.hpp>
#include <refrain/schema/protocol/777/play_clientbound.hpp>
#include <refrain/schema/protocol/777/play_serverbound.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace refrain::net {

namespace {

namespace hs_sb = ::refrain::protocol::v777::handshake::serverbound;
namespace login_sb = ::refrain::protocol::v777::login::serverbound;
namespace login_cb = ::refrain::protocol::v777::login::clientbound;
namespace config_sb = ::refrain::protocol::v777::configuration::serverbound;
namespace config_cb = ::refrain::protocol::v777::configuration::clientbound;
namespace play_cb = ::refrain::protocol::v777::play::clientbound;
namespace play_sb = ::refrain::protocol::v777::play::serverbound;
namespace common = ::refrain::protocol::v777::common;

[[nodiscard]] auto random_uuid() -> uuid {
    std::random_device device{};
    uuid value{};
    for (auto &byte : value.bytes) {
        byte = static_cast<std::uint8_t>(device() & 0xFFU);
    }
    value.bytes[6] =
        static_cast<std::uint8_t>((value.bytes[6] & 0x0FU) | 0x40U);
    value.bytes[8] =
        static_cast<std::uint8_t>((value.bytes[8] & 0x3FU) | 0x80U);
    return value;
}

[[nodiscard]] auto uuid_halves(const uuid &value)
    -> std::pair<std::int64_t, std::int64_t> {
    std::uint64_t high{0};
    std::uint64_t low{0};
    for (std::size_t index{0}; index < 8; ++index) {
        high = (high << 8U) | value.bytes[index];
        low = (low << 8U) | value.bytes[index + 8U];
    }
    return {static_cast<std::int64_t>(high), static_cast<std::int64_t>(low)};
}

enum class bot_state : std::uint8_t { login, configuration, play };

class bot_session {
  public:
    explicit bot_session(bot_options options)
        : options_{std::move(options)}, profile_{random_uuid()} {}

    [[nodiscard]] auto run() -> result<bot_report> {
        auto transport{transport::open(
            {.on_open =
                 [this](std::uint64_t id) {
                     connection_ = id;
                     send_intention();
                     send_hello();
                 },
             .on_data =
                 [this](std::uint64_t, std::span<const std::byte> bytes) {
                     on_data(bytes);
                 },
             .on_close =
                 [this](std::uint64_t, std::string_view) {
                     std::lock_guard lock{mutex_};
                     if (!joined_) {
                         fail_locked("connection closed");
                     }
                 },
             .on_error =
                 [this](std::string_view message) {
                     std::lock_guard lock{mutex_};
                     fail_locked(std::string{message});
                 }},
            1)};
        if (!transport) {
            return std::unexpected{std::move(transport.error())};
        }
        transport_ = std::move(*transport);
        auto connection{transport_->connect(options_.target)};
        if (!connection) {
            transport_->stop();
            return std::unexpected{std::move(connection.error())};
        }
        connection_ = *connection;

        {
            std::unique_lock lock{mutex_};
            if (!condition_.wait_for(lock, options_.timeout,
                                     [this] { return done_; })) {
                fail_locked("timed out waiting for play state");
            }
        }
        if (joined_ && options_.linger.count() > 0) {
            std::this_thread::sleep_for(options_.linger);
        }
        transport_->stop();
        transport_.reset();

        std::lock_guard lock{mutex_};
        if (!error_.empty()) {
            return std::unexpected{
                make_error(error_code::io_error, std::move(error_))};
        }
        return bot_report{.packets = packets_,
                          .chunks = chunks_,
                          .name = std::move(name_),
                          .messages = std::move(messages_)};
    }

  private:
    void send_intention() {
        hs_sb::intention packet{};
        packet.protocol = 777;
        packet.host = options_.target.host;
        packet.port = options_.target.port;
        packet.next = 2;
        send_packet(hs_sb::intention::packet_id, packet,
                    hs_sb::encode_intention);
    }

    void send_hello() {
        const auto [high, low]{uuid_halves(profile_)};
        login_sb::hello packet{};
        packet.name = options_.username;
        packet.profile_high = high;
        packet.profile_low = low;
        send_packet(login_sb::hello::packet_id, packet, login_sb::encode_hello);
    }

    void send_client_information() {
        config_sb::client_information packet{};
        packet.information.language = "en_us";
        packet.information.view_distance = options_.view_distance;
        packet.information.visibility = common::chat_visibility::full;
        packet.information.chat_colors = true;
        packet.information.model_customization = 0;
        packet.information.main_hand = common::humanoid_arm::right;
        packet.information.text_filtering = false;
        packet.information.allows_listing = true;
        packet.information.particles = common::particle_status::all;
        send_packet(config_sb::client_information::packet_id, packet,
                    config_sb::encode_client_information);
    }

    template <typename Packet, typename Encoder>
    void send_packet(std::uint16_t packet_id, const Packet &packet,
                     Encoder encode) {
        std::vector<std::byte> body;
        writer out{body};
        out.write_varint(packet_id);
        if (auto written{encode(out, packet)}; !written) {
            std::lock_guard lock{mutex_};
            fail_locked(written.error().message);
            return;
        }
        std::vector<std::byte> frame;
        if (auto framed{encoder_.encode(body, frame)}; !framed) {
            std::lock_guard lock{mutex_};
            fail_locked(framed.error().message);
            return;
        }
        if (cipher_) {
            auto encrypted{cipher_->encrypt(frame)};
            if (!encrypted) {
                std::lock_guard lock{mutex_};
                fail_locked(encrypted.error().message);
                return;
            }
            frame = std::move(*encrypted);
        }
        transport_->send(connection_, frame);
    }

    void on_data(std::span<const std::byte> bytes) {
        std::vector<std::byte> plain;
        std::span<const std::byte> framed{bytes};
        if (cipher_) {
            auto decrypted{cipher_->decrypt(bytes)};
            if (!decrypted) {
                fail("decryption failed");
                return;
            }
            plain = std::move(*decrypted);
            framed = plain;
        }
        if (auto pushed{decoder_.push(framed)}; !pushed) {
            std::lock_guard lock{mutex_};
            fail_locked(pushed.error().message);
            return;
        }
        while (true) {
            std::vector<std::byte> body;
            auto next{decoder_.next(body)};
            if (!next) {
                std::lock_guard lock{mutex_};
                fail_locked(next.error().message);
                return;
            }
            if (!*next) {
                return;
            }
            {
                std::lock_guard lock{mutex_};
                ++packets_;
            }
            handle(body);
        }
    }

    void handle(std::span<const std::byte> body) {
        reader in{body};
        auto packet_id{in.read_varint()};
        if (!packet_id) {
            std::lock_guard lock{mutex_};
            fail_locked("malformed packet");
            return;
        }
        if (state_ == bot_state::login) {
            handle_login(*packet_id, in);
        } else if (state_ == bot_state::configuration) {
            handle_configuration(*packet_id, in);
        } else {
            handle_play(*packet_id, in);
        }
    }

    void handle_login(std::uint32_t packet_id, reader &in) {
        if (packet_id == login_cb::login_compression::packet_id) {
            login_cb::login_compression packet{};
            if (auto decoded{login_cb::decode_login_compression(in, packet)};
                !decoded) {
                fail("malformed login_compression");
                return;
            }
            encoder_.set_threshold(static_cast<int>(packet.threshold));
            decoder_.set_threshold(static_cast<int>(packet.threshold));
            return;
        }
        if (packet_id == login_cb::login_finished::packet_id) {
            login_cb::login_finished packet{};
            if (auto decoded{login_cb::decode_login_finished(in, packet)};
                !decoded) {
                fail("malformed login_finished");
                return;
            }
            name_ = packet.name;
            login_sb::login_acknowledged ack{};
            send_packet(login_sb::login_acknowledged::packet_id, ack,
                        login_sb::encode_login_acknowledged);
            send_client_information();
            state_ = bot_state::configuration;
            return;
        }
        if (packet_id == login_cb::login_disconnect::packet_id) {
            login_cb::login_disconnect packet{};
            static_cast<void>(login_cb::decode_login_disconnect(in, packet));
            fail("server rejected login: " + packet.reason);
            return;
        }
        if (packet_id == login_cb::hello::packet_id) {
            login_cb::hello packet{};
            if (auto decoded{login_cb::decode_hello(in, packet)}; !decoded) {
                fail("malformed login hello");
                return;
            }
            std::vector<std::byte> secret(aes128_key_bytes);
            std::random_device device{};
            for (auto &byte : secret) {
                byte = static_cast<std::byte>(device() & 0xFFU);
            }
            auto encrypted_secret{
                rsa_encrypt_pkcs1_v1_5(packet.public_key, secret)};
            auto encrypted_challenge{
                rsa_encrypt_pkcs1_v1_5(packet.public_key, packet.challenge)};
            if (!encrypted_secret || !encrypted_challenge) {
                fail("failed to encrypt the login key");
                return;
            }
            login_sb::key key_packet{};
            key_packet.keybytes = std::move(*encrypted_secret);
            key_packet.encrypted_challenge = std::move(*encrypted_challenge);
            send_packet(login_sb::key::packet_id, key_packet,
                        login_sb::encode_key);
            auto cipher{aes128_cfb8::create(secret, secret)};
            if (!cipher) {
                fail("failed to create the connection cipher");
                return;
            }
            cipher_ = std::make_unique<aes128_cfb8>(std::move(*cipher));
        }
    }

    void handle_configuration(std::uint32_t packet_id, reader &in) {
        if (packet_id == config_cb::select_known_packs::packet_id) {
            config_cb::select_known_packs packet{};
            if (auto decoded{config_cb::decode_select_known_packs(in, packet)};
                !decoded) {
                fail("malformed select_known_packs");
                return;
            }
            config_sb::select_known_packs response{};
            send_packet(config_sb::select_known_packs::packet_id, response,
                        config_sb::encode_select_known_packs);
            return;
        }
        if (packet_id == config_cb::finish_configuration::packet_id) {
            config_sb::finish_configuration ack{};
            send_packet(config_sb::finish_configuration::packet_id, ack,
                        config_sb::encode_finish_configuration);
            state_ = bot_state::play;
            return;
        }
        if (packet_id == config_cb::keep_alive::packet_id) {
            config_cb::keep_alive packet{};
            if (auto decoded{config_cb::decode_keep_alive(in, packet)};
                !decoded) {
                fail("malformed configuration keep_alive");
                return;
            }
            config_sb::keep_alive response{.id = packet.id};
            send_packet(config_sb::keep_alive::packet_id, response,
                        config_sb::encode_keep_alive);
            return;
        }
        if (packet_id == config_cb::disconnect::packet_id) {
            fail("server disconnected during configuration");
        }
    }

    void handle_play(std::uint32_t packet_id, reader &in) {
        if (packet_id == play_cb::level_chunk_with_light::packet_id) {
            std::lock_guard lock{mutex_};
            ++chunks_;
            if (joined_ && chunks_ >= options_.wait_chunks &&
                messages_.size() >= options_.wait_messages) {
                done_ = true;
                condition_.notify_one();
            }
            return;
        }
        if (packet_id == play_cb::system_chat::packet_id) {
            play_cb::system_chat message_packet{};
            if (auto decoded{play_cb::decode_system_chat(in, message_packet)};
                !decoded) {
                fail("malformed system_chat");
                return;
            }
            std::lock_guard lock{mutex_};
            messages_.push_back(message_packet.content.text);
            if (joined_ && chunks_ >= options_.wait_chunks &&
                messages_.size() >= options_.wait_messages) {
                done_ = true;
                condition_.notify_one();
            }
            return;
        }
        if (packet_id == play_cb::keep_alive::packet_id) {
            play_cb::keep_alive keepalive_packet{};
            if (auto decoded{play_cb::decode_keep_alive(in, keepalive_packet)};
                !decoded) {
                fail("malformed play keep_alive");
                return;
            }
            play_sb::keep_alive response{.id = keepalive_packet.id};
            send_packet(play_sb::keep_alive::packet_id, response,
                        play_sb::encode_keep_alive);
            return;
        }
        if (packet_id == play_cb::login::packet_id) {
            play_cb::login packet{};
            if (auto decoded{play_cb::decode_login(in, packet)}; !decoded) {
                fail("malformed play login");
                return;
            }
            {
                std::lock_guard lock{mutex_};
                joined_ = true;
                if (options_.wait_chunks == 0 && options_.wait_messages == 0) {
                    done_ = true;
                    condition_.notify_one();
                }
            }
            if (!options_.command.empty()) {
                std::string command{options_.command};
                if (!command.empty() && command.front() == '/') {
                    command.erase(command.begin());
                }
                play_sb::chat_command command_packet{};
                command_packet.command = std::move(command);
                send_packet(play_sb::chat_command::packet_id, command_packet,
                            play_sb::encode_chat_command);
            }
        }
    }

    void fail(std::string message) {
        std::lock_guard lock{mutex_};
        fail_locked(std::move(message));
    }

    void fail_locked(std::string message) {
        if (error_.empty()) {
            error_ = std::move(message);
        }
        done_ = true;
        condition_.notify_one();
    }

    bot_options options_{};
    uuid profile_{};
    std::unique_ptr<transport> transport_{};
    std::uint64_t connection_{0};
    frame_encoder encoder_{-1};
    frame_decoder decoder_{-1};
    std::unique_ptr<aes128_cfb8> cipher_{};
    bot_state state_{bot_state::login};
    std::mutex mutex_{};
    std::condition_variable condition_{};
    bool done_{false};
    bool joined_{false};
    std::size_t packets_{0};
    std::size_t chunks_{0};
    std::string error_{};
    std::string name_{};
    std::vector<std::string> messages_{};
};

} // namespace

auto run_bot(const bot_options &options) -> result<bot_report> {
    bot_session session{options};
    return session.run();
}

} // namespace refrain::net
