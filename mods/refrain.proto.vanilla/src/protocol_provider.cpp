// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Protocol 777 provider surface. Packet framing and the state machine are
// still owned by the in-process network server; this provider exposes the
// edition/version descriptor and the reserved codec/extension entry points so
// the composition can select and inspect it.

#include <refrain/contracts/abi.hpp>
#include <refrain/contracts/services/net_protocol_abi.h>
#include <refrain/mod/host.hpp>
#include <refrain/mod/static_mod.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace {

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
    out->transport_protocol = 0U; // transport_protocol::tcp
    out->requires_stream = 1;
    static constexpr std::string_view capabilities{"unified-protocol"};
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

auto next_packet(void *, refrain_core_handle_connection,
                 refrain_services_net_protocol_packet_view *,
                 refrain_services_net_protocol_frame_header *)
    -> refrain_status {
    return REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_ERROR_UNSUPPORTED_VERSION;
}

auto send_packet(void *, refrain_core_handle_connection,
                 const refrain_services_net_protocol_packet_view *)
    -> refrain_status {
    return REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_ERROR_UNSUPPORTED_VERSION;
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
        .next_packet = &next_packet,
        .send_packet = &send_packet,
        .state_of = &state_of,
        .query_extension = &query_extension,
    };
}

auto make_protocol(refrain::mod::host &host) -> refrain::status {
    static const auto vtable{make_vtable()};
    static int self{0};
    const refrain::mod::provider_api api{
        .vtable = &vtable,
        .self = &self,
        .abi_major = REFRAIN_SERVICES_NET_PROTOCOL_ABI_MAJOR,
    };
    return host.register_provider("refrain:net.protocol", "java_777", api,
                                  "1.0.0");
}

} // namespace

REFR_MOD_STATIC("refrain.proto.vanilla", "1.0.0", 1, make_protocol);
