// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: c51c38b6cd952844

#ifndef REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_NET_PROTOCOL_ABI_H
#define REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_NET_PROTOCOL_ABI_H

#include <refrain/contracts/abi.h>
#include <refrain/contracts/core_abi.h>

#ifdef __cplusplus
extern "C" {
#endif

// Extension ABI v1: a service exposes provider-implemented extensions
// through its `query_extension` vtable slot. A reference is a
// struct_size-prologue value; the vtable pointer stays type-erased in C and is
// typed by the generated C++ wrapper.
#ifndef REFRAIN_EXTENSION_ABI_DEFINED
#define REFRAIN_EXTENSION_ABI_DEFINED

#define REFRAIN_EXTENSION_ABI_MAJOR 1u
#define REFRAIN_EXTENSION_ABI_MINOR 0u

// Query status values; the numeric codes mirror the shared core error taxonomy
// (invalid_argument, not_found, version_mismatch).
#define REFRAIN_EXTENSION_INVALID ((refrain_status)1u)
#define REFRAIN_EXTENSION_NOT_FOUND ((refrain_status)2u)
#define REFRAIN_EXTENSION_VERSION_MISMATCH ((refrain_status)8u)

// Packed minimum extension version requested by a query:
// (major << 16) | minor.
#define REFRAIN_EXTENSION_VERSION(major, minor)                                \
    ((((uint32_t)(major)) << 16u) | ((uint32_t)(minor) & 0xFFFFu))

typedef struct refrain_extension_ref {
    // sizeof(refrain_extension_ref) written by the producer; a host accepts
    // only its own compiled size.
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t abi_patch;
    // Stable id derived from the parent service id and the declaration name.
    refrain_str id;
    // Provider-owned extension vtable (first member: uint32_t struct_size).
    const void *vtable;
    // Provider-owned extension instance passed to every vtable method.
    void *self;
} refrain_extension_ref;

#define REFRAIN_EXTENSION_REF_SIZE ((uint32_t)sizeof(refrain_extension_ref))
#endif // REFRAIN_EXTENSION_ABI_DEFINED

typedef struct refrain_services_net_protocol_protocol_descriptor refrain_services_net_protocol_protocol_descriptor;
typedef struct refrain_services_net_protocol_protocol_version refrain_services_net_protocol_protocol_version;
typedef struct refrain_services_net_protocol_frame_header refrain_services_net_protocol_frame_header;
typedef struct refrain_services_net_protocol_packet_view refrain_services_net_protocol_packet_view;

typedef uint32_t refrain_services_net_protocol_protocol_edition; // enum 'protocol_edition'
#define REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_EDITION_JAVA ((refrain_services_net_protocol_protocol_edition)(0))
#define REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_EDITION_BEDROCK ((refrain_services_net_protocol_protocol_edition)(1))
#define REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_EDITION_REFRAIN ((refrain_services_net_protocol_protocol_edition)(2))
#define REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_EDITION_CUSTOM ((refrain_services_net_protocol_protocol_edition)(3))

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

// Provider identity and transport requirements. `transport_protocol` mirrors
// the transport service values (tcp=0, tls_tcp=1, raknet=2, ...).
struct refrain_services_net_protocol_protocol_descriptor {
    uint32_t struct_size;
    refrain_services_net_protocol_protocol_edition edition;
    refrain_str family;
    uint32_t canonical_version;
    uint8_t reserved_0[4];
    refrain_str accepted_versions;
    uint32_t transport_protocol;
    uint8_t requires_stream;
    uint8_t reserved_1[3];
    refrain_str capabilities;
}; // refrain_services_net_protocol_protocol_descriptor size = 72
#define REFRAIN_SERVICES_NET_PROTOCOL_PROTOCOL_DESCRIPTOR_SIZE ((uint32_t)sizeof(refrain_services_net_protocol_protocol_descriptor))

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

// Optional chunk-to-buffer codec implemented by chunk-aware providers.
#define REFRAIN_SERVICES_NET_PROTOCOL_CHUNK_CODEC_ID "refrain:net.protocol/chunk_codec"
#define REFRAIN_SERVICES_NET_PROTOCOL_CHUNK_CODEC_ABI_MAJOR 1u
#define REFRAIN_SERVICES_NET_PROTOCOL_CHUNK_CODEC_ABI_MINOR 0u
#define REFRAIN_SERVICES_NET_PROTOCOL_CHUNK_CODEC_ABI_PATCH 0u
#define REFRAIN_SERVICES_NET_PROTOCOL_CHUNK_CODEC_CAPABILITY "chunk-codec"
#define REFRAIN_SERVICES_NET_PROTOCOL_CHUNK_CODEC_EXTENDS "refrain:net.protocol"

typedef struct refrain_services_net_protocol_chunk_codec_vtable {
    uint32_t struct_size;
    // Serialize one borrowed chunk view into the caller-provided buffer.
    // attributes: #[thread(worker)] #[can_fail]
    refrain_status (*encode_chunk)(void *self, const refrain_core_chunk_view *chunk, refrain_core_chunk_buffer *encoded);
} refrain_services_net_protocol_chunk_codec_vtable; // extension refrain:net.protocol/chunk_codec

#define REFRAIN_SERVICES_NET_PROTOCOL_CHUNK_CODEC_VTABLE_SIZE ((uint32_t)sizeof(refrain_services_net_protocol_chunk_codec_vtable))

#define REFRAIN_SERVICES_NET_PROTOCOL_SERVICE_ID "refrain:net.protocol"
#define REFRAIN_SERVICES_NET_PROTOCOL_ABI_MAJOR 2u
#define REFRAIN_SERVICES_NET_PROTOCOL_ABI_MINOR 0u
#define REFRAIN_SERVICES_NET_PROTOCOL_ABI_PATCH 0u
#define REFRAIN_SERVICES_NET_PROTOCOL_SIDE REFRAIN_SIDE_SERVER
#define REFRAIN_SERVICES_NET_PROTOCOL_THREAD REFRAIN_THREAD_NET
#define REFRAIN_SERVICES_NET_PROTOCOL_COMPOSITION REFRAIN_COMPOSITION_INSTANCED
#define REFRAIN_SERVICES_NET_PROTOCOL_CAPABILITIES "frame-compression,registry-sync"
#define REFRAIN_SERVICES_NET_PROTOCOL_REQUIRES "refrain:core>=1.0.0"
#define REFRAIN_SERVICES_NET_PROTOCOL_SERVICE_IDS "refrain:net.protocol"
#define REFRAIN_SERVICES_NET_PROTOCOL_EXTENSION_IDS "refrain:net.protocol/chunk_codec"

typedef struct refrain_services_net_protocol_vtable {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t abi_patch;
    // attributes: #[thread(any)]
    void (*descriptor)(void *self, refrain_services_net_protocol_protocol_descriptor *value);
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
    // Extension query. Returns REFRAIN_EXTENSION_NOT_FOUND when the
    // provider does not offer `id`, REFRAIN_EXTENSION_VERSION_MISMATCH
    // when its major/minor cannot satisfy `min_version`, and
    // REFRAIN_EXTENSION_INVALID for a malformed reference. The host
    // initialises `out->struct_size` before the call.
    refrain_status (*query_extension)(void *self, refrain_str id,
                                      uint32_t min_version,
                                      refrain_extension_ref *out);
} refrain_services_net_protocol_vtable; // service refrain:net.protocol

#define REFRAIN_SERVICES_NET_PROTOCOL_VTABLE_SIZE ((uint32_t)sizeof(refrain_services_net_protocol_vtable))

#ifdef __cplusplus
} // extern "C"
#endif

#endif // REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_NET_PROTOCOL_ABI_H
