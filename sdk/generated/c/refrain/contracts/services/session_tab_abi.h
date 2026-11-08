// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 25b2c961ad0ac277

#ifndef REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_SESSION_TAB_ABI_H
#define REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_SESSION_TAB_ABI_H

#include <refrain/contracts/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct refrain_services_session_tab_tab_request refrain_services_session_tab_tab_request;
typedef struct refrain_services_session_tab_tab_view refrain_services_session_tab_tab_view;

struct refrain_services_session_tab_tab_request {
    uint32_t struct_size;
    uint8_t reserved_0[4];
    uint64_t connection;
    refrain_str player_name;
}; // refrain_services_session_tab_tab_request size = 32
#define REFRAIN_SERVICES_SESSION_TAB_TAB_REQUEST_SIZE ((uint32_t)sizeof(refrain_services_session_tab_tab_request))

struct refrain_services_session_tab_tab_view {
    uint32_t struct_size;
    uint8_t reserved_0[4];
    refrain_str header;
    refrain_str footer;
}; // refrain_services_session_tab_tab_view size = 40
#define REFRAIN_SERVICES_SESSION_TAB_TAB_VIEW_SIZE ((uint32_t)sizeof(refrain_services_session_tab_tab_view))

#define REFRAIN_SERVICES_SESSION_TAB_SERVICE_ID "refrain:session.tab"
#define REFRAIN_SERVICES_SESSION_TAB_ABI_MAJOR 1u
#define REFRAIN_SERVICES_SESSION_TAB_ABI_MINOR 0u
#define REFRAIN_SERVICES_SESSION_TAB_ABI_PATCH 0u
#define REFRAIN_SERVICES_SESSION_TAB_SIDE REFRAIN_SIDE_SERVER
#define REFRAIN_SERVICES_SESSION_TAB_THREAD REFRAIN_THREAD_WORKER
#define REFRAIN_SERVICES_SESSION_TAB_COMPOSITION REFRAIN_COMPOSITION_INSTANCED
#define REFRAIN_SERVICES_SESSION_TAB_CAPABILITIES "dynamic-tab"
#define REFRAIN_SERVICES_SESSION_TAB_REQUIRES "refrain:core>=1.0.0"
#define REFRAIN_SERVICES_SESSION_TAB_SERVICE_IDS "refrain:session.tab"

typedef struct refrain_services_session_tab_vtable {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t abi_patch;
    // Return true when this provider handled the player. Strings are
    // borrowed until the next call on this provider instance.
    // attributes: #[thread(worker)]
    uint8_t (*tab)(void *self, const refrain_services_session_tab_tab_request *request, refrain_services_session_tab_tab_view *view);
} refrain_services_session_tab_vtable; // service refrain:session.tab

#define REFRAIN_SERVICES_SESSION_TAB_VTABLE_SIZE ((uint32_t)sizeof(refrain_services_session_tab_vtable))

#ifdef __cplusplus
} // extern "C"
#endif

#endif // REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_SESSION_TAB_ABI_H
