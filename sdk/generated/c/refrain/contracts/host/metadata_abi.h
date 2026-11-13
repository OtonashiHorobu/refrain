// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 996e38144c4577ab

#ifndef REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_HOST_METADATA_ABI_H
#define REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_HOST_METADATA_ABI_H

#include <refrain/contracts/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct refrain_host_metadata_type_info refrain_host_metadata_type_info;
typedef struct refrain_host_metadata_field_info refrain_host_metadata_field_info;
typedef struct refrain_host_metadata_enum_value_info refrain_host_metadata_enum_value_info;
typedef struct refrain_host_metadata_contract_info refrain_host_metadata_contract_info;

typedef uint32_t refrain_host_metadata_metadata_error; // enum 'metadata_error'
#define REFRAIN_HOST_METADATA_METADATA_ERROR_INVALID_ARGUMENT ((refrain_host_metadata_metadata_error)(1))
#define REFRAIN_HOST_METADATA_METADATA_ERROR_NOT_FOUND ((refrain_host_metadata_metadata_error)(2))
#define REFRAIN_HOST_METADATA_METADATA_ERROR_INTERNAL ((refrain_host_metadata_metadata_error)(3))

typedef uint32_t refrain_host_metadata_type_kind; // enum 'type_kind'
#define REFRAIN_HOST_METADATA_TYPE_KIND_STRUCTURE ((refrain_host_metadata_type_kind)(0))
#define REFRAIN_HOST_METADATA_TYPE_KIND_ENUMERATION ((refrain_host_metadata_type_kind)(1))
#define REFRAIN_HOST_METADATA_TYPE_KIND_FLAGS ((refrain_host_metadata_type_kind)(2))
#define REFRAIN_HOST_METADATA_TYPE_KIND_RECORD ((refrain_host_metadata_type_kind)(3))
#define REFRAIN_HOST_METADATA_TYPE_KIND_PACKET ((refrain_host_metadata_type_kind)(4))

struct refrain_host_metadata_type_info {
    uint32_t struct_size;
    uint8_t reserved_0[4];
    refrain_str id;
    refrain_host_metadata_type_kind kind;
    uint32_t size;
    uint32_t alignment;
    uint32_t field_count;
    uint32_t enum_count;
    uint8_t reserved_1[4];
}; // refrain_host_metadata_type_info size = 48
#define REFRAIN_HOST_METADATA_TYPE_INFO_SIZE ((uint32_t)sizeof(refrain_host_metadata_type_info))

struct refrain_host_metadata_field_info {
    uint32_t struct_size;
    uint8_t reserved_0[4];
    refrain_str name;
    refrain_str type_id;
    uint32_t offset;
    uint32_t size;
    uint32_t flags;
    uint8_t reserved_1[4];
}; // refrain_host_metadata_field_info size = 56
#define REFRAIN_HOST_METADATA_FIELD_INFO_SIZE ((uint32_t)sizeof(refrain_host_metadata_field_info))

struct refrain_host_metadata_enum_value_info {
    uint32_t struct_size;
    uint8_t reserved_0[4];
    refrain_str name;
    int64_t value;
}; // refrain_host_metadata_enum_value_info size = 32
#define REFRAIN_HOST_METADATA_ENUM_VALUE_INFO_SIZE ((uint32_t)sizeof(refrain_host_metadata_enum_value_info))

struct refrain_host_metadata_contract_info {
    uint32_t struct_size;
    uint8_t reserved_0[4];
    refrain_str id;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t abi_patch;
    uint8_t reserved_1[4];
    refrain_str schema_hash;
    uint32_t method_count;
    uint8_t reserved_2[4];
}; // refrain_host_metadata_contract_info size = 64
#define REFRAIN_HOST_METADATA_CONTRACT_INFO_SIZE ((uint32_t)sizeof(refrain_host_metadata_contract_info))

#define REFRAIN_HOST_METADATA_SERVICE_ID "refrain:host.metadata"
#define REFRAIN_HOST_METADATA_ABI_MAJOR 1u
#define REFRAIN_HOST_METADATA_ABI_MINOR 0u
#define REFRAIN_HOST_METADATA_ABI_PATCH 0u
#define REFRAIN_HOST_METADATA_SIDE REFRAIN_SIDE_BOTH
#define REFRAIN_HOST_METADATA_THREAD REFRAIN_THREAD_ANY
#define REFRAIN_HOST_METADATA_COMPOSITION REFRAIN_COMPOSITION_SEALED
#define REFRAIN_HOST_METADATA_CAPABILITIES "types,contracts"
#define REFRAIN_HOST_METADATA_REQUIRES "refrain:core>=1.0.0"
#define REFRAIN_HOST_METADATA_SERVICE_IDS "refrain:host.metadata"

typedef struct refrain_host_metadata_vtable {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t abi_patch;
    // attributes: #[thread(any)]
    uint32_t (*type_count)(void *self);
    // attributes: #[thread(any)] #[can_fail]
    refrain_status (*type_at)(void *self, uint32_t index, refrain_host_metadata_type_info *out_value);
    // attributes: #[thread(any)] #[can_fail]
    refrain_status (*type_find)(void *self, refrain_str id, refrain_host_metadata_type_info *out_value);
    // attributes: #[thread(any)] #[can_fail]
    refrain_status (*field_at)(void *self, refrain_str type_id, uint32_t index, refrain_host_metadata_field_info *out_value);
    // attributes: #[thread(any)] #[can_fail]
    refrain_status (*enum_value_at)(void *self, refrain_str type_id, uint32_t index, refrain_host_metadata_enum_value_info *out_value);
    // attributes: #[thread(any)]
    uint32_t (*contract_count)(void *self);
    // attributes: #[thread(any)] #[can_fail]
    refrain_status (*contract_at)(void *self, uint32_t index, refrain_host_metadata_contract_info *out_value);
    // attributes: #[thread(any)]
    refrain_str (*schema_hash)(void *self);
} refrain_host_metadata_vtable; // service refrain:host.metadata

#define REFRAIN_HOST_METADATA_VTABLE_SIZE ((uint32_t)sizeof(refrain_host_metadata_vtable))

#ifdef __cplusplus
} // extern "C"
#endif

#endif // REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_HOST_METADATA_ABI_H
