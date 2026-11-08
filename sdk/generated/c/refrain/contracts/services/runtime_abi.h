// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 927eab4ffcbbd2c4

#ifndef REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_RUNTIME_ABI_H
#define REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_RUNTIME_ABI_H

#include <refrain/contracts/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct refrain_services_runtime_runtime_config refrain_services_runtime_runtime_config;

typedef uint32_t refrain_services_runtime_runtime_error; // enum 'runtime_error'
#define REFRAIN_SERVICES_RUNTIME_RUNTIME_ERROR_INVALID_CONFIG ((refrain_services_runtime_runtime_error)(1))
#define REFRAIN_SERVICES_RUNTIME_RUNTIME_ERROR_IO_ERROR ((refrain_services_runtime_runtime_error)(2))
#define REFRAIN_SERVICES_RUNTIME_RUNTIME_ERROR_INTERNAL ((refrain_services_runtime_runtime_error)(3))

// Process-wide runtime configuration assembled by the host from the parsed
// CLI. Strings are borrowed for the duration of `run`; the provider copies
// any value it needs to keep.
struct refrain_services_runtime_runtime_config {
    uint32_t struct_size;
    uint8_t reserved_0[4];
    // RWF world directory (`--world`); required by `run`.
    refrain_str world_path;
    // Run exactly this many ticks, then stop. 0 = until `stop`/EOF.
    int64_t ticks;
    // Optional batch command file (one console command per line, tick 0).
    refrain_str command_file;
    // Optional tick-indexed replay script (`<tick> <command>` per line).
    refrain_str script_file;
    // Log the state hash every N ticks; 0 disables.
    uint64_t hash_every;
    // Default zstd level for regions opened by the runtime (1..19).
    int32_t world_zstd_level;
    // Adaptive zstd policy (levels 1..19, radii in chunks, near <= mid).
    int32_t zstd_near_level;
    int32_t zstd_mid_level;
    int32_t zstd_far_level;
    int32_t zstd_near_radius;
    int32_t zstd_mid_radius;
    // 0 disables the tick watchdog.
    uint32_t watchdog_timeout_ms;
    uint8_t reserved_1[4];
    // Flush dirty chunks every N ticks; 0 disables periodic autosave.
    uint64_t autosave_interval_ticks;
    // Try runtime region compaction every N ticks; 0 disables.
    uint64_t compact_interval_ticks;
    // Minimum region bytes before compaction is considered.
    uint64_t compact_min_bytes;
    uint32_t compact_garbage_percent;
    uint8_t reserved_2[4];
    // Compact aux object/blob stores every N ticks; 0 disables.
    uint64_t store_compact_interval_ticks;
    // Optional content store for periodic mark-sweep GC.
    refrain_str content_store;
    uint8_t content_gc_apply;
    uint8_t reserved_3[3];
    int32_t content_gc_keep_days;
    uint64_t content_gc_budget_ms;
    uint64_t content_gc_interval_ticks;
}; // refrain_services_runtime_runtime_config size = 184
#define REFRAIN_SERVICES_RUNTIME_RUNTIME_CONFIG_SIZE ((uint32_t)sizeof(refrain_services_runtime_runtime_config))

#define REFRAIN_SERVICES_RUNTIME_SERVICE_ID "refrain:runtime"
#define REFRAIN_SERVICES_RUNTIME_ABI_MAJOR 2u
#define REFRAIN_SERVICES_RUNTIME_ABI_MINOR 0u
#define REFRAIN_SERVICES_RUNTIME_ABI_PATCH 0u
#define REFRAIN_SERVICES_RUNTIME_SIDE REFRAIN_SIDE_SERVER
#define REFRAIN_SERVICES_RUNTIME_THREAD REFRAIN_THREAD_MAIN
#define REFRAIN_SERVICES_RUNTIME_COMPOSITION REFRAIN_COMPOSITION_EXCLUSIVE
#define REFRAIN_SERVICES_RUNTIME_CAPABILITIES "tick,console"
#define REFRAIN_SERVICES_RUNTIME_REQUIRES "refrain:core>=1.0.0"
#define REFRAIN_SERVICES_RUNTIME_SERVICE_IDS "refrain:runtime"

typedef struct refrain_services_runtime_vtable {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t abi_patch;
    // Start the headless server runtime and block until it stops (console
    // `stop`, the tick limit, EOF, or a fatal runtime error). Returns the
    // process exit code: 0 for a clean stop, non-zero for a failed startup or
    // an unclean shutdown. `runtime_error` is reserved for calls the provider
    // rejects before the tick loop starts (for example an invalid config);
    // once the loop owns the process, the exit code is the authoritative
    // result.
    // attributes: #[thread(main)] #[can_fail]
    refrain_status (*run)(void *self, const refrain_services_runtime_runtime_config *config, int32_t *out_value);
} refrain_services_runtime_vtable; // service refrain:runtime

#define REFRAIN_SERVICES_RUNTIME_VTABLE_SIZE ((uint32_t)sizeof(refrain_services_runtime_vtable))

#ifdef __cplusplus
} // extern "C"
#endif

#endif // REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_RUNTIME_ABI_H
