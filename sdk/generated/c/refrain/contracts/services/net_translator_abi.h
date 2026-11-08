// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 8a20087cf2517c49

#ifndef REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_NET_TRANSLATOR_ABI_H
#define REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_NET_TRANSLATOR_ABI_H

#include <refrain/contracts/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct refrain_services_net_translator_translator_descriptor refrain_services_net_translator_translator_descriptor;
typedef struct refrain_services_net_translator_translation_request refrain_services_net_translator_translation_request;
typedef struct refrain_services_net_translator_translation_result refrain_services_net_translator_translation_result;

typedef uint32_t refrain_services_net_translator_translation_error; // enum 'translation_error'
#define REFRAIN_SERVICES_NET_TRANSLATOR_TRANSLATION_ERROR_UNSUPPORTED ((refrain_services_net_translator_translation_error)(1))
#define REFRAIN_SERVICES_NET_TRANSLATOR_TRANSLATION_ERROR_MALFORMED_PACKET ((refrain_services_net_translator_translation_error)(2))
#define REFRAIN_SERVICES_NET_TRANSLATOR_TRANSLATION_ERROR_INTERNAL ((refrain_services_net_translator_translation_error)(3))

struct refrain_services_net_translator_translator_descriptor {
    uint32_t struct_size;
    uint8_t reserved_0[4];
    refrain_str name;
    // Source edition/family, for example `minecraft-java`.
    refrain_str source_family;
    // Target edition/family served by the session.
    refrain_str target_family;
    refrain_str capabilities;
}; // refrain_services_net_translator_translator_descriptor size = 72
#define REFRAIN_SERVICES_NET_TRANSLATOR_TRANSLATOR_DESCRIPTOR_SIZE ((uint32_t)sizeof(refrain_services_net_translator_translator_descriptor))

struct refrain_services_net_translator_translation_request {
    uint32_t struct_size;
    uint8_t reserved_0[4];
    uint64_t connection;
    uint32_t source_protocol;
    uint32_t target_protocol;
    uint32_t packet_id;
    uint8_t reserved_1[4];
    // Borrowed packet payload, valid for the duration of the call.
    refrain_bytes payload;
}; // refrain_services_net_translator_translation_request size = 48
#define REFRAIN_SERVICES_NET_TRANSLATOR_TRANSLATION_REQUEST_SIZE ((uint32_t)sizeof(refrain_services_net_translator_translation_request))

struct refrain_services_net_translator_translation_result {
    uint32_t struct_size;
    uint32_t packet_id;
    // Borrowed translated payload; the provider keeps it valid until the next
    // call on the same connection.
    refrain_bytes payload;
}; // refrain_services_net_translator_translation_result size = 24
#define REFRAIN_SERVICES_NET_TRANSLATOR_TRANSLATION_RESULT_SIZE ((uint32_t)sizeof(refrain_services_net_translator_translation_result))

#define REFRAIN_SERVICES_NET_TRANSLATOR_SERVICE_ID "refrain:net.translator"
#define REFRAIN_SERVICES_NET_TRANSLATOR_ABI_MAJOR 1u
#define REFRAIN_SERVICES_NET_TRANSLATOR_ABI_MINOR 0u
#define REFRAIN_SERVICES_NET_TRANSLATOR_ABI_PATCH 0u
#define REFRAIN_SERVICES_NET_TRANSLATOR_SIDE REFRAIN_SIDE_SERVER
#define REFRAIN_SERVICES_NET_TRANSLATOR_THREAD REFRAIN_THREAD_WORKER
#define REFRAIN_SERVICES_NET_TRANSLATOR_COMPOSITION REFRAIN_COMPOSITION_EXCLUSIVE
#define REFRAIN_SERVICES_NET_TRANSLATOR_CAPABILITIES "packet-rewrite"
#define REFRAIN_SERVICES_NET_TRANSLATOR_REQUIRES "refrain:core>=1.0.0"
#define REFRAIN_SERVICES_NET_TRANSLATOR_SERVICE_IDS "refrain:net.translator"

typedef struct refrain_services_net_translator_vtable {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t abi_patch;
    // attributes: #[thread(any)]
    void (*descriptor)(void *self, refrain_services_net_translator_translator_descriptor *value);
    // attributes: #[thread(worker)] #[can_fail]
    refrain_status (*translate)(void *self, const refrain_services_net_translator_translation_request *request, refrain_services_net_translator_translation_result *result);
} refrain_services_net_translator_vtable; // service refrain:net.translator

#define REFRAIN_SERVICES_NET_TRANSLATOR_VTABLE_SIZE ((uint32_t)sizeof(refrain_services_net_translator_vtable))

#ifdef __cplusplus
} // extern "C"
#endif

#endif // REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_NET_TRANSLATOR_ABI_H
