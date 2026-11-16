// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: c735c6db25ca24c9

#ifndef REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_MOD_LOADER_ABI_H
#define REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_MOD_LOADER_ABI_H

#include <refrain/contracts/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct refrain_services_mod_loader_mod_loader_capabilities refrain_services_mod_loader_mod_loader_capabilities;
typedef struct refrain_services_mod_loader_mod_entry_manifest refrain_services_mod_loader_mod_entry_manifest;
typedef struct refrain_services_mod_loader_mod_load_request refrain_services_mod_loader_mod_load_request;

// How a loaded mod can be removed. The host records the declaration and the
// lifecycle layer never assumes a stronger guarantee than this.
typedef uint8_t refrain_services_mod_loader_mod_loader_unload; // enum 'mod_loader_unload'
#define REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_UNLOAD_CLEAN ((refrain_services_mod_loader_mod_loader_unload)(0))
#define REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_UNLOAD_RETIRE ((refrain_services_mod_loader_mod_loader_unload)(1))
#define REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_UNLOAD_RESTART_REQUIRED ((refrain_services_mod_loader_mod_loader_unload)(2))

// Loader failures. Zero is success, so entries start at 1.
typedef uint32_t refrain_services_mod_loader_mod_loader_error; // enum 'mod_loader_error'
#define REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_ERROR_INVALID_ARGUMENT ((refrain_services_mod_loader_mod_loader_error)(1))
#define REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_ERROR_NOT_FOUND ((refrain_services_mod_loader_mod_loader_error)(2))
#define REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_ERROR_ALREADY_EXISTS ((refrain_services_mod_loader_mod_loader_error)(3))
#define REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_ERROR_PERMISSION_DENIED ((refrain_services_mod_loader_mod_loader_error)(4))
#define REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_ERROR_IO_ERROR ((refrain_services_mod_loader_mod_loader_error)(5))
#define REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_ERROR_UNSUPPORTED ((refrain_services_mod_loader_mod_loader_error)(6))
#define REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_ERROR_VERSION_MISMATCH ((refrain_services_mod_loader_mod_loader_error)(7))
#define REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_ERROR_SANDBOX_VIOLATION ((refrain_services_mod_loader_mod_loader_error)(8))
#define REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_ERROR_INTERNAL ((refrain_services_mod_loader_mod_loader_error)(9))

// Why the host is asking a loader to unload one instance.
typedef uint32_t refrain_services_mod_loader_mod_unload_reason; // enum 'mod_unload_reason'
#define REFRAIN_SERVICES_MOD_LOADER_MOD_UNLOAD_REASON_SHUTDOWN ((refrain_services_mod_loader_mod_unload_reason)(0))
#define REFRAIN_SERVICES_MOD_LOADER_MOD_UNLOAD_REASON_EXPLICIT_REQUEST ((refrain_services_mod_loader_mod_unload_reason)(1))
#define REFRAIN_SERVICES_MOD_LOADER_MOD_UNLOAD_REASON_RELOAD ((refrain_services_mod_loader_mod_unload_reason)(2))
#define REFRAIN_SERVICES_MOD_LOADER_MOD_UNLOAD_REASON_UNINSTALL ((refrain_services_mod_loader_mod_unload_reason)(3))

// Opaque handle for one mod instance a loader owns. Zero is invalid.
typedef uint64_t refrain_services_mod_loader_handle_mod_instance;
#define REFRAIN_SERVICES_MOD_LOADER_HANDLE_MOD_INSTANCE_INVALID ((refrain_services_mod_loader_handle_mod_instance)(REFRAIN_HANDLE_INVALID))

// What a loader can do. The host validates the declarations before it
// dispatches anything through the loader, and a provider that changes them
// while running fails the next load instead of silently reconfiguring.
struct refrain_services_mod_loader_mod_loader_capabilities {
    uint32_t struct_size;
    uint8_t reserved_0[4];
    // Loader id this provider implements; must equal the instance name the
    // host registered it on.
    refrain_str loader_id;
    // Loader ABI major the provider implements, compared with the contract
    // ABI major the host was built against.
    uint32_t abi_major;
    // Removal guarantee for every instance this loader owns.
    refrain_services_mod_loader_mod_loader_unload unload;
    // Guest code runs in a sandbox: it may not register providers, claim
    // services or receive a raw provider table.
    uint8_t sandboxed;
    // The loader can load new instances while the host is running.
    uint8_t supports_hot_install;
    uint8_t reserved_1[1];
}; // refrain_services_mod_loader_mod_loader_capabilities size = 32
#define REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_CAPABILITIES_SIZE ((uint32_t)sizeof(refrain_services_mod_loader_mod_loader_capabilities))

// Manifest subset the host hands to a loader. Strings are borrowed for the
// duration of the call.
struct refrain_services_mod_loader_mod_entry_manifest {
    uint32_t struct_size;
    uint8_t reserved_0[4];
    refrain_str mod_id;
    refrain_str version;
    // Manifest ABI major the mod was built against.
    uint32_t abi;
    // Manifest side: 1 server, 2 client, 3 both.
    uint32_t side;
    // Effective loader id the manifest resolves to.
    refrain_str loader;
    // Package root, empty for manifests read from the built-in image.
    refrain_str package_root;
    // Native shared library declared by `[entry].native`; empty when absent.
    refrain_str native_path;
    // Script module declared by `[entry].wasm`; empty when absent.
    refrain_str wasm_path;
    // Optional native entry symbol.
    refrain_str symbol;
    // The composition plan selected this mod for provider code. A loader may
    // skip code execution when false and keep the package as data only.
    uint8_t activate_code;
    uint8_t reserved_1[7];
}; // refrain_services_mod_loader_mod_entry_manifest size = 136
#define REFRAIN_SERVICES_MOD_LOADER_MOD_ENTRY_MANIFEST_SIZE ((uint32_t)sizeof(refrain_services_mod_loader_mod_entry_manifest))

// One load attempt for an already validated manifest. The loader owns the
// instance from `load` until `unload` or process exit.
struct refrain_services_mod_loader_mod_load_request {
    uint32_t struct_size;
    uint8_t reserved_0[4];
    refrain_services_mod_loader_mod_entry_manifest manifest;
}; // refrain_services_mod_loader_mod_load_request size = 144
#define REFRAIN_SERVICES_MOD_LOADER_MOD_LOAD_REQUEST_SIZE ((uint32_t)sizeof(refrain_services_mod_loader_mod_load_request))

#define REFRAIN_SERVICES_MOD_LOADER_SERVICE_ID "refrain:mod.loader"
#define REFRAIN_SERVICES_MOD_LOADER_ABI_MAJOR 1u
#define REFRAIN_SERVICES_MOD_LOADER_ABI_MINOR 1u
#define REFRAIN_SERVICES_MOD_LOADER_ABI_PATCH 0u
#define REFRAIN_SERVICES_MOD_LOADER_SIDE REFRAIN_SIDE_SERVER
#define REFRAIN_SERVICES_MOD_LOADER_THREAD REFRAIN_THREAD_MAIN
#define REFRAIN_SERVICES_MOD_LOADER_COMPOSITION REFRAIN_COMPOSITION_INSTANCED
#define REFRAIN_SERVICES_MOD_LOADER_CAPABILITIES "builtin,native,sandboxed,hot-install"
#define REFRAIN_SERVICES_MOD_LOADER_REQUIRES "refrain:core>=1.0.0"
#define REFRAIN_SERVICES_MOD_LOADER_SERVICE_IDS "refrain:mod.loader"

typedef struct refrain_services_mod_loader_vtable {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t abi_patch;
    // Fill in this loader's capabilities. The host calls it once at
    // registration and again before every dispatch to detect drift.
    // attributes: #[thread(main)]
    void (*capabilities)(void *self, refrain_services_mod_loader_mod_loader_capabilities *caps);
    // Check the manifest before any mod code runs. A loader rejects the entry
    // shape it does not own; the host has already validated identity,
    // version, side and host requirements.
    // attributes: #[thread(main)] #[can_fail]
    refrain_status (*validate)(void *self, const refrain_services_mod_loader_mod_entry_manifest *entry);
    // Load one validated manifest and return the handle the host records.
    // attributes: #[thread(main)] #[can_fail]
    refrain_status (*load)(void *self, const refrain_services_mod_loader_mod_load_request *request, refrain_services_mod_loader_handle_mod_instance *out_value);
    // Release one instance. The loader's declared unload semantics bound what
    // it may actually release.
    // attributes: #[thread(main)] #[can_fail]
    refrain_status (*unload)(void *self, refrain_services_mod_loader_handle_mod_instance instance, refrain_services_mod_loader_mod_unload_reason reason);
    // Build a replacement generation for one instance and return the new
    // handle. The old instance must stay alive and callable until the host
    // calls `unload` for it after the composition epoch commits: a staged
    // replacement cannot release code the running composition still calls.
    // A loader that cannot build a replacement without releasing the old
    // instance reports `unsupported`.
    // attributes: #[thread(main)] #[can_fail]
    refrain_status (*reload)(void *self, refrain_services_mod_loader_handle_mod_instance instance, const refrain_services_mod_loader_mod_load_request *request, refrain_services_mod_loader_handle_mod_instance *out_value);
} refrain_services_mod_loader_vtable; // service refrain:mod.loader

#define REFRAIN_SERVICES_MOD_LOADER_VTABLE_SIZE ((uint32_t)sizeof(refrain_services_mod_loader_vtable))

#ifdef __cplusplus
} // extern "C"
#endif

#endif // REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_MOD_LOADER_ABI_H
