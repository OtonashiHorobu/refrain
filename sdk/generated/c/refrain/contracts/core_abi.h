// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: f522b723b120f5d1

#ifndef REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_CORE_ABI_H
#define REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_CORE_ABI_H

#include <refrain/contracts/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct refrain_core_chunk_pos refrain_core_chunk_pos;
typedef struct refrain_core_block_pos refrain_core_block_pos;
typedef struct refrain_core_chunk_section_view refrain_core_chunk_section_view;
typedef struct refrain_core_chunk_view refrain_core_chunk_view;
typedef struct refrain_core_block_change refrain_core_block_change;
typedef struct refrain_core_chunk_buffer refrain_core_chunk_buffer;

// Error codes shared by all services. Zero is reserved for success
// (REFRAIN_STATUS_OK), so this enum starts at 1.
typedef uint32_t refrain_core_core_error; // enum 'core_error'
#define REFRAIN_CORE_CORE_ERROR_INVALID_ARGUMENT ((refrain_core_core_error)(1))
#define REFRAIN_CORE_CORE_ERROR_NOT_FOUND ((refrain_core_core_error)(2))
#define REFRAIN_CORE_CORE_ERROR_ALREADY_EXISTS ((refrain_core_core_error)(3))
#define REFRAIN_CORE_CORE_ERROR_PERMISSION_DENIED ((refrain_core_core_error)(4))
#define REFRAIN_CORE_CORE_ERROR_IO_ERROR ((refrain_core_core_error)(5))
#define REFRAIN_CORE_CORE_ERROR_OUT_OF_MEMORY ((refrain_core_core_error)(6))
#define REFRAIN_CORE_CORE_ERROR_UNSUPPORTED ((refrain_core_core_error)(7))
#define REFRAIN_CORE_CORE_ERROR_VERSION_MISMATCH ((refrain_core_core_error)(8))

typedef uint32_t refrain_core_chunk_status; // enum 'chunk_status'
#define REFRAIN_CORE_CHUNK_STATUS_EMPTY ((refrain_core_chunk_status)(0))
#define REFRAIN_CORE_CHUNK_STATUS_STRUCTURE_STARTS ((refrain_core_chunk_status)(1))
#define REFRAIN_CORE_CHUNK_STATUS_STRUCTURE_REFERENCES ((refrain_core_chunk_status)(2))
#define REFRAIN_CORE_CHUNK_STATUS_BIOMES ((refrain_core_chunk_status)(3))
#define REFRAIN_CORE_CHUNK_STATUS_NOISE ((refrain_core_chunk_status)(4))
#define REFRAIN_CORE_CHUNK_STATUS_SURFACE ((refrain_core_chunk_status)(5))
#define REFRAIN_CORE_CHUNK_STATUS_CARVERS ((refrain_core_chunk_status)(6))
#define REFRAIN_CORE_CHUNK_STATUS_FEATURES ((refrain_core_chunk_status)(7))
#define REFRAIN_CORE_CHUNK_STATUS_LIGHT ((refrain_core_chunk_status)(8))
#define REFRAIN_CORE_CHUNK_STATUS_SPAWN ((refrain_core_chunk_status)(9))
#define REFRAIN_CORE_CHUNK_STATUS_FULL ((refrain_core_chunk_status)(10))

typedef uint8_t refrain_core_block_face; // enum 'block_face'
#define REFRAIN_CORE_BLOCK_FACE_NORTH ((refrain_core_block_face)(0))
#define REFRAIN_CORE_BLOCK_FACE_EAST ((refrain_core_block_face)(1))
#define REFRAIN_CORE_BLOCK_FACE_SOUTH ((refrain_core_block_face)(2))
#define REFRAIN_CORE_BLOCK_FACE_WEST ((refrain_core_block_face)(3))
#define REFRAIN_CORE_BLOCK_FACE_UP ((refrain_core_block_face)(4))
#define REFRAIN_CORE_BLOCK_FACE_DOWN ((refrain_core_block_face)(5))

// Opaque host-side handles. The tag is the C++ handle type.
typedef uint64_t refrain_core_handle_player;
#define REFRAIN_CORE_HANDLE_PLAYER_INVALID ((refrain_core_handle_player)(REFRAIN_HANDLE_INVALID))

typedef uint64_t refrain_core_handle_connection;
#define REFRAIN_CORE_HANDLE_CONNECTION_INVALID ((refrain_core_handle_connection)(REFRAIN_HANDLE_INVALID))

typedef uint64_t refrain_core_handle_gen_ctx;
#define REFRAIN_CORE_HANDLE_GEN_CTX_INVALID ((refrain_core_handle_gen_ctx)(REFRAIN_HANDLE_INVALID))

typedef uint64_t refrain_core_handle_storage_world;
#define REFRAIN_CORE_HANDLE_STORAGE_WORLD_INVALID ((refrain_core_handle_storage_world)(REFRAIN_HANDLE_INVALID))

typedef struct refrain_core_slice_block_pos {
    const refrain_core_block_pos *data;
    uint64_t len;
} refrain_core_slice_block_pos; // slice<block_pos>

typedef struct refrain_core_slice_chunk_section_view {
    const refrain_core_chunk_section_view *data;
    uint64_t len;
} refrain_core_slice_chunk_section_view; // slice<chunk_section_view>

typedef struct refrain_core_slice_block_change {
    const refrain_core_block_change *data;
    uint64_t len;
} refrain_core_slice_block_change; // slice<block_change>

// Chunk column coordinate on the horizontal plane.
struct refrain_core_chunk_pos {
    uint32_t struct_size;
    int32_t x;
    int32_t z;
}; // refrain_core_chunk_pos size = 12
#define REFRAIN_CORE_CHUNK_POS_SIZE ((uint32_t)sizeof(refrain_core_chunk_pos))

// Block coordinate inside a chunk column.
struct refrain_core_block_pos {
    uint32_t struct_size;
    int32_t x;
    int32_t y;
    int32_t z;
}; // refrain_core_block_pos size = 16
#define REFRAIN_CORE_BLOCK_POS_SIZE ((uint32_t)sizeof(refrain_core_block_pos))

// One section of the Chunk ABI: the versioned in-memory layout shared by
// `refrain:world`, `refrain:worldgen` and `refrain:sim`.
struct refrain_core_chunk_section_view {
    uint32_t struct_size;
    int32_t section_y;
    uint16_t block_count;
    uint8_t bits_per_entry;
    uint8_t flags;
    uint8_t reserved_0[4];
    refrain_core_slice_block_pos block_entities;
}; // refrain_core_chunk_section_view size = 32
#define REFRAIN_CORE_CHUNK_SECTION_VIEW_SIZE ((uint32_t)sizeof(refrain_core_chunk_section_view))

// Borrowed view of a loaded chunk. The slice lifetime is the contract call
// during which it was produced.
struct refrain_core_chunk_view {
    uint32_t struct_size;
    refrain_core_chunk_pos pos;
    refrain_core_chunk_status status;
    uint32_t section_count;
    refrain_core_slice_chunk_section_view sections;
}; // refrain_core_chunk_view size = 40
#define REFRAIN_CORE_CHUNK_VIEW_SIZE ((uint32_t)sizeof(refrain_core_chunk_view))

// One block-state change, batched by the world/sim contracts.
struct refrain_core_block_change {
    uint32_t struct_size;
    refrain_core_block_pos pos;
    uint32_t old_state;
    uint32_t new_state;
}; // refrain_core_block_change size = 28
#define REFRAIN_CORE_BLOCK_CHANGE_SIZE ((uint32_t)sizeof(refrain_core_block_change))

// Caller-provided chunk serialization buffer (out parameters use this).
struct refrain_core_chunk_buffer {
    uint32_t struct_size;
    refrain_core_chunk_pos pos;
    uint32_t generation;
    uint32_t word_count;
    // Borrowed serialized payload. The provider owns the bytes; the view is
    // valid until the next call on the same world/gen context. Empty means
    // "no payload" (for example a rejected generation).
    refrain_bytes data;
}; // refrain_core_chunk_buffer size = 40
#define REFRAIN_CORE_CHUNK_BUFFER_SIZE ((uint32_t)sizeof(refrain_core_chunk_buffer))

#ifdef __cplusplus
} // extern "C"
#endif

#endif // REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_CORE_ABI_H
