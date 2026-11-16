// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 9bfa84cb449b5a93

#ifndef REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_HOST_CORE_ABI_H
#define REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_HOST_CORE_ABI_H

#include <refrain/contracts/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct refrain_host_core_host_version refrain_host_core_host_version;
typedef struct refrain_host_core_provider_ref refrain_host_core_provider_ref;
typedef struct refrain_host_core_provider_handle refrain_host_core_provider_handle;

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
#define REFRAIN_HOST_CORE_HOST_ERROR_STALE ((refrain_host_core_host_error)(13))

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
// 
// A `provider_ref` carries no composition state. It stays usable only for a
// sealed/process-lifetime provider, or while the caller knows the queried
// slot has not been replaced; `query_service_handle` wraps it with the
// generation that answers that question at a tick boundary.
struct refrain_host_core_provider_ref {
    uint32_t struct_size;
    uint8_t reserved_0[4];
    void * vtable;
    void * self;
    uint32_t abi_major;
    uint32_t reserved;
}; // refrain_host_core_provider_ref size = 32
#define REFRAIN_HOST_CORE_PROVIDER_REF_SIZE ((uint32_t)sizeof(refrain_host_core_provider_ref))

// A provider table together with the composition state it was read from.
// 
// `generation` is the committed generation of the `(service, instance)` slot
// that produced `provider`. A non-zero generation is the value a caller
// hands back to `validate_handle` when it crosses a tick boundary or a
// composition epoch; zero identifies a sealed, process-lifetime provider
// table, which never changes and needs no re-query. `epoch` is the committed
// composition epoch at query time, for diagnostics and audit.
struct refrain_host_core_provider_handle {
    uint32_t struct_size;
    uint8_t reserved_0[4];
    refrain_host_core_provider_ref provider;
    uint64_t generation;
    uint64_t epoch;
}; // refrain_host_core_provider_handle size = 56
#define REFRAIN_HOST_CORE_PROVIDER_HANDLE_SIZE ((uint32_t)sizeof(refrain_host_core_provider_handle))

#define REFRAIN_HOST_CORE_SERVICE_ID "refrain:host.core"
#define REFRAIN_HOST_CORE_ABI_MAJOR 1u
#define REFRAIN_HOST_CORE_ABI_MINOR 1u
#define REFRAIN_HOST_CORE_ABI_PATCH 0u
#define REFRAIN_HOST_CORE_SIDE REFRAIN_SIDE_BOTH
#define REFRAIN_HOST_CORE_THREAD REFRAIN_THREAD_ANY
#define REFRAIN_HOST_CORE_COMPOSITION REFRAIN_COMPOSITION_SEALED
#define REFRAIN_HOST_CORE_CAPABILITIES "log,config,services,clock,alloc"
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
    // Read a value from the named config namespace. Namespaces are mounted by
    // the host composition root (`host`, `network`, `mod/<id>`, ...); a
    // native mod may read any mounted namespace, while a sandboxed guest is
    // restricted by the host import layer. The string is borrowed until the
    // next call on the same thread.
    // attributes: #[thread(any)] #[can_fail]
    refrain_status (*config_get_i64)(void *self, refrain_str ns, refrain_str key, int64_t *out_value);
    // attributes: #[thread(any)] #[can_fail]
    refrain_status (*config_get_bool)(void *self, refrain_str ns, refrain_str key, uint8_t *out_value);
    // attributes: #[thread(any)] #[can_fail]
    refrain_status (*config_get_string)(void *self, refrain_str ns, refrain_str key, refrain_str *out_value);
    // Look up a service provider without composition state. `instance` is
    // empty for the default instance.
    // 
    // This is the legacy/process-lifetime form: the returned table is the
    // provider committed at the call, and nothing in the result tells the
    // caller whether a later composition epoch replaced it. Only a sealed
    // host table (which never changes) and a caller that never crosses a
    // composition boundary may keep the result. Use `query_service_handle`
    // for an ordinary service the caller caches across ticks.
    // 
    // Errors: `not_found` when the instance has no active provider, `stale`
    // while the instance is quiescing for a staged replacement.
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
    // Look up a service provider together with the generation it was read
    // from. `instance` is empty for the default instance.
    // 
    // A sealed host table is returned with `generation == 0`: it is
    // process-lifetime and never needs validation. Any other service returns
    // the generation of its committed slot; a caller caches the handle for
    // one tick and either calls `validate_handle` at the next tick boundary or
    // re-queries around a composition epoch.
    // 
    // Errors: `not_found` when the instance has no active provider (never
    // active or retired), `stale` while the instance is quiescing for a
    // staged replacement.
    // attributes: #[thread(any)] #[can_fail]
    refrain_status (*query_service_handle)(void *self, refrain_str service, refrain_str instance, refrain_host_core_provider_handle *out_value);
    // Check a cached provider generation against the committed composition.
    // 
    // Success means `generation` is the current committed generation, or that
    // `service` is a sealed host table and `generation` is zero. `stale`
    // means the slot was replaced, retired or is quiescing: the caller must
    // re-query and use the new table instead of the one it cached. The
    // generation identifies a slot of one service instance, so the caller
    // passes the same `service` and `instance` it queried with.
    // attributes: #[thread(any)] #[can_fail]
    refrain_status (*validate_handle)(void *self, refrain_str service, refrain_str instance, uint64_t generation);
} refrain_host_core_vtable; // service refrain:host.core

#define REFRAIN_HOST_CORE_VTABLE_SIZE ((uint32_t)sizeof(refrain_host_core_vtable))

#ifdef __cplusplus
} // extern "C"
#endif

#endif // REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_HOST_CORE_ABI_H
