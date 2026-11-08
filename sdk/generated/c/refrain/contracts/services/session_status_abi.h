// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 58ef0c35f02ee803

#ifndef REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_SESSION_STATUS_ABI_H
#define REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_SESSION_STATUS_ABI_H

#include <refrain/contracts/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct refrain_services_session_status_status_request refrain_services_session_status_status_request;
typedef struct refrain_services_session_status_status_view refrain_services_session_status_status_view;

struct refrain_services_session_status_status_request {
    uint32_t struct_size;
    uint32_t protocol;
    refrain_str host;
    uint16_t port;
    uint8_t reserved_0[2];
    uint32_t online_players;
}; // refrain_services_session_status_status_request size = 32
#define REFRAIN_SERVICES_SESSION_STATUS_STATUS_REQUEST_SIZE ((uint32_t)sizeof(refrain_services_session_status_status_request))

struct refrain_services_session_status_status_view {
    uint32_t struct_size;
    uint8_t reserved_0[4];
    refrain_str description;
    uint32_t online_players;
    uint32_t max_players;
}; // refrain_services_session_status_status_view size = 32
#define REFRAIN_SERVICES_SESSION_STATUS_STATUS_VIEW_SIZE ((uint32_t)sizeof(refrain_services_session_status_status_view))

#define REFRAIN_SERVICES_SESSION_STATUS_SERVICE_ID "refrain:session.status"
#define REFRAIN_SERVICES_SESSION_STATUS_ABI_MAJOR 1u
#define REFRAIN_SERVICES_SESSION_STATUS_ABI_MINOR 0u
#define REFRAIN_SERVICES_SESSION_STATUS_ABI_PATCH 0u
#define REFRAIN_SERVICES_SESSION_STATUS_SIDE REFRAIN_SIDE_SERVER
#define REFRAIN_SERVICES_SESSION_STATUS_THREAD REFRAIN_THREAD_WORKER
#define REFRAIN_SERVICES_SESSION_STATUS_COMPOSITION REFRAIN_COMPOSITION_INSTANCED
#define REFRAIN_SERVICES_SESSION_STATUS_CAPABILITIES "dynamic-motd,dynamic-player-count"
#define REFRAIN_SERVICES_SESSION_STATUS_REQUIRES "refrain:core>=1.0.0"
#define REFRAIN_SERVICES_SESSION_STATUS_SERVICE_IDS "refrain:session.status"

typedef struct refrain_services_session_status_vtable {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t abi_patch;
    // Return true when this provider handled the request. `description` and
    // counts are borrowed until the next call on this provider instance.
    // attributes: #[thread(worker)]
    uint8_t (*status)(void *self, const refrain_services_session_status_status_request *request, refrain_services_session_status_status_view *view);
} refrain_services_session_status_vtable; // service refrain:session.status

#define REFRAIN_SERVICES_SESSION_STATUS_VTABLE_SIZE ((uint32_t)sizeof(refrain_services_session_status_vtable))

#ifdef __cplusplus
} // extern "C"
#endif

#endif // REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_SESSION_STATUS_ABI_H
