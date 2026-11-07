// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Play phase, serverbound.

module refrain::protocol::v777::play::serverbound;
version 777;

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
