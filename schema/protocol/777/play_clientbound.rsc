// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Play phase, clientbound. Chunk data, entity metadata, item stacks and chat
// components remain hand-written payloads with their own codecs.

module refrain::protocol::v777::play::clientbound;
version 777;

struct global_pos {
    dimension: identifier;
    position: i64;
}

struct spawn_info {
    /// Dimension type registry holder.
    #[varint] dimension_type: u32;
    dimension: identifier;
    seed: i64;
    #[varint] game_type: u32;
    /// `0` means absent; a present value is stored as `value + 1`.
    #[optional_varint] previous_game_type: optional<u32>;
    debug: bool;
    flat: bool;
    last_death_location: optional<global_pos>;
    #[varint] portal_cooldown: u32;
    #[varint] sea_level: u32;
}

struct clock_update {
    clock: identifier;
    #[varint] state: u32;
}

packet block_changed_ack = 4 {
    #[varint] sequence: u32;
}

packet block_update = 8 {
    position: i64;
    #[varint] state: u32;
}

packet change_difficulty = 10 {
    #[varint] difficulty: u32;
    locked: bool;
}

packet chunk_batch_finished = 11 {
    #[varint] batch_size: u32;
}

packet chunk_batch_start = 12 {
}

packet entity_event = 34 {
    #[fixed] entity_id: i32;
    event: u8;
}

packet game_event = 39 {
    event: u8;
    value: f32;
}

packet initialize_border = 44 {
    center_x: f64;
    center_z: f64;
    old_size: f64;
    new_size: f64;
    #[varlong] lerp_time: i64;
    #[varint] absolute_max_size: u32;
    #[varint] warning_blocks: u32;
    #[varint] warning_time: u32;
}

packet keep_alive = 45 {
    id: i64;
}

packet login = 50 {
    #[fixed] player_id: i32;
    hardcore: bool;
    levels: array<identifier>;
    #[varint] max_players: u32;
    #[varint] chunk_radius: u32;
    #[varint] simulation_distance: u32;
    reduced_debug_info: bool;
    show_death_screen: bool;
    limited_crafting: bool;
    spawn: spawn_info;
    online_mode: bool;
    enforces_secure_chat: bool;
}

packet player_abilities = 65 {
    flags: u8;
    flying_speed: f32;
    walking_speed: f32;
}

packet player_position = 73 {
    #[varint] id: u32;
    x: f64;
    y: f64;
    z: f64;
    delta_x: f64;
    delta_y: f64;
    delta_z: f64;
    y_rot: f32;
    x_rot: f32;
    #[fixed] relatives: i32;
}

packet set_default_spawn_position = 99 {
    dimension: identifier;
    position: i64;
    yaw: f32;
    pitch: f32;
}

packet set_held_slot = 107 {
    #[varint] slot: u32;
}

packet set_time = 115 {
    game_time: i64;
    clock_updates: array<clock_update>;
}
