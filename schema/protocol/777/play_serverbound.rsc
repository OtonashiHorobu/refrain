// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Play phase, serverbound.

module refrain::protocol::v777::play::serverbound;
version 777;
import refrain::protocol::v777::wire;

import refrain::protocol::v777::common;

packet accept_teleportation = 0 {
    #[varint] id: u32;
    x: f64;
    y: f64;
    z: f64;
    y_rot: f32;
    x_rot: f32;
}

packet chat_command = 7 {
    #[max(256)] command: string;
}

packet chat = 9 {
    #[max(256)] message: string;
    timestamp: i64;
    salt: i64;
    #[fixed_bytes(256)] signature: optional<bytes>;
    #[varint] offset: u32;
    #[fixed_bytes(20)] acknowledged: bytes;
    #[fixed] checksum: i8;
}

packet chunk_batch_received = 11 {
    desired_chunks: f32;
}

packet client_information = 14 {
    information: client_information_data;
}

packet keep_alive = 28 {
    id: i64;
}

packet move_player_pos = 30 {
    x: f64;
    y: f64;
    z: f64;
    flags: u8;
}

packet move_player_pos_rot = 31 {
    x: f64;
    y: f64;
    z: f64;
    y_rot: f32;
    x_rot: f32;
    flags: u8;
}

packet move_player_rot = 32 {
    y_rot: f32;
    x_rot: f32;
    flags: u8;
}

packet move_player_status_only = 33 {
    flags: u8;
}

packet player_action = 41 {
    #[varint] action: u32;
    position: i64;
    direction: u8;
    #[varint] sequence: u32;
}

packet player_loaded = 44 {
}

packet set_carried_item = 54 {
    #[fixed] slot: i16;
}

packet use_item_on = 66 {
    #[varint] hand: u32;
    position: i64;
    #[varint] face: u32;
    click_x: f32;
    click_y: f32;
    click_z: f32;
    inside: bool;
    world_border: bool;
    #[varint] sequence: u32;
}

// Generated protocol layouts.

packet attack = 1 {
    #[varint] entity_id: i32;
}

packet block_entity_tag_query = 2 {
    #[varint] transaction_id: i32;
    pos: i64;
}

packet bundle_item_selected = 3 {
    #[varint] slot_id: i32;
    #[varint] selected_item_index: i32;
}

packet change_difficulty = 4 {
    #[varint] difficulty: u32;
}

packet change_game_mode = 5 {
    #[varint] mode: u32;
}

packet chat_ack = 6 {
    #[varint] offset: i32;
}

packet client_command = 12 {
    #[varint] action: u32;
}

packet client_tick_end = 13 {
}

packet configuration_acknowledged = 16 {
}

packet container_button_click = 17 {
    #[varint] container_id: i32;
    #[varint] button_id: i32;
}

packet container_close = 19 {
    #[varint] container_id: i32;
}

packet container_slot_state_changed = 20 {
    #[varint] slot_id: i32;
    #[varint] container_id: i32;
    new_state: bool;
}

packet debug_subscription_request = 23 {
    #[varint] payload: i32;
}

packet edit_book = 24 {
    #[varint] slot: i32;
    pages: array<string>;
    title: optional<string>;
}

packet entity_tag_query = 25 {
    #[varint] transaction_id: i32;
    #[varint] entity_id: i32;
}

packet jigsaw_generate = 27 {
    pos: i64;
    #[varint] levels: i32;
    keep_jigsaws: bool;
}

packet lock_difficulty = 29 {
    locked: bool;
}

packet move_vehicle = 34 {
    moving_to: wire_position_and_rotation;
    on_ground: bool;
}

packet paddle_boat = 35 {
    left: bool;
    right: bool;
}

packet pick_item_from_block = 36 {
    pos: i64;
    include_data: bool;
}

packet pick_item_from_entity = 37 {
    #[varint] id: i32;
    include_data: bool;
}

packet ping_request = 38 {
    time: i64;
}

packet place_recipe = 39 {
    #[varint] container_id: i32;
    recipe: wire_recipe_display_id;
    use_max_items: bool;
}

packet player_abilities = 40 {
    field_1: i8;
}

packet player_command = 42 {
    #[varint] id: i32;
    #[varint] action: u32;
    #[varint] data: i32;
}

packet player_input = 43 {
    input: i8;
}

packet pong = 45 {
    #[fixed] id: i32;
}

packet punch = 46 {
}

packet recipe_book_change_settings = 47 {
    #[varint] book_type: u32;
    is_open: bool;
    is_filtering: bool;
}

packet recipe_book_seen_recipe = 48 {
    recipe: wire_recipe_display_id;
}

packet rename_item = 49 {
    #[max(32767)] name: string;
}

packet resource_pack = 50 {
    id: uuid;
    #[varint] action: u32;
}

packet select_trade = 52 {
    #[varint] item: i32;
}

packet set_beacon = 53 {
    primary: optional<i32>;
    secondary: optional<i32>;
}

packet set_command_block = 55 {
    pos: i64;
    #[max(32767)] command: string;
    #[varint] mode: u32;
    conditional: i8;
}

packet set_command_minecart = 56 {
    #[varint] entity: i32;
    #[max(32767)] command: string;
    track_output: bool;
}

packet set_creative_mode_slot = 57 {
    #[fixed] slot_num: i16;
    item_stack: item_stack;
}

packet set_jigsaw_block = 59 {
    pos: i64;
    name: identifier;
    target: identifier;
    pool: identifier;
    #[max(32767)] final_state: string;
    #[max(32767)] joint: string;
    #[varint] selection_priority: i32;
    #[varint] placement_priority: i32;
}

packet set_structure_block = 60 {
    pos: i64;
    #[varint] update_type: u32;
    #[varint] mode: u32;
    #[max(32767)] name: string;
    offset: i8;
    size: i8;
    mirror: i8;
    rotation: i8;
    data: i8;
    integrity: i8;
    #[varint] mirror_2: u32;
    #[varint] rotation_2: u32;
    #[max(32767)] data_2: string;
    integrity_2: f32;
    #[varlong] seed: i64;
    field_16: i8;
}

packet set_test_block = 61 {
    position: i64;
    #[varint] mode: i32;
    #[max(32767)] message: string;
}

packet sign_update = 62 {
    pos: i64;
    lines: array<string>;
    #[varint] slot: i32;
}

packet spectator_action = 63 {
    #[optional_varint] spectate_entity_id: optional<i32>;
}

packet teleport_to_entity = 64 {
    uuid: uuid;
}

packet use_item = 67 {
    #[varint] hand: u32;
    #[varint] sequence: i32;
    y_rot: f32;
    x_rot: f32;
}
