// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 3e46c08897b5a857

#ifndef REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_NET_PROTOCOL_ABI_H
#define REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_NET_PROTOCOL_ABI_H

#include <refrain/contracts/abi.h>
#include <refrain/contracts/core_abi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct refrain_services_net_protocol_protocol_version refrain_services_net_protocol_protocol_version;
typedef struct refrain_services_net_protocol_frame_header refrain_services_net_protocol_frame_header;
typedef struct refrain_services_net_protocol_packet_view refrain_services_net_protocol_packet_view;

typedef uint32_t refrain_services_net_protocol_protocol_error; // enum 'protocol_error'
#define REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_ERROR_FRAME_TOO_LARGE ((refrain_services_net_protocol_protocol_error)(1))
#define REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_ERROR_MALFORMED_PACKET ((refrain_services_net_protocol_protocol_error)(2))
#define REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_ERROR_UNKNOWN_PACKET ((refrain_services_net_protocol_protocol_error)(3))
#define REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_ERROR_UNSUPPORTED_VERSION ((refrain_services_net_protocol_protocol_error)(4))
#define REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_ERROR_COMPRESSION_FAILED ((refrain_services_net_protocol_protocol_error)(5))

typedef uint32_t refrain_services_net_protocol_connection_state; // enum 'connection_state'
#define REFRAIN_SERVICES_NET_PROTOCOL_CONNECTION_STATE_HANDSHAKE ((refrain_services_net_protocol_connection_state)(0))
#define REFRAIN_SERVICES_NET_PROTOCOL_CONNECTION_STATE_STATUS ((refrain_services_net_protocol_connection_state)(1))
#define REFRAIN_SERVICES_NET_PROTOCOL_CONNECTION_STATE_LOGIN ((refrain_services_net_protocol_connection_state)(2))
#define REFRAIN_SERVICES_NET_PROTOCOL_CONNECTION_STATE_CONFIGURATION ((refrain_services_net_protocol_connection_state)(3))
#define REFRAIN_SERVICES_NET_PROTOCOL_CONNECTION_STATE_PLAY ((refrain_services_net_protocol_connection_state)(4))

struct refrain_services_net_protocol_protocol_version {
    uint32_t struct_size;
    uint32_t protocol;
    refrain_str name;
}; // refrain_services_net_protocol_protocol_version size = 24
#define REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_VERSION_SIZE ((uint32_t)sizeof(refrain_services_net_protocol_protocol_version))

struct refrain_services_net_protocol_frame_header {
    uint32_t struct_size;
    uint32_t length;
    uint32_t uncompressed_length;
}; // refrain_services_net_protocol_frame_header size = 12
#define REFRAIN_SERVICES_NET_PROTOCOL_FRAME_HEADER_SIZE ((uint32_t)sizeof(refrain_services_net_protocol_frame_header))

// Borrowed packet payload; valid for the duration of the contract call.
struct refrain_services_net_protocol_packet_view {
    uint32_t struct_size;
    uint32_t packet_id;
    refrain_bytes payload;
}; // refrain_services_net_protocol_packet_view size = 24
#define REFRAIN_SERVICES_NET_PROTOCOL_PACKET_VIEW_SIZE ((uint32_t)sizeof(refrain_services_net_protocol_packet_view))

#define REFRAIN_SERVICES_NET_PROTOCOL_SERVICE_ID "refrain:net.protocol"
#define REFRAIN_SERVICES_NET_PROTOCOL_ABI_MAJOR 1u
#define REFRAIN_SERVICES_NET_PROTOCOL_ABI_MINOR 0u
#define REFRAIN_SERVICES_NET_PROTOCOL_ABI_PATCH 0u
#define REFRAIN_SERVICES_NET_PROTOCOL_SIDE REFRAIN_SIDE_SERVER
#define REFRAIN_SERVICES_NET_PROTOCOL_THREAD REFRAIN_THREAD_NET
#define REFRAIN_SERVICES_NET_PROTOCOL_COMPOSITION REFRAIN_COMPOSITION_EXCLUSIVE
#define REFRAIN_SERVICES_NET_PROTOCOL_CAPABILITIES "frame-compression,registry-sync"
#define REFRAIN_SERVICES_NET_PROTOCOL_REQUIRES "refrain:core>=1.0.0"
#define REFRAIN_SERVICES_NET_PROTOCOL_SERVICE_IDS "refrain:net.protocol"

typedef struct refrain_services_net_protocol_vtable {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t abi_patch;
    // Protocol number implemented by this provider (777 for vanilla 26.3).
    // attributes: #[thread(any)]
    uint32_t (*protocol_id)(void *self);
    // attributes: #[thread(any)]
    uint8_t (*accepts_version)(void *self, uint32_t protocol);
    // Decode one packet from a connection's inbound buffer.
    // attributes: #[thread(worker)] #[can_fail]
    refrain_status (*next_packet)(void *self, refrain_core_handle_connection conn, refrain_services_net_protocol_packet_view *packet, refrain_services_net_protocol_frame_header *out_value);
    // Encode and enqueue one packet on the connection.
    // attributes: #[thread(sim)] #[can_fail]
    refrain_status (*send_packet)(void *self, refrain_core_handle_connection conn, const refrain_services_net_protocol_packet_view *packet);
    // attributes: #[thread(sim)]
    refrain_services_net_protocol_connection_state (*state_of)(void *self, refrain_core_handle_connection conn);
} refrain_services_net_protocol_vtable; // service refrain:net.protocol

#define REFRAIN_SERVICES_NET_PROTOCOL_VTABLE_SIZE ((uint32_t)sizeof(refrain_services_net_protocol_vtable))

#ifdef __cplusplus
} // extern "C"
#endif

#endif // REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_NET_PROTOCOL_ABI_H
