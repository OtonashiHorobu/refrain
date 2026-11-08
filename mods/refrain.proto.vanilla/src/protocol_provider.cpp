// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Protocol 777 provider: edition/version descriptor plus the connection codec
// (framing, compression, AES-128-CFB8). Packet semantics and the state machine
// remain with the network/session layer during the migration.

#include <refrain/contracts/abi.hpp>
#include <refrain/contracts/services/net_protocol_abi.h>
#include <refrain/mod/host.hpp>
#include <refrain/mod/static_mod.hpp>
#include <refrain/net/base.hpp>
#include <refrain/protocol/current.hpp>
#include <refrain/protocol/java777_codec.hpp>

#include <cstdint>
#include <span>
#include <string_view>

namespace {

using refrain::contracts::as_bytes;
using refrain::contracts::as_string_view;

struct protocol_state {
    refrain::protocol::java777_codec codec{};
    std::vector<std::byte> current_tab{};
};

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
    static_cast<protocol_state *>(self)->codec.create_connection(connection);
}

void close_connection(void *self, std::uint64_t connection) {
    static_cast<protocol_state *>(self)->codec.close_connection(connection);
}

auto set_compression(void *self, std::uint64_t connection,
                     std::int32_t threshold) -> refrain_status {
    auto result{static_cast<protocol_state *>(self)->codec.set_compression(
        connection, threshold)};
    return result
               ? REFRAIN_STATUS_OK
               : REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_ERROR_UNSUPPORTED_VERSION;
}

auto set_cipher(void *self, std::uint64_t connection, refrain_bytes key,
                refrain_bytes iv) -> refrain_status {
    const auto key_bytes{as_bytes(key)};
    const auto iv_bytes{as_bytes(iv)};
    auto result{static_cast<protocol_state *>(self)->codec.set_cipher(
        connection,
        std::span<const std::byte>{
            reinterpret_cast<const std::byte *>(key_bytes.data()),
            key_bytes.size()},
        std::span<const std::byte>{
            reinterpret_cast<const std::byte *>(iv_bytes.data()),
            iv_bytes.size()})};
    return result
               ? REFRAIN_STATUS_OK
               : REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_ERROR_MALFORMED_PACKET;
}

auto feed(void *self, std::uint64_t connection, refrain_bytes data)
    -> refrain_status {
    const auto bytes{as_bytes(data)};
    auto result{static_cast<protocol_state *>(self)->codec.feed(
        connection,
        std::span<const std::byte>{
            reinterpret_cast<const std::byte *>(bytes.data()), bytes.size()})};
    return result
               ? REFRAIN_STATUS_OK
               : REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_ERROR_MALFORMED_PACKET;
}

auto next_packet(void *self, std::uint64_t connection,
                 refrain_services_net_protocol_decoded_packet *out,
                 std::uint8_t *result) -> refrain_status {
    if (out == nullptr || result == nullptr) {
        return REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_ERROR_MALFORMED_PACKET;
    }
    std::uint32_t packet_id{0};
    std::span<const std::byte> payload{};
    auto next{static_cast<protocol_state *>(self)->codec.next_packet(
        connection, packet_id, payload)};
    if (!next) {
        return REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_ERROR_MALFORMED_PACKET;
    }
    if (!*next) {
        *result = 0;
        return REFRAIN_STATUS_OK;
    }
    out->struct_size = REFRAIN_SERVICES_NET_PROTOCOL_DECODED_PACKET_SIZE;
    out->header.struct_size = REFRAIN_SERVICES_NET_PROTOCOL_FRAME_HEADER_SIZE;
    out->header.length = static_cast<std::uint32_t>(payload.size());
    out->header.uncompressed_length = out->header.length;
    out->packet.struct_size = REFRAIN_SERVICES_NET_PROTOCOL_PACKET_VIEW_SIZE;
    out->packet.packet_id = packet_id;
    out->packet.payload =
        refrain::contracts::to_borrowed(std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t *>(payload.data()),
            payload.size()});
    *result = 1;
    return REFRAIN_STATUS_OK;
}

auto send_packet(void *self, std::uint64_t connection,
                 const refrain_services_net_protocol_packet_view *packet)
    -> refrain_status {
    if (packet == nullptr) {
        return REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_ERROR_MALFORMED_PACKET;
    }
    const auto payload{as_bytes(packet->payload)};
    auto result{static_cast<protocol_state *>(self)->codec.send_packet(
        connection, packet->packet_id,
        std::span<const std::byte>{
            reinterpret_cast<const std::byte *>(payload.data()),
            payload.size()})};
    return result
               ? REFRAIN_STATUS_OK
               : REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_ERROR_MALFORMED_PACKET;
}

auto take_outbound(void *self, std::uint64_t connection,
                   refrain_services_net_protocol_frame_chunk *out)
    -> std::uint8_t {
    if (out == nullptr) {
        return 0;
    }
    std::span<const std::byte> data{};
    if (!static_cast<protocol_state *>(self)->codec.take_outbound(connection,
                                                                  data)) {
        return 0;
    }
    out->struct_size = REFRAIN_SERVICES_NET_PROTOCOL_FRAME_CHUNK_SIZE;
    out->data = refrain::contracts::to_borrowed(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t *>(data.data()), data.size()});
    return 1;
}

auto state_of(void *, refrain_core_handle_connection)
    -> refrain_services_net_protocol_connection_state {
    return REFRAIN_SERVICES_NET_PROTOCOL_CONNECTION_STATE_HANDSHAKE;
}

auto encode_tab(void *self, refrain_str header, refrain_str footer,
                refrain_services_net_protocol_encoded_packet *out)
    -> refrain_status {
    auto &state{*static_cast<protocol_state *>(self)};
    refrain::protocol::current::play::clientbound::tab_list packet{};
    packet.header.text = std::string{as_string_view(header)};
    packet.footer.text = std::string{as_string_view(footer)};
    state.current_tab.clear();
    refrain::net::writer writer{state.current_tab};
    writer.write_varint(
        refrain::protocol::current::play::clientbound::tab_list::packet_id);
    if (auto encoded{
            refrain::protocol::current::play::clientbound::encode_tab_list(
                writer, packet)};
        !encoded) {
        return REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_ERROR_MALFORMED_PACKET;
    }
    out->struct_size = REFRAIN_SERVICES_NET_PROTOCOL_ENCODED_PACKET_SIZE;
    out->data = refrain::contracts::to_borrowed(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t *>(state.current_tab.data()),
        state.current_tab.size()});
    return REFRAIN_STATUS_OK;
}

auto query_extension(void *self, refrain_str id, std::uint32_t min_version,
                     refrain_extension_ref *out) -> refrain_status {
    if (self == nullptr || out == nullptr) {
        return REFRAIN_EXTENSION_INVALID;
    }
    static const refrain_services_net_protocol_tab_codec_vtable tab_vtable{
        .struct_size = REFRAIN_SERVICES_NET_PROTOCOL_TAB_CODEC_VTABLE_SIZE,
        .encode_tab = &encode_tab,
    };
    if (as_string_view(id) != REFRAIN_SERVICES_NET_PROTOCOL_TAB_CODEC_ID) {
        return REFRAIN_EXTENSION_NOT_FOUND;
    }
    const auto version{
        (REFRAIN_SERVICES_NET_PROTOCOL_TAB_CODEC_ABI_MAJOR << 16U) |
        REFRAIN_SERVICES_NET_PROTOCOL_TAB_CODEC_ABI_MINOR};
    if (min_version > version) {
        return REFRAIN_EXTENSION_VERSION_MISMATCH;
    }
    out->struct_size = REFRAIN_EXTENSION_REF_SIZE;
    out->abi_major = REFRAIN_SERVICES_NET_PROTOCOL_TAB_CODEC_ABI_MAJOR;
    out->abi_minor = REFRAIN_SERVICES_NET_PROTOCOL_TAB_CODEC_ABI_MINOR;
    out->abi_patch = REFRAIN_SERVICES_NET_PROTOCOL_TAB_CODEC_ABI_PATCH;
    out->id = refrain::contracts::to_borrowed(
        REFRAIN_SERVICES_NET_PROTOCOL_TAB_CODEC_ID);
    out->vtable = &tab_vtable;
    out->self = self;
    return REFRAIN_STATUS_OK;
}

const refrain_services_net_protocol_vtable &vtable() {
    static const refrain_services_net_protocol_vtable table{
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
    return table;
}

auto make_protocol(refrain::mod::host &host) -> refrain::status {
    static protocol_state state{};
    const refrain::mod::provider_api api{
        .vtable = &vtable(),
        .self = &state,
        .abi_major = REFRAIN_SERVICES_NET_PROTOCOL_ABI_MAJOR,
    };
    return host.register_provider("refrain:net.protocol", "java_777", api,
                                  "1.0.0");
}

} // namespace

REFR_MOD_STATIC("refrain.proto.vanilla", "1.0.0", 1, make_protocol);
