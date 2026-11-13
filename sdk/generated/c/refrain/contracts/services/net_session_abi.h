// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 9acb7bb41c6b4563

#ifndef REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_NET_SESSION_ABI_H
#define REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_NET_SESSION_ABI_H

#include <refrain/contracts/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct refrain_services_net_session_session_profile refrain_services_net_session_session_profile;
typedef struct refrain_services_net_session_menu_click_result refrain_services_net_session_menu_click_result;

typedef uint32_t refrain_services_net_session_session_error; // enum 'session_error'
#define REFRAIN_SERVICES_NET_SESSION_SESSION_ERROR_INVALID_ARGUMENT ((refrain_services_net_session_session_error)(1))
#define REFRAIN_SERVICES_NET_SESSION_SESSION_ERROR_NOT_JOINED ((refrain_services_net_session_session_error)(2))
#define REFRAIN_SERVICES_NET_SESSION_SESSION_ERROR_UNAVAILABLE ((refrain_services_net_session_session_error)(3))
#define REFRAIN_SERVICES_NET_SESSION_SESSION_ERROR_MALFORMED_PROFILE ((refrain_services_net_session_session_error)(4))
#define REFRAIN_SERVICES_NET_SESSION_SESSION_ERROR_NOT_FOUND ((refrain_services_net_session_session_error)(5))
#define REFRAIN_SERVICES_NET_SESSION_SESSION_ERROR_ALREADY_OPEN ((refrain_services_net_session_session_error)(6))
#define REFRAIN_SERVICES_NET_SESSION_SESSION_ERROR_UNSUPPORTED ((refrain_services_net_session_session_error)(7))

typedef uint64_t refrain_services_net_session_handle_session_menu;
#define REFRAIN_SERVICES_NET_SESSION_HANDLE_SESSION_MENU_INVALID ((refrain_services_net_session_handle_session_menu)(REFRAIN_HANDLE_INVALID))

// A connection opened a menu.
typedef struct refrain_services_net_session_menu_opened {
    // Raw `session_menu` handle.
    uint64_t menu;
    uint64_t connection;
    // Container id; 0 is the player inventory.
    uint32_t container;
    uint8_t reserved_0[4];
} refrain_services_net_session_menu_opened; // event menu_opened size = 24
#define REFRAIN_SERVICES_NET_SESSION_EVENT_MENU_OPENED_SIZE ((uint32_t)sizeof(refrain_services_net_session_menu_opened))
#define REFRAIN_SERVICES_NET_SESSION_EVENT_MENU_OPENED_ID "refrain:event/refrain/services/net_session/menu_opened"
#define REFRAIN_SERVICES_NET_SESSION_EVENT_MENU_OPENED_TYPE ((uint64_t)0xa91d4a6475644df2ULL)

// A menu closed.
typedef struct refrain_services_net_session_menu_closed {
    uint64_t menu;
    uint64_t connection;
} refrain_services_net_session_menu_closed; // event menu_closed size = 16
#define REFRAIN_SERVICES_NET_SESSION_EVENT_MENU_CLOSED_SIZE ((uint32_t)sizeof(refrain_services_net_session_menu_closed))
#define REFRAIN_SERVICES_NET_SESSION_EVENT_MENU_CLOSED_ID "refrain:event/refrain/services/net_session/menu_closed"
#define REFRAIN_SERVICES_NET_SESSION_EVENT_MENU_CLOSED_TYPE ((uint64_t)0x4c199e0f991d9763ULL)

// One slot of an open menu changed.
typedef struct refrain_services_net_session_menu_slot_changed {
    uint64_t menu;
    int32_t slot;
    uint32_t count;
    refrain_str item;
} refrain_services_net_session_menu_slot_changed; // event menu_slot_changed size = 32
#define REFRAIN_SERVICES_NET_SESSION_EVENT_MENU_SLOT_CHANGED_SIZE ((uint32_t)sizeof(refrain_services_net_session_menu_slot_changed))
#define REFRAIN_SERVICES_NET_SESSION_EVENT_MENU_SLOT_CHANGED_ID "refrain:event/refrain/services/net_session/menu_slot_changed"
#define REFRAIN_SERVICES_NET_SESSION_EVENT_MENU_SLOT_CHANGED_TYPE ((uint64_t)0x7821eb4a3cf00260ULL)

// A mouse click was applied to an open menu.
typedef struct refrain_services_net_session_menu_clicked {
    uint64_t menu;
    int32_t slot;
    int32_t button;
    uint8_t accepted;
    uint8_t reserved_0[3];
    uint32_t slot_count;
    refrain_str slot_item;
    uint32_t carried_count;
    uint8_t reserved_1[4];
    refrain_str carried_item;
} refrain_services_net_session_menu_clicked; // event menu_clicked size = 64
#define REFRAIN_SERVICES_NET_SESSION_EVENT_MENU_CLICKED_SIZE ((uint32_t)sizeof(refrain_services_net_session_menu_clicked))
#define REFRAIN_SERVICES_NET_SESSION_EVENT_MENU_CLICKED_ID "refrain:event/refrain/services/net_session/menu_clicked"
#define REFRAIN_SERVICES_NET_SESSION_EVENT_MENU_CLICKED_TYPE ((uint64_t)0x2e6c36a831df39a8ULL)

struct refrain_services_net_session_session_profile {
    uint32_t struct_size;
    uint8_t reserved_0[4];
    // Canonical UUID string (`8-4-4-4-12`).
    refrain_str id;
    refrain_str name;
    // Borrowed JSON array of profile properties; empty means none.
    refrain_bytes properties;
}; // refrain_services_net_session_session_profile size = 56
#define REFRAIN_SERVICES_NET_SESSION_SESSION_PROFILE_SIZE ((uint32_t)sizeof(refrain_services_net_session_session_profile))

// Outcome of one mouse click on an open menu.
// 
// The model owns the carried stack, like the client does: a click either
// picks a stack up, puts it down, merges it or swaps it. `accepted` is false
// when the click changed nothing, in which case the reported slot and carried
// contents are the unchanged state.
struct refrain_services_net_session_menu_click_result {
    uint32_t struct_size;
    // Menu slot the click addressed; negative means outside the window.
    int32_t slot;
    // 0 = left click, 1 = right click.
    int32_t button;
    uint8_t accepted;
    uint8_t reserved_0[3];
    // Slot contents after the click. `count` 0 means the slot is empty.
    uint32_t slot_count;
    uint8_t reserved_1[4];
    // Namespaced item key, for example `minecraft:stone`; empty when the
    // slot is empty.
    refrain_str slot_item;
    // Carried stack after the click.
    uint32_t carried_count;
    uint8_t reserved_2[4];
    refrain_str carried_item;
}; // refrain_services_net_session_menu_click_result size = 64
#define REFRAIN_SERVICES_NET_SESSION_MENU_CLICK_RESULT_SIZE ((uint32_t)sizeof(refrain_services_net_session_menu_click_result))

#define REFRAIN_SERVICES_NET_SESSION_SERVICE_ID "refrain:net.session"
#define REFRAIN_SERVICES_NET_SESSION_ABI_MAJOR 1u
#define REFRAIN_SERVICES_NET_SESSION_ABI_MINOR 0u
#define REFRAIN_SERVICES_NET_SESSION_ABI_PATCH 0u
#define REFRAIN_SERVICES_NET_SESSION_SIDE REFRAIN_SIDE_SERVER
#define REFRAIN_SERVICES_NET_SESSION_THREAD REFRAIN_THREAD_ANY
#define REFRAIN_SERVICES_NET_SESSION_COMPOSITION REFRAIN_COMPOSITION_EXCLUSIVE
#define REFRAIN_SERVICES_NET_SESSION_CAPABILITIES "offline-profile,online-profile,container-menus"
#define REFRAIN_SERVICES_NET_SESSION_REQUIRES "refrain:core>=1.0.0"
#define REFRAIN_SERVICES_NET_SESSION_SERVICE_IDS "refrain:net.session"

typedef struct refrain_services_net_session_vtable {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t abi_patch;
    // attributes: #[thread(any)] #[can_fail]
    refrain_status (*offline_profile)(void *self, refrain_str name, refrain_services_net_session_session_profile *profile);
    // attributes: #[thread(worker)] #[can_fail]
    refrain_status (*joined_profile)(void *self, refrain_str name, refrain_str server_hash, refrain_str remote_ip, refrain_services_net_session_session_profile *profile);
    // Open a container menu for a connection and publish `menu_opened`.
    // Fails with `already_open` when the connection already has a menu and
    // with `unsupported` for a container the model does not implement.
    // attributes: #[thread(sim)] #[can_fail]
    refrain_status (*menu_open)(void *self, uint64_t connection, uint32_t container, refrain_services_net_session_handle_session_menu *out_value);
    // Close a menu and publish `menu_closed`. Slots are discarded.
    // attributes: #[thread(sim)] #[can_fail]
    refrain_status (*menu_close)(void *self, refrain_services_net_session_handle_session_menu menu);
    // Replace one slot and publish `menu_slot_changed` when the item or the
    // count differs. An empty `item` or a zero `count` clears the slot.
    // attributes: #[thread(sim)] #[can_fail]
    refrain_status (*menu_set_slot)(void *self, refrain_services_net_session_handle_session_menu menu, int32_t slot, refrain_str item, uint32_t count, uint8_t *out_value);
    // Apply one mouse click, publish `menu_clicked` and fill `result` with
    // the transmitted slot and carried state.
    // attributes: #[thread(sim)] #[can_fail]
    refrain_status (*menu_click)(void *self, refrain_services_net_session_handle_session_menu menu, int32_t slot, int32_t button, refrain_services_net_session_menu_click_result *result);
} refrain_services_net_session_vtable; // service refrain:net.session

#define REFRAIN_SERVICES_NET_SESSION_VTABLE_SIZE ((uint32_t)sizeof(refrain_services_net_session_vtable))

#ifdef __cplusplus
} // extern "C"
#endif

#endif // REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_SERVICES_NET_SESSION_ABI_H
