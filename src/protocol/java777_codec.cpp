// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/protocol/java777_codec.hpp>

#include <refrain/net/base.hpp>
#include <refrain/net/crypto.hpp>
#include <refrain/net/frame.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace refrain::protocol {

namespace {

struct connection_codec {
    refrain::net::frame_decoder decoder{-1};
    refrain::net::frame_encoder encoder{-1};
    std::unique_ptr<refrain::net::aes128_cfb8> cipher{};
    std::vector<std::byte> current_body{};
    std::deque<std::vector<std::byte>> outbound{};
    std::vector<std::byte> current_outbound{};
};

[[nodiscard]] auto codec_failure(std::string_view message) -> refrain::status {
    return std::unexpected{make_error(error_code::parse_error, message)};
}

} // namespace

struct java777_codec::impl {
    std::mutex mutex{};
    std::unordered_map<std::uint64_t, std::unique_ptr<connection_codec>>
        connections{};
};

java777_codec::java777_codec() : impl_{std::make_unique<impl>()} {}
java777_codec::~java777_codec() = default;

void java777_codec::create_connection(std::uint64_t connection) {
    const std::lock_guard lock{impl_->mutex};
    impl_->connections[connection] = std::make_unique<connection_codec>();
}

void java777_codec::close_connection(std::uint64_t connection) {
    const std::lock_guard lock{impl_->mutex};
    impl_->connections.erase(connection);
}

auto java777_codec::set_compression(std::uint64_t connection,
                                    std::int32_t threshold) -> status {
    const std::lock_guard lock{impl_->mutex};
    const auto found{impl_->connections.find(connection)};
    if (found == impl_->connections.end()) {
        return codec_failure("unknown protocol connection");
    }
    found->second->decoder.set_threshold(threshold);
    found->second->encoder.set_threshold(threshold);
    return status_ok();
}

auto java777_codec::set_cipher(std::uint64_t connection,
                               std::span<const std::byte> key,
                               std::span<const std::byte> iv) -> status {
    if (key.size() != net::aes128_key_bytes ||
        iv.size() != net::aes128_key_bytes) {
        return codec_failure("invalid cipher key/iv size");
    }
    auto cipher{net::aes128_cfb8::create(key, iv)};
    if (!cipher) {
        return std::unexpected{std::move(cipher.error())};
    }
    const std::lock_guard lock{impl_->mutex};
    const auto found{impl_->connections.find(connection)};
    if (found == impl_->connections.end()) {
        return codec_failure("unknown protocol connection");
    }
    found->second->cipher =
        std::make_unique<net::aes128_cfb8>(std::move(*cipher));
    return status_ok();
}

auto java777_codec::feed(std::uint64_t connection,
                         std::span<const std::byte> data) -> status {
    std::vector<std::byte> plain{};
    const std::lock_guard lock{impl_->mutex};
    const auto found{impl_->connections.find(connection)};
    if (found == impl_->connections.end()) {
        return codec_failure("unknown protocol connection");
    }
    auto &codec{*found->second};
    if (codec.cipher) {
        auto decrypted{codec.cipher->decrypt(data)};
        if (!decrypted) {
            return std::unexpected{std::move(decrypted.error())};
        }
        plain = std::move(*decrypted);
    } else {
        plain.assign(data.begin(), data.end());
    }
    if (auto pushed{codec.decoder.push(plain)}; !pushed) {
        return pushed;
    }
    return status_ok();
}

auto java777_codec::next_packet(std::uint64_t connection,
                                std::uint32_t &packet_id,
                                std::span<const std::byte> &payload)
    -> result<bool> {
    const std::lock_guard lock{impl_->mutex};
    const auto found{impl_->connections.find(connection)};
    if (found == impl_->connections.end()) {
        return std::unexpected{
            make_error(error_code::not_found, "unknown protocol connection")};
    }
    auto &codec{*found->second};
    auto decoded{codec.decoder.next(codec.current_body)};
    if (!decoded) {
        return std::unexpected{std::move(decoded.error())};
    }
    if (!*decoded) {
        return false;
    }
    net::reader reader{codec.current_body};
    auto id{reader.read_varint()};
    if (!id) {
        return std::unexpected{
            make_error(error_code::parse_error, "missing packet id")};
    }
    packet_id = *id;
    payload = std::span<const std::byte>{
        codec.current_body.data() + reader.position(), reader.remaining()};
    return true;
}

auto java777_codec::send_packet(std::uint64_t connection,
                                std::uint32_t packet_id,
                                std::span<const std::byte> payload) -> status {
    const std::lock_guard lock{impl_->mutex};
    const auto found{impl_->connections.find(connection)};
    if (found == impl_->connections.end()) {
        return codec_failure("unknown protocol connection");
    }
    auto &codec{*found->second};
    std::vector<std::byte> body{};
    net::writer writer{body};
    writer.write_varint(packet_id);
    writer.write_bytes(payload);
    std::vector<std::byte> frame{};
    if (auto encoded{codec.encoder.encode(body, frame)}; !encoded) {
        return encoded;
    }
    if (codec.cipher) {
        auto encrypted{codec.cipher->encrypt(frame)};
        if (!encrypted) {
            return std::unexpected{std::move(encrypted.error())};
        }
        frame = std::move(*encrypted);
    }
    codec.outbound.push_back(std::move(frame));
    return status_ok();
}

void java777_codec::notify_state(std::uint64_t connection,
                                 net::connection_state state) {
    static_cast<void>(connection);
    static_cast<void>(state);
}

auto java777_codec::take_outbound(std::uint64_t connection,
                                  std::span<const std::byte> &data) -> bool {
    const std::lock_guard lock{impl_->mutex};
    const auto found{impl_->connections.find(connection)};
    if (found == impl_->connections.end() || found->second->outbound.empty()) {
        return false;
    }
    auto &codec{*found->second};
    codec.current_outbound = std::move(codec.outbound.front());
    codec.outbound.pop_front();
    data = codec.current_outbound;
    return true;
}

} // namespace refrain::protocol
