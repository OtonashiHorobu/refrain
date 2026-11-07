// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Payload structs shared by the protocol 777 stage schemas. Each
// field mirrors the packet wire order; encodings that differ from
// the struct defaults carry an attribute.

module refrain::protocol::v777::wire;
version 777;

struct wire_lerp_steps {
    position: wire_vec3;
    movement: wire_vec3;
    y_rot: i8;
    x_rot: i8;
    weight: f32;
}

struct wire_recipe_display_id {
    #[varint] index: i32;
}

struct wire_type_settings {
    open: bool;
    filtering: bool;
}

struct wire_book_settings {
    o_o_crafting: wire_type_settings;
    o_o_furnace: wire_type_settings;
    o_o_blast_furnace: wire_type_settings;
    o_o_smoker: wire_type_settings;
}

struct wire_swing_animation {
    #[varint] type_: i32;
    #[varint] duration: i32;
}

struct wire_position_and_rotation {
    position: wire_vec3;
    y_rot: f32;
    x_rot: f32;
}

struct wire_position_move_rotation {
    position: wire_vec3;
    delta_movement: wire_vec3;
    y_rot: f32;
    x_rot: f32;
}

struct wire_vec3 {
    x: f64;
    y: f64;
    z: f64;
}

