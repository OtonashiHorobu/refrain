// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 927eab4ffcbbd2c4

#ifndef REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_SESSION_LIFECYCLE_ABI_H
#define REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_SESSION_LIFECYCLE_ABI_H

#include <refrain/contracts/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct refrain_services_session_lifecycle_player_info refrain_services_session_lifecycle_player_info;

struct refrain_services_session_lifecycle_player_info {
    uint32_t struct_size;
    uint8_t reserved_0[4];
    uint64_t connection;
    refrain_str name;
    refrain_str world;
}; // refrain_services_session_lifecycle_player_info size = 48
#define REFRAIN_SERVICES_SESSION_LIFECYCLE_PLAYER_INFO_SIZE ((uint32_t)sizeof(refrain_services_session_lifecycle_player_info))

#define REFRAIN_SERVICES_SESSION_LIFECYCLE_SERVICE_ID "refrain:session.lifecycle"
#define REFRAIN_SERVICES_SESSION_LIFECYCLE_ABI_MAJOR 1u
#define REFRAIN_SERVICES_SESSION_LIFECYCLE_ABI_MINOR 0u
#define REFRAIN_SERVICES_SESSION_LIFECYCLE_ABI_PATCH 0u
#define REFRAIN_SERVICES_SESSION_LIFECYCLE_SIDE REFRAIN_SIDE_SERVER
#define REFRAIN_SERVICES_SESSION_LIFECYCLE_THREAD REFRAIN_THREAD_SIM
#define REFRAIN_SERVICES_SESSION_LIFECYCLE_COMPOSITION REFRAIN_COMPOSITION_INSTANCED
#define REFRAIN_SERVICES_SESSION_LIFECYCLE_CAPABILITIES "player-lifecycle"
#define REFRAIN_SERVICES_SESSION_LIFECYCLE_REQUIRES "refrain:core>=1.0.0"
#define REFRAIN_SERVICES_SESSION_LIFECYCLE_SERVICE_IDS "refrain:session.lifecycle"

typedef struct refrain_services_session_lifecycle_vtable {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t abi_patch;
    // attributes: #[thread(sim)]
    void (*player_join)(void *self, const refrain_services_session_lifecycle_player_info *player);
    // attributes: #[thread(sim)]
    void (*player_leave)(void *self, const refrain_services_session_lifecycle_player_info *player);
    // Called once per simulation tick after the world advanced.
    // attributes: #[thread(sim)]
    void (*tick)(void *self, uint64_t tick);
} refrain_services_session_lifecycle_vtable; // service refrain:session.lifecycle

#define REFRAIN_SERVICES_SESSION_LIFECYCLE_VTABLE_SIZE ((uint32_t)sizeof(refrain_services_session_lifecycle_vtable))

#ifdef __cplusplus
} // extern "C"
#endif

#endif // REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_SESSION_LIFECYCLE_ABI_H
