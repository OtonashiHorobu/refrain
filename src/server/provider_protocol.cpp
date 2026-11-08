// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/provider_protocol.hpp>

#include <refrain/contracts/abi.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

namespace refrain::server {

namespace {

[[nodiscard]] auto protocol_error(services::net_protocol::protocol_error error)
    -> status {
    static_cast<void>(error);
    return std::unexpected{
        make_error(error_code::parse_error, "protocol codec failure")};
}

} // namespace

provider_protocol::provider_protocol(
    std::unique_ptr<services::net_protocol::provider> provider)
    : provider_{std::move(provider)} {}

void provider_protocol::create_connection(std::uint64_t connection) {
    provider_->create_connection(connection);
}

void provider_protocol::close_connection(std::uint64_t connection) {
    provider_->close_connection(connection);
}

auto provider_protocol::set_compression(std::uint64_t connection,
                                        std::int32_t threshold) -> status {
    auto result{provider_->set_compression(connection, threshold)};
    if (!result) {
        return protocol_error(result.error());
    }
    return status_ok();
}

auto provider_protocol::set_cipher(std::uint64_t connection,
                                   std::span<const std::byte> key,
                                   std::span<const std::byte> iv) -> status {
    auto result{provider_->set_cipher(
        connection,
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t *>(key.data()), key.size()},
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t *>(iv.data()), iv.size()})};
    if (!result) {
        return protocol_error(result.error());
    }
    return status_ok();
}

auto provider_protocol::feed(std::uint64_t connection,
                             std::span<const std::byte> data) -> status {
    auto result{provider_->feed(
        connection,
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t *>(data.data()), data.size()})};
    if (!result) {
        return protocol_error(result.error());
    }
    return status_ok();
}

auto provider_protocol::next_packet(std::uint64_t connection,
                                    std::uint32_t &packet_id,
                                    std::span<const std::byte> &payload)
    -> result<bool> {
    auto decoded{services::net_protocol::make_decoded_packet()};
    auto result{provider_->next_packet(connection, decoded)};
    if (!result) {
        return std::unexpected{protocol_error(result.error()).error()};
    }
    if (!*result) {
        return false;
    }
    packet_id = decoded.packet.packet_id;
    const auto bytes{contracts::as_bytes(decoded.packet.payload)};
    payload = std::span<const std::byte>{
        reinterpret_cast<const std::byte *>(bytes.data()), bytes.size()};
    return true;
}

auto provider_protocol::send_packet(std::uint64_t connection,
                                    std::uint32_t packet_id,
                                    std::span<const std::byte> payload)
    -> status {
    auto view{services::net_protocol::make_packet_view()};
    view.packet_id = packet_id;
    view.payload = contracts::to_borrowed(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t *>(payload.data()),
        payload.size()});
    auto result{provider_->send_packet(connection, view)};
    if (!result) {
        return protocol_error(result.error());
    }
    return status_ok();
}

auto provider_protocol::take_outbound(std::uint64_t connection,
                                      std::span<const std::byte> &data)
    -> bool {
    auto chunk{services::net_protocol::make_frame_chunk()};
    if (!provider_->take_outbound(connection, chunk)) {
        return false;
    }
    const auto bytes{contracts::as_bytes(chunk.data)};
    data = std::span<const std::byte>{
        reinterpret_cast<const std::byte *>(bytes.data()), bytes.size()};
    return true;
}

void provider_protocol::notify_state(std::uint64_t connection,
                                     net::connection_state state) {
    provider_->notify_state(
        connection,
        static_cast<services::net_protocol::connection_state>(state));
}

auto make_provider_protocol(
    std::unique_ptr<services::net_protocol::provider> provider)
    -> std::unique_ptr<net::protocol_codec> {
    return std::make_unique<provider_protocol>(std::move(provider));
}

} // namespace refrain::server
