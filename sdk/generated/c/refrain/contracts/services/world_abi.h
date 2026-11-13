// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 9acb7bb41c6b4563

#ifndef REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_WORLD_ABI_H
#define REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_WORLD_ABI_H

#include <refrain/contracts/abi.h>
#include <refrain/contracts/core_abi.h>
#include <refrain/contracts/services/storage_abi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct refrain_services_world_world_config refrain_services_world_world_config;
typedef struct refrain_services_world_level_info refrain_services_world_level_info;
typedef struct refrain_services_world_level_projection refrain_services_world_level_projection;
typedef struct refrain_services_world_zstd_policy refrain_services_world_zstd_policy;

typedef uint32_t refrain_services_world_world_error; // enum 'world_error'
#define REFRAIN_SERVICES_WORLD_WORLD_ERROR_CHUNK_NOT_LOADED ((refrain_services_world_world_error)(1))
#define REFRAIN_SERVICES_WORLD_WORLD_ERROR_OUT_OF_BOUNDS ((refrain_services_world_world_error)(2))
#define REFRAIN_SERVICES_WORLD_WORLD_ERROR_NOT_IMPLEMENTED ((refrain_services_world_world_error)(3))
#define REFRAIN_SERVICES_WORLD_WORLD_ERROR_INTERNAL ((refrain_services_world_world_error)(4))
#define REFRAIN_SERVICES_WORLD_WORLD_ERROR_ALREADY_OPEN ((refrain_services_world_world_error)(5))
#define REFRAIN_SERVICES_WORLD_WORLD_ERROR_OPEN_FAILED ((refrain_services_world_world_error)(6))
#define REFRAIN_SERVICES_WORLD_WORLD_ERROR_STORAGE_UNAVAILABLE ((refrain_services_world_world_error)(7))

struct refrain_services_world_world_config {
    uint32_t struct_size;
    uint8_t reserved_0[4];
    // The store the world persists through. The runtime opened it once from
    // the level directory and passes the same handle to every provider of the
    // composition; the provider never opens the directory itself.
    refrain_services_storage_storage_handle storage;
    // Deterministic seed for the provider's RNG stream.
    int64_t seed;
}; // refrain_services_world_world_config size = 72
#define REFRAIN_SERVICES_WORLD_WORLD_CONFIG_SIZE ((uint32_t)sizeof(refrain_services_world_world_config))

// Read-only projection of the world's level record. A provider resolves it
// from the store it was handed; nothing here lets a provider open the level
// directory itself.
struct refrain_services_world_level_info {
    uint32_t struct_size;
    // `DataVersion` of the saved level.
    int32_t data_version;
    // World generation seed.
    int64_t seed;
    // Spawn position in block coordinates.
    int32_t spawn_x;
    int32_t spawn_y;
    int32_t spawn_z;
    uint8_t reserved_0[4];
    // `Time` of the saved level.
    int64_t time;
    // `LevelName` as stored; empty when the tag is absent.
    refrain_str level_name;
    // `GameType`: 0 survival, 1 creative, 2 adventure, 3 spectator.
    int32_t game_mode;
    // `hardcore` (vanilla stores it as a byte).
    uint8_t hardcore;
    uint8_t reserved_1[3];
    // `Difficulty`: 0 peaceful, 1 easy, 2 normal, 3 hard.
    int32_t difficulty;
    // `DifficultyLocked` (vanilla stores it as a byte).
    uint8_t difficulty_locked;
    uint8_t reserved_2[3];
}; // refrain_services_world_level_info size = 72
#define REFRAIN_SERVICES_WORLD_LEVEL_INFO_SIZE ((uint32_t)sizeof(refrain_services_world_level_info))

// Boot projection of the world level. `has_chunks` is true once the store
// holds a chunk, which tells the runtime that the spawn area does not need to
// be filled.
struct refrain_services_world_level_projection {
    uint32_t struct_size;
    uint8_t reserved_0[4];
    refrain_services_world_level_info level;
    uint8_t has_chunks;
    uint8_t reserved_1[7];
}; // refrain_services_world_level_projection size = 88
#define REFRAIN_SERVICES_WORLD_LEVEL_PROJECTION_SIZE ((uint32_t)sizeof(refrain_services_world_level_projection))

// Distance-adaptive zstd settings a provider applies to the chunks it stores.
// Levels are zstd compression levels; radii are horizontal chunk distances
// from the player. A chunk inside `near_radius` is stored with `near_level`,
// one inside `mid_radius` with `mid_level`, and everything beyond with
// `far_level`.
struct refrain_services_world_zstd_policy {
    uint32_t struct_size;
    int32_t near_level;
    int32_t mid_level;
    int32_t far_level;
    int32_t near_radius;
    int32_t mid_radius;
}; // refrain_services_world_zstd_policy size = 24
#define REFRAIN_SERVICES_WORLD_ZSTD_POLICY_SIZE ((uint32_t)sizeof(refrain_services_world_zstd_policy))

#define REFRAIN_SERVICES_WORLD_SERVICE_ID "refrain:world"
#define REFRAIN_SERVICES_WORLD_ABI_MAJOR 1u
#define REFRAIN_SERVICES_WORLD_ABI_MINOR 2u
#define REFRAIN_SERVICES_WORLD_ABI_PATCH 0u
#define REFRAIN_SERVICES_WORLD_SIDE REFRAIN_SIDE_SERVER
#define REFRAIN_SERVICES_WORLD_THREAD REFRAIN_THREAD_SIM
#define REFRAIN_SERVICES_WORLD_COMPOSITION REFRAIN_COMPOSITION_EXCLUSIVE
#define REFRAIN_SERVICES_WORLD_CAPABILITIES "lighting,heightmaps,block_entities"
#define REFRAIN_SERVICES_WORLD_REQUIRES "refrain:core>=1.0.0,refrain:data.packs>=1.0.0,refrain:storage>=1.0.0"
#define REFRAIN_SERVICES_WORLD_SERVICE_IDS "refrain:world"

typedef struct refrain_services_world_vtable {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t abi_patch;
    // Open the singleton world over the given store. The provider owns the
    // world until `close_world`; the server hosts exactly one world.
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
    // Read-only boot projection: the level record and whether the world
    // already holds a chunk. The provider answers from the store it was
    // opened over; the level directory stays private to the storage provider.
    // attributes: #[thread(sim)] #[can_fail]
    refrain_status (*level)(void *self, refrain_services_world_level_projection *out_value);
    // Optional. Effective zstd level the provider stores chunks with; a
    // provider that does not apply compression settings leaves the slot
    // empty.
    // attributes: #[thread(sim)]
    int32_t (*zstd_level)(void *self);
    // Optional. Replace the zstd level chunks are stored with. A provider
    // that does not apply compression settings leaves the slot empty, which
    // means the setting does not apply to it rather than that the call
    // failed.
    // attributes: #[thread(sim)] #[can_fail]
    refrain_status (*set_zstd_level)(void *self, int32_t level);
    // Optional. Replace the distance-adaptive zstd policy. A provider that
    // does not apply compression settings leaves the slot empty, which means
    // the setting does not apply to it rather than that the call failed.
    // attributes: #[thread(sim)] #[can_fail]
    refrain_status (*set_zstd_policy)(void *self, const refrain_services_world_zstd_policy *policy);
} refrain_services_world_vtable; // service refrain:world

#define REFRAIN_SERVICES_WORLD_VTABLE_SIZE ((uint32_t)sizeof(refrain_services_world_vtable))

#ifdef __cplusplus
} // extern "C"
#endif

#endif // REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_WORLD_ABI_H
