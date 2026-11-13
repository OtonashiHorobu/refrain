// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// The provider adapter is the seam between the network session's shared send
// and a `refrain:net.protocol` provider. A provider that exposes the broadcast
// extension must receive one call for a cohort; one that does not must keep the
// per-recipient fallback. Both are observed on a fake provider vtable.

#include <refrain/contracts/abi.hpp>
#include <refrain/contracts/services/net_protocol_abi.h>
#include <refrain/protocol/java777_codec.hpp>
#include <refrain/server/provider_protocol.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace net_protocol = refrain::services::net_protocol;

struct fake_state {
    std::size_t shared_calls{0};
    std::size_t single_calls{0};
    std::vector<std::uint64_t> connections{};
    std::uint32_t packet_id{0};
    std::vector<std::byte> payload{};
};

[[nodiscard]] auto
copy_payload(const refrain_services_net_protocol_packet_view *packet)
    -> std::vector<std::byte> {
    const auto bytes{refrain::contracts::as_bytes(packet->payload)};
    return std::vector<std::byte>{
        reinterpret_cast<const std::byte *>(bytes.data()),
        reinterpret_cast<const std::byte *>(bytes.data()) + bytes.size()};
}

refrain_status
fake_send_shared(void *self,
                 refrain_services_net_protocol_slice_connection_ref connections,
                 const refrain_services_net_protocol_packet_view *packet) {
    if (packet == nullptr) {
        return REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_ERROR_MALFORMED_PACKET;
    }
    auto &state{*static_cast<fake_state *>(self)};
    ++state.shared_calls;
    state.connections.clear();
    for (std::uint64_t index{0}; index < connections.len; ++index) {
        state.connections.push_back(connections.data[index].connection);
    }
    state.packet_id = packet->packet_id;
    state.payload = copy_payload(packet);
    return REFRAIN_STATUS_OK;
}

refrain_status
fake_send_packet(void *self, std::uint64_t connection,
                 const refrain_services_net_protocol_packet_view *packet) {
    if (packet == nullptr) {
        return REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_ERROR_MALFORMED_PACKET;
    }
    auto &state{*static_cast<fake_state *>(self)};
    ++state.single_calls;
    state.connections.push_back(connection);
    state.packet_id = packet->packet_id;
    state.payload = copy_payload(packet);
    return REFRAIN_STATUS_OK;
}

const refrain_services_net_protocol_broadcast_codec_vtable broadcast_vtable{
    REFRAIN_SERVICES_NET_PROTOCOL_BROADCAST_CODEC_VTABLE_SIZE,
    &fake_send_shared,
};

refrain_status query_broadcast(void *self, refrain_str id,
                               std::uint32_t min_version,
                               refrain_extension_ref *out) {
    if (out == nullptr ||
        refrain::contracts::as_string_view(id) !=
            REFRAIN_SERVICES_NET_PROTOCOL_BROADCAST_CODEC_ID) {
        return REFRAIN_EXTENSION_NOT_FOUND;
    }
    if (min_version >
        ((REFRAIN_SERVICES_NET_PROTOCOL_BROADCAST_CODEC_ABI_MAJOR << 16U) |
         REFRAIN_SERVICES_NET_PROTOCOL_BROADCAST_CODEC_ABI_MINOR)) {
        return REFRAIN_EXTENSION_VERSION_MISMATCH;
    }
    out->struct_size = REFRAIN_EXTENSION_REF_SIZE;
    out->abi_major = REFRAIN_SERVICES_NET_PROTOCOL_BROADCAST_CODEC_ABI_MAJOR;
    out->abi_minor = REFRAIN_SERVICES_NET_PROTOCOL_BROADCAST_CODEC_ABI_MINOR;
    out->abi_patch = REFRAIN_SERVICES_NET_PROTOCOL_BROADCAST_CODEC_ABI_PATCH;
    out->id = refrain::contracts::to_borrowed(
        REFRAIN_SERVICES_NET_PROTOCOL_BROADCAST_CODEC_ID);
    out->vtable = &broadcast_vtable;
    out->self = self;
    return REFRAIN_STATUS_OK;
}

refrain_status query_without_broadcast(void *, refrain_str, std::uint32_t,
                                       refrain_extension_ref *) {
    return REFRAIN_EXTENSION_NOT_FOUND;
}

/// The provider keeps a borrowed pointer to its vtable, so the table has to
/// outlive the adapter; one static table per extension shape does that.
[[nodiscard]] auto make_table(bool broadcast)
    -> const refrain_services_net_protocol_vtable & {
    const auto build = [broadcast](auto query) {
        refrain_services_net_protocol_vtable table{};
        table.struct_size = static_cast<std::uint32_t>(
            sizeof(refrain_services_net_protocol_vtable));
        table.abi_major = REFRAIN_SERVICES_NET_PROTOCOL_ABI_MAJOR;
        table.abi_minor = REFRAIN_SERVICES_NET_PROTOCOL_ABI_MINOR;
        table.abi_patch = REFRAIN_SERVICES_NET_PROTOCOL_ABI_PATCH;
        table.send_packet = &fake_send_packet;
        table.query_extension = query;
        return table;
    };
    static const auto with_broadcast{build(&query_broadcast)};
    static const auto without_broadcast{build(&query_without_broadcast)};
    return broadcast ? with_broadcast : without_broadcast;
}

[[nodiscard]] auto make_provider(fake_state &state, bool broadcast)
    -> std::unique_ptr<net_protocol::provider> {
    return std::make_unique<net_protocol::provider>(make_table(broadcast),
                                                    &state);
}

/// A provider that owns a real protocol-777 codec, so the shared path can be
/// compared with a per-recipient encode byte for byte.
struct codec_state {
    refrain::protocol::java777_codec codec{};
};

refrain_status codec_send_shared(
    void *self, refrain_services_net_protocol_slice_connection_ref connections,
    const refrain_services_net_protocol_packet_view *packet) {
    auto &state{*static_cast<codec_state *>(self)};
    std::vector<std::uint64_t> cohort{};
    cohort.reserve(static_cast<std::size_t>(connections.len));
    for (std::uint64_t index{0}; index < connections.len; ++index) {
        cohort.push_back(connections.data[index].connection);
    }
    const auto payload{refrain::contracts::as_bytes(packet->payload)};
    auto sent{state.codec.send_shared_packet(
        cohort, packet->packet_id,
        std::span<const std::byte>{
            reinterpret_cast<const std::byte *>(payload.data()),
            payload.size()})};
    return sent ? REFRAIN_STATUS_OK
                : REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_ERROR_MALFORMED_PACKET;
}

refrain_status
codec_send_packet(void *self, std::uint64_t connection,
                  const refrain_services_net_protocol_packet_view *packet) {
    auto &state{*static_cast<codec_state *>(self)};
    const auto payload{refrain::contracts::as_bytes(packet->payload)};
    auto sent{state.codec.send_packet(
        connection, packet->packet_id,
        std::span<const std::byte>{
            reinterpret_cast<const std::byte *>(payload.data()),
            payload.size()})};
    return sent ? REFRAIN_STATUS_OK
                : REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_ERROR_MALFORMED_PACKET;
}

std::uint8_t
codec_take_outbound(void *self, std::uint64_t connection,
                    refrain_services_net_protocol_frame_chunk *out) {
    std::span<const std::byte> data{};
    if (!static_cast<codec_state *>(self)->codec.take_outbound(connection,
                                                               data)) {
        return 0;
    }
    out->struct_size = REFRAIN_SERVICES_NET_PROTOCOL_FRAME_CHUNK_SIZE;
    out->data = refrain::contracts::to_borrowed(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t *>(data.data()), data.size()});
    return 1;
}

const refrain_services_net_protocol_broadcast_codec_vtable
    codec_broadcast_vtable{
        REFRAIN_SERVICES_NET_PROTOCOL_BROADCAST_CODEC_VTABLE_SIZE,
        &codec_send_shared,
};

refrain_status query_codec_broadcast(void *self, refrain_str id,
                                     std::uint32_t min_version,
                                     refrain_extension_ref *out) {
    if (out == nullptr ||
        refrain::contracts::as_string_view(id) !=
            REFRAIN_SERVICES_NET_PROTOCOL_BROADCAST_CODEC_ID) {
        return REFRAIN_EXTENSION_NOT_FOUND;
    }
    if (min_version >
        ((REFRAIN_SERVICES_NET_PROTOCOL_BROADCAST_CODEC_ABI_MAJOR << 16U) |
         REFRAIN_SERVICES_NET_PROTOCOL_BROADCAST_CODEC_ABI_MINOR)) {
        return REFRAIN_EXTENSION_VERSION_MISMATCH;
    }
    out->struct_size = REFRAIN_EXTENSION_REF_SIZE;
    out->abi_major = REFRAIN_SERVICES_NET_PROTOCOL_BROADCAST_CODEC_ABI_MAJOR;
    out->abi_minor = REFRAIN_SERVICES_NET_PROTOCOL_BROADCAST_CODEC_ABI_MINOR;
    out->abi_patch = REFRAIN_SERVICES_NET_PROTOCOL_BROADCAST_CODEC_ABI_PATCH;
    out->id = refrain::contracts::to_borrowed(
        REFRAIN_SERVICES_NET_PROTOCOL_BROADCAST_CODEC_ID);
    out->vtable = &codec_broadcast_vtable;
    out->self = self;
    return REFRAIN_STATUS_OK;
}

[[nodiscard]] auto make_codec_table()
    -> const refrain_services_net_protocol_vtable & {
    const auto build = [] {
        refrain_services_net_protocol_vtable table{};
        table.struct_size = static_cast<std::uint32_t>(
            sizeof(refrain_services_net_protocol_vtable));
        table.abi_major = REFRAIN_SERVICES_NET_PROTOCOL_ABI_MAJOR;
        table.abi_minor = REFRAIN_SERVICES_NET_PROTOCOL_ABI_MINOR;
        table.abi_patch = REFRAIN_SERVICES_NET_PROTOCOL_ABI_PATCH;
        table.send_packet = &codec_send_packet;
        table.take_outbound = &codec_take_outbound;
        table.query_extension = &query_codec_broadcast;
        return table;
    };
    static const auto table{build()};
    return table;
}

suite<"server/provider-protocol"> provider_protocol_tests = [] {
    "a broadcast provider receives one call for the whole cohort"_test = [] {
        fake_state state{};
        refrain::server::provider_protocol adapter{make_provider(state, true)};
        constexpr std::array<std::uint64_t, 3> cohort{7, 8, 9};
        constexpr std::array payload{std::byte{0x2C}, std::byte{0x01},
                                     std::byte{0x02}};
        const auto sent{adapter.send_shared_packet(cohort, 0x2CU, payload)};
        expect(sent.has_value());
        expect(state.shared_calls == 1U);
        expect(state.single_calls == 0U);
        expect(state.connections == std::vector<std::uint64_t>{7, 8, 9});
        expect(state.packet_id == 0x2CU);
        expect(state.payload ==
               std::vector<std::byte>{payload.begin(), payload.end()});
    };

    "a provider without the extension keeps the per-recipient fallback"_test =
        [] {
            fake_state state{};
            refrain::server::provider_protocol adapter{
                make_provider(state, false)};
            constexpr std::array<std::uint64_t, 2> cohort{4, 5};
            constexpr std::array payload{std::byte{0x11}, std::byte{0x22}};
            const auto sent{adapter.send_shared_packet(cohort, 0x11U, payload)};
            expect(sent.has_value());
            expect(state.shared_calls == 0U);
            expect(state.single_calls == 2U);
            expect(state.connections == std::vector<std::uint64_t>{4, 5});
        };

    "a shared send frames the same bytes as a per-recipient send"_test = [] {
        constexpr std::array<std::uint64_t, 3> cohort{101, 102, 103};
        constexpr std::array payload{std::byte{0x2C}, std::byte{0x11},
                                     std::byte{0x22}, std::byte{0x33}};
        // Compression is on, so the comparison exercises the frame format,
        // not just the packet body.
        constexpr std::int32_t threshold{0};

        codec_state shared_state{};
        for (const auto connection : cohort) {
            shared_state.codec.create_connection(connection);
            expect(static_cast<bool>(
                shared_state.codec.set_compression(connection, threshold)));
        }
        refrain::server::provider_protocol adapter{
            std::make_unique<net_protocol::provider>(make_codec_table(),
                                                     &shared_state)};
        const auto sent{adapter.send_shared_packet(cohort, 0x2CU, payload)};
        expect(sent.has_value());

        refrain::protocol::java777_codec reference{};
        for (const auto connection : cohort) {
            reference.create_connection(connection);
            expect(static_cast<bool>(
                reference.set_compression(connection, threshold)));
            expect(static_cast<bool>(
                reference.send_packet(connection, 0x2CU, payload)));
        }

        for (const auto connection : cohort) {
            std::span<const std::byte> shared{};
            expect(shared_state.codec.take_outbound(connection, shared));
            std::span<const std::byte> expected{};
            expect(reference.take_outbound(connection, expected));
            expect(shared.size() == expected.size());
            expect(std::equal(shared.begin(), shared.end(), expected.begin(),
                              expected.end()));
        }
    };

    "an empty cohort sends nothing"_test = [] {
        fake_state state{};
        refrain::server::provider_protocol adapter{make_provider(state, true)};
        constexpr std::array<std::byte, 1> payload{std::byte{0x01}};
        const auto sent{adapter.send_shared_packet({}, 0x01U, payload)};
        expect(sent.has_value());
        expect(state.shared_calls == 0U);
        expect(state.single_calls == 0U);
    };
};

} // namespace
