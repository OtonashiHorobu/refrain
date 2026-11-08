// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Protocol 777 connection codec provider: framing, compression and the
// AES-128-CFB8 connection cipher. Packet semantics and the state machine stay
// with the session/network layer during the migration.

#include <refrain/contracts/abi.hpp>
#include <refrain/contracts/services/net_protocol_abi.h>
#include <refrain/mod/host.hpp>
#include <refrain/mod/static_mod.hpp>
#include <refrain/net/base.hpp>
#include <refrain/net/crypto.hpp>
#include <refrain/net/frame.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using refrain::contracts::as_bytes;
using refrain::contracts::as_string_view;

struct connection_codec {
    refrain::net::frame_decoder decoder{-1};
    refrain::net::frame_encoder encoder{-1};
    std::unique_ptr<refrain::net::aes128_cfb8> cipher{};
    std::vector<std::byte> current_body{};
    std::deque<std::vector<std::byte>> outbound{};
    std::vector<std::byte> current_outbound{};
};

struct protocol_state {
    std::mutex mutex{};
    std::unordered_map<std::uint64_t, std::unique_ptr<connection_codec>>
        connections{};
};

[[nodiscard]] auto protocol_error(refrain::error_code code) -> refrain_status {
    switch (code) {
    case refrain::error_code::invalid_argument:
        return REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_ERROR_MALFORMED_PACKET;
    case refrain::error_code::parse_error:
        return REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_ERROR_MALFORMED_PACKET;
    case refrain::error_code::unsupported:
        return REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_ERROR_UNSUPPORTED_VERSION;
    default:
        return REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_ERROR_COMPRESSION_FAILED;
    }
}

void protocol_descriptor(
    void *self, refrain_services_net_protocol_protocol_descriptor *out) {
    static_cast<void>(self);
    if (out == nullptr) {
        return;
    }
    out->struct_size = REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_DESCRIPTOR_SIZE;
    out->edition = REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_EDITION_JAVA;
    static constexpr std::string_view family{"minecraft-java"};
    out->family = refrain::contracts::to_borrowed(family);
    out->canonical_version = 777U;
    static constexpr std::string_view versions{"777"};
    out->accepted_versions = refrain::contracts::to_borrowed(versions);
    out->transport_protocol = 0U;
    out->requires_stream = 1;
    static constexpr std::string_view capabilities{
        "frame-compression,registry-sync"};
    out->capabilities = refrain::contracts::to_borrowed(capabilities);
}

auto protocol_id(void *self) -> std::uint32_t {
    static_cast<void>(self);
    return 777U;
}

auto accepts_version(void *self, std::uint32_t protocol) -> std::uint8_t {
    static_cast<void>(self);
    return protocol == 777U ? 1U : 0U;
}

void create_connection(void *self, std::uint64_t connection) {
    if (self == nullptr) {
        return;
    }
    auto &state{*static_cast<protocol_state *>(self)};
    const std::lock_guard lock{state.mutex};
    state.connections[connection] = std::make_unique<connection_codec>();
}

void close_connection(void *self, std::uint64_t connection) {
    if (self == nullptr) {
        return;
    }
    auto &state{*static_cast<protocol_state *>(self)};
    const std::lock_guard lock{state.mutex};
    state.connections.erase(connection);
}

auto set_compression(void *self, std::uint64_t connection,
                     std::int32_t threshold) -> refrain_status {
    if (self == nullptr) {
        return REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_ERROR_MALFORMED_PACKET;
    }
    auto &state{*static_cast<protocol_state *>(self)};
    const std::lock_guard lock{state.mutex};
    const auto found{state.connections.find(connection)};
    if (found == state.connections.end()) {
        return REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_ERROR_MALFORMED_PACKET;
    }
    found->second->decoder.set_threshold(threshold);
    found->second->encoder.set_threshold(threshold);
    return REFRAIN_STATUS_OK;
}

auto set_cipher(void *self, std::uint64_t connection, refrain_bytes key,
                refrain_bytes iv) -> refrain_status {
    if (self == nullptr) {
        return REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_ERROR_MALFORMED_PACKET;
    }
    const auto key_bytes{as_bytes(key)};
    const auto iv_bytes{as_bytes(iv)};
    if (key_bytes.size() != refrain::net::aes128_key_bytes ||
        iv_bytes.size() != refrain::net::aes128_key_bytes) {
        return REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_ERROR_MALFORMED_PACKET;
    }
    auto cipher{refrain::net::aes128_cfb8::create(
        std::span<const std::byte>{
            reinterpret_cast<const std::byte *>(key_bytes.data()),
            key_bytes.size()},
        std::span<const std::byte>{
            reinterpret_cast<const std::byte *>(iv_bytes.data()),
            iv_bytes.size()})};
    if (!cipher) {
        return REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_ERROR_MALFORMED_PACKET;
    }
    auto &state{*static_cast<protocol_state *>(self)};
    const std::lock_guard lock{state.mutex};
    const auto found{state.connections.find(connection)};
    if (found == state.connections.end()) {
        return REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_ERROR_MALFORMED_PACKET;
    }
    found->second->cipher =
        std::make_unique<refrain::net::aes128_cfb8>(std::move(*cipher));
    return REFRAIN_STATUS_OK;
}

auto feed(void *self, std::uint64_t connection, refrain_bytes data)
    -> refrain_status {
    if (self == nullptr) {
        return REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_ERROR_MALFORMED_PACKET;
    }
    auto &state{*static_cast<protocol_state *>(self)};
    std::vector<std::byte> plain{};
    {
        const std::lock_guard lock{state.mutex};
        const auto found{state.connections.find(connection)};
        if (found == state.connections.end()) {
            return REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_ERROR_MALFORMED_PACKET;
        }
        auto &codec{*found->second};
        const auto bytes{as_bytes(data)};
        const std::span<const std::byte> framed{
            reinterpret_cast<const std::byte *>(bytes.data()), bytes.size()};
        if (codec.cipher) {
            auto decrypted{codec.cipher->decrypt(framed)};
            if (!decrypted) {
                return REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_ERROR_MALFORMED_PACKET;
            }
            plain = std::move(*decrypted);
        } else {
            plain.assign(framed.begin(), framed.end());
        }
        if (auto pushed{codec.decoder.push(plain)}; !pushed) {
            return protocol_error(pushed.error().code);
        }
    }
    return REFRAIN_STATUS_OK;
}

auto next_packet(void *self, std::uint64_t connection,
                 refrain_services_net_protocol_decoded_packet *out,
                 std::uint8_t *result) -> refrain_status {
    if (self == nullptr || out == nullptr || result == nullptr) {
        return REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_ERROR_MALFORMED_PACKET;
    }
    auto &state{*static_cast<protocol_state *>(self)};
    const std::lock_guard lock{state.mutex};
    const auto found{state.connections.find(connection)};
    if (found == state.connections.end()) {
        return REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_ERROR_MALFORMED_PACKET;
    }
    auto &codec{*found->second};
    auto decoded{codec.decoder.next(codec.current_body)};
    if (!decoded) {
        return protocol_error(decoded.error().code);
    }
    if (!*decoded) {
        *result = 0;
        return REFRAIN_STATUS_OK;
    }
    refrain::net::reader reader{codec.current_body};
    auto packet_id{reader.read_varint()};
    if (!packet_id) {
        return REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_ERROR_MALFORMED_PACKET;
    }
    const auto *payload{codec.current_body.data() + reader.position()};
    const auto payload_size{reader.remaining()};
    out->struct_size = REFRAIN_SERVICES_NET_PROTOCOL_DECODED_PACKET_SIZE;
    out->header.struct_size = REFRAIN_SERVICES_NET_PROTOCOL_FRAME_HEADER_SIZE;
    out->header.length = static_cast<std::uint32_t>(codec.current_body.size());
    out->header.uncompressed_length = out->header.length;
    out->packet.struct_size = REFRAIN_SERVICES_NET_PROTOCOL_PACKET_VIEW_SIZE;
    out->packet.packet_id = *packet_id;
    out->packet.payload =
        refrain::contracts::to_borrowed(std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t *>(payload), payload_size});
    *result = 1;
    return REFRAIN_STATUS_OK;
}

auto send_packet(void *self, std::uint64_t connection,
                 const refrain_services_net_protocol_packet_view *packet)
    -> refrain_status {
    if (self == nullptr || packet == nullptr) {
        return REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_ERROR_MALFORMED_PACKET;
    }
    auto &state{*static_cast<protocol_state *>(self)};
    const std::lock_guard lock{state.mutex};
    const auto found{state.connections.find(connection)};
    if (found == state.connections.end()) {
        return REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_ERROR_MALFORMED_PACKET;
    }
    std::vector<std::byte> body{};
    refrain::net::writer writer{body};
    writer.write_varint(packet->packet_id);
    const auto payload{as_bytes(packet->payload)};
    writer.write_bytes(std::span<const std::byte>{
        reinterpret_cast<const std::byte *>(payload.data()), payload.size()});
    auto &codec{*found->second};
    std::vector<std::byte> frame{};
    if (auto encoded{codec.encoder.encode(body, frame)}; !encoded) {
        return protocol_error(encoded.error().code);
    }
    if (codec.cipher) {
        auto encrypted{codec.cipher->encrypt(frame)};
        if (!encrypted) {
            return REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_ERROR_MALFORMED_PACKET;
        }
        frame = std::move(*encrypted);
    }
    codec.outbound.push_back(std::move(frame));
    return REFRAIN_STATUS_OK;
}

auto take_outbound(void *self, std::uint64_t connection,
                   refrain_services_net_protocol_frame_chunk *out)
    -> std::uint8_t {
    if (self == nullptr || out == nullptr) {
        return 0;
    }
    auto &state{*static_cast<protocol_state *>(self)};
    const std::lock_guard lock{state.mutex};
    const auto found{state.connections.find(connection)};
    if (found == state.connections.end() || found->second->outbound.empty()) {
        return 0;
    }
    auto &codec{*found->second};
    codec.current_outbound = std::move(codec.outbound.front());
    codec.outbound.pop_front();
    out->struct_size = REFRAIN_SERVICES_NET_PROTOCOL_FRAME_CHUNK_SIZE;
    out->data = refrain::contracts::to_borrowed(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t *>(codec.current_outbound.data()),
        codec.current_outbound.size()});
    return 1;
}

auto state_of(void *, refrain_core_handle_connection)
    -> refrain_services_net_protocol_connection_state {
    return REFRAIN_SERVICES_NET_PROTOCOL_CONNECTION_STATE_HANDSHAKE;
}

auto query_extension(void *, refrain_str, std::uint32_t,
                     refrain_extension_ref *) -> refrain_status {
    return REFRAIN_EXTENSION_NOT_FOUND;
}

refrain_services_net_protocol_vtable make_vtable() {
    return refrain_services_net_protocol_vtable{
        .struct_size = REFRAIN_SERVICES_NET_PROTOCOL_VTABLE_SIZE,
        .abi_major = REFRAIN_SERVICES_NET_PROTOCOL_ABI_MAJOR,
        .abi_minor = REFRAIN_SERVICES_NET_PROTOCOL_ABI_MINOR,
        .abi_patch = REFRAIN_SERVICES_NET_PROTOCOL_ABI_PATCH,
        .descriptor = &protocol_descriptor,
        .protocol_id = &protocol_id,
        .accepts_version = &accepts_version,
        .create_connection = &create_connection,
        .close_connection = &close_connection,
        .set_compression = &set_compression,
        .set_cipher = &set_cipher,
        .feed = &feed,
        .next_packet = &next_packet,
        .send_packet = &send_packet,
        .take_outbound = &take_outbound,
        .state_of = &state_of,
        .query_extension = &query_extension,
    };
}

auto make_protocol(refrain::mod::host &host) -> refrain::status {
    static protocol_state state{};
    static const auto vtable{make_vtable()};
    const refrain::mod::provider_api api{
        .vtable = &vtable,
        .self = &state,
        .abi_major = REFRAIN_SERVICES_NET_PROTOCOL_ABI_MAJOR,
    };
    return host.register_provider("refrain:net.protocol", "java_777", api,
                                  "1.0.0");
}

} // namespace

REFR_MOD_STATIC("refrain.proto.vanilla", "1.0.0", 1, make_protocol);
