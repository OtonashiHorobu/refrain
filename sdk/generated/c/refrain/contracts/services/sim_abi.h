// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 23c7796fb66be5e7

#ifndef REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_SIM_ABI_H
#define REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_SIM_ABI_H

#include <refrain/contracts/abi.h>
#include <refrain/contracts/core_abi.h>
#include <refrain/contracts/services/storage_abi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct refrain_services_sim_sim_config refrain_services_sim_sim_config;
typedef struct refrain_services_sim_player_info refrain_services_sim_player_info;
typedef struct refrain_services_sim_player_registration refrain_services_sim_player_registration;

typedef uint32_t refrain_services_sim_sim_error; // enum 'sim_error'
#define REFRAIN_SERVICES_SIM_SIM_ERROR_INVALID_HANDLE ((refrain_services_sim_sim_error)(1))
#define REFRAIN_SERVICES_SIM_SIM_ERROR_ALREADY_OPEN ((refrain_services_sim_sim_error)(2))
#define REFRAIN_SERVICES_SIM_SIM_ERROR_NOT_FOUND ((refrain_services_sim_sim_error)(3))
#define REFRAIN_SERVICES_SIM_SIM_ERROR_IO_ERROR ((refrain_services_sim_sim_error)(4))
#define REFRAIN_SERVICES_SIM_SIM_ERROR_INTERNAL ((refrain_services_sim_sim_error)(5))
#define REFRAIN_SERVICES_SIM_SIM_ERROR_OUT_OF_BOUNDS ((refrain_services_sim_sim_error)(6))
#define REFRAIN_SERVICES_SIM_SIM_ERROR_INVALID_ARGUMENT ((refrain_services_sim_sim_error)(7))
#define REFRAIN_SERVICES_SIM_SIM_ERROR_STORAGE_UNAVAILABLE ((refrain_services_sim_sim_error)(8))

typedef uint64_t refrain_services_sim_handle_sim_world;
#define REFRAIN_SERVICES_SIM_HANDLE_SIM_WORLD_INVALID ((refrain_services_sim_handle_sim_world)(REFRAIN_HANDLE_INVALID))

// Published once per tick, after the player/RNG step.
typedef struct refrain_services_sim_tick_event {
    int64_t tick;
} refrain_services_sim_tick_event; // event tick_event size = 8
#define REFRAIN_SERVICES_SIM_EVENT_TICK_EVENT_SIZE ((uint32_t)sizeof(refrain_services_sim_tick_event))
#define REFRAIN_SERVICES_SIM_EVENT_TICK_EVENT_ID "refrain:event/refrain/services/sim/tick_event"
#define REFRAIN_SERVICES_SIM_EVENT_TICK_EVENT_TYPE ((uint64_t)0x66bfb309ee05273aULL)

// One block mutation that changed the world, published once per applied
// position in ascending position order.
typedef struct refrain_services_sim_block_changed {
    int32_t x;
    int32_t y;
    int32_t z;
    uint32_t old_state;
    uint32_t new_state;
} refrain_services_sim_block_changed; // event block_changed size = 20
#define REFRAIN_SERVICES_SIM_EVENT_BLOCK_CHANGED_SIZE ((uint32_t)sizeof(refrain_services_sim_block_changed))
#define REFRAIN_SERVICES_SIM_EVENT_BLOCK_CHANGED_ID "refrain:event/refrain/services/sim/block_changed"
#define REFRAIN_SERVICES_SIM_EVENT_BLOCK_CHANGED_TYPE ((uint64_t)0xd6414cf41965c306ULL)

// An entity was added to the world.
typedef struct refrain_services_sim_entity_spawned {
    // Process-local entity id.
    uint64_t id;
    // Namespaced entity type key, for example `minecraft:pig`.
    refrain_str type;
    double x;
    double y;
    double z;
} refrain_services_sim_entity_spawned; // event entity_spawned size = 48
#define REFRAIN_SERVICES_SIM_EVENT_ENTITY_SPAWNED_SIZE ((uint32_t)sizeof(refrain_services_sim_entity_spawned))
#define REFRAIN_SERVICES_SIM_EVENT_ENTITY_SPAWNED_ID "refrain:event/refrain/services/sim/entity_spawned"
#define REFRAIN_SERVICES_SIM_EVENT_ENTITY_SPAWNED_TYPE ((uint64_t)0x001789c64fa4b85aULL)

// An entity was removed from the world.
typedef struct refrain_services_sim_entity_removed {
    // Process-local entity id.
    uint64_t id;
} refrain_services_sim_entity_removed; // event entity_removed size = 8
#define REFRAIN_SERVICES_SIM_EVENT_ENTITY_REMOVED_SIZE ((uint32_t)sizeof(refrain_services_sim_entity_removed))
#define REFRAIN_SERVICES_SIM_EVENT_ENTITY_REMOVED_ID "refrain:event/refrain/services/sim/entity_removed"
#define REFRAIN_SERVICES_SIM_EVENT_ENTITY_REMOVED_TYPE ((uint64_t)0xa8d216d1efdcebd0ULL)

struct refrain_services_sim_sim_config {
    uint32_t struct_size;
    uint8_t reserved_0[4];
    // The store the sim persists through. The runtime opened it once from the
    // level directory and passes the same handle to every provider of the
    // composition; the provider never opens the directory itself.
    refrain_services_storage_storage_handle storage;
    // World seed; providers use it for their deterministic RNG stream.
    int64_t seed;
}; // refrain_services_sim_sim_config size = 72
#define REFRAIN_SERVICES_SIM_SIM_CONFIG_SIZE ((uint32_t)sizeof(refrain_services_sim_sim_config))

// Read-only projection of one player record. A provider fills it from the
// record it owns; the name and the profile uuid are borrowed for the duration
// of the call, like the level projection's level name.
struct refrain_services_sim_player_info {
    uint32_t struct_size;
    uint8_t reserved_0[4];
    // Network connection that owns the player, or the reserved synthetic
    // connection for the player the console and replay scripts drive.
    uint64_t connection;
    // Profile uuid in wire byte order, 16 bytes; empty when the record has
    // none.
    refrain_bytes profile_id;
    // Player name.
    refrain_str name;
    // World the player is in.
    refrain_str world;
    // Process-local entity id, unique across players and spawned entities.
    uint64_t entity_id;
    // Feet position in block coordinates.
    double x;
    double y;
    double z;
    // View angles in degrees.
    float yaw;
    float pitch;
    // True when the player stands on a block.
    uint8_t on_ground;
    uint8_t reserved_1[3];
    // Game mode: 0 survival, 1 creative, 2 adventure, 3 spectator.
    uint32_t game_mode;
    // Health points.
    float health;
    // View distance in chunks the provider keeps for the player.
    int32_t view_distance;
}; // refrain_services_sim_player_info size = 120
#define REFRAIN_SERVICES_SIM_PLAYER_INFO_SIZE ((uint32_t)sizeof(refrain_services_sim_player_info))

// Everything needed to register one player. The provider allocates the
// entity id and keeps the record; the caller reads it back through the
// projection.
struct refrain_services_sim_player_registration {
    uint32_t struct_size;
    uint8_t reserved_0[4];
    // Network connection that will own the player, or the reserved synthetic
    // connection.
    uint64_t connection;
    // Profile uuid in wire byte order, 16 bytes; empty when the record has
    // none.
    refrain_bytes profile_id;
    refrain_str name;
    refrain_str world;
    // Feet position in block coordinates.
    double x;
    double y;
    double z;
    // Game mode: 0 survival, 1 creative, 2 adventure, 3 spectator.
    uint32_t game_mode;
    // Preallocated empty inventory slots. The state hash covers their count,
    // so a registration carries it even though item operations stay out of
    // this contract.
    uint32_t inventory_slots;
}; // refrain_services_sim_player_registration size = 96
#define REFRAIN_SERVICES_SIM_PLAYER_REGISTRATION_SIZE ((uint32_t)sizeof(refrain_services_sim_player_registration))

#define REFRAIN_SERVICES_SIM_SERVICE_ID "refrain:sim"
#define REFRAIN_SERVICES_SIM_ABI_MAJOR 1u
#define REFRAIN_SERVICES_SIM_ABI_MINOR 1u
#define REFRAIN_SERVICES_SIM_ABI_PATCH 0u
#define REFRAIN_SERVICES_SIM_SIDE REFRAIN_SIDE_SERVER
#define REFRAIN_SERVICES_SIM_THREAD REFRAIN_THREAD_SIM
#define REFRAIN_SERVICES_SIM_COMPOSITION REFRAIN_COMPOSITION_EXCLUSIVE
#define REFRAIN_SERVICES_SIM_CAPABILITIES "tick,replay,deterministic-rng,block-events,entity-events"
#define REFRAIN_SERVICES_SIM_REQUIRES "refrain:core>=1.0.0,refrain:world>=1.0.0,refrain:storage>=1.0.0"
#define REFRAIN_SERVICES_SIM_SERVICE_IDS "refrain:sim"

typedef struct refrain_services_sim_vtable {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t abi_patch;
    // Open a sim world over the given store. The provider never opens the
    // level directory itself; the store was opened once by the runtime.
    // attributes: #[thread(sim)] #[can_fail]
    refrain_status (*open_world)(void *self, const refrain_services_sim_sim_config *config, refrain_services_sim_handle_sim_world *out_value);
    // attributes: #[thread(sim)] #[can_fail]
    refrain_status (*close_world)(void *self, refrain_services_sim_handle_sim_world world);
    // Advance exactly one tick.
    // attributes: #[thread(sim)] #[can_fail]
    refrain_status (*tick)(void *self, refrain_services_sim_handle_sim_world world);
    // attributes: #[thread(any)]
    int64_t (*ticks)(void *self, refrain_services_sim_handle_sim_world world);
    // Lowercase hex BLAKE3 of the deterministic replay state.
    // attributes: #[thread(sim)]
    refrain_str (*state_hash)(void *self, refrain_services_sim_handle_sim_world world);
    // Apply a batch of block mutations and publish one `block_changed` event
    // per applied change. Returns the number of changes that modified the
    // world; a change that writes the current state is skipped.
    // attributes: #[thread(sim)] #[can_fail]
    refrain_status (*set_blocks)(void *self, refrain_services_sim_handle_sim_world world, refrain_core_slice_block_change changes, uint32_t *out_value);
    // Spawn an entity at the given feet position and publish
    // `entity_spawned`. The id is allocated in ascending order.
    // attributes: #[thread(sim)] #[can_fail]
    refrain_status (*spawn_entity)(void *self, refrain_services_sim_handle_sim_world world, refrain_str type, double x, double y, double z, uint64_t *out_value);
    // Remove one entity and publish `entity_removed`. Returns false when the
    // id is unknown.
    // attributes: #[thread(sim)]
    uint8_t (*remove_entity)(void *self, refrain_services_sim_handle_sim_world world, uint64_t id);
    // Number of player records. `player_at` enumerates them in ascending
    // entity id order, which is the order the state hash covers.
    // 
    // A count plus an indexed accessor is the read shape because every field
    // but the name and the profile uuid is a fixed-size value: a packed
    // buffer would need either a per-call buffer whose size the consumer must
    // guess or an allocation the provider owns, and neither carries a
    // borrowed string without a second layout. A provider callback per player
    // would put provider code on the runtime's call stack for a read the
    // runtime already batches once per tick. The indexed form keeps one
    // dispatch per record, lets a caller stop at the count it read, and
    // leaves the string lifetime rule identical to the level projection.
    // attributes: #[thread(sim)]
    uint32_t (*player_count)(void *self, refrain_services_sim_handle_sim_world world);
    // One player record by index, in ascending entity id order. The name and
    // the profile uuid are borrowed for the duration of the call, like the
    // level projection's level name.
    // attributes: #[thread(sim)] #[can_fail]
    refrain_status (*player_at)(void *self, refrain_services_sim_handle_sim_world world, uint32_t index, refrain_services_sim_player_info *out_value);
    // One player record by connection. The write calls are connection
    // addressed because a packet names a connection, not a table index; this
    // is the read a caller uses to refresh one record after a write.
    // attributes: #[thread(sim)] #[can_fail] #[hot_path]
    refrain_status (*player_by_connection)(void *self, refrain_services_sim_handle_sim_world world, uint64_t connection, refrain_services_sim_player_info *out_value);
    // Register a player record and allocate its entity id. Fails with
    // `invalid_argument` for an empty name or a connection that already owns
    // a player.
    // attributes: #[thread(sim)] #[can_fail]
    refrain_status (*add_player)(void *self, refrain_services_sim_handle_sim_world world, const refrain_services_sim_player_registration *record, uint64_t *out_value);
    // Remove the player owned by `connection`. Returns false when the
    // connection owns no record.
    // attributes: #[thread(sim)]
    uint8_t (*remove_player)(void *self, refrain_services_sim_handle_sim_world world, uint64_t connection);
    // Set position and rotation in one call, the way a movement packet
    // updates both at once. Velocity is left untouched.
    // attributes: #[thread(sim)] #[can_fail] #[hot_path]
    refrain_status (*set_player_transform)(void *self, refrain_services_sim_handle_sim_world world, uint64_t connection, double x, double y, double z, float yaw, float pitch);
    // Set position and clear velocity and the ground flag, the teleport
    // semantics the console move command uses.
    // attributes: #[thread(sim)] #[can_fail]
    refrain_status (*set_player_position)(void *self, refrain_services_sim_handle_sim_world world, uint64_t connection, double x, double y, double z);
    // Replace the view distance the provider keeps for the player.
    // attributes: #[thread(sim)] #[can_fail]
    refrain_status (*set_player_view_distance)(void *self, refrain_services_sim_handle_sim_world world, uint64_t connection, int32_t distance);
    // Move by a relative offset with the provider's collision response.
    // Returns false when the move was fully blocked.
    // attributes: #[thread(sim)] #[can_fail]
    refrain_status (*move_player)(void *self, refrain_services_sim_handle_sim_world world, uint64_t connection, double dx, double dy, double dz, uint8_t *out_value);
    // Make the player jump when it stands on the ground.
    // attributes: #[thread(sim)] #[can_fail]
    refrain_status (*jump)(void *self, refrain_services_sim_handle_sim_world world, uint64_t connection);
} refrain_services_sim_vtable; // service refrain:sim

#define REFRAIN_SERVICES_SIM_VTABLE_SIZE ((uint32_t)sizeof(refrain_services_sim_vtable))

#ifdef __cplusplus
} // extern "C"
#endif

#endif // REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_SIM_ABI_H
