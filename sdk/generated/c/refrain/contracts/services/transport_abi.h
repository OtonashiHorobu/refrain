// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 01de959e836fba16

#ifndef REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_TRANSPORT_ABI_H
#define REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_TRANSPORT_ABI_H

#include <refrain/contracts/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct refrain_services_transport_transport_endpoint refrain_services_transport_transport_endpoint;
typedef struct refrain_services_transport_transport_descriptor refrain_services_transport_transport_descriptor;
typedef struct refrain_services_transport_transport_event refrain_services_transport_transport_event;

typedef uint32_t refrain_services_transport_transport_error; // enum 'transport_error'
#define REFRAIN_SERVICES_TRANSPORT_TRANSPORT_ERROR_INVALID_ARGUMENT ((refrain_services_transport_transport_error)(1))
#define REFRAIN_SERVICES_TRANSPORT_TRANSPORT_ERROR_NOT_FOUND ((refrain_services_transport_transport_error)(2))
#define REFRAIN_SERVICES_TRANSPORT_TRANSPORT_ERROR_ALREADY_EXISTS ((refrain_services_transport_transport_error)(3))
#define REFRAIN_SERVICES_TRANSPORT_TRANSPORT_ERROR_PERMISSION_DENIED ((refrain_services_transport_transport_error)(4))
#define REFRAIN_SERVICES_TRANSPORT_TRANSPORT_ERROR_IO_ERROR ((refrain_services_transport_transport_error)(5))
#define REFRAIN_SERVICES_TRANSPORT_TRANSPORT_ERROR_OUT_OF_MEMORY ((refrain_services_transport_transport_error)(6))
#define REFRAIN_SERVICES_TRANSPORT_TRANSPORT_ERROR_UNSUPPORTED ((refrain_services_transport_transport_error)(7))
#define REFRAIN_SERVICES_TRANSPORT_TRANSPORT_ERROR_CLOSED ((refrain_services_transport_transport_error)(8))

// Carrying protocol. A protocol provider declares the subset it accepts.
typedef uint32_t refrain_services_transport_transport_protocol; // enum 'transport_protocol'
#define REFRAIN_SERVICES_TRANSPORT_TRANSPORT_PROTOCOL_TCP ((refrain_services_transport_transport_protocol)(0))
#define REFRAIN_SERVICES_TRANSPORT_TRANSPORT_PROTOCOL_TLS_TCP ((refrain_services_transport_transport_protocol)(1))
#define REFRAIN_SERVICES_TRANSPORT_TRANSPORT_PROTOCOL_RAKNET ((refrain_services_transport_transport_protocol)(2))
#define REFRAIN_SERVICES_TRANSPORT_TRANSPORT_PROTOCOL_UNIX_STREAM ((refrain_services_transport_transport_protocol)(3))
#define REFRAIN_SERVICES_TRANSPORT_TRANSPORT_PROTOCOL_MEMORY_STREAM ((refrain_services_transport_transport_protocol)(4))
#define REFRAIN_SERVICES_TRANSPORT_TRANSPORT_PROTOCOL_MEMORY_MESSAGE ((refrain_services_transport_transport_protocol)(5))
#define REFRAIN_SERVICES_TRANSPORT_TRANSPORT_PROTOCOL_CUSTOM ((refrain_services_transport_transport_protocol)(6))

typedef uint32_t refrain_services_transport_transport_event_kind; // enum 'transport_event_kind'
#define REFRAIN_SERVICES_TRANSPORT_TRANSPORT_EVENT_KIND_OPENED ((refrain_services_transport_transport_event_kind)(1))
#define REFRAIN_SERVICES_TRANSPORT_TRANSPORT_EVENT_KIND_DATA ((refrain_services_transport_transport_event_kind)(2))
#define REFRAIN_SERVICES_TRANSPORT_TRANSPORT_EVENT_KIND_CLOSED ((refrain_services_transport_transport_event_kind)(3))
#define REFRAIN_SERVICES_TRANSPORT_TRANSPORT_EVENT_KIND_ERROR ((refrain_services_transport_transport_event_kind)(4))

struct refrain_services_transport_transport_endpoint {
    uint32_t struct_size;
    uint8_t reserved_0[4];
    refrain_str host;
    uint16_t port;
    uint8_t reserved_1[6];
}; // refrain_services_transport_transport_endpoint size = 32
#define REFRAIN_SERVICES_TRANSPORT_TRANSPORT_ENDPOINT_SIZE ((uint32_t)sizeof(refrain_services_transport_transport_endpoint))

struct refrain_services_transport_transport_descriptor {
    uint32_t struct_size;
    uint8_t reserved_0[4];
    // Stable provider id, for example "refrain.transport.asio.tcp".
    refrain_str provider;
    refrain_services_transport_transport_protocol protocol;
    uint8_t reliable;
    uint8_t ordered;
    uint8_t message_oriented;
    uint8_t reserved_1[1];
    uint32_t mtu;
    uint8_t backpressure;
    uint8_t reserved_2[3];
}; // refrain_services_transport_transport_descriptor size = 40
#define REFRAIN_SERVICES_TRANSPORT_TRANSPORT_DESCRIPTOR_SIZE ((uint32_t)sizeof(refrain_services_transport_transport_descriptor))

// One queued transport event. `data` is borrowed from the provider and stays
// valid until the next poll() call on that provider instance.
struct refrain_services_transport_transport_event {
    uint32_t struct_size;
    refrain_services_transport_transport_event_kind kind;
    uint64_t connection;
    refrain_bytes data;
    refrain_str error;
}; // refrain_services_transport_transport_event size = 48
#define REFRAIN_SERVICES_TRANSPORT_TRANSPORT_EVENT_SIZE ((uint32_t)sizeof(refrain_services_transport_transport_event))

#define REFRAIN_SERVICES_TRANSPORT_SERVICE_ID "refrain:transport"
#define REFRAIN_SERVICES_TRANSPORT_ABI_MAJOR 1u
#define REFRAIN_SERVICES_TRANSPORT_ABI_MINOR 0u
#define REFRAIN_SERVICES_TRANSPORT_ABI_PATCH 0u
#define REFRAIN_SERVICES_TRANSPORT_SIDE REFRAIN_SIDE_SERVER
#define REFRAIN_SERVICES_TRANSPORT_THREAD REFRAIN_THREAD_WORKER
#define REFRAIN_SERVICES_TRANSPORT_COMPOSITION REFRAIN_COMPOSITION_INSTANCED
#define REFRAIN_SERVICES_TRANSPORT_CAPABILITIES "stream,message,reliable"
#define REFRAIN_SERVICES_TRANSPORT_REQUIRES "refrain:core>=1.0.0"
#define REFRAIN_SERVICES_TRANSPORT_SERVICE_IDS "refrain:transport"

typedef struct refrain_services_transport_vtable {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t abi_patch;
    // attributes: #[thread(any)]
    void (*descriptor)(void *self, refrain_services_transport_transport_descriptor *value);
    // attributes: #[thread(any)] #[can_fail]
    refrain_status (*listen)(void *self, const refrain_services_transport_transport_endpoint *address);
    // attributes: #[thread(any)] #[can_fail]
    refrain_status (*connect)(void *self, const refrain_services_transport_transport_endpoint *address, uint64_t *out_value);
    // attributes: #[thread(any)] #[can_fail]
    refrain_status (*send)(void *self, uint64_t connection, refrain_bytes data);
    // attributes: #[thread(any)]
    void (*close)(void *self, uint64_t connection);
    // Blocking poll with a bounded timeout. Returns false on timeout.
    // Provides at most one event per call.
    // attributes: #[thread(worker)] #[can_fail]
    refrain_status (*poll)(void *self, uint32_t timeout_ms, refrain_services_transport_transport_event *event, uint8_t *out_value);
    // attributes: #[thread(any)]
    uint16_t (*local_port)(void *self);
    // attributes: #[thread(any)]
    uint64_t (*connection_count)(void *self);
    // attributes: #[thread(any)]
    void (*stop)(void *self);
} refrain_services_transport_vtable; // service refrain:transport

#define REFRAIN_SERVICES_TRANSPORT_VTABLE_SIZE ((uint32_t)sizeof(refrain_services_transport_vtable))

#ifdef __cplusplus
} // extern "C"
#endif

#endif // REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_TRANSPORT_ABI_H
