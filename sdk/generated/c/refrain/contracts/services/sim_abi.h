// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 04d13b3ba7228746

#ifndef REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_SIM_ABI_H
#define REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_SIM_ABI_H

#include <refrain/contracts/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct refrain_services_sim_sim_config refrain_services_sim_sim_config;

typedef uint32_t refrain_services_sim_sim_error; // enum 'sim_error'
#define REFRAIN_SERVICES_SIM_SIM_ERROR_INVALID_HANDLE ((refrain_services_sim_sim_error)(1))
#define REFRAIN_SERVICES_SIM_SIM_ERROR_ALREADY_OPEN ((refrain_services_sim_sim_error)(2))
#define REFRAIN_SERVICES_SIM_SIM_ERROR_NOT_FOUND ((refrain_services_sim_sim_error)(3))
#define REFRAIN_SERVICES_SIM_SIM_ERROR_IO_ERROR ((refrain_services_sim_sim_error)(4))
#define REFRAIN_SERVICES_SIM_SIM_ERROR_INTERNAL ((refrain_services_sim_sim_error)(5))

typedef uint64_t refrain_services_sim_handle_sim_world;
#define REFRAIN_SERVICES_SIM_HANDLE_SIM_WORLD_INVALID ((refrain_services_sim_handle_sim_world)(REFRAIN_HANDLE_INVALID))

// Published once per tick, after the player/RNG step.
#define REFRAIN_SERVICES_SIM_EVENT_TICK_EVENT_ID "refrain:event/refrain/services/sim/tick_event"

struct refrain_services_sim_sim_config {
    uint32_t struct_size;
    uint8_t reserved_0[4];
    // RWF world directory.
    refrain_str world_path;
    // World seed; providers use it for their deterministic RNG stream.
    int64_t seed;
}; // refrain_services_sim_sim_config size = 32
#define REFRAIN_SERVICES_SIM_SIM_CONFIG_SIZE ((uint32_t)sizeof(refrain_services_sim_sim_config))

#define REFRAIN_SERVICES_SIM_SERVICE_ID "refrain:sim"
#define REFRAIN_SERVICES_SIM_ABI_MAJOR 1u
#define REFRAIN_SERVICES_SIM_ABI_MINOR 0u
#define REFRAIN_SERVICES_SIM_ABI_PATCH 0u
#define REFRAIN_SERVICES_SIM_SIDE REFRAIN_SIDE_SERVER
#define REFRAIN_SERVICES_SIM_THREAD REFRAIN_THREAD_SIM
#define REFRAIN_SERVICES_SIM_COMPOSITION REFRAIN_COMPOSITION_EXCLUSIVE
#define REFRAIN_SERVICES_SIM_CAPABILITIES "tick,replay,deterministic-rng"
#define REFRAIN_SERVICES_SIM_REQUIRES "refrain:core>=1.0.0,refrain:world>=1.0.0"
#define REFRAIN_SERVICES_SIM_SERVICE_IDS "refrain:sim"

typedef struct refrain_services_sim_vtable {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t abi_patch;
    // attributes: #[thread(sim)] #[can_fail]
    refrain_status (*open_world)(void *self, const refrain_services_sim_sim_config *config, refrain_services_sim_handle_sim_world *out_value);
    // attributes: #[thread(sim)] #[can_fail]
    refrain_status (*close_world)(void *self, refrain_services_sim_handle_sim_world world);
    // Advance exactly one tick.
    // attributes: #[thread(sim)] #[can_fail]
    refrain_status (*tick)(void *self, refrain_services_sim_handle_sim_world world);
    // attributes: #[thread(any)]
    int64_t (*ticks)(void *self, refrain_services_sim_handle_sim_world world);
    // Lowercase hex BLAKE3 of the deterministic replay state.
    // attributes: #[thread(sim)]
    refrain_str (*state_hash)(void *self, refrain_services_sim_handle_sim_world world);
} refrain_services_sim_vtable; // service refrain:sim

#define REFRAIN_SERVICES_SIM_VTABLE_SIZE ((uint32_t)sizeof(refrain_services_sim_vtable))

#ifdef __cplusplus
} // extern "C"
#endif

#endif // REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_SIM_ABI_H
