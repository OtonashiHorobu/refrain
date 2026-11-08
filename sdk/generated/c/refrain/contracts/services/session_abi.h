// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 9a9c3f62f2fea89b

#ifndef REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_SESSION_ABI_H
#define REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_SESSION_ABI_H

#include <refrain/contracts/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct refrain_services_session_session_descriptor refrain_services_session_session_descriptor;

struct refrain_services_session_session_descriptor {
    uint32_t struct_size;
    uint8_t reserved_0[4];
    refrain_str family;
    refrain_str edition;
    refrain_str capabilities;
}; // refrain_services_session_session_descriptor size = 56
#define REFRAIN_SERVICES_SESSION_SESSION_DESCRIPTOR_SIZE ((uint32_t)sizeof(refrain_services_session_session_descriptor))

#define REFRAIN_SERVICES_SESSION_SERVICE_ID "refrain:session"
#define REFRAIN_SERVICES_SESSION_ABI_MAJOR 1u
#define REFRAIN_SERVICES_SESSION_ABI_MINOR 0u
#define REFRAIN_SERVICES_SESSION_ABI_PATCH 0u
#define REFRAIN_SERVICES_SESSION_SIDE REFRAIN_SIDE_SERVER
#define REFRAIN_SERVICES_SESSION_THREAD REFRAIN_THREAD_MAIN
#define REFRAIN_SERVICES_SESSION_COMPOSITION REFRAIN_COMPOSITION_INSTANCED
#define REFRAIN_SERVICES_SESSION_CAPABILITIES "tick,console"
#define REFRAIN_SERVICES_SESSION_REQUIRES "refrain:core>=1.0.0"
#define REFRAIN_SERVICES_SESSION_SERVICE_IDS "refrain:session"

typedef struct refrain_services_session_vtable {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t abi_patch;
    // attributes: #[thread(any)]
    void (*descriptor)(void *self, refrain_services_session_session_descriptor *value);
} refrain_services_session_vtable; // service refrain:session

#define REFRAIN_SERVICES_SESSION_VTABLE_SIZE ((uint32_t)sizeof(refrain_services_session_vtable))

#ifdef __cplusplus
} // extern "C"
#endif

#endif // REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_SESSION_ABI_H
