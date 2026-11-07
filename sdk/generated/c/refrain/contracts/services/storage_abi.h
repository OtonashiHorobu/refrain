// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 26f951db567137ba

#ifndef REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_STORAGE_ABI_H
#define REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_STORAGE_ABI_H

#include <refrain/contracts/abi.h>
#include <refrain/contracts/core_abi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct refrain_services_storage_world_open_config refrain_services_storage_world_open_config;
typedef struct refrain_services_storage_chunk_key refrain_services_storage_chunk_key;
typedef struct refrain_services_storage_chunk_read_result refrain_services_storage_chunk_read_result;

typedef uint32_t refrain_services_storage_storage_error; // enum 'storage_error'
#define REFRAIN_SERVICES_STORAGE_STORAGE_ERROR_OPEN_FAILED ((refrain_services_storage_storage_error)(1))
#define REFRAIN_SERVICES_STORAGE_STORAGE_ERROR_NOT_FOUND ((refrain_services_storage_storage_error)(2))
#define REFRAIN_SERVICES_STORAGE_STORAGE_ERROR_CORRUPT_DATA ((refrain_services_storage_storage_error)(3))
#define REFRAIN_SERVICES_STORAGE_STORAGE_ERROR_IO_ERROR ((refrain_services_storage_storage_error)(4))
#define REFRAIN_SERVICES_STORAGE_STORAGE_ERROR_VERSION_MISMATCH ((refrain_services_storage_storage_error)(5))
#define REFRAIN_SERVICES_STORAGE_STORAGE_ERROR_READ_ONLY ((refrain_services_storage_storage_error)(6))

struct refrain_services_storage_world_open_config {
    uint32_t struct_size;
    uint8_t reserved_0[4];
    refrain_str world_path;
    refrain_str gamepack_id;
    uint32_t flags;
    uint8_t reserved_1[4];
}; // refrain_services_storage_world_open_config size = 48
#define REFRAIN_SERVICES_STORAGE_WORLD_OPEN_CONFIG_SIZE ((uint32_t)sizeof(refrain_services_storage_world_open_config))

struct refrain_services_storage_chunk_key {
    uint32_t struct_size;
    refrain_core_chunk_pos pos;
    uint32_t format_version;
}; // refrain_services_storage_chunk_key size = 20
#define REFRAIN_SERVICES_STORAGE_CHUNK_KEY_SIZE ((uint32_t)sizeof(refrain_services_storage_chunk_key))

struct refrain_services_storage_chunk_read_result {
    uint32_t struct_size;
    uint32_t bytes_read;
    uint32_t format_version;
    uint8_t reserved_0[4];
    uint64_t record_version;
}; // refrain_services_storage_chunk_read_result size = 24
#define REFRAIN_SERVICES_STORAGE_CHUNK_READ_RESULT_SIZE ((uint32_t)sizeof(refrain_services_storage_chunk_read_result))

#define REFRAIN_SERVICES_STORAGE_SERVICE_ID "refrain:storage"
#define REFRAIN_SERVICES_STORAGE_ABI_MAJOR 1u
#define REFRAIN_SERVICES_STORAGE_ABI_MINOR 0u
#define REFRAIN_SERVICES_STORAGE_ABI_PATCH 0u
#define REFRAIN_SERVICES_STORAGE_SIDE REFRAIN_SIDE_SERVER
#define REFRAIN_SERVICES_STORAGE_THREAD REFRAIN_THREAD_IO
#define REFRAIN_SERVICES_STORAGE_COMPOSITION REFRAIN_COMPOSITION_EXCLUSIVE
#define REFRAIN_SERVICES_STORAGE_CAPABILITIES "transactions,compaction,anvil-import,anvil-export"
#define REFRAIN_SERVICES_STORAGE_REQUIRES "refrain:core>=1.0.0"
#define REFRAIN_SERVICES_STORAGE_SERVICE_IDS "refrain:storage"

typedef struct refrain_services_storage_vtable {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t abi_patch;
    // Open (or create) a world store. The returned handle is owned by the
    // caller and released with close_world.
    // attributes: #[thread(main)] #[can_fail]
    refrain_status (*open_world)(void *self, const refrain_services_storage_world_open_config *config, refrain_core_handle_storage_world *out_value);
    // attributes: #[thread(main)] #[can_fail]
    refrain_status (*close_world)(void *self, refrain_core_handle_storage_world world);
    // Read one chunk record into a caller provided buffer. The buffer's
    // word_count is updated to the actual size.
    // attributes: #[thread(worker)] #[can_fail]
    refrain_status (*read_chunk)(void *self, refrain_core_handle_storage_world world, const refrain_services_storage_chunk_key *key, refrain_core_chunk_buffer *buffer, refrain_services_storage_chunk_read_result *out_value);
    // Write one chunk record; the payload is borrowed for the duration of
    // the call (the provider copies before returning).
    // attributes: #[thread(worker)] #[can_fail]
    refrain_status (*write_chunk)(void *self, refrain_core_handle_storage_world world, const refrain_services_storage_chunk_key *key, refrain_bytes payload);
    // Flush pending writes (group commit). Called at tick boundaries.
    // attributes: #[thread(worker)] #[can_fail]
    refrain_status (*sync)(void *self, refrain_core_handle_storage_world world);
    // attributes: #[thread(any)]
    uint8_t (*is_open)(void *self, refrain_core_handle_storage_world world);
} refrain_services_storage_vtable; // service refrain:storage

#define REFRAIN_SERVICES_STORAGE_VTABLE_SIZE ((uint32_t)sizeof(refrain_services_storage_vtable))

#ifdef __cplusplus
} // extern "C"
#endif

#endif // REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_STORAGE_ABI_H
