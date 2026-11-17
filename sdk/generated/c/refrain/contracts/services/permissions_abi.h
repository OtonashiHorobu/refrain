// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 22ed9cba76524a0f

#ifndef REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_PERMISSIONS_ABI_H
#define REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_PERMISSIONS_ABI_H

#include <refrain/contracts/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct refrain_services_permissions_permission_context_entry refrain_services_permissions_permission_context_entry;
typedef struct refrain_services_permissions_permission_context refrain_services_permissions_permission_context;
typedef struct refrain_services_permissions_subject_ref refrain_services_permissions_subject_ref;
typedef struct refrain_services_permissions_permission_query refrain_services_permissions_permission_query;

// Kinds a query subject can have. `console` is the server console,
// `player` a connected player, `entity` a non-player entity and `plugin` a
// mod calling on behalf of itself.
typedef uint32_t refrain_services_permissions_subject_kind; // enum 'subject_kind'
#define REFRAIN_SERVICES_PERMISSIONS_SUBJECT_KIND_CONSOLE ((refrain_services_permissions_subject_kind)(0))
#define REFRAIN_SERVICES_PERMISSIONS_SUBJECT_KIND_PLAYER ((refrain_services_permissions_subject_kind)(1))
#define REFRAIN_SERVICES_PERMISSIONS_SUBJECT_KIND_ENTITY ((refrain_services_permissions_subject_kind)(2))
#define REFRAIN_SERVICES_PERMISSIONS_SUBJECT_KIND_PLUGIN ((refrain_services_permissions_subject_kind)(3))

// Answer of one query. `denied` always wins over a grant; `not_handled` means
// the provider holds no rule for the node and the chain must continue.
typedef uint32_t refrain_services_permissions_permission_result; // enum 'permission_result'
#define REFRAIN_SERVICES_PERMISSIONS_PERMISSION_RESULT_ALLOWED ((refrain_services_permissions_permission_result)(0))
#define REFRAIN_SERVICES_PERMISSIONS_PERMISSION_RESULT_DENIED ((refrain_services_permissions_permission_result)(1))
#define REFRAIN_SERVICES_PERMISSIONS_PERMISSION_RESULT_NOT_HANDLED ((refrain_services_permissions_permission_result)(2))

// Errors of the resolver operations that can fail. Zero is success, so the
// enum starts at one.
typedef uint32_t refrain_services_permissions_permission_error; // enum 'permission_error'
#define REFRAIN_SERVICES_PERMISSIONS_PERMISSION_ERROR_INVALID_ARGUMENT ((refrain_services_permissions_permission_error)(1))
#define REFRAIN_SERVICES_PERMISSIONS_PERMISSION_ERROR_PARSE_ERROR ((refrain_services_permissions_permission_error)(2))
#define REFRAIN_SERVICES_PERMISSIONS_PERMISSION_ERROR_NOT_INITIALIZED ((refrain_services_permissions_permission_error)(3))
#define REFRAIN_SERVICES_PERMISSIONS_PERMISSION_ERROR_UNAVAILABLE ((refrain_services_permissions_permission_error)(4))

typedef struct refrain_services_permissions_slice_permission_context_entry {
    const refrain_services_permissions_permission_context_entry *data;
    uint64_t len;
} refrain_services_permissions_slice_permission_context_entry; // slice<permission_context_entry>

// One key/value pair of the extensible query context.
struct refrain_services_permissions_permission_context_entry {
    uint32_t struct_size;
    uint8_t reserved_0[4];
    // Context key, for example `dimension_type`.
    refrain_str key;
    // Context value for the key.
    refrain_str value;
}; // refrain_services_permissions_permission_context_entry size = 40
#define REFRAIN_SERVICES_PERMISSIONS_PERMISSION_CONTEXT_ENTRY_SIZE ((uint32_t)sizeof(refrain_services_permissions_permission_context_entry))

// Context a rule may match. The default resolver answers globally, so v1
// carries the context without matching rules against it; wrapper providers
// and later rule sets can use it without a contract change.
struct refrain_services_permissions_permission_context {
    uint32_t struct_size;
    uint8_t reserved_0[4];
    // Level/dimension id, for example `minecraft:overworld`.
    refrain_str world;
    // Game mode name, for example `survival`.
    refrain_str gamemode;
    // Additional keys a provider understands. Duplicate keys are rejected by
    // consumers that match context.
    refrain_services_permissions_slice_permission_context_entry extra;
}; // refrain_services_permissions_permission_context size = 56
#define REFRAIN_SERVICES_PERMISSIONS_PERMISSION_CONTEXT_SIZE ((uint32_t)sizeof(refrain_services_permissions_permission_context))

// Who a query is about. `console` leaves both strings empty. A `player`
// carries its canonical profile uuid in `id` and its profile name in
// `profile_name`; an `entity` carries its uuid in `id`; a `plugin` carries
// its mod id in `id`.
struct refrain_services_permissions_subject_ref {
    uint32_t struct_size;
    refrain_services_permissions_subject_kind kind;
    refrain_str id;
    refrain_str profile_name;
}; // refrain_services_permissions_subject_ref size = 40
#define REFRAIN_SERVICES_PERMISSIONS_SUBJECT_REF_SIZE ((uint32_t)sizeof(refrain_services_permissions_subject_ref))

// One permission question.
struct refrain_services_permissions_permission_query {
    uint32_t struct_size;
    uint8_t reserved_0[4];
    refrain_services_permissions_subject_ref subject;
    // Dot-separated node, for example `refrain.command.teleport`. A node is
    // concrete: it has no `!` prefix and no `*` wildcard, which are rule
    // syntax only.
    refrain_str node;
    refrain_services_permissions_permission_context context;
    // Vanilla permission ladder of the subject (0..4) when the caller knows
    // it, zero otherwise. Only a player subject uses it: the operator rule of
    // the default resolver matches on this value.
    uint32_t op_level;
    uint8_t reserved_1[4];
}; // refrain_services_permissions_permission_query size = 128
#define REFRAIN_SERVICES_PERMISSIONS_PERMISSION_QUERY_SIZE ((uint32_t)sizeof(refrain_services_permissions_permission_query))

#define REFRAIN_SERVICES_PERMISSIONS_SERVICE_ID "refrain:permissions"
#define REFRAIN_SERVICES_PERMISSIONS_ABI_MAJOR 1u
#define REFRAIN_SERVICES_PERMISSIONS_ABI_MINOR 0u
#define REFRAIN_SERVICES_PERMISSIONS_ABI_PATCH 0u
#define REFRAIN_SERVICES_PERMISSIONS_SIDE REFRAIN_SIDE_SERVER
#define REFRAIN_SERVICES_PERMISSIONS_THREAD REFRAIN_THREAD_ANY
#define REFRAIN_SERVICES_PERMISSIONS_COMPOSITION REFRAIN_COMPOSITION_CHAIN
#define REFRAIN_SERVICES_PERMISSIONS_CAPABILITIES "default-resolver,config-policy,revision"
#define REFRAIN_SERVICES_PERMISSIONS_REQUIRES "refrain:core>=1.0.0"
#define REFRAIN_SERVICES_PERMISSIONS_SERVICE_IDS "refrain:permissions"

typedef struct refrain_services_permissions_vtable {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t abi_patch;
    // Answer one query from the provider's current policy snapshot. Read-only
    // and safe to call from any thread. A malformed query (wrong struct size,
    // unknown subject kind, invalid node) is answered `denied`: the resolver
    // fails closed instead of granting on input it cannot evaluate.
    // attributes: #[thread(any)]
    refrain_services_permissions_permission_result (*query)(void *self, const refrain_services_permissions_permission_query *query);
    // Answer the way the base resolver would on its own, ignoring wrapper
    // grants. The chain falls back to this answer when every link answers
    // `not_handled`; the base provider answers both calls from the same
    // policy.
    // attributes: #[thread(any)]
    refrain_services_permissions_permission_result (*default_result)(void *self, const refrain_services_permissions_permission_query *query);
    // Convenience predicate: true unless the query is denied, so `allowed`
    // and `not_handled` both read as "maybe" for a caller that layers its own
    // fallback.
    // attributes: #[thread(any)]
    uint8_t (*has)(void *self, const refrain_services_permissions_permission_query *query);
    // Monotonic change counter of the published policy snapshot. It advances
    // when the configuration is reloaded successfully; a consumer compares it
    // against the value its cache was built with.
    // attributes: #[thread(any)]
    uint64_t (*revision)(void *self);
    // Re-read the provider configuration, publish it and return the new
    // revision. On failure the current snapshot and revision stay in effect
    // and the error names the offending file or key.
    // attributes: #[thread(main)] #[can_fail]
    refrain_status (*reload)(void *self, uint64_t *out_value);
} refrain_services_permissions_vtable; // service refrain:permissions

#define REFRAIN_SERVICES_PERMISSIONS_VTABLE_SIZE ((uint32_t)sizeof(refrain_services_permissions_vtable))

#ifdef __cplusplus
} // extern "C"
#endif

#endif // REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_PERMISSIONS_ABI_H
