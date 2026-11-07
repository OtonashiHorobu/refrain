// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Play phase, clientbound. Chunk data, entity metadata, item stacks and chat
// components remain hand-written payloads with their own codecs.

module refrain::protocol::v777::play::clientbound;
version 777;
import refrain::protocol::v777::wire;

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

packet set_chunk_cache_center = 96 {
    #[varint] chunk_x: i32;
    #[varint] chunk_z: i32;
}

packet set_chunk_cache_radius = 97 {
    #[varint] radius: i32;
}

packet set_time = 115 {
    game_time: i64;
    clock_updates: array<clock_update>;
}

// Generated protocol layouts.

packet bundle_delimiter = 0 {
}

packet add_entity = 1 {
    #[varint] id: i32;
    uuid: uuid;
    type_: varint_id;
    x: f64;
    y: f64;
    z: f64;
    movement: lp_vec3;
    x_rot: i8;
    y_rot: i8;
    y_head_rot: i8;
    #[varint] data: i32;
}

packet animate = 2 {
    #[varint] id: i32;
    action: i8;
}

packet block_destruction = 5 {
    #[varint] id: i32;
    pos: i64;
    progress: i8;
}

packet block_entity_data = 6 {
    get_pos: i64;
    get_type: varint_id;
    get_tag: nbt;
}

packet block_event = 7 {
    pos: i64;
    b0: i8;
    b1: i8;
    block: varint_id;
}

packet boss_event = 9 {
}

packet chunks_biomes = 13 {
    chunk_biome_data: array<wire_chunk_biome_data>;
}

packet clear_titles = 14 {
    reset_times: bool;
}

packet command_suggestions = 15 {
    #[varint] id: i32;
    #[varint] start: i32;
    #[varint] length: i32;
    suggestions: array<wire_entry>;
}

packet container_close = 17 {
    #[varint] container_id: i32;
}

packet container_set_content = 18 {
    #[varint] container_id: i32;
    #[varint] state_id: i32;
    items: array<item_stack>;
    carried_item: item_stack;
}

packet container_set_data = 19 {
    #[varint] container_id: i32;
    #[fixed] id: i16;
    #[fixed] value: i16;
}

packet container_set_slot = 20 {
    #[varint] container_id: i32;
    #[varint] state_id: i32;
    #[fixed] slot: i16;
    item_stack: item_stack;
}

packet cookie_request = 21 {
    key: identifier;
}

packet cooldown = 22 {
    cooldown_group: identifier;
    #[varint] duration: i32;
}

packet custom_chat_completions = 23 {
    action: varint_id;
    entries: array<string>;
}

packet custom_payload = 24 {
    channel: identifier;
    #[rest] data: bytes;
}

packet damage_event = 25 {
    #[varint] entity_id: i32;
    #[varint] source_type: i32;
    #[optional_varint] field_2: optional<i32>;
    #[optional_varint] field_3: optional<i32>;
    source_position: optional<wire_optional_payload>;
}

packet debug_sample = 30 {
    sample: array<i64>;
    debug_sample_type: varint_id;
}

packet disconnect = 32 {
    payload: text_component;
}

packet disguised_chat = 33 {
    message: text_component;
    chat_type: wire_bound;
}

packet add_transient_block = 37 {
    pos: i64;
    block_state: varint_id;
}

packet forget_level_chunk = 38 {
    pos: i64;
}

packet game_rule_values = 40 {
    payload: map<identifier, string>;
}

packet game_test_highlight_pos = 41 {
    absolute_pos: i64;
    relative_pos: i64;
}

packet mount_screen_open = 42 {
    #[varint] container_id: i32;
    #[varint] inventory_columns: i32;
    #[fixed] entity_id: i32;
}

packet hurt_animation = 43 {
    #[varint] id: i32;
    yaw: f32;
}

packet level_chunk_with_light = 46 {
    #[fixed] x: i32;
    #[fixed] z: i32;
    chunk_data: wire_clientbound_level_chunk_packet_data;
    light_data: wire_light_data;
}

packet level_event = 47 {
    #[fixed] type_: i32;
    pos: i64;
    #[fixed] data: i32;
    global_event: bool;
}

packet light_update = 49 {
    #[varint] x: i32;
    #[varint] z: i32;
    light_data: wire_light_data;
}

packet low_disk_space_warning = 51 {
}

packet move_minecart_along_track = 56 {
    #[varint] entity_id: i32;
    lerp_steps: array<wire_lerp_steps>;
}

packet move_vehicle = 58 {
    payload: wire_position_and_rotation;
}

packet open_book = 59 {
    hand: varint_id;
}

packet open_screen = 60 {
    #[varint] get_container_id: i32;
    get_type: varint_id;
    get_title: text_component;
}

packet open_sign_editor = 61 {
    pos: i64;
    slot: varint_id;
}

packet ping = 62 {
    #[fixed] id: i32;
}

packet pong_response = 63 {
    time: i64;
}

packet place_ghost_recipe = 64 {
    #[varint] container_id: i32;
    recipe_display: varint_id;
}

packet player_combat_end = 67 {
    #[varint] duration: i32;
}

packet player_combat_enter = 68 {
}

packet player_combat_kill = 69 {
    #[varint] player_id: i32;
    message: text_component;
}

packet player_info_remove = 70 {
    profile_ids: array<uuid>;
}

packet player_rotation = 74 {
    y_rot: f32;
    relative_y: bool;
    x_rot: f32;
    relative_x: bool;
}

packet recipe_book_remove = 76 {
    recipes: array<wire_recipe_display_id>;
}

packet recipe_book_settings = 77 {
    book_settings: wire_book_settings;
}

packet remove_entities = 78 {
    entity_ids: array<varint_id>;
}

packet remove_mob_effect = 79 {
    #[varint] entity_id: i32;
    effect: varint_id;
}

packet reset_score = 80 {
    #[max(32767)] owner: string;
    objective_name: optional<string>;
}

packet resource_pack_pop = 81 {
    id: optional<uuid>;
}

packet resource_pack_push = 82 {
    id: uuid;
    #[max(32767)] url: string;
    #[max(40)] hash: string;
    required: bool;
    prompt: optional<text_component>;
}

packet post_effects = 83 {
    post_effects: array<identifier>;
}

packet respawn = 84 {
    common_player_spawn_info: wire_common_player_spawn_info;
    data_to_keep: i8;
}

packet rotate_head = 85 {
    #[varint] entity_id: i32;
    y_head_rot: i8;
}

packet section_blocks_update = 86 {
    section_pos: i64;
    #[varlong] positions: array<i64>;
}

packet select_advancements_tab = 87 {
    tab: optional<identifier>;
}

packet server_data = 88 {
    motd: text_component;
    icon_bytes: optional<bytes>;
}

packet set_action_bar_text = 89 {
    text: text_component;
}

packet set_border_center = 90 {
    new_center_x: f64;
    new_center_z: f64;
}

packet set_border_lerp_size = 91 {
    old_size: f64;
    new_size: f64;
    #[varlong] lerp_time: i64;
}

packet set_border_size = 92 {
    size: f64;
}

packet set_border_warning_delay = 93 {
    #[varint] warning_delay: i32;
}

packet set_border_warning_distance = 94 {
    #[varint] warning_blocks: i32;
}

packet set_camera = 95 {
    #[varint] camera_id: i32;
}

packet set_cursor_item = 98 {
    contents: item_stack;
}

packet set_display_objective = 100 {
    slot: varint_id;
    #[max(32767)] objective_name: string;
}

packet set_entity_link = 102 {
    #[fixed] source_id: i32;
    #[fixed] dest_id: i32;
}

packet set_entity_motion = 103 {
    #[varint] id: i32;
    movement: lp_vec3;
}

packet set_equipment = 104 {
    entries: array<wire_entries_entry>;
}

packet set_experience = 105 {
    experience_progress: f32;
    #[varint] experience_level: i32;
    #[varint] total_experience: i32;
}

packet set_health = 106 {
    health: f32;
    #[varint] food: i32;
    saturation: f32;
}

packet set_passengers = 109 {
    #[varint] vehicle: i32;
    #[varint] passengers: array<i32>;
}

packet set_player_inventory = 110 {
    #[varint] slot: i32;
    contents: item_stack;
}

packet set_score = 112 {
    #[max(32767)] owner: string;
    #[max(32767)] objective_name: string;
    #[varint] score: i32;
    display: optional<text_component>;
    number_format: optional<nbt>;
}

packet set_simulation_distance = 113 {
    #[varint] simulation_distance: i32;
}

packet set_subtitle_text = 114 {
    text: text_component;
}

packet set_title_text = 116 {
    text: text_component;
}

packet set_titles_animation = 117 {
    #[fixed] fade_in: i32;
    #[fixed] stay: i32;
    #[fixed] fade_out: i32;
}

packet sound_entity = 118 {
    sound: holder<wire_sound_event>;
    source: varint_id;
    #[varint] id: i32;
    volume: f32;
    pitch: f32;
    seed: i64;
}

packet sound = 119 {
    sound: holder<wire_sound_event>;
    source: varint_id;
    #[fixed] x: i32;
    #[fixed] y: i32;
    #[fixed] z: i32;
    volume: f32;
    pitch: f32;
    seed: i64;
}

packet start_configuration = 120 {
}

packet store_cookie = 122 {
    key: identifier;
    #[max(5120)] payload: bytes;
}

packet swing_animation = 123 {
    #[varint] entity_id: i32;
    hand: varint_id;
    animation: wire_swing_animation;
}

packet system_chat = 124 {
    content: text_component;
    overlay: bool;
}

packet tab_list = 125 {
    header: text_component;
    footer: text_component;
}

packet tag_query = 126 {
    #[varint] transaction_id: i32;
    tag: nbt;
}

packet take_item_entity = 127 {
    #[varint] item_id: i32;
    #[varint] player_id: i32;
    #[varint] amount: i32;
}

packet teleport_entity = 128 {
    #[varint] id: i32;
    change: wire_position_move_rotation;
    #[fixed] relatives: i32;
    on_ground: bool;
}

packet test_instance_block_status = 129 {
    status: text_component;
    size: optional<wire_vec3i>;
}

packet ticking_state = 130 {
    tick_rate: f32;
    is_frozen: bool;
}

packet ticking_step = 131 {
    #[varint] tick_steps: i32;
}

packet transfer = 132 {
    #[max(32767)] host: string;
    #[varint] port: i32;
}

packet update_mob_effect = 135 {
    #[varint] entity_id: i32;
    effect: varint_id;
    #[varint] effect_amplifier: i32;
    #[varint] effect_duration_ticks: i32;
    flags_: i8;
}

packet projectile_power = 138 {
    #[varint] id: i32;
    acceleration_power: f64;
}

packet custom_report_details = 139 {
    details: map<string, string>;
}

packet server_links = 140 {
    links: array<wire_untrusted_entry>;
}

packet clear_dialog = 142 {
}

packet show_dialog = 143 {
    dialog: holder<nbt>;
}
