// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: a29acb77eba22b5e

#ifndef REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_HOST_CORE_ABI_H
#define REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_HOST_CORE_ABI_H

#include <refrain/contracts/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct refrain_host_core_host_version refrain_host_core_host_version;
typedef struct refrain_host_core_provider_ref refrain_host_core_provider_ref;

typedef uint32_t refrain_host_core_host_error; // enum 'host_error'
#define REFRAIN_HOST_CORE_HOST_ERROR_INVALID_ARGUMENT ((refrain_host_core_host_error)(1))
#define REFRAIN_HOST_CORE_HOST_ERROR_NOT_FOUND ((refrain_host_core_host_error)(2))
#define REFRAIN_HOST_CORE_HOST_ERROR_PERMISSION_DENIED ((refrain_host_core_host_error)(3))
#define REFRAIN_HOST_CORE_HOST_ERROR_VERSION_MISMATCH ((refrain_host_core_host_error)(4))
#define REFRAIN_HOST_CORE_HOST_ERROR_UNSUPPORTED ((refrain_host_core_host_error)(5))
#define REFRAIN_HOST_CORE_HOST_ERROR_NOT_INITIALIZED ((refrain_host_core_host_error)(6))
#define REFRAIN_HOST_CORE_HOST_ERROR_OUT_OF_MEMORY ((refrain_host_core_host_error)(7))
#define REFRAIN_HOST_CORE_HOST_ERROR_LIMIT_EXCEEDED ((refrain_host_core_host_error)(8))
#define REFRAIN_HOST_CORE_HOST_ERROR_THREAD_VIOLATION ((refrain_host_core_host_error)(9))
#define REFRAIN_HOST_CORE_HOST_ERROR_INTERNAL ((refrain_host_core_host_error)(10))
#define REFRAIN_HOST_CORE_HOST_ERROR_ALREADY_EXISTS ((refrain_host_core_host_error)(11))
#define REFRAIN_HOST_CORE_HOST_ERROR_IO_ERROR ((refrain_host_core_host_error)(12))

struct refrain_host_core_host_version {
    uint32_t struct_size;
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
}; // refrain_host_core_host_version size = 16
#define REFRAIN_HOST_CORE_HOST_VERSION_SIZE ((uint32_t)sizeof(refrain_host_core_host_version))

// A service provider table plus its instance. `vtable` and `self` are
// opaque: the consumer casts them to the generated vtable of the requested
// service. The host validates the vtable header before returning a ref.
struct refrain_host_core_provider_ref {
    uint32_t struct_size;
    uint8_t reserved_0[4];
    void * vtable;
    void * self;
    uint32_t abi_major;
    uint32_t reserved;
}; // refrain_host_core_provider_ref size = 32
#define REFRAIN_HOST_CORE_PROVIDER_REF_SIZE ((uint32_t)sizeof(refrain_host_core_provider_ref))

#define REFRAIN_HOST_CORE_SERVICE_ID "refrain:host.core"
#define REFRAIN_HOST_CORE_ABI_MAJOR 1u
#define REFRAIN_HOST_CORE_ABI_MINOR 0u
#define REFRAIN_HOST_CORE_ABI_PATCH 0u
#define REFRAIN_HOST_CORE_SIDE REFRAIN_SIDE_BOTH
#define REFRAIN_HOST_CORE_THREAD REFRAIN_THREAD_ANY
#define REFRAIN_HOST_CORE_COMPOSITION REFRAIN_COMPOSITION_SEALED
#define REFRAIN_HOST_CORE_CAPABILITIES "log,services,clock,alloc"
#define REFRAIN_HOST_CORE_REQUIRES "refrain:core>=1.0.0"
#define REFRAIN_HOST_CORE_SERVICE_IDS "refrain:host.core"

typedef struct refrain_host_core_vtable {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t abi_patch;
    // attributes: #[thread(any)]
    void (*version)(void *self, refrain_host_core_host_version *value);
    // attributes: #[thread(any)]
    void (*log)(void *self, uint32_t level, refrain_str message);
    // attributes: #[thread(any)]
    int64_t (*now_ns)(void *self);
    // attributes: #[thread(any)]
    int64_t (*tick_now)(void *self);
    // Read a value from the calling mod's config namespace. The host returns
    // `not_found` for a missing key; the string is borrowed until the next
    // config reload.
    // attributes: #[thread(any)] #[can_fail]
    refrain_status (*config_get_i64)(void *self, refrain_str key, int64_t *out_value);
    // attributes: #[thread(any)] #[can_fail]
    refrain_status (*config_get_bool)(void *self, refrain_str key, uint8_t *out_value);
    // attributes: #[thread(any)] #[can_fail]
    refrain_status (*config_get_string)(void *self, refrain_str key, refrain_str *out_value);
    // Look up a frozen service provider. `instance` is empty for the default
    // instance. The returned vtable/self stay valid for the process lifetime.
    // attributes: #[thread(any)] #[can_fail]
    refrain_status (*query_service)(void *self, refrain_str service, refrain_str instance, refrain_host_core_provider_ref *out_value);
    // Register the calling mod's provider during load. The host checks the
    // manifest `[provides]`, the gamepack plan and the vtable ABI header.
    // Sealed services are rejected here; they are registered by the host.
    // attributes: #[thread(main)] #[can_fail]
    refrain_status (*register_provider)(void *self, refrain_str service, refrain_str instance, const refrain_host_core_provider_ref *ref);
    // Allocate a host-owned block. The caller releases it with `free`.
    // attributes: #[thread(any)] #[can_fail]
    refrain_status (*alloc)(void *self, uint64_t size, uint64_t alignment, refrain_mut_bytes *out_value);
    // attributes: #[thread(any)]
    void (*free)(void *self, refrain_mut_bytes block);
    // Thread-local diagnostic for the last failed host call on this thread.
    // attributes: #[thread(any)] #[can_fail]
    refrain_status (*last_error_message)(void *self, refrain_str *out_value);
} refrain_host_core_vtable; // service refrain:host.core

#define REFRAIN_HOST_CORE_VTABLE_SIZE ((uint32_t)sizeof(refrain_host_core_vtable))

#ifdef __cplusplus
} // extern "C"
#endif

#endif // REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_HOST_CORE_ABI_H
