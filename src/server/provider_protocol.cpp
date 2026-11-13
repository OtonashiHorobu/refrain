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
#include <vector>

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
    : provider_{std::move(provider)} {
    if (!provider_) {
        return;
    }
    // The extension is optional: a provider that does not expose a broadcast
    // encoder keeps the per-recipient fallback of `send_shared_packet`.
    if (auto codec{
            provider_->extension<services::net_protocol::broadcast_codec>()}) {
        broadcast_ = std::move(*codec);
    }
}

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

auto provider_protocol::send_shared_packet(
    std::span<const std::uint64_t> connections, std::uint32_t packet_id,
    std::span<const std::byte> payload) -> status {
    if (connections.empty()) {
        return status_ok();
    }
    if (!broadcast_) {
        return net::protocol_codec::send_shared_packet(connections, packet_id,
                                                       payload);
    }
    cohort_.clear();
    cohort_.reserve(connections.size());
    for (const auto connection : connections) {
        auto reference{services::net_protocol::make_connection_ref()};
        reference.connection = connection;
        cohort_.push_back(reference);
    }
    auto view{services::net_protocol::make_packet_view()};
    view.packet_id = packet_id;
    view.payload = contracts::to_borrowed(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t *>(payload.data()),
        payload.size()});
    auto sent{broadcast_->send_shared(cohort_, view)};
    if (!sent) {
        return protocol_error(sent.error());
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

auto provider_protocol::transition(std::uint64_t connection,
                                   net::protocol_event event)
    -> net::connection_state {
    return static_cast<net::connection_state>(provider_->transition(
        connection,
        static_cast<services::net_protocol::protocol_event>(event)));
}

auto make_provider_protocol(
    std::unique_ptr<services::net_protocol::provider> provider)
    -> std::unique_ptr<net::protocol_codec> {
    return std::make_unique<provider_protocol>(std::move(provider));
}

} // namespace refrain::server
