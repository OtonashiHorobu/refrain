// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: b6025b8dffb79efe

#ifndef REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_NET_SESSION_ABI_H
#define REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_NET_SESSION_ABI_H

#include <refrain/contracts/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct refrain_services_net_session_session_profile refrain_services_net_session_session_profile;

typedef uint32_t refrain_services_net_session_session_error; // enum 'session_error'
#define REFRAIN_SERVICES_NET_SESSION_SESSION_ERROR_INVALID_ARGUMENT ((refrain_services_net_session_session_error)(1))
#define REFRAIN_SERVICES_NET_SESSION_SESSION_ERROR_NOT_JOINED ((refrain_services_net_session_session_error)(2))
#define REFRAIN_SERVICES_NET_SESSION_SESSION_ERROR_UNAVAILABLE ((refrain_services_net_session_session_error)(3))
#define REFRAIN_SERVICES_NET_SESSION_SESSION_ERROR_MALFORMED_PROFILE ((refrain_services_net_session_session_error)(4))

struct refrain_services_net_session_session_profile {
    uint32_t struct_size;
    uint8_t reserved_0[4];
    // Canonical UUID string (`8-4-4-4-12`).
    refrain_str id;
    refrain_str name;
    // Borrowed JSON array of profile properties; empty means none.
    refrain_bytes properties;
}; // refrain_services_net_session_session_profile size = 56
#define REFRAIN_SERVICES_NET_SESSION_SESSION_PROFILE_SIZE ((uint32_t)sizeof(refrain_services_net_session_session_profile))

#define REFRAIN_SERVICES_NET_SESSION_SERVICE_ID "refrain:net.session"
#define REFRAIN_SERVICES_NET_SESSION_ABI_MAJOR 1u
#define REFRAIN_SERVICES_NET_SESSION_ABI_MINOR 0u
#define REFRAIN_SERVICES_NET_SESSION_ABI_PATCH 0u
#define REFRAIN_SERVICES_NET_SESSION_SIDE REFRAIN_SIDE_SERVER
#define REFRAIN_SERVICES_NET_SESSION_THREAD REFRAIN_THREAD_ANY
#define REFRAIN_SERVICES_NET_SESSION_COMPOSITION REFRAIN_COMPOSITION_EXCLUSIVE
#define REFRAIN_SERVICES_NET_SESSION_CAPABILITIES "offline-profile,online-profile"
#define REFRAIN_SERVICES_NET_SESSION_REQUIRES "refrain:core>=1.0.0"
#define REFRAIN_SERVICES_NET_SESSION_SERVICE_IDS "refrain:net.session"

typedef struct refrain_services_net_session_vtable {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t abi_patch;
    // attributes: #[thread(any)] #[can_fail]
    refrain_status (*offline_profile)(void *self, refrain_str name, refrain_services_net_session_session_profile *profile);
    // attributes: #[thread(worker)] #[can_fail]
    refrain_status (*joined_profile)(void *self, refrain_str name, refrain_str server_hash, refrain_str remote_ip, refrain_services_net_session_session_profile *profile);
} refrain_services_net_session_vtable; // service refrain:net.session

#define REFRAIN_SERVICES_NET_SESSION_VTABLE_SIZE ((uint32_t)sizeof(refrain_services_net_session_vtable))

#ifdef __cplusplus
} // extern "C"
#endif

#endif // REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_NET_SESSION_ABI_H
