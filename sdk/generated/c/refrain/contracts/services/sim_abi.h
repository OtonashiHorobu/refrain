// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 996e38144c4577ab

#ifndef REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_SIM_ABI_H
#define REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_SIM_ABI_H

#include <refrain/contracts/abi.h>
#include <refrain/contracts/core_abi.h>
#include <refrain/contracts/services/storage_abi.h>

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
#define REFRAIN_SERVICES_SIM_SIM_ERROR_OUT_OF_BOUNDS ((refrain_services_sim_sim_error)(6))
#define REFRAIN_SERVICES_SIM_SIM_ERROR_INVALID_ARGUMENT ((refrain_services_sim_sim_error)(7))
#define REFRAIN_SERVICES_SIM_SIM_ERROR_STORAGE_UNAVAILABLE ((refrain_services_sim_sim_error)(8))

typedef uint64_t refrain_services_sim_handle_sim_world;
#define REFRAIN_SERVICES_SIM_HANDLE_SIM_WORLD_INVALID ((refrain_services_sim_handle_sim_world)(REFRAIN_HANDLE_INVALID))

// Published once per tick, after the player/RNG step.
typedef struct refrain_services_sim_tick_event {
    int64_t tick;
} refrain_services_sim_tick_event; // event tick_event size = 8
#define REFRAIN_SERVICES_SIM_EVENT_TICK_EVENT_SIZE ((uint32_t)sizeof(refrain_services_sim_tick_event))
#define REFRAIN_SERVICES_SIM_EVENT_TICK_EVENT_ID "refrain:event/refrain/services/sim/tick_event"
#define REFRAIN_SERVICES_SIM_EVENT_TICK_EVENT_TYPE ((uint64_t)0x66bfb309ee05273aULL)

// One block mutation that changed the world, published once per applied
// position in ascending position order.
typedef struct refrain_services_sim_block_changed {
    int32_t x;
    int32_t y;
    int32_t z;
    uint32_t old_state;
    uint32_t new_state;
} refrain_services_sim_block_changed; // event block_changed size = 20
#define REFRAIN_SERVICES_SIM_EVENT_BLOCK_CHANGED_SIZE ((uint32_t)sizeof(refrain_services_sim_block_changed))
#define REFRAIN_SERVICES_SIM_EVENT_BLOCK_CHANGED_ID "refrain:event/refrain/services/sim/block_changed"
#define REFRAIN_SERVICES_SIM_EVENT_BLOCK_CHANGED_TYPE ((uint64_t)0xd6414cf41965c306ULL)

// An entity was added to the world.
typedef struct refrain_services_sim_entity_spawned {
    // Process-local entity id.
    uint64_t id;
    // Namespaced entity type key, for example `minecraft:pig`.
    refrain_str type;
    double x;
    double y;
    double z;
} refrain_services_sim_entity_spawned; // event entity_spawned size = 48
#define REFRAIN_SERVICES_SIM_EVENT_ENTITY_SPAWNED_SIZE ((uint32_t)sizeof(refrain_services_sim_entity_spawned))
#define REFRAIN_SERVICES_SIM_EVENT_ENTITY_SPAWNED_ID "refrain:event/refrain/services/sim/entity_spawned"
#define REFRAIN_SERVICES_SIM_EVENT_ENTITY_SPAWNED_TYPE ((uint64_t)0x001789c64fa4b85aULL)

// An entity was removed from the world.
typedef struct refrain_services_sim_entity_removed {
    // Process-local entity id.
    uint64_t id;
} refrain_services_sim_entity_removed; // event entity_removed size = 8
#define REFRAIN_SERVICES_SIM_EVENT_ENTITY_REMOVED_SIZE ((uint32_t)sizeof(refrain_services_sim_entity_removed))
#define REFRAIN_SERVICES_SIM_EVENT_ENTITY_REMOVED_ID "refrain:event/refrain/services/sim/entity_removed"
#define REFRAIN_SERVICES_SIM_EVENT_ENTITY_REMOVED_TYPE ((uint64_t)0xa8d216d1efdcebd0ULL)

struct refrain_services_sim_sim_config {
    uint32_t struct_size;
    uint8_t reserved_0[4];
    // The store the sim persists through. The runtime opened it once from the
    // level directory and passes the same handle to every provider of the
    // composition; the provider never opens the directory itself.
    refrain_services_storage_storage_handle storage;
    // World seed; providers use it for their deterministic RNG stream.
    int64_t seed;
}; // refrain_services_sim_sim_config size = 72
#define REFRAIN_SERVICES_SIM_SIM_CONFIG_SIZE ((uint32_t)sizeof(refrain_services_sim_sim_config))

#define REFRAIN_SERVICES_SIM_SERVICE_ID "refrain:sim"
#define REFRAIN_SERVICES_SIM_ABI_MAJOR 1u
#define REFRAIN_SERVICES_SIM_ABI_MINOR 0u
#define REFRAIN_SERVICES_SIM_ABI_PATCH 0u
#define REFRAIN_SERVICES_SIM_SIDE REFRAIN_SIDE_SERVER
#define REFRAIN_SERVICES_SIM_THREAD REFRAIN_THREAD_SIM
#define REFRAIN_SERVICES_SIM_COMPOSITION REFRAIN_COMPOSITION_EXCLUSIVE
#define REFRAIN_SERVICES_SIM_CAPABILITIES "tick,replay,deterministic-rng,block-events,entity-events"
#define REFRAIN_SERVICES_SIM_REQUIRES "refrain:core>=1.0.0,refrain:world>=1.0.0,refrain:storage>=1.0.0"
#define REFRAIN_SERVICES_SIM_SERVICE_IDS "refrain:sim"

typedef struct refrain_services_sim_vtable {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t abi_patch;
    // Open a sim world over the given store. The provider never opens the
    // level directory itself; the store was opened once by the runtime.
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
    // Apply a batch of block mutations and publish one `block_changed` event
    // per applied change. Returns the number of changes that modified the
    // world; a change that writes the current state is skipped.
    // attributes: #[thread(sim)] #[can_fail]
    refrain_status (*set_blocks)(void *self, refrain_services_sim_handle_sim_world world, refrain_core_slice_block_change changes, uint32_t *out_value);
    // Spawn an entity at the given feet position and publish
    // `entity_spawned`. The id is allocated in ascending order.
    // attributes: #[thread(sim)] #[can_fail]
    refrain_status (*spawn_entity)(void *self, refrain_services_sim_handle_sim_world world, refrain_str type, double x, double y, double z, uint64_t *out_value);
    // Remove one entity and publish `entity_removed`. Returns false when the
    // id is unknown.
    // attributes: #[thread(sim)]
    uint8_t (*remove_entity)(void *self, refrain_services_sim_handle_sim_world world, uint64_t id);
} refrain_services_sim_vtable; // service refrain:sim

#define REFRAIN_SERVICES_SIM_VTABLE_SIZE ((uint32_t)sizeof(refrain_services_sim_vtable))

#ifdef __cplusplus
} // extern "C"
#endif

#endif // REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_SIM_ABI_H
