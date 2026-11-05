// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 21bfdf943ae85aa9

#ifndef REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_WORLD_ABI_H
#define REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_WORLD_ABI_H

#include <refrain/contracts/abi.h>
#include <refrain/contracts/core_abi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct refrain_services_world_world_config refrain_services_world_world_config;

typedef uint32_t refrain_services_world_world_error; // enum 'world_error'
#define REFRAIN_SERVICES_WORLD_WORLD_ERROR_CHUNK_NOT_LOADED ((refrain_services_world_world_error)(1))
#define REFRAIN_SERVICES_WORLD_WORLD_ERROR_OUT_OF_BOUNDS ((refrain_services_world_world_error)(2))
#define REFRAIN_SERVICES_WORLD_WORLD_ERROR_NOT_IMPLEMENTED ((refrain_services_world_world_error)(3))
#define REFRAIN_SERVICES_WORLD_WORLD_ERROR_INTERNAL ((refrain_services_world_world_error)(4))
#define REFRAIN_SERVICES_WORLD_WORLD_ERROR_ALREADY_OPEN ((refrain_services_world_world_error)(5))
#define REFRAIN_SERVICES_WORLD_WORLD_ERROR_OPEN_FAILED ((refrain_services_world_world_error)(6))

struct refrain_services_world_world_config {
    uint32_t struct_size;
    uint8_t reserved_0[4];
    // RWF world directory.
    refrain_str world_path;
    // Deterministic seed for the provider's RNG stream.
    int64_t seed;
}; // refrain_services_world_world_config size = 32
#define REFRAIN_SERVICES_WORLD_WORLD_CONFIG_SIZE ((uint32_t)sizeof(refrain_services_world_world_config))

#define REFRAIN_SERVICES_WORLD_SERVICE_ID "refrain:world"
#define REFRAIN_SERVICES_WORLD_ABI_MAJOR 1u
#define REFRAIN_SERVICES_WORLD_ABI_MINOR 0u
#define REFRAIN_SERVICES_WORLD_ABI_PATCH 0u
#define REFRAIN_SERVICES_WORLD_SIDE REFRAIN_SIDE_SERVER
#define REFRAIN_SERVICES_WORLD_THREAD REFRAIN_THREAD_SIM
#define REFRAIN_SERVICES_WORLD_COMPOSITION REFRAIN_COMPOSITION_EXCLUSIVE
#define REFRAIN_SERVICES_WORLD_CAPABILITIES "lighting,heightmaps,block_entities"
#define REFRAIN_SERVICES_WORLD_REQUIRES "refrain:core>=1.0.0,refrain:data.packs>=1.0.0"
#define REFRAIN_SERVICES_WORLD_SERVICE_IDS "refrain:world"

typedef struct refrain_services_world_vtable {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t abi_patch;
    // Open the singleton world. The provider owns it until
    // `close_world`; the server hosts exactly one world.
    // attributes: #[thread(sim)] #[can_fail]
    refrain_status (*open_world)(void *self, const refrain_services_world_world_config *config);
    // attributes: #[thread(sim)] #[can_fail]
    refrain_status (*close_world)(void *self);
    // Borrowed view of a loaded chunk; invalid after the chunk unloads.
    // attributes: #[thread(sim)] #[borrow] #[can_fail]
    refrain_status (*chunk_at)(void *self, const refrain_core_chunk_pos *pos, refrain_core_chunk_view *out_value);
    // Block state id at `pos`; 0 is the provider's air id.
    // attributes: #[thread(sim)]
    uint32_t (*block_at)(void *self, const refrain_core_block_pos *pos);
    // Maximum motion-blocking height at the column, in world Y coordinates.
    // attributes: #[thread(sim)]
    int32_t (*height_at)(void *self, const refrain_core_block_pos *pos);
    // True once the chunk reached the provider's "full" status.
    // attributes: #[thread(any)]
    uint8_t (*is_chunk_loaded)(void *self, const refrain_core_chunk_pos *pos);
    // Batched block mutation; returns the number of applied changes. Changes
    // are committed at the next tick boundary, not immediately.
    // attributes: #[thread(sim)] #[can_fail]
    refrain_status (*apply_block_changes)(void *self, refrain_core_slice_block_change changes, uint32_t *out_value);
    // World tick counter (monotonic, survives save/load).
    // attributes: #[thread(sim)]
    int64_t (*world_tick)(void *self);
} refrain_services_world_vtable; // service refrain:world

#define REFRAIN_SERVICES_WORLD_VTABLE_SIZE ((uint32_t)sizeof(refrain_services_world_vtable))

#ifdef __cplusplus
} // extern "C"
#endif

#endif // REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_WORLD_ABI_H
