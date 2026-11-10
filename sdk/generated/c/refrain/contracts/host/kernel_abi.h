// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 3a0899b8b7e5f861

#ifndef REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_HOST_KERNEL_ABI_H
#define REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_HOST_KERNEL_ABI_H

#include <refrain/contracts/abi.h>

#ifdef __cplusplus
extern "C" {
#endif


typedef uint32_t refrain_host_kernel_kernel_error; // enum 'kernel_error'
#define REFRAIN_HOST_KERNEL_KERNEL_ERROR_INVALID_ARGUMENT ((refrain_host_kernel_kernel_error)(1))
#define REFRAIN_HOST_KERNEL_KERNEL_ERROR_NOT_FOUND ((refrain_host_kernel_kernel_error)(2))
#define REFRAIN_HOST_KERNEL_KERNEL_ERROR_PERMISSION_DENIED ((refrain_host_kernel_kernel_error)(3))
#define REFRAIN_HOST_KERNEL_KERNEL_ERROR_UNSUPPORTED ((refrain_host_kernel_kernel_error)(4))
#define REFRAIN_HOST_KERNEL_KERNEL_ERROR_OUT_OF_MEMORY ((refrain_host_kernel_kernel_error)(5))
#define REFRAIN_HOST_KERNEL_KERNEL_ERROR_CANCELLED ((refrain_host_kernel_kernel_error)(6))
#define REFRAIN_HOST_KERNEL_KERNEL_ERROR_TIMEOUT ((refrain_host_kernel_kernel_error)(7))
#define REFRAIN_HOST_KERNEL_KERNEL_ERROR_THREAD_VIOLATION ((refrain_host_kernel_kernel_error)(8))
#define REFRAIN_HOST_KERNEL_KERNEL_ERROR_INTERNAL ((refrain_host_kernel_kernel_error)(9))

typedef uint64_t refrain_host_kernel_handle_job;
#define REFRAIN_HOST_KERNEL_HANDLE_JOB_INVALID ((refrain_host_kernel_handle_job)(REFRAIN_HANDLE_INVALID))

typedef uint64_t refrain_host_kernel_handle_subscription;
#define REFRAIN_HOST_KERNEL_HANDLE_SUBSCRIPTION_INVALID ((refrain_host_kernel_handle_subscription)(REFRAIN_HANDLE_INVALID))

// `void (*)(void *userdata)`.
typedef void (*refrain_host_kernel_job_callback)(void * userdata);

// `void (*)(void *userdata, void *payload, u64 size)`; the payload is
// borrowed for the duration of the callback.
typedef void (*refrain_host_kernel_event_callback)(void * userdata, void * payload, uint64_t size);

#define REFRAIN_HOST_KERNEL_SERVICE_ID "refrain:host.kernel"
#define REFRAIN_HOST_KERNEL_ABI_MAJOR 1u
#define REFRAIN_HOST_KERNEL_ABI_MINOR 0u
#define REFRAIN_HOST_KERNEL_ABI_PATCH 0u
#define REFRAIN_HOST_KERNEL_SIDE REFRAIN_SIDE_BOTH
#define REFRAIN_HOST_KERNEL_THREAD REFRAIN_THREAD_ANY
#define REFRAIN_HOST_KERNEL_COMPOSITION REFRAIN_COMPOSITION_SEALED
#define REFRAIN_HOST_KERNEL_CAPABILITIES "jobs,events"
#define REFRAIN_HOST_KERNEL_REQUIRES "refrain:core>=1.0.0"
#define REFRAIN_HOST_KERNEL_SERVICE_IDS "refrain:host.kernel"

typedef struct refrain_host_kernel_vtable {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t abi_patch;
    // Submit a background job. Priority: 0 interactive, 1 player-visible,
    // 2 normal, 3 background. The callback runs on a host worker thread.
    // attributes: #[thread(any)] #[can_fail]
    refrain_status (*job_submit)(void *self, uint32_t priority, refrain_host_kernel_job_callback callback, void * userdata, refrain_host_kernel_handle_job *out_value);
    // Mark a queued job as cancelled. A job already running is not
    // interrupted.
    // attributes: #[thread(any)] #[can_fail]
    refrain_status (*job_cancel)(void *self, refrain_host_kernel_handle_job handle);
    // Block until the job's callback returns. Must not be called from a job
    // callback (deadlock).
    // attributes: #[thread(any)] #[can_fail]
    refrain_status (*job_wait)(void *self, refrain_host_kernel_handle_job handle);
    // Subscribe to `event_type` (an application-defined 64-bit id). Returns a
    // handle released by `event_unsubscribe`.
    // attributes: #[thread(any)] #[can_fail]
    refrain_status (*event_subscribe)(void *self, uint64_t event_type, refrain_host_kernel_event_callback callback, void * userdata, refrain_host_kernel_handle_subscription *out_value);
    // attributes: #[thread(any)] #[can_fail]
    refrain_status (*event_unsubscribe)(void *self, refrain_host_kernel_handle_subscription handle);
    // Publish to every current subscriber on the calling thread. The payload
    // pointer is borrowed for the duration of the call.
    // attributes: #[thread(any)]
    void (*event_publish)(void *self, uint64_t event_type, void * payload, uint64_t size);
} refrain_host_kernel_vtable; // service refrain:host.kernel

#define REFRAIN_HOST_KERNEL_VTABLE_SIZE ((uint32_t)sizeof(refrain_host_kernel_vtable))

#ifdef __cplusplus
} // extern "C"
#endif

#endif // REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_HOST_KERNEL_ABI_H
