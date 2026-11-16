// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: c735c6db25ca24c9

#ifndef REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_WORLD_MIGRATION_ABI_H
#define REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_WORLD_MIGRATION_ABI_H

#include <refrain/contracts/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct refrain_services_world_migration_world_migration_capabilities refrain_services_world_migration_world_migration_capabilities;
typedef struct refrain_services_world_migration_world_migration_plan refrain_services_world_migration_world_migration_plan;
typedef struct refrain_services_world_migration_world_migration_request refrain_services_world_migration_world_migration_request;
typedef struct refrain_services_world_migration_world_migration_report refrain_services_world_migration_world_migration_report;

// Failure classes the caller can act on. Every value starts at 1 so that a
// zero status is success on the ABI.
typedef uint32_t refrain_services_world_migration_world_migration_error; // enum 'world_migration_error'
#define REFRAIN_SERVICES_WORLD_MIGRATION_WORLD_MIGRATION_ERROR_UNSUPPORTED_VERSION ((refrain_services_world_migration_world_migration_error)(1))
#define REFRAIN_SERVICES_WORLD_MIGRATION_WORLD_MIGRATION_ERROR_INVALID_REQUEST ((refrain_services_world_migration_world_migration_error)(2))
#define REFRAIN_SERVICES_WORLD_MIGRATION_WORLD_MIGRATION_ERROR_IO_ERROR ((refrain_services_world_migration_world_migration_error)(3))
#define REFRAIN_SERVICES_WORLD_MIGRATION_WORLD_MIGRATION_ERROR_CORRUPT_WORLD ((refrain_services_world_migration_world_migration_error)(4))
#define REFRAIN_SERVICES_WORLD_MIGRATION_WORLD_MIGRATION_ERROR_MIGRATION_FAILED ((refrain_services_world_migration_world_migration_error)(5))

// Source versions one provider accepts and the version it upgrades them to.
struct refrain_services_world_migration_world_migration_capabilities {
    uint32_t struct_size;
    // Oldest source `DataVersion` accepted; older worlds fail closed.
    int32_t min_version;
    // `DataVersion` every accepted migration ends at.
    int32_t target_version;
}; // refrain_services_world_migration_world_migration_capabilities size = 12
#define REFRAIN_SERVICES_WORLD_MIGRATION_WORLD_MIGRATION_CAPABILITIES_SIZE ((uint32_t)sizeof(refrain_services_world_migration_world_migration_capabilities))

// What a migration from `from_version` to `to_version` runs. The count covers
// every upgrade step plus the version stamp; zero means the world already
// records `to_version`.
struct refrain_services_world_migration_world_migration_plan {
    uint32_t struct_size;
    int32_t from_version;
    int32_t to_version;
    uint32_t steps;
}; // refrain_services_world_migration_world_migration_plan size = 16
#define REFRAIN_SERVICES_WORLD_MIGRATION_WORLD_MIGRATION_PLAN_SIZE ((uint32_t)sizeof(refrain_services_world_migration_world_migration_plan))

// One migration request. `dry_run` validates the world and reports the work
// that would be done without writing anything.
struct refrain_services_world_migration_world_migration_request {
    uint32_t struct_size;
    uint8_t reserved_0[4];
    // World directory to upgrade. Read and written directly by the provider.
    refrain_str world_path;
    // Version the caller read from the world; the migration fails closed when
    // the record disagrees instead of rewriting a different world.
    int32_t from_version;
    // Version to write; must equal the provider's target version.
    int32_t to_version;
    uint8_t dry_run;
    uint8_t reserved_1[7];
}; // refrain_services_world_migration_world_migration_request size = 40
#define REFRAIN_SERVICES_WORLD_MIGRATION_WORLD_MIGRATION_REQUEST_SIZE ((uint32_t)sizeof(refrain_services_world_migration_world_migration_request))

// Outcome of one successful migration (or of its dry run).
struct refrain_services_world_migration_world_migration_report {
    uint32_t struct_size;
    int32_t from_version;
    int32_t to_version;
    // Upgrade steps run, including the version stamp.
    uint32_t steps;
    // Chunk records inspected and rewritten.
    uint64_t chunks_scanned;
    uint64_t chunks_migrated;
    // Player records inspected and rewritten.
    uint64_t player_records_scanned;
    uint64_t player_records_migrated;
    // True when every rewritten record was already at `to_version`, so no
    // file changed.
    uint8_t no_op;
    // True when this was a dry run: the counts describe the work a real run
    // would do and nothing was written.
    uint8_t dry_run;
    uint8_t reserved_0[6];
}; // refrain_services_world_migration_world_migration_report size = 56
#define REFRAIN_SERVICES_WORLD_MIGRATION_WORLD_MIGRATION_REPORT_SIZE ((uint32_t)sizeof(refrain_services_world_migration_world_migration_report))

#define REFRAIN_SERVICES_WORLD_MIGRATION_SERVICE_ID "refrain:world.migration"
#define REFRAIN_SERVICES_WORLD_MIGRATION_ABI_MAJOR 1u
#define REFRAIN_SERVICES_WORLD_MIGRATION_ABI_MINOR 0u
#define REFRAIN_SERVICES_WORLD_MIGRATION_ABI_PATCH 0u
#define REFRAIN_SERVICES_WORLD_MIGRATION_SIDE REFRAIN_SIDE_SERVER
#define REFRAIN_SERVICES_WORLD_MIGRATION_THREAD REFRAIN_THREAD_ANY
#define REFRAIN_SERVICES_WORLD_MIGRATION_COMPOSITION REFRAIN_COMPOSITION_EXCLUSIVE
#define REFRAIN_SERVICES_WORLD_MIGRATION_CAPABILITIES "data-version,chunk-nbt,player-data"
#define REFRAIN_SERVICES_WORLD_MIGRATION_REQUIRES "refrain:core>=1.0.0"
#define REFRAIN_SERVICES_WORLD_MIGRATION_SERVICE_IDS "refrain:world.migration"

typedef struct refrain_services_world_migration_vtable {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t abi_patch;
    // Versions this provider upgrades from and to. Callers plan only inside
    // the returned window; a wider window needs a provider upgrade.
    // attributes: #[thread(any)] #[can_fail]
    refrain_status (*capabilities)(void *self, refrain_services_world_migration_world_migration_capabilities *out_value);
    // Validate that `from_version` can be upgraded to `to_version` and return
    // the step count. Fails with `unsupported_version` when either version
    // lies outside the window, and with `invalid_request` when `from_version`
    // is newer than `to_version`.
    // attributes: #[thread(any)] #[can_fail]
    refrain_status (*plan)(void *self, int32_t from_version, int32_t to_version, refrain_services_world_migration_world_migration_plan *out_value);
    // Upgrade the world, or report the work without writing when `dry_run` is
    // set. A world already at `to_version` succeeds as a no-op. Each rewritten
    // record is written through a temporary file and renamed into place, and
    // the version stamp is written last, so a failed migration leaves the
    // world recorded at its original version.
    // attributes: #[thread(any)] #[can_fail]
    refrain_status (*migrate)(void *self, const refrain_services_world_migration_world_migration_request *request, refrain_services_world_migration_world_migration_report *out_value);
} refrain_services_world_migration_vtable; // service refrain:world.migration

#define REFRAIN_SERVICES_WORLD_MIGRATION_VTABLE_SIZE ((uint32_t)sizeof(refrain_services_world_migration_vtable))

#ifdef __cplusplus
} // extern "C"
#endif

#endif // REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_WORLD_MIGRATION_ABI_H
