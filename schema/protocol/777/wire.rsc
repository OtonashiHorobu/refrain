// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Payload structs shared by the protocol 777 stage schemas. Each
// field mirrors the packet wire order; encodings that differ from
// the struct defaults carry an attribute.

module refrain::protocol::v777::wire;
version 777;

struct wire_at_entity_payload {
    #[varint] entity: i32;
    to_anchor: varint_id;
}

struct wire_entries_entry {
    field_0: i8;
    field_1: item_stack;
}

struct wire_message_signature {
    #[fixed_bytes(256)] field_0: bytes;
}

struct wire_optional_payload {
    field_0: f64;
    field_1: f64;
    field_2: f64;
}

struct wire_attribute_snapshot {
    id: identifier;
    amount: f64;
    operation: varint_id;
}

struct wire_get_values {
    attribute: varint_id;
    base: f64;
    modifiers: array<wire_attribute_snapshot>;
}

struct wire_entry_3 {
    #[max(16)] name: string;
    signature: wire_message_signature;
}

struct wire_argument_signatures {
    entries: array<wire_entry_3>;
}

struct wire_update {
    #[varint] offset: i32;
    #[fixed_bytes(20)] acknowledged: bytes;
    checksum: i8;
}

struct wire_data {
    expires_at: i64;
    #[max(512)] key: bytes;
    #[max(4096)] key_signature: bytes;
}

struct wire_type_83 {
    added_components: map<varint_id, i32>;
    removed_components: array<varint_id>;
}

struct wire_actual_item {
    item: varint_id;
    #[varint] count: i32;
    components: wire_type_83;
}

struct wire_entry_4 {
    game_rule_key: identifier;
    #[max(32767)] value: string;
}

struct wire_data_3 {
    test: optional<identifier>;
    size: wire_vec3i;
    rotation: varint_id;
    ignore_entities: bool;
    status: varint_id;
    error_message: optional<text_component>;
}

struct wire_untrusted_entry {
    type_: either<varint_id, text_component>;
    #[max(32767)] link: string;
}

struct wire_chunk_biome_data {
    pos: i64;
    #[max(2097152)] buffer: bytes;
}

struct wire_entry {
    #[max(32767)] text: string;
    tooltip: optional<text_component>;
}

struct wire_chat_type_decoration {
    #[max(32767)] translation_key: string;
    parameters: array<varint_id>;
    style: nbt;
}

struct wire_chat_type {
    chat: wire_chat_type_decoration;
    narration: wire_chat_type_decoration;
}

struct wire_bound {
    chat_type: holder<wire_chat_type>;
    name: text_component;
    target_name: optional<text_component>;
}

struct wire_packed_registry_entry {
    id: identifier;
    data: optional<nbt>;
}

struct wire_sound_event {
    location: identifier;
    fixed_range: optional<f32>;
}

struct wire_block_entities_data {
    packed_xz: i8;
    #[fixed] y: i16;
    type_: varint_id;
    tag: nbt;
}

struct wire_clientbound_level_chunk_packet_data {
    heightmaps: map<varint_id, array<i64>>;
    #[max(2097152)] buffer: bytes;
    block_entities_data: array<wire_block_entities_data>;
}

struct wire_light_data {
    #[max(32767)] sky_y_mask: bytes;
    #[max(32767)] block_y_mask: bytes;
    #[max(32767)] empty_sky_y_mask: bytes;
    #[max(32767)] empty_block_y_mask: bytes;
    sky_updates: array<bytes>;
    block_updates: array<bytes>;
}

struct wire_global_pos {
    dimension: identifier;
    pos: i64;
}

struct wire_common_player_spawn_info {
    dimension_type: varint_id;
    dimension: identifier;
    seed: i64;
    game_type: varint_id;
    previous_game_type: optional<u32>;
    is_debug: bool;
    is_flat: bool;
    last_death_location: optional<wire_global_pos>;
    #[varint] portal_cooldown: i32;
    #[varint] sea_level: i32;
}

struct wire_map_decoration {
    type_: varint_id;
    x: i8;
    y: i8;
    rot: i8;
    name: optional<nbt>;
}

struct wire_lerp_steps {
    position: wire_vec3;
    movement: wire_vec3;
    y_rot: i8;
    x_rot: i8;
    weight: f32;
}

struct wire_packed {
    entries: array<message_signature_packed>;
}

struct wire_body {
    #[max(256)] content: string;
    time_stamp: i64;
    salt: i64;
    last_seen: wire_packed;
}

struct wire_recipe_display_id {
    #[varint] index: i32;
}

struct wire_type_settings {
    open: bool;
    filtering: bool;
}

struct wire_book_settings {
    crafting: wire_type_settings;
    furnace: wire_type_settings;
    blast_furnace: wire_type_settings;
    smoker: wire_type_settings;
}

struct wire_network_payload {
    tags: map<identifier, array<varint_id>>;
}

struct wire_swing_animation {
    type_: varint_id;
    #[varint] duration: i32;
}

struct wire_advancement_holder {
    id: identifier;
    value: advancement;
}

struct wire_added {
    advancement: wire_advancement_holder;
    x: f32;
    y: f32;
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

struct wire_vec3i {
    #[varint] x: i32;
    #[varint] y: i32;
    #[varint] z: i32;
}

