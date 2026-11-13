// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/protocol/java777_codec.hpp>

#include <refrain/net/base.hpp>
#include <refrain/net/crypto.hpp>
#include <refrain/net/frame.hpp>

#include <algorithm>
#include <array>
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

/// Framing, cipher and queues of one connection. `mutex` guards every member
/// and is the only lock on the encode path, so two connections never wait for
/// each other. A lookup copies the owning `shared_ptr` under its shard lock and
/// releases that lock before this one is taken, so a connection closed in
/// between stays alive for the call in flight.
struct connection_codec {
    std::mutex mutex{};
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
    /// The id-to-session table is sharded the way the network server shards its
    /// connection table: a lookup contends only with the connections hashing to
    /// the same shard, and membership changes are rare compared with sends.
    ///
    /// A connection's codec and its protocol state share a shard and its lock,
    /// so the pair is never observed half-updated and one connection cannot be
    /// encoded while another connection's state changes underneath it.
    ///
    /// Lock order (strict): a caller holds at most one shard mutex, and never
    /// takes a session mutex while holding it. `find` copies the `shared_ptr`
    /// under the shard lock, releases the shard lock, and only then the caller
    /// takes the session mutex. No path holds two shard mutexes, and no path
    /// takes a shard mutex while holding a session mutex.
    static constexpr std::size_t shard_count{64};
    struct shard {
        mutable std::mutex mutex{};
        std::unordered_map<std::uint64_t, std::shared_ptr<connection_codec>>
            connections{};
        std::unordered_map<std::uint64_t, net::connection_state> states{};
    };
    std::array<shard, shard_count> shards{};
    encode_probe probe{};

    [[nodiscard]] static constexpr auto
    shard_for(std::uint64_t connection) noexcept -> std::size_t {
        return static_cast<std::size_t>(connection) % shard_count;
    }
    [[nodiscard]] auto bucket(std::uint64_t connection) -> shard & {
        return shards[shard_for(connection)];
    }
    /// Find one session and release the shard lock before returning it.
    [[nodiscard]] auto find(std::uint64_t connection)
        -> std::shared_ptr<connection_codec> {
        auto &owner{bucket(connection)};
        const std::lock_guard lock{owner.mutex};
        const auto found{owner.connections.find(connection)};
        if (found == owner.connections.end()) {
            return {};
        }
        return found->second;
    }
};

java777_codec::java777_codec() : java777_codec(encode_probe{}) {}

java777_codec::java777_codec(encode_probe probe)
    : impl_{std::make_unique<impl>()} {
    impl_->probe = std::move(probe);
}

java777_codec::~java777_codec() = default;

void java777_codec::create_connection(std::uint64_t connection) {
    auto &owner{impl_->bucket(connection)};
    const std::lock_guard lock{owner.mutex};
    owner.connections[connection] = std::make_shared<connection_codec>();
    owner.states[connection] = net::connection_state::handshake;
}

void java777_codec::close_connection(std::uint64_t connection) {
    auto &owner{impl_->bucket(connection)};
    const std::lock_guard lock{owner.mutex};
    owner.connections.erase(connection);
    owner.states.erase(connection);
}

auto java777_codec::set_compression(std::uint64_t connection,
                                    std::int32_t threshold) -> status {
    const auto codec{impl_->find(connection)};
    if (!codec) {
        return codec_failure("unknown protocol connection");
    }
    const std::lock_guard lock{codec->mutex};
    codec->decoder.set_threshold(threshold);
    codec->encoder.set_threshold(threshold);
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
    const auto codec{impl_->find(connection)};
    if (!codec) {
        return codec_failure("unknown protocol connection");
    }
    const std::lock_guard lock{codec->mutex};
    codec->cipher = std::make_unique<net::aes128_cfb8>(std::move(*cipher));
    return status_ok();
}

auto java777_codec::feed(std::uint64_t connection,
                         std::span<const std::byte> data) -> status {
    const auto codec{impl_->find(connection)};
    if (!codec) {
        return codec_failure("unknown protocol connection");
    }
    const std::lock_guard lock{codec->mutex};
    std::vector<std::byte> plain{};
    if (codec->cipher) {
        auto decrypted{codec->cipher->decrypt(data)};
        if (!decrypted) {
            return std::unexpected{std::move(decrypted.error())};
        }
        plain = std::move(*decrypted);
    } else {
        plain.assign(data.begin(), data.end());
    }
    return codec->decoder.push(plain);
}

auto java777_codec::next_packet(std::uint64_t connection,
                                std::uint32_t &packet_id,
                                std::span<const std::byte> &payload)
    -> result<bool> {
    const auto codec{impl_->find(connection)};
    if (!codec) {
        return std::unexpected{
            make_error(error_code::not_found, "unknown protocol connection")};
    }
    const std::lock_guard lock{codec->mutex};
    auto decoded{codec->decoder.next(codec->current_body)};
    if (!decoded) {
        return std::unexpected{std::move(decoded.error())};
    }
    if (!*decoded) {
        return false;
    }
    net::reader reader{codec->current_body};
    auto id{reader.read_varint()};
    if (!id) {
        return std::unexpected{
            make_error(error_code::parse_error, "missing packet id")};
    }
    packet_id = *id;
    payload = std::span<const std::byte>{
        codec->current_body.data() + reader.position(), reader.remaining()};
    return true;
}

auto java777_codec::send_packet(std::uint64_t connection,
                                std::uint32_t packet_id,
                                std::span<const std::byte> payload) -> status {
    const auto codec{impl_->find(connection)};
    if (!codec) {
        return codec_failure("unknown protocol connection");
    }
    const std::lock_guard lock{codec->mutex};
    if (impl_->probe) {
        impl_->probe(connection);
    }
    std::vector<std::byte> body{};
    net::writer writer{body};
    writer.write_varint(packet_id);
    writer.write_bytes(payload);
    std::vector<std::byte> frame{};
    if (auto encoded{codec->encoder.encode(body, frame)}; !encoded) {
        return encoded;
    }
    if (codec->cipher) {
        auto encrypted{codec->cipher->encrypt(frame)};
        if (!encrypted) {
            return std::unexpected{std::move(encrypted.error())};
        }
        frame = std::move(*encrypted);
    }
    codec->outbound.push_back(std::move(frame));
    return status_ok();
}

auto java777_codec::send_shared_packet(
    std::span<const std::uint64_t> connections, std::uint32_t packet_id,
    std::span<const std::byte> payload) -> status {
    if (connections.empty()) {
        return status_ok();
    }
    std::vector<std::byte> body{};
    net::writer writer{body};
    writer.write_varint(packet_id);
    writer.write_bytes(payload);

    // The frame depends only on the connection's compression threshold, so it
    // is encoded once per distinct threshold. A connection with an active
    // cipher still needs its own ciphertext.
    std::vector<std::pair<int, std::vector<std::byte>>> frames{};
    for (const auto connection : connections) {
        const auto codec{impl_->find(connection)};
        if (!codec) {
            continue;
        }
        if (impl_->probe) {
            impl_->probe(connection);
        }
        const std::lock_guard lock{codec->mutex};
        const auto threshold{codec->encoder.threshold()};
        auto frame{std::find_if(frames.begin(), frames.end(),
                                [threshold](const auto &entry) {
                                    return entry.first == threshold;
                                })};
        if (frame == frames.end()) {
            std::vector<std::byte> encoded{};
            if (auto encoded_status{codec->encoder.encode(body, encoded)};
                !encoded_status) {
                return encoded_status;
            }
            frames.emplace_back(threshold, std::move(encoded));
            frame = std::prev(frames.end());
        }
        if (codec->cipher) {
            auto encrypted{codec->cipher->encrypt(frame->second)};
            if (!encrypted) {
                return std::unexpected{std::move(encrypted.error())};
            }
            codec->outbound.push_back(std::move(*encrypted));
        } else {
            codec->outbound.push_back(frame->second);
        }
    }
    return status_ok();
}

void java777_codec::notify_state(std::uint64_t connection,
                                 net::connection_state state) {
    auto &owner{impl_->bucket(connection)};
    const std::lock_guard lock{owner.mutex};
    owner.states[connection] = state;
}

auto java777_codec::transition(std::uint64_t connection,
                               net::protocol_event event)
    -> net::connection_state {
    auto &owner{impl_->bucket(connection)};
    const std::lock_guard lock{owner.mutex};
    auto &current{owner.states[connection]};
    switch (event) {
    case net::protocol_event::status_request:
        if (current == net::connection_state::handshake) {
            current = net::connection_state::status;
        }
        break;
    case net::protocol_event::login_request:
        if (current == net::connection_state::handshake) {
            current = net::connection_state::login;
        }
        break;
    case net::protocol_event::login_acknowledged:
        if (current == net::connection_state::login) {
            current = net::connection_state::configuration;
        }
        break;
    case net::protocol_event::configuration_acknowledged:
        if (current == net::connection_state::configuration) {
            current = net::connection_state::play;
        }
        break;
    }
    return current;
}

auto java777_codec::take_outbound(std::uint64_t connection,
                                  std::span<const std::byte> &data) -> bool {
    const auto codec{impl_->find(connection)};
    if (!codec) {
        return false;
    }
    const std::lock_guard lock{codec->mutex};
    if (codec->outbound.empty()) {
        return false;
    }
    codec->current_outbound = std::move(codec->outbound.front());
    codec->outbound.pop_front();
    data = codec->current_outbound;
    return true;
}

} // namespace refrain::protocol
