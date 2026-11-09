// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 8597ddcddbb41e5d

#ifndef REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_HOST_PLATFORM_ABI_H
#define REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_HOST_PLATFORM_ABI_H

#include <refrain/contracts/abi.h>

#ifdef __cplusplus
extern "C" {
#endif


typedef uint32_t refrain_host_platform_platform_error; // enum 'platform_error'
#define REFRAIN_HOST_PLATFORM_PLATFORM_ERROR_INVALID_ARGUMENT ((refrain_host_platform_platform_error)(1))
#define REFRAIN_HOST_PLATFORM_PLATFORM_ERROR_NOT_FOUND ((refrain_host_platform_platform_error)(2))
#define REFRAIN_HOST_PLATFORM_PLATFORM_ERROR_PERMISSION_DENIED ((refrain_host_platform_platform_error)(3))
#define REFRAIN_HOST_PLATFORM_PLATFORM_ERROR_IO_ERROR ((refrain_host_platform_platform_error)(4))
#define REFRAIN_HOST_PLATFORM_PLATFORM_ERROR_OUT_OF_MEMORY ((refrain_host_platform_platform_error)(5))
#define REFRAIN_HOST_PLATFORM_PLATFORM_ERROR_UNSUPPORTED ((refrain_host_platform_platform_error)(6))
#define REFRAIN_HOST_PLATFORM_PLATFORM_ERROR_ALREADY_EXISTS ((refrain_host_platform_platform_error)(7))
#define REFRAIN_HOST_PLATFORM_PLATFORM_ERROR_READ_ONLY ((refrain_host_platform_platform_error)(8))
#define REFRAIN_HOST_PLATFORM_PLATFORM_ERROR_LIMIT_EXCEEDED ((refrain_host_platform_platform_error)(9))
#define REFRAIN_HOST_PLATFORM_PLATFORM_ERROR_INTERNAL ((refrain_host_platform_platform_error)(10))

typedef uint32_t refrain_host_platform_file_open_flags; // flags 'file_open_flags'
#define REFRAIN_HOST_PLATFORM_FILE_OPEN_FLAGS_READ_ONLY ((refrain_host_platform_file_open_flags)(1))
#define REFRAIN_HOST_PLATFORM_FILE_OPEN_FLAGS_READ_WRITE ((refrain_host_platform_file_open_flags)(2))
#define REFRAIN_HOST_PLATFORM_FILE_OPEN_FLAGS_CREATE ((refrain_host_platform_file_open_flags)(4))
#define REFRAIN_HOST_PLATFORM_FILE_OPEN_FLAGS_EXCLUSIVE ((refrain_host_platform_file_open_flags)(8))
#define REFRAIN_HOST_PLATFORM_FILE_OPEN_FLAGS_TRUNCATE ((refrain_host_platform_file_open_flags)(16))
#define REFRAIN_HOST_PLATFORM_FILE_OPEN_FLAGS_NO_FOLLOW ((refrain_host_platform_file_open_flags)(32))

typedef uint64_t refrain_host_platform_handle_file;
#define REFRAIN_HOST_PLATFORM_HANDLE_FILE_INVALID ((refrain_host_platform_handle_file)(REFRAIN_HANDLE_INVALID))

typedef uint64_t refrain_host_platform_handle_map;
#define REFRAIN_HOST_PLATFORM_HANDLE_MAP_INVALID ((refrain_host_platform_handle_map)(REFRAIN_HANDLE_INVALID))

#define REFRAIN_HOST_PLATFORM_SERVICE_ID "refrain:host.platform"
#define REFRAIN_HOST_PLATFORM_ABI_MAJOR 1u
#define REFRAIN_HOST_PLATFORM_ABI_MINOR 0u
#define REFRAIN_HOST_PLATFORM_ABI_PATCH 0u
#define REFRAIN_HOST_PLATFORM_SIDE REFRAIN_SIDE_BOTH
#define REFRAIN_HOST_PLATFORM_THREAD REFRAIN_THREAD_ANY
#define REFRAIN_HOST_PLATFORM_COMPOSITION REFRAIN_COMPOSITION_SEALED
#define REFRAIN_HOST_PLATFORM_CAPABILITIES "file,mmap,crash"
#define REFRAIN_HOST_PLATFORM_REQUIRES "refrain:core>=1.0.0"
#define REFRAIN_HOST_PLATFORM_SERVICE_IDS "refrain:host.platform"

typedef struct refrain_host_platform_vtable {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t abi_patch;
    // Open a host file. Exactly one of `read_only`/`read_write` must be set.
    // `no_follow` refuses a symlink at the final path component.
    // attributes: #[thread(any)] #[can_fail]
    refrain_status (*file_open)(void *self, refrain_str path, refrain_host_platform_file_open_flags flags, refrain_host_platform_handle_file *out_value);
    // attributes: #[thread(any)] #[can_fail]
    refrain_status (*file_close)(void *self, refrain_host_platform_handle_file handle);
    // Read exactly `buffer.len` bytes at `offset` into the caller's buffer.
    // Returns the byte count (the whole buffer) on success; a short read is
    // `io_error`.
    // attributes: #[thread(any)] #[can_fail]
    refrain_status (*file_read)(void *self, refrain_host_platform_handle_file handle, uint64_t offset, refrain_mut_bytes buffer, uint64_t *out_value);
    // Write `data` at `offset`, extending the file as needed.
    // attributes: #[thread(any)] #[can_fail]
    refrain_status (*file_write)(void *self, refrain_host_platform_handle_file handle, uint64_t offset, refrain_bytes data, uint64_t *out_value);
    // attributes: #[thread(any)] #[can_fail]
    refrain_status (*file_size)(void *self, refrain_host_platform_handle_file handle, uint64_t *out_value);
    // Flush data and length to stable storage.
    // attributes: #[thread(any)] #[can_fail]
    refrain_status (*file_sync)(void *self, refrain_host_platform_handle_file handle);
    // attributes: #[thread(any)]
    uint8_t (*file_exists)(void *self, refrain_str path);
    // Map a file read-only. The returned bytes from `mmap_data` stay valid
    // until `mmap_close`.
    // attributes: #[thread(any)] #[can_fail]
    refrain_status (*mmap_open)(void *self, refrain_str path, refrain_host_platform_handle_map *out_value);
    // attributes: #[thread(any)] #[can_fail]
    refrain_status (*mmap_data)(void *self, refrain_host_platform_handle_map handle, refrain_bytes *out_value);
    // attributes: #[thread(any)] #[can_fail]
    refrain_status (*mmap_close)(void *self, refrain_host_platform_handle_map handle);
    // Attach a key/value to the next crash report. Never fails; oversized or
    // invalid annotations are dropped by the crash reporter.
    // attributes: #[thread(any)]
    void (*crash_annotation)(void *self, refrain_str key, refrain_str value);
    // Host temporary directory as a borrowed UTF-8 string.
    // attributes: #[thread(any)]
    refrain_str (*temp_directory)(void *self);
} refrain_host_platform_vtable; // service refrain:host.platform

#define REFRAIN_HOST_PLATFORM_VTABLE_SIZE ((uint32_t)sizeof(refrain_host_platform_vtable))

#ifdef __cplusplus
} // extern "C"
#endif

#endif // REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_HOST_PLATFORM_ABI_H
