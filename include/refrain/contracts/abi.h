// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Refrain stable C ABI primitives.
//
// This header is hand-maintained (not generated) and is the smallest stable
// surface shared by every generated service contract and native plugin. The
// rules it implements are:
//   * fixed-width integers only (no long/size_t/unsigned in contracts),
//   * borrowed spans are { pointer, length } pairs, never NUL-terminated,
//   * handles are opaque uint64_t values, never cross-module pointers,
//   * cross-boundary structs start with `uint32_t struct_size`,
//   * a 32-bit status carries either REFRAIN_STATUS_OK or a contract error.
//
// The ABI major version only changes for incompatible layout/calling changes;
// minor versions add symbols/slots. Plugins declare the major they were built
// against in their manifest.
//
// Include guard: this header is deliberately guarded with
// `#ifndef REFRAIN_CONTRACTS_ABI_H` instead of `#pragma once`. It is part of
// the public C ABI and must compile as C11 (native plugins are not required to
// build as C++); `#pragma once` is a non-standard extension, so the classic
// guard is the documented exception to the repository's "#pragma once"
// convention. Keep the two in sync: every hand-written header is C++-only and
// uses `#pragma once`, every C-facing header uses the portable guard.

#ifndef REFRAIN_CONTRACTS_ABI_H
#define REFRAIN_CONTRACTS_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REFRAIN_ABI_MAJOR 1u
#define REFRAIN_ABI_MINOR 0u

/// Success code; generated contracts return contract-specific error enum
/// values (never zero) on failure.
#define REFRAIN_STATUS_OK 0u

typedef uint32_t refrain_status;

/// Opaque handle. Handles are host-owned table indices with a type tag in
/// debug builds; 0 is always the invalid/null handle.
typedef uint64_t refrain_handle;

#define REFRAIN_HANDLE_INVALID ((refrain_handle)0)

/// UTF-8 string view. Borrowed for the duration of the call unless the
/// contract documents otherwise; never assumed to be NUL-terminated.
typedef struct refrain_str {
    const uint8_t *data;
    uint64_t len;
} refrain_str;

/// Immutable byte span (borrowed).
typedef struct refrain_bytes {
    const uint8_t *data;
    uint64_t len;
} refrain_bytes;

/// Caller-provided output byte span (two-call pattern).
typedef struct refrain_mut_bytes {
    uint8_t *data;
    uint64_t len;
} refrain_mut_bytes;

/// Host allocator table: plugins allocate memory that outlives a call only
/// through the host, and the host only frees memory it allocated itself.
typedef struct refrain_allocator {
    void *self;
    void *(*alloc)(void *self, uint64_t size, uint64_t alignment);
    void (*free)(void *self, void *ptr);
} refrain_allocator;

/// Initialize the mandatory `struct_size` field of a generated ABI struct.
#define REFRAIN_ABI_INIT_STRUCT(ptr)                                           \
    ((ptr)->struct_size = (uint32_t)sizeof(*(ptr)))

/// Service metadata constants (also emitted per contract as macros).
#define REFRAIN_SIDE_NONE 0u
#define REFRAIN_SIDE_SERVER 1u
#define REFRAIN_SIDE_CLIENT 2u
#define REFRAIN_SIDE_BOTH 3u

#define REFRAIN_THREAD_ANY 0u
#define REFRAIN_THREAD_MAIN 1u
#define REFRAIN_THREAD_SIM 2u
#define REFRAIN_THREAD_RENDER 3u
#define REFRAIN_THREAD_WORKER 4u
#define REFRAIN_THREAD_IO 5u
#define REFRAIN_THREAD_NET 6u

/// Contract composition modes. A RIDL service declares its mode in
/// the contract (`composition = exclusive|instanced|chain|registry;`);
/// generated headers expose it as `<PREFIX>_COMPOSITION` and C++ consumers as
/// the typed `refrain::contracts::composition_kind`. The value is part of the
/// contract metadata, not of the vtable layout.
///
/// * exclusive (default, value 0 so zero-initialised metadata means "default"):
///   exactly one provider instance is active per service.
/// * instanced: several provider instances may be active and are addressed by
///   instance name.
/// * chain: providers wrap a base implementation (`attach(base)`) and
///   selectively override slots.
/// * registry: providers extend a registry-style service (many registered
///   entries under one service).
/// * sealed: the process owns the only provider; it is registered by the host
///   bootstrap and cannot be selected or replaced by a gamepack/mod.
#define REFRAIN_COMPOSITION_EXCLUSIVE 0u
#define REFRAIN_COMPOSITION_INSTANCED 1u
#define REFRAIN_COMPOSITION_CHAIN 2u
#define REFRAIN_COMPOSITION_REGISTRY 3u
/// The provider is owned by the process itself, not by a mod. A sealed
/// service is registered by the host bootstrap before mod loading; a gamepack
/// must not select it and a mod must not claim it in `[provides]`. Used for
/// host-owned runtime APIs (see the host runtime contract).
#define REFRAIN_COMPOSITION_SEALED 4u

/// ABI version as a single 32-bit value: (major << 16) | minor.
#define REFRAIN_ABI_VERSION                                                    \
    (((uint32_t)REFRAIN_ABI_MAJOR << 16u) | (uint32_t)REFRAIN_ABI_MINOR)

#ifdef __cplusplus
} // extern "C"
#endif

#endif // REFRAIN_CONTRACTS_ABI_H
