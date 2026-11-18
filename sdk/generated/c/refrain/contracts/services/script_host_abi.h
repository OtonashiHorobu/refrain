// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 29871d2bb99d3b05

#ifndef REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_SCRIPT_HOST_ABI_H
#define REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_SCRIPT_HOST_ABI_H

#include <refrain/contracts/abi.h>
#include <refrain/contracts/services/mod_loader_abi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct refrain_services_script_host_script_call_request refrain_services_script_host_script_call_request;
typedef struct refrain_services_script_host_script_call_result refrain_services_script_host_script_call_result;

// Script host failures. Zero is success, so entries start at one. The codes
// distinguish a caller mistake (`invalid_argument`, `not_found`) from guest
// misbehaviour (`budget_exceeded`, `deadline_exceeded`, `guest_trapped`) and
// the state a trapped instance enters (`quarantined`).
typedef uint32_t refrain_services_script_host_script_error; // enum 'script_error'
#define REFRAIN_SERVICES_SCRIPT_HOST_SCRIPT_ERROR_INVALID_ARGUMENT ((refrain_services_script_host_script_error)(1))
#define REFRAIN_SERVICES_SCRIPT_HOST_SCRIPT_ERROR_NOT_FOUND ((refrain_services_script_host_script_error)(2))
#define REFRAIN_SERVICES_SCRIPT_HOST_SCRIPT_ERROR_PERMISSION_DENIED ((refrain_services_script_host_script_error)(3))
#define REFRAIN_SERVICES_SCRIPT_HOST_SCRIPT_ERROR_UNSUPPORTED ((refrain_services_script_host_script_error)(4))
#define REFRAIN_SERVICES_SCRIPT_HOST_SCRIPT_ERROR_BUDGET_EXCEEDED ((refrain_services_script_host_script_error)(5))
#define REFRAIN_SERVICES_SCRIPT_HOST_SCRIPT_ERROR_DEADLINE_EXCEEDED ((refrain_services_script_host_script_error)(6))
#define REFRAIN_SERVICES_SCRIPT_HOST_SCRIPT_ERROR_GUEST_TRAPPED ((refrain_services_script_host_script_error)(7))
#define REFRAIN_SERVICES_SCRIPT_HOST_SCRIPT_ERROR_QUARANTINED ((refrain_services_script_host_script_error)(8))
#define REFRAIN_SERVICES_SCRIPT_HOST_SCRIPT_ERROR_INTERNAL ((refrain_services_script_host_script_error)(9))

// One callback invocation.
struct refrain_services_script_host_script_call_request {
    uint32_t struct_size;
    uint8_t reserved_0[4];
    // Exported guest function the host calls. It must be a wasm export name:
    // 1 to 64 characters from `[A-Za-z0-9_.-]`, starting with a letter, digit
    // or underscore.
    refrain_str callback;
    // Argument bytes staged at the start of the guest's linear memory before
    // the call. At most 256 bytes; a guest that accepts arguments keeps that
    // prefix of its memory reserved.
    refrain_bytes arguments;
}; // refrain_services_script_host_script_call_request size = 40
#define REFRAIN_SERVICES_SCRIPT_HOST_SCRIPT_CALL_REQUEST_SIZE ((uint32_t)sizeof(refrain_services_script_host_script_call_request))

// Result of one callback invocation.
struct refrain_services_script_host_script_call_result {
    uint32_t struct_size;
    // The guest callback's i32 return value.
    int32_t value;
}; // refrain_services_script_host_script_call_result size = 8
#define REFRAIN_SERVICES_SCRIPT_HOST_SCRIPT_CALL_RESULT_SIZE ((uint32_t)sizeof(refrain_services_script_host_script_call_result))

#define REFRAIN_SERVICES_SCRIPT_HOST_SERVICE_ID "refrain:script.host"
#define REFRAIN_SERVICES_SCRIPT_HOST_ABI_MAJOR 1u
#define REFRAIN_SERVICES_SCRIPT_HOST_ABI_MINOR 0u
#define REFRAIN_SERVICES_SCRIPT_HOST_ABI_PATCH 0u
#define REFRAIN_SERVICES_SCRIPT_HOST_SIDE REFRAIN_SIDE_SERVER
#define REFRAIN_SERVICES_SCRIPT_HOST_THREAD REFRAIN_THREAD_MAIN
#define REFRAIN_SERVICES_SCRIPT_HOST_COMPOSITION REFRAIN_COMPOSITION_SEALED
#define REFRAIN_SERVICES_SCRIPT_HOST_CAPABILITIES "call-dispatch,budgeted-calls"
#define REFRAIN_SERVICES_SCRIPT_HOST_REQUIRES "refrain:core>=1.0.0"
#define REFRAIN_SERVICES_SCRIPT_HOST_SERVICE_IDS "refrain:script.host"

typedef struct refrain_services_script_host_vtable {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t abi_patch;
    // Invoke one exported guest callback. The call runs under the loader's
    // per-call instruction budget and wall-clock deadline and is refused for
    // a quarantined instance. A guest that traps is quarantined: the instance
    // stays registered but executes no further guest code until it is
    // unloaded, and `unload` releases the whole store either way.
    // attributes: #[thread(main)] #[can_fail]
    refrain_status (*call)(void *self, refrain_services_mod_loader_handle_mod_instance instance, const refrain_services_script_host_script_call_request *request, refrain_services_script_host_script_call_result *out_value);
} refrain_services_script_host_vtable; // service refrain:script.host

#define REFRAIN_SERVICES_SCRIPT_HOST_VTABLE_SIZE ((uint32_t)sizeof(refrain_services_script_host_vtable))

#ifdef __cplusplus
} // extern "C"
#endif

#endif // REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_SCRIPT_HOST_ABI_H
