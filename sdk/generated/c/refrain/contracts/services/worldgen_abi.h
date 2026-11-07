// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 7cfeb406836d8806

#ifndef REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_WORLDGEN_ABI_H
#define REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_WORLDGEN_ABI_H

#include <refrain/contracts/abi.h>
#include <refrain/contracts/core_abi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct refrain_services_worldgen_worldgen_config refrain_services_worldgen_worldgen_config;
typedef struct refrain_services_worldgen_generate_outcome refrain_services_worldgen_generate_outcome;

typedef uint32_t refrain_services_worldgen_worldgen_error; // enum 'worldgen_error'
#define REFRAIN_SERVICES_WORLDGEN_WORLDGEN_ERROR_INVALID_CONFIG ((refrain_services_worldgen_worldgen_error)(1))
#define REFRAIN_SERVICES_WORLDGEN_WORLDGEN_ERROR_CANCELLED ((refrain_services_worldgen_worldgen_error)(2))
#define REFRAIN_SERVICES_WORLDGEN_WORLDGEN_ERROR_PROVIDER_UNAVAILABLE ((refrain_services_worldgen_worldgen_error)(3))
#define REFRAIN_SERVICES_WORLDGEN_WORLDGEN_ERROR_INTERNAL ((refrain_services_worldgen_worldgen_error)(4))

// Generator stages, mirroring the vanilla chunk status pipeline.
typedef uint32_t refrain_services_worldgen_gen_stage; // enum 'gen_stage'
#define REFRAIN_SERVICES_WORLDGEN_GEN_STAGE_EMPTY ((refrain_services_worldgen_gen_stage)(0))
#define REFRAIN_SERVICES_WORLDGEN_GEN_STAGE_BIOMES ((refrain_services_worldgen_gen_stage)(1))
#define REFRAIN_SERVICES_WORLDGEN_GEN_STAGE_NOISE ((refrain_services_worldgen_gen_stage)(2))
#define REFRAIN_SERVICES_WORLDGEN_GEN_STAGE_SURFACE ((refrain_services_worldgen_gen_stage)(3))
#define REFRAIN_SERVICES_WORLDGEN_GEN_STAGE_FEATURES ((refrain_services_worldgen_gen_stage)(4))
#define REFRAIN_SERVICES_WORLDGEN_GEN_STAGE_FULL ((refrain_services_worldgen_gen_stage)(5))

struct refrain_services_worldgen_worldgen_config {
    uint32_t struct_size;
    uint8_t reserved_0[4];
    uint64_t seed;
    int32_t view_distance;
    uint32_t flags;
    refrain_str preset;
}; // refrain_services_worldgen_worldgen_config size = 40
#define REFRAIN_SERVICES_WORLDGEN_WORLDGEN_CONFIG_SIZE ((uint32_t)sizeof(refrain_services_worldgen_worldgen_config))

struct refrain_services_worldgen_generate_outcome {
    uint32_t struct_size;
    refrain_services_worldgen_gen_stage stage;
    uint32_t generated_blocks;
    uint8_t reserved_0[4];
    uint64_t elapsed_us;
}; // refrain_services_worldgen_generate_outcome size = 24
#define REFRAIN_SERVICES_WORLDGEN_GENERATE_OUTCOME_SIZE ((uint32_t)sizeof(refrain_services_worldgen_generate_outcome))

#define REFRAIN_SERVICES_WORLDGEN_SERVICE_ID "refrain:worldgen"
#define REFRAIN_SERVICES_WORLDGEN_ABI_MAJOR 1u
#define REFRAIN_SERVICES_WORLDGEN_ABI_MINOR 0u
#define REFRAIN_SERVICES_WORLDGEN_ABI_PATCH 0u
#define REFRAIN_SERVICES_WORLDGEN_SIDE REFRAIN_SIDE_SERVER
#define REFRAIN_SERVICES_WORLDGEN_THREAD REFRAIN_THREAD_WORKER
#define REFRAIN_SERVICES_WORLDGEN_COMPOSITION REFRAIN_COMPOSITION_EXCLUSIVE
#define REFRAIN_SERVICES_WORLDGEN_CAPABILITIES "structures,biomes,carvers,noise"
#define REFRAIN_SERVICES_WORLDGEN_REQUIRES "refrain:core>=1.0.0,refrain:world>=1.0.0,refrain:data.packs>=1.0.0"
#define REFRAIN_SERVICES_WORLDGEN_SERVICE_IDS "refrain:worldgen"

typedef struct refrain_services_worldgen_vtable {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t abi_patch;
    // World-level setup; called once before any generation job is submitted.
    // attributes: #[thread(main)] #[on_load_only] #[can_fail]
    refrain_status (*configure)(void *self, const refrain_services_worldgen_worldgen_config *config);
    // Generate all stages up to `full` for one chunk column.
    // attributes: #[thread(worker)] #[can_fail]
    refrain_status (*generate_chunk)(void *self, refrain_core_handle_gen_ctx ctx, const refrain_core_chunk_pos *pos, refrain_core_chunk_buffer *buffer, refrain_services_worldgen_generate_outcome *out_value);
    // Advance one chunk to a specific stage (status-driven parallel pipeline).
    // attributes: #[thread(worker)] #[can_fail]
    refrain_status (*generate_stage)(void *self, refrain_core_handle_gen_ctx ctx, const refrain_core_chunk_pos *pos, refrain_services_worldgen_gen_stage target, refrain_core_chunk_buffer *buffer, refrain_services_worldgen_generate_outcome *out_value);
    // Structure placement query used by the session/chunk-streaming layer.
    // attributes: #[thread(worker)] #[can_fail]
    refrain_status (*locate_structure)(void *self, refrain_core_handle_gen_ctx ctx, const refrain_core_chunk_pos *origin, uint32_t structure_id, refrain_core_block_pos *out_value);
} refrain_services_worldgen_vtable; // service refrain:worldgen

#define REFRAIN_SERVICES_WORLDGEN_VTABLE_SIZE ((uint32_t)sizeof(refrain_services_worldgen_vtable))

#ifdef __cplusplus
} // extern "C"
#endif

#endif // REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_WORLDGEN_ABI_H
