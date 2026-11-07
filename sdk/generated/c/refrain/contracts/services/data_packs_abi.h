// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 3e46c08897b5a857

#ifndef REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_DATA_PACKS_ABI_H
#define REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_DATA_PACKS_ABI_H

#include <refrain/contracts/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct refrain_services_data_packs_pack_source refrain_services_data_packs_pack_source;
typedef struct refrain_services_data_packs_pack_info refrain_services_data_packs_pack_info;

typedef uint32_t refrain_services_data_packs_pack_kind; // enum 'pack_kind'
#define REFRAIN_SERVICES_DATA_PACKS_PACK_KIND_DATA ((refrain_services_data_packs_pack_kind)(0))
#define REFRAIN_SERVICES_DATA_PACKS_PACK_KIND_RESOURCE ((refrain_services_data_packs_pack_kind)(1))
#define REFRAIN_SERVICES_DATA_PACKS_PACK_KIND_MIXED ((refrain_services_data_packs_pack_kind)(2))

// Pack errors. Zero is success, so entries start at 1.
typedef uint32_t refrain_services_data_packs_pack_error; // enum 'pack_error'
#define REFRAIN_SERVICES_DATA_PACKS_PACK_ERROR_NOT_FOUND ((refrain_services_data_packs_pack_error)(1))
#define REFRAIN_SERVICES_DATA_PACKS_PACK_ERROR_INVALID_FORMAT ((refrain_services_data_packs_pack_error)(2))
#define REFRAIN_SERVICES_DATA_PACKS_PACK_ERROR_VERSION_MISMATCH ((refrain_services_data_packs_pack_error)(3))
#define REFRAIN_SERVICES_DATA_PACKS_PACK_ERROR_IO_ERROR ((refrain_services_data_packs_pack_error)(4))
#define REFRAIN_SERVICES_DATA_PACKS_PACK_ERROR_ALREADY_MOUNTED ((refrain_services_data_packs_pack_error)(5))

// Opaque handle for one mounted pack.
typedef uint64_t refrain_services_data_packs_handle_pack;
#define REFRAIN_SERVICES_DATA_PACKS_HANDLE_PACK_INVALID ((refrain_services_data_packs_handle_pack)(REFRAIN_HANDLE_INVALID))

// Where a pack comes from; `path` is a UTF-8 filesystem path or virtual
// location understood by the provider.
struct refrain_services_data_packs_pack_source {
    uint32_t struct_size;
    uint8_t reserved_0[4];
    refrain_str path;
    refrain_services_data_packs_pack_kind kind;
    uint32_t pack_format;
}; // refrain_services_data_packs_pack_source size = 32
#define REFRAIN_SERVICES_DATA_PACKS_PACK_SOURCE_SIZE ((uint32_t)sizeof(refrain_services_data_packs_pack_source))

struct refrain_services_data_packs_pack_info {
    uint32_t struct_size;
    uint8_t reserved_0[4];
    refrain_services_data_packs_handle_pack pack;
    refrain_str id;
    refrain_str version;
    uint32_t pack_format;
    uint8_t reserved_1[4];
    uint64_t file_count;
}; // refrain_services_data_packs_pack_info size = 64
#define REFRAIN_SERVICES_DATA_PACKS_PACK_INFO_SIZE ((uint32_t)sizeof(refrain_services_data_packs_pack_info))

#define REFRAIN_SERVICES_DATA_PACKS_SERVICE_ID "refrain:data.packs"
#define REFRAIN_SERVICES_DATA_PACKS_ABI_MAJOR 1u
#define REFRAIN_SERVICES_DATA_PACKS_ABI_MINOR 0u
#define REFRAIN_SERVICES_DATA_PACKS_ABI_PATCH 0u
#define REFRAIN_SERVICES_DATA_PACKS_SIDE REFRAIN_SIDE_BOTH
#define REFRAIN_SERVICES_DATA_PACKS_THREAD REFRAIN_THREAD_MAIN
#define REFRAIN_SERVICES_DATA_PACKS_COMPOSITION REFRAIN_COMPOSITION_EXCLUSIVE
#define REFRAIN_SERVICES_DATA_PACKS_CAPABILITIES "jar,datapack,resourcepack"
#define REFRAIN_SERVICES_DATA_PACKS_REQUIRES "refrain:core>=1.0.0"
#define REFRAIN_SERVICES_DATA_PACKS_SERVICE_IDS "refrain:data.packs"

typedef struct refrain_services_data_packs_vtable {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t abi_patch;
    // Mount a pack and return its metadata. Only callable while the registry
    // is still open (on_load phase).
    // attributes: #[thread(main)] #[on_load_only] #[can_fail]
    refrain_status (*mount)(void *self, const refrain_services_data_packs_pack_source *source, refrain_services_data_packs_pack_info *out_value);
    // attributes: #[thread(main)] #[on_load_only] #[can_fail]
    refrain_status (*unmount)(void *self, refrain_services_data_packs_handle_pack handle);
    // Number of mounted packs.
    // attributes: #[thread(any)]
    uint32_t (*mounted_count)(void *self);
    // Borrowed resource bytes; valid until the next registry mutation.
    // attributes: #[thread(worker)] #[borrow] #[can_fail]
    refrain_status (*read_resource)(void *self, refrain_services_data_packs_handle_pack handle, refrain_str name, refrain_bytes *out_value);
} refrain_services_data_packs_vtable; // service refrain:data.packs

#define REFRAIN_SERVICES_DATA_PACKS_VTABLE_SIZE ((uint32_t)sizeof(refrain_services_data_packs_vtable))

#ifdef __cplusplus
} // extern "C"
#endif

#endif // REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_DATA_PACKS_ABI_H
