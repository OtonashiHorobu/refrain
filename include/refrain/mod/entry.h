// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Stable entry ABI for external native mods. A shared library exports
// `refrain_mod_main` (or the symbol named by `[entry].symbol`) with
// `REFRAIN_MOD_MAIN`; the host calls it during mod loading, then calls the
// returned descriptor's `on_load` with the sealed `refrain:host.core` table.
//
// Include guard is the classic C form because the header is part of the
// public C ABI (C11 consumers), like <refrain/contracts/abi.h>.

#ifndef REFRAIN_MOD_ENTRY_H
#define REFRAIN_MOD_ENTRY_H

#include <refrain/contracts/abi.h>
#include <refrain/contracts/host/core_abi.h>
#include <refrain/platform/export.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REFRAIN_MOD_ABI_MAJOR 1u

/// Process-owned host runtime table passed to the mod entry point. Strings
/// are borrowed from the host for the duration of the call; the mod must
/// copy anything it keeps.
typedef struct refrain_mod_init {
    uint32_t struct_size;
    uint32_t abi_major;
    refrain_str mod_id;
    refrain_str mod_version;
    /// Sealed `refrain:host.core` table and its context. Both stay valid for
    /// the process lifetime.
    const refrain_host_core_vtable *host_core;
    void *host_core_self;
    /// Optional host allocator; all-zero when unused.
    refrain_allocator allocator;
} refrain_mod_init;

/// Per-mod context passed to `on_load`. The mod may query/register services
/// through `host_core`; the host validates the manifest and gamepack plan.
typedef struct refrain_mod_context {
    uint32_t struct_size;
    const refrain_host_core_vtable *host_core;
    void *host_core_self;
    refrain_str mod_id;
    refrain_str mod_version;
} refrain_mod_context;

/// Called once during mod loading. `user` is mod-owned state passed back to
/// `on_unload`; it may stay null.
typedef refrain_status (*refrain_mod_on_load)(
    const refrain_mod_context *context, void **user);

/// Called at host shutdown, after the service registry is no longer used.
/// Optional; the library stays loaded until after this callback returns.
typedef void (*refrain_mod_on_unload)(void *user);

typedef struct refrain_mod_desc {
    uint32_t struct_size;
    uint32_t abi_major;
    refrain_mod_on_load on_load;
    refrain_mod_on_unload on_unload;
} refrain_mod_desc;

typedef const refrain_mod_desc *(*refrain_mod_main_fn)(
    const refrain_mod_init *init);

#ifdef __cplusplus
#define REFRAIN_MOD_EXTERN_C extern "C"
#else
#define REFRAIN_MOD_EXTERN_C
#endif

/// Define the exported entry point:
///
///   REFRAIN_MOD_MAIN(refrain_mod_main) {
///       static const refrain_mod_desc desc{...};
///       return &desc;
///   }
#define REFRAIN_MOD_MAIN(name)                                                 \
    REFRAIN_MOD_EXTERN_C REFRAIN_EXPORT const refrain_mod_desc *name(      \
        const refrain_mod_init *init)

#ifdef __cplusplus
} // extern "C"
#endif

#endif // REFRAIN_MOD_ENTRY_H
