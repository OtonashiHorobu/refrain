// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: dde5b4dbceca0c8c

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_PLAY_CLIENTBOUND_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_PLAY_CLIENTBOUND_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <refrain/core/error.hpp>
#include <refrain/net/base.hpp>
#include <refrain/data/resource_location.hpp>
#include <refrain/net/wire.hpp>
#include <map>
#include <utility>
#include <variant>
#include <refrain/schema/protocol/777/wire.hpp>

namespace refrain::protocol::v777::play::clientbound {

struct global_pos {
    ::refrain::data::resource_location dimension;
    std::int64_t position;
};

struct spawn_info {
    /// Dimension type registry holder.
    // attributes: #[varint]
    std::uint32_t dimension_type;
    ::refrain::data::resource_location dimension;
    std::int64_t seed;
    // attributes: #[varint]
    std::uint32_t game_type;
    /// `0` means absent; a present value is stored as `value + 1`.
    // attributes: #[optional_varint]
    std::optional<std::uint32_t> previous_game_type;
    bool debug;
    bool flat;
    std::optional<::refrain::protocol::v777::play::clientbound::global_pos> last_death_location;
    // attributes: #[varint]
    std::uint32_t portal_cooldown;
    // attributes: #[varint]
    std::uint32_t sea_level;
};

struct clock_update {
    ::refrain::data::resource_location clock;
    // attributes: #[varint]
    std::uint32_t state;
};

struct block_changed_ack {
    static constexpr std::uint16_t packet_id = 0x4;
    static constexpr std::string_view packet_name = "block_changed_ack";

    // attributes: #[varint]
    std::uint32_t sequence;
};

struct block_update {
    static constexpr std::uint16_t packet_id = 0x8;
    static constexpr std::string_view packet_name = "block_update";

    std::int64_t position;
    // attributes: #[varint]
    std::uint32_t state;
};

struct change_difficulty {
    static constexpr std::uint16_t packet_id = 0xA;
    static constexpr std::string_view packet_name = "change_difficulty";

    // attributes: #[varint]
    std::uint32_t difficulty;
    bool locked;
};

struct chunk_batch_finished {
    static constexpr std::uint16_t packet_id = 0xB;
    static constexpr std::string_view packet_name = "chunk_batch_finished";

    // attributes: #[varint]
    std::uint32_t batch_size;
};

struct chunk_batch_start {
    static constexpr std::uint16_t packet_id = 0xC;
    static constexpr std::string_view packet_name = "chunk_batch_start";

};

struct entity_event {
    static constexpr std::uint16_t packet_id = 0x22;
    static constexpr std::string_view packet_name = "entity_event";

    // attributes: #[fixed]
    std::int32_t entity_id;
    std::uint8_t event;
};

struct game_event {
    static constexpr std::uint16_t packet_id = 0x27;
    static constexpr std::string_view packet_name = "game_event";

    std::uint8_t event;
    float value;
};

struct initialize_border {
    static constexpr std::uint16_t packet_id = 0x2C;
    static constexpr std::string_view packet_name = "initialize_border";

    double center_x;
    double center_z;
    double old_size;
    double new_size;
    // attributes: #[varlong]
    std::int64_t lerp_time;
    // attributes: #[varint]
    std::uint32_t absolute_max_size;
    // attributes: #[varint]
    std::uint32_t warning_blocks;
    // attributes: #[varint]
    std::uint32_t warning_time;
};

struct keep_alive {
    static constexpr std::uint16_t packet_id = 0x2D;
    static constexpr std::string_view packet_name = "keep_alive";

    std::int64_t id;
};

struct login {
    static constexpr std::uint16_t packet_id = 0x32;
    static constexpr std::string_view packet_name = "login";

    // attributes: #[fixed]
    std::int32_t player_id;
    bool hardcore;
    std::vector<::refrain::data::resource_location> levels;
    // attributes: #[varint]
    std::uint32_t max_players;
    // attributes: #[varint]
    std::uint32_t chunk_radius;
    // attributes: #[varint]
    std::uint32_t simulation_distance;
    bool reduced_debug_info;
    bool show_death_screen;
    bool limited_crafting;
    ::refrain::protocol::v777::play::clientbound::spawn_info spawn;
    bool online_mode;
    bool enforces_secure_chat;
};

struct player_abilities {
    static constexpr std::uint16_t packet_id = 0x41;
    static constexpr std::string_view packet_name = "player_abilities";

    std::uint8_t flags;
    float flying_speed;
    float walking_speed;
};

struct player_position {
    static constexpr std::uint16_t packet_id = 0x49;
    static constexpr std::string_view packet_name = "player_position";

    // attributes: #[varint]
    std::uint32_t id;
    double x;
    double y;
    double z;
    double delta_x;
    double delta_y;
    double delta_z;
    float y_rot;
    float x_rot;
    // attributes: #[fixed]
    std::int32_t relatives;
};

struct set_default_spawn_position {
    static constexpr std::uint16_t packet_id = 0x63;
    static constexpr std::string_view packet_name = "set_default_spawn_position";

    ::refrain::data::resource_location dimension;
    std::int64_t position;
    float yaw;
    float pitch;
};

struct set_held_slot {
    static constexpr std::uint16_t packet_id = 0x6B;
    static constexpr std::string_view packet_name = "set_held_slot";

    // attributes: #[varint]
    std::uint32_t slot;
};

struct set_chunk_cache_center {
    static constexpr std::uint16_t packet_id = 0x60;
    static constexpr std::string_view packet_name = "set_chunk_cache_center";

    // attributes: #[varint]
    std::int32_t chunk_x;
    // attributes: #[varint]
    std::int32_t chunk_z;
};

struct set_chunk_cache_radius {
    static constexpr std::uint16_t packet_id = 0x61;
    static constexpr std::string_view packet_name = "set_chunk_cache_radius";

    // attributes: #[varint]
    std::int32_t radius;
};

struct set_time {
    static constexpr std::uint16_t packet_id = 0x73;
    static constexpr std::string_view packet_name = "set_time";

    std::int64_t game_time;
    std::vector<::refrain::protocol::v777::play::clientbound::clock_update> clock_updates;
};

struct bundle_delimiter {
    static constexpr std::uint16_t packet_id = 0x0;
    static constexpr std::string_view packet_name = "bundle_delimiter";

};

struct add_entity {
    static constexpr std::uint16_t packet_id = 0x1;
    static constexpr std::string_view packet_name = "add_entity";

    // attributes: #[varint]
    std::int32_t id;
    ::refrain::net::uuid uuid;
    std::uint32_t type_;
    double x;
    double y;
    double z;
    ::refrain::net::lp_vec3 movement;
    // attributes: #[fixed]
    std::int8_t x_rot;
    // attributes: #[fixed]
    std::int8_t y_rot;
    // attributes: #[fixed]
    std::int8_t y_head_rot;
    // attributes: #[varint]
    std::int32_t data;
};

struct animate {
    static constexpr std::uint16_t packet_id = 0x2;
    static constexpr std::string_view packet_name = "animate";

    // attributes: #[varint]
    std::int32_t id;
    // attributes: #[fixed]
    std::int8_t action;
};

struct award_stats {
    static constexpr std::uint16_t packet_id = 0x3;
    static constexpr std::string_view packet_name = "award_stats";

    std::map<::refrain::net::stat_entry, std::uint32_t> payload;
};

struct block_destruction {
    static constexpr std::uint16_t packet_id = 0x5;
    static constexpr std::string_view packet_name = "block_destruction";

    // attributes: #[varint]
    std::int32_t id;
    std::int64_t pos;
    // attributes: #[fixed]
    std::int8_t progress;
};

struct block_entity_data {
    static constexpr std::uint16_t packet_id = 0x6;
    static constexpr std::string_view packet_name = "block_entity_data";

    std::int64_t get_pos;
    std::uint32_t get_type;
    ::refrain::net::nbt_blob get_tag;
};

struct block_event {
    static constexpr std::uint16_t packet_id = 0x7;
    static constexpr std::string_view packet_name = "block_event";

    std::int64_t pos;
    // attributes: #[fixed]
    std::int8_t b0;
    // attributes: #[fixed]
    std::int8_t b1;
    std::uint32_t block;
};

struct boss_event {
    static constexpr std::uint16_t packet_id = 0x9;
    static constexpr std::string_view packet_name = "boss_event";

};

struct chunks_biomes {
    static constexpr std::uint16_t packet_id = 0xD;
    static constexpr std::string_view packet_name = "chunks_biomes";

    std::vector<::refrain::protocol::v777::wire::wire_chunk_biome_data> chunk_biome_data;
};

struct clear_titles {
    static constexpr std::uint16_t packet_id = 0xE;
    static constexpr std::string_view packet_name = "clear_titles";

    bool reset_times;
};

struct command_suggestions {
    static constexpr std::uint16_t packet_id = 0xF;
    static constexpr std::string_view packet_name = "command_suggestions";

    // attributes: #[varint]
    std::int32_t id;
    // attributes: #[varint]
    std::int32_t start;
    // attributes: #[varint]
    std::int32_t length;
    std::vector<::refrain::protocol::v777::wire::wire_entry> suggestions;
};

struct commands {
    static constexpr std::uint16_t packet_id = 0x10;
    static constexpr std::string_view packet_name = "commands";

    ::refrain::net::command_tree tree;
};

struct container_close {
    static constexpr std::uint16_t packet_id = 0x11;
    static constexpr std::string_view packet_name = "container_close";

    // attributes: #[varint]
    std::int32_t container_id;
};

struct container_set_content {
    static constexpr std::uint16_t packet_id = 0x12;
    static constexpr std::string_view packet_name = "container_set_content";

    // attributes: #[varint]
    std::int32_t container_id;
    // attributes: #[varint]
    std::int32_t state_id;
    std::vector<::refrain::net::item_stack> items;
    ::refrain::net::item_stack carried_item;
};

struct container_set_data {
    static constexpr std::uint16_t packet_id = 0x13;
    static constexpr std::string_view packet_name = "container_set_data";

    // attributes: #[varint]
    std::int32_t container_id;
    // attributes: #[fixed]
    std::int16_t id;
    // attributes: #[fixed]
    std::int16_t value;
};

struct container_set_slot {
    static constexpr std::uint16_t packet_id = 0x14;
    static constexpr std::string_view packet_name = "container_set_slot";

    // attributes: #[varint]
    std::int32_t container_id;
    // attributes: #[varint]
    std::int32_t state_id;
    // attributes: #[fixed]
    std::int16_t slot;
    ::refrain::net::item_stack item_stack;
};

struct cookie_request {
    static constexpr std::uint16_t packet_id = 0x15;
    static constexpr std::string_view packet_name = "cookie_request";

    ::refrain::data::resource_location key;
};

struct cooldown {
    static constexpr std::uint16_t packet_id = 0x16;
    static constexpr std::string_view packet_name = "cooldown";

    ::refrain::data::resource_location cooldown_group;
    // attributes: #[varint]
    std::int32_t duration;
};

struct custom_chat_completions {
    static constexpr std::uint16_t packet_id = 0x17;
    static constexpr std::string_view packet_name = "custom_chat_completions";

    std::uint32_t action;
    std::vector<std::string> entries;
};

struct custom_payload {
    static constexpr std::uint16_t packet_id = 0x18;
    static constexpr std::string_view packet_name = "custom_payload";

    ::refrain::data::resource_location channel;
    // attributes: #[rest]
    std::vector<std::byte> data;
};

struct damage_event {
    static constexpr std::uint16_t packet_id = 0x19;
    static constexpr std::string_view packet_name = "damage_event";

    // attributes: #[varint]
    std::int32_t entity_id;
    // attributes: #[varint]
    std::int32_t source_type;
    // attributes: #[optional_varint]
    std::optional<std::int32_t> field_2;
    // attributes: #[optional_varint]
    std::optional<std::int32_t> field_3;
    std::optional<::refrain::protocol::v777::wire::wire_optional_payload> source_position;
};

struct debug_block_value {
    static constexpr std::uint16_t packet_id = 0x1A;
    static constexpr std::string_view packet_name = "debug/block_value";

    std::int64_t block_pos;
    ::refrain::net::debug_subscription_update update;
};

struct debug_chunk_value {
    static constexpr std::uint16_t packet_id = 0x1B;
    static constexpr std::string_view packet_name = "debug/chunk_value";

    std::int64_t chunk_pos;
    ::refrain::net::debug_subscription_update update;
};

struct debug_entity_value {
    static constexpr std::uint16_t packet_id = 0x1C;
    static constexpr std::string_view packet_name = "debug/entity_value";

    // attributes: #[varint]
    std::int32_t entity_id;
    ::refrain::net::debug_subscription_update update;
};

struct debug_event {
    static constexpr std::uint16_t packet_id = 0x1D;
    static constexpr std::string_view packet_name = "debug/event";

    ::refrain::net::debug_subscription_event event_;
};

struct debug_sample {
    static constexpr std::uint16_t packet_id = 0x1E;
    static constexpr std::string_view packet_name = "debug_sample";

    std::vector<std::int64_t> sample;
    std::uint32_t debug_sample_type;
};

struct delete_chat {
    static constexpr std::uint16_t packet_id = 0x1F;
    static constexpr std::string_view packet_name = "delete_chat";

    ::refrain::net::message_signature_packed message_signature;
};

struct disconnect {
    static constexpr std::uint16_t packet_id = 0x20;
    static constexpr std::string_view packet_name = "disconnect";

    ::refrain::net::text_component payload;
};

struct disguised_chat {
    static constexpr std::uint16_t packet_id = 0x21;
    static constexpr std::string_view packet_name = "disguised_chat";

    ::refrain::net::text_component message;
    ::refrain::protocol::v777::wire::wire_bound chat_type;
};

struct entity_position_sync {
    static constexpr std::uint16_t packet_id = 0x23;
    static constexpr std::string_view packet_name = "entity_position_sync";

    // attributes: #[varint]
    std::int32_t id;
    ::refrain::net::position_path position;
    float y_rot;
    float x_rot;
    bool on_ground;
};

struct explode {
    static constexpr std::uint16_t packet_id = 0x24;
    static constexpr std::string_view packet_name = "explode";

    ::refrain::protocol::v777::wire::wire_vec3 center;
    float radius;
    // attributes: #[fixed]
    std::int32_t block_count;
    std::optional<::refrain::protocol::v777::wire::wire_vec3> player_knockback;
    ::refrain::net::particle_options explosion_particle;
    std::variant<std::uint32_t, ::refrain::protocol::v777::wire::wire_sound_event> explosion_sound;
    std::vector<::refrain::protocol::v777::wire::wire_weighted_entry> block_particles;
    bool play_sound;
};

struct add_transient_block {
    static constexpr std::uint16_t packet_id = 0x25;
    static constexpr std::string_view packet_name = "add_transient_block";

    std::int64_t pos;
    std::uint32_t block_state;
};

struct forget_level_chunk {
    static constexpr std::uint16_t packet_id = 0x26;
    static constexpr std::string_view packet_name = "forget_level_chunk";

    std::int64_t pos;
};

struct game_rule_values {
    static constexpr std::uint16_t packet_id = 0x28;
    static constexpr std::string_view packet_name = "game_rule_values";

    std::map<::refrain::data::resource_location, std::string> payload;
};

struct game_test_highlight_pos {
    static constexpr std::uint16_t packet_id = 0x29;
    static constexpr std::string_view packet_name = "game_test_highlight_pos";

    std::int64_t absolute_pos;
    std::int64_t relative_pos;
};

struct mount_screen_open {
    static constexpr std::uint16_t packet_id = 0x2A;
    static constexpr std::string_view packet_name = "mount_screen_open";

    // attributes: #[varint]
    std::int32_t container_id;
    // attributes: #[varint]
    std::int32_t inventory_columns;
    // attributes: #[fixed]
    std::int32_t entity_id;
};

struct hurt_animation {
    static constexpr std::uint16_t packet_id = 0x2B;
    static constexpr std::string_view packet_name = "hurt_animation";

    // attributes: #[varint]
    std::int32_t id;
    float yaw;
};

struct level_chunk_with_light {
    static constexpr std::uint16_t packet_id = 0x2E;
    static constexpr std::string_view packet_name = "level_chunk_with_light";

    // attributes: #[fixed]
    std::int32_t x;
    // attributes: #[fixed]
    std::int32_t z;
    ::refrain::protocol::v777::wire::wire_clientbound_level_chunk_packet_data chunk_data;
    ::refrain::protocol::v777::wire::wire_light_data light_data;
};

struct level_event {
    static constexpr std::uint16_t packet_id = 0x2F;
    static constexpr std::string_view packet_name = "level_event";

    // attributes: #[fixed]
    std::int32_t type_;
    std::int64_t pos;
    // attributes: #[fixed]
    std::int32_t data;
    bool global_event;
};

struct level_particles {
    static constexpr std::uint16_t packet_id = 0x30;
    static constexpr std::string_view packet_name = "level_particles";

    ::refrain::net::particle_options particle;
    bool override_limiter;
    bool always_show;
    double x;
    double y;
    double z;
    float x_dist;
    float y_dist;
    float z_dist;
    float x_max_speed;
    float y_max_speed;
    float z_max_speed;
    // attributes: #[varint]
    std::int32_t count;
    std::uint32_t randomization_type;
};

struct light_update {
    static constexpr std::uint16_t packet_id = 0x31;
    static constexpr std::string_view packet_name = "light_update";

    // attributes: #[varint]
    std::int32_t x;
    // attributes: #[varint]
    std::int32_t z;
    ::refrain::protocol::v777::wire::wire_light_data light_data;
};

struct low_disk_space_warning {
    static constexpr std::uint16_t packet_id = 0x33;
    static constexpr std::string_view packet_name = "low_disk_space_warning";

};

struct map_item_data {
    static constexpr std::uint16_t packet_id = 0x34;
    static constexpr std::string_view packet_name = "map_item_data";

    // attributes: #[varint]
    std::int32_t map_id;
    // attributes: #[fixed]
    std::int8_t scale;
    bool locked;
    std::optional<std::vector<::refrain::protocol::v777::wire::wire_map_decoration>> decorations;
    ::refrain::net::map_patch color_patch;
};

struct merchant_offers {
    static constexpr std::uint16_t packet_id = 0x35;
    static constexpr std::string_view packet_name = "merchant_offers";

    // attributes: #[varint]
    std::int32_t container_id;
    std::vector<::refrain::net::merchant_offer> offers;
    // attributes: #[varint]
    std::int32_t villager_level;
    // attributes: #[varint]
    std::int32_t villager_xp;
    bool show_progress;
    bool can_restock;
};

struct move_entity_pos {
    static constexpr std::uint16_t packet_id = 0x36;
    static constexpr std::string_view packet_name = "move_entity_pos";

    std::uint32_t entity_id;
    ::refrain::net::vec_delta delta;
};

struct move_entity_pos_rot {
    static constexpr std::uint16_t packet_id = 0x37;
    static constexpr std::string_view packet_name = "move_entity_pos_rot";

    std::uint32_t entity_id;
    ::refrain::net::vec_delta delta;
    // attributes: #[fixed]
    std::int8_t y_rot;
    // attributes: #[fixed]
    std::int8_t x_rot;
};

struct move_minecart_along_track {
    static constexpr std::uint16_t packet_id = 0x38;
    static constexpr std::string_view packet_name = "move_minecart_along_track";

    // attributes: #[varint]
    std::int32_t entity_id;
    std::vector<::refrain::protocol::v777::wire::wire_lerp_steps> lerp_steps;
};

struct move_entity_rot {
    static constexpr std::uint16_t packet_id = 0x39;
    static constexpr std::string_view packet_name = "move_entity_rot";

    std::uint32_t entity_id;
    bool on_ground;
    // attributes: #[fixed]
    std::int8_t y_rot;
    // attributes: #[fixed]
    std::int8_t x_rot;
};

struct move_vehicle {
    static constexpr std::uint16_t packet_id = 0x3A;
    static constexpr std::string_view packet_name = "move_vehicle";

    ::refrain::protocol::v777::wire::wire_position_and_rotation payload;
};

struct open_book {
    static constexpr std::uint16_t packet_id = 0x3B;
    static constexpr std::string_view packet_name = "open_book";

    std::uint32_t hand;
};

struct open_screen {
    static constexpr std::uint16_t packet_id = 0x3C;
    static constexpr std::string_view packet_name = "open_screen";

    // attributes: #[varint]
    std::int32_t get_container_id;
    std::uint32_t get_type;
    ::refrain::net::text_component get_title;
};

struct open_sign_editor {
    static constexpr std::uint16_t packet_id = 0x3D;
    static constexpr std::string_view packet_name = "open_sign_editor";

    std::int64_t pos;
    std::uint32_t slot;
};

struct ping {
    static constexpr std::uint16_t packet_id = 0x3E;
    static constexpr std::string_view packet_name = "ping";

    // attributes: #[fixed]
    std::int32_t id;
};

struct pong_response {
    static constexpr std::uint16_t packet_id = 0x3F;
    static constexpr std::string_view packet_name = "pong_response";

    std::int64_t time;
};

struct place_ghost_recipe {
    static constexpr std::uint16_t packet_id = 0x40;
    static constexpr std::string_view packet_name = "place_ghost_recipe";

    // attributes: #[varint]
    std::int32_t container_id;
    ::refrain::net::recipe_display recipe_display;
};

struct player_chat {
    static constexpr std::uint16_t packet_id = 0x42;
    static constexpr std::string_view packet_name = "player_chat";

    // attributes: #[varint]
    std::int32_t global_index;
    ::refrain::net::uuid sender;
    // attributes: #[varint]
    std::int32_t index;
    std::optional<::refrain::protocol::v777::wire::wire_message_signature> signature;
    ::refrain::protocol::v777::wire::wire_body body;
    std::optional<::refrain::net::text_component> unsigned_content;
    ::refrain::net::filter_mask filter_mask;
    ::refrain::protocol::v777::wire::wire_bound chat_type;
};

struct player_combat_end {
    static constexpr std::uint16_t packet_id = 0x43;
    static constexpr std::string_view packet_name = "player_combat_end";

    // attributes: #[varint]
    std::int32_t duration;
};

struct player_combat_enter {
    static constexpr std::uint16_t packet_id = 0x44;
    static constexpr std::string_view packet_name = "player_combat_enter";

};

struct player_combat_kill {
    static constexpr std::uint16_t packet_id = 0x45;
    static constexpr std::string_view packet_name = "player_combat_kill";

    // attributes: #[varint]
    std::int32_t player_id;
    ::refrain::net::text_component message;
};

struct player_info_remove {
    static constexpr std::uint16_t packet_id = 0x46;
    static constexpr std::string_view packet_name = "player_info_remove";

    std::vector<::refrain::net::uuid> profile_ids;
};

struct player_info_update {
    static constexpr std::uint16_t packet_id = 0x47;
    static constexpr std::string_view packet_name = "player_info_update";

    ::refrain::net::player_info_update payload;
};

struct player_look_at {
    static constexpr std::uint16_t packet_id = 0x48;
    static constexpr std::string_view packet_name = "player_look_at";

    std::uint32_t from_anchor;
    double x;
    double y;
    double z;
    std::optional<::refrain::protocol::v777::wire::wire_at_entity_payload> at_entity;
};

struct player_rotation {
    static constexpr std::uint16_t packet_id = 0x4A;
    static constexpr std::string_view packet_name = "player_rotation";

    float y_rot;
    bool relative_y;
    float x_rot;
    bool relative_x;
};

struct recipe_book_add {
    static constexpr std::uint16_t packet_id = 0x4B;
    static constexpr std::string_view packet_name = "recipe_book_add";

    std::vector<::refrain::protocol::v777::wire::wire_entry_2> entries;
    bool replace;
};

struct recipe_book_remove {
    static constexpr std::uint16_t packet_id = 0x4C;
    static constexpr std::string_view packet_name = "recipe_book_remove";

    std::vector<::refrain::protocol::v777::wire::wire_recipe_display_id> recipes;
};

struct recipe_book_settings {
    static constexpr std::uint16_t packet_id = 0x4D;
    static constexpr std::string_view packet_name = "recipe_book_settings";

    ::refrain::protocol::v777::wire::wire_book_settings book_settings;
};

struct remove_entities {
    static constexpr std::uint16_t packet_id = 0x4E;
    static constexpr std::string_view packet_name = "remove_entities";

    std::vector<std::uint32_t> entity_ids;
};

struct remove_mob_effect {
    static constexpr std::uint16_t packet_id = 0x4F;
    static constexpr std::string_view packet_name = "remove_mob_effect";

    // attributes: #[varint]
    std::int32_t entity_id;
    std::uint32_t effect;
};

struct reset_score {
    static constexpr std::uint16_t packet_id = 0x50;
    static constexpr std::string_view packet_name = "reset_score";

    // attributes: #[max(32767)]
    std::string owner;
    std::optional<std::string> objective_name;
};

struct resource_pack_pop {
    static constexpr std::uint16_t packet_id = 0x51;
    static constexpr std::string_view packet_name = "resource_pack_pop";

    std::optional<::refrain::net::uuid> id;
};

struct resource_pack_push {
    static constexpr std::uint16_t packet_id = 0x52;
    static constexpr std::string_view packet_name = "resource_pack_push";

    ::refrain::net::uuid id;
    // attributes: #[max(32767)]
    std::string url;
    // attributes: #[max(40)]
    std::string hash;
    bool required;
    std::optional<::refrain::net::text_component> prompt;
};

struct post_effects {
    static constexpr std::uint16_t packet_id = 0x53;
    static constexpr std::string_view packet_name = "post_effects";

    std::vector<::refrain::data::resource_location> post_effects;
};

struct respawn {
    static constexpr std::uint16_t packet_id = 0x54;
    static constexpr std::string_view packet_name = "respawn";

    ::refrain::protocol::v777::wire::wire_common_player_spawn_info common_player_spawn_info;
    // attributes: #[fixed]
    std::int8_t data_to_keep;
};

struct rotate_head {
    static constexpr std::uint16_t packet_id = 0x55;
    static constexpr std::string_view packet_name = "rotate_head";

    // attributes: #[varint]
    std::int32_t entity_id;
    // attributes: #[fixed]
    std::int8_t y_head_rot;
};

struct section_blocks_update {
    static constexpr std::uint16_t packet_id = 0x56;
    static constexpr std::string_view packet_name = "section_blocks_update";

    std::int64_t section_pos;
    // attributes: #[varlong]
    std::vector<std::int64_t> positions;
};

struct select_advancements_tab {
    static constexpr std::uint16_t packet_id = 0x57;
    static constexpr std::string_view packet_name = "select_advancements_tab";

    std::optional<::refrain::data::resource_location> tab;
};

struct server_data {
    static constexpr std::uint16_t packet_id = 0x58;
    static constexpr std::string_view packet_name = "server_data";

    ::refrain::net::text_component motd;
    std::optional<std::vector<std::byte>> icon_bytes;
};

struct set_action_bar_text {
    static constexpr std::uint16_t packet_id = 0x59;
    static constexpr std::string_view packet_name = "set_action_bar_text";

    ::refrain::net::text_component text;
};

struct set_border_center {
    static constexpr std::uint16_t packet_id = 0x5A;
    static constexpr std::string_view packet_name = "set_border_center";

    double new_center_x;
    double new_center_z;
};

struct set_border_lerp_size {
    static constexpr std::uint16_t packet_id = 0x5B;
    static constexpr std::string_view packet_name = "set_border_lerp_size";

    double old_size;
    double new_size;
    // attributes: #[varlong]
    std::int64_t lerp_time;
};

struct set_border_size {
    static constexpr std::uint16_t packet_id = 0x5C;
    static constexpr std::string_view packet_name = "set_border_size";

    double size;
};

struct set_border_warning_delay {
    static constexpr std::uint16_t packet_id = 0x5D;
    static constexpr std::string_view packet_name = "set_border_warning_delay";

    // attributes: #[varint]
    std::int32_t warning_delay;
};

struct set_border_warning_distance {
    static constexpr std::uint16_t packet_id = 0x5E;
    static constexpr std::string_view packet_name = "set_border_warning_distance";

    // attributes: #[varint]
    std::int32_t warning_blocks;
};

struct set_camera {
    static constexpr std::uint16_t packet_id = 0x5F;
    static constexpr std::string_view packet_name = "set_camera";

    // attributes: #[varint]
    std::int32_t camera_id;
};

struct set_cursor_item {
    static constexpr std::uint16_t packet_id = 0x62;
    static constexpr std::string_view packet_name = "set_cursor_item";

    ::refrain::net::item_stack contents;
};

struct set_display_objective {
    static constexpr std::uint16_t packet_id = 0x64;
    static constexpr std::string_view packet_name = "set_display_objective";

    std::uint32_t slot;
    // attributes: #[max(32767)]
    std::string objective_name;
};

struct set_entity_data {
    static constexpr std::uint16_t packet_id = 0x65;
    static constexpr std::string_view packet_name = "set_entity_data";

    std::uint32_t entity_id;
    ::refrain::net::entity_metadata values;
};

struct set_entity_link {
    static constexpr std::uint16_t packet_id = 0x66;
    static constexpr std::string_view packet_name = "set_entity_link";

    // attributes: #[fixed]
    std::int32_t source_id;
    // attributes: #[fixed]
    std::int32_t dest_id;
};

struct set_entity_motion {
    static constexpr std::uint16_t packet_id = 0x67;
    static constexpr std::string_view packet_name = "set_entity_motion";

    // attributes: #[varint]
    std::int32_t id;
    ::refrain::net::lp_vec3 movement;
};

struct set_equipment {
    static constexpr std::uint16_t packet_id = 0x68;
    static constexpr std::string_view packet_name = "set_equipment";

    std::vector<::refrain::protocol::v777::wire::wire_entries_entry> entries;
};

struct set_experience {
    static constexpr std::uint16_t packet_id = 0x69;
    static constexpr std::string_view packet_name = "set_experience";

    float experience_progress;
    // attributes: #[varint]
    std::int32_t experience_level;
    // attributes: #[varint]
    std::int32_t total_experience;
};

struct set_health {
    static constexpr std::uint16_t packet_id = 0x6A;
    static constexpr std::string_view packet_name = "set_health";

    float health;
    // attributes: #[varint]
    std::int32_t food;
    float saturation;
};

struct set_objective {
    static constexpr std::uint16_t packet_id = 0x6C;
    static constexpr std::string_view packet_name = "set_objective";

    ::refrain::net::objective_update payload;
};

struct set_passengers {
    static constexpr std::uint16_t packet_id = 0x6D;
    static constexpr std::string_view packet_name = "set_passengers";

    // attributes: #[varint]
    std::int32_t vehicle;
    // attributes: #[varint]
    std::vector<std::int32_t> passengers;
};

struct set_player_inventory {
    static constexpr std::uint16_t packet_id = 0x6E;
    static constexpr std::string_view packet_name = "set_player_inventory";

    // attributes: #[varint]
    std::int32_t slot;
    ::refrain::net::item_stack contents;
};

struct set_player_team {
    static constexpr std::uint16_t packet_id = 0x6F;
    static constexpr std::string_view packet_name = "set_player_team";

    ::refrain::net::team_update payload;
};

struct set_score {
    static constexpr std::uint16_t packet_id = 0x70;
    static constexpr std::string_view packet_name = "set_score";

    // attributes: #[max(32767)]
    std::string owner;
    // attributes: #[max(32767)]
    std::string objective_name;
    // attributes: #[varint]
    std::int32_t score;
    std::optional<::refrain::net::text_component> display;
    std::optional<::refrain::net::nbt_blob> number_format;
};

struct set_simulation_distance {
    static constexpr std::uint16_t packet_id = 0x71;
    static constexpr std::string_view packet_name = "set_simulation_distance";

    // attributes: #[varint]
    std::int32_t simulation_distance;
};

struct set_subtitle_text {
    static constexpr std::uint16_t packet_id = 0x72;
    static constexpr std::string_view packet_name = "set_subtitle_text";

    ::refrain::net::text_component text;
};

struct set_title_text {
    static constexpr std::uint16_t packet_id = 0x74;
    static constexpr std::string_view packet_name = "set_title_text";

    ::refrain::net::text_component text;
};

struct set_titles_animation {
    static constexpr std::uint16_t packet_id = 0x75;
    static constexpr std::string_view packet_name = "set_titles_animation";

    // attributes: #[fixed]
    std::int32_t fade_in;
    // attributes: #[fixed]
    std::int32_t stay;
    // attributes: #[fixed]
    std::int32_t fade_out;
};

struct sound_entity {
    static constexpr std::uint16_t packet_id = 0x76;
    static constexpr std::string_view packet_name = "sound_entity";

    std::variant<std::uint32_t, ::refrain::protocol::v777::wire::wire_sound_event> sound;
    std::uint32_t source;
    // attributes: #[varint]
    std::int32_t id;
    float volume;
    float pitch;
    std::int64_t seed;
};

struct sound {
    static constexpr std::uint16_t packet_id = 0x77;
    static constexpr std::string_view packet_name = "sound";

    std::variant<std::uint32_t, ::refrain::protocol::v777::wire::wire_sound_event> sound;
    std::uint32_t source;
    // attributes: #[fixed]
    std::int32_t x;
    // attributes: #[fixed]
    std::int32_t y;
    // attributes: #[fixed]
    std::int32_t z;
    float volume;
    float pitch;
    std::int64_t seed;
};

struct start_configuration {
    static constexpr std::uint16_t packet_id = 0x78;
    static constexpr std::string_view packet_name = "start_configuration";

};

struct stop_sound {
    static constexpr std::uint16_t packet_id = 0x79;
    static constexpr std::string_view packet_name = "stop_sound";

    ::refrain::net::sound_selector payload;
};

struct store_cookie {
    static constexpr std::uint16_t packet_id = 0x7A;
    static constexpr std::string_view packet_name = "store_cookie";

    ::refrain::data::resource_location key;
    // attributes: #[max(5120)]
    std::vector<std::byte> payload;
};

struct swing_animation {
    static constexpr std::uint16_t packet_id = 0x7B;
    static constexpr std::string_view packet_name = "swing_animation";

    // attributes: #[varint]
    std::int32_t entity_id;
    std::uint32_t hand;
    ::refrain::protocol::v777::wire::wire_swing_animation animation;
};

struct system_chat {
    static constexpr std::uint16_t packet_id = 0x7C;
    static constexpr std::string_view packet_name = "system_chat";

    ::refrain::net::text_component content;
    bool overlay;
};

struct tab_list {
    static constexpr std::uint16_t packet_id = 0x7D;
    static constexpr std::string_view packet_name = "tab_list";

    ::refrain::net::text_component header;
    ::refrain::net::text_component footer;
};

struct tag_query {
    static constexpr std::uint16_t packet_id = 0x7E;
    static constexpr std::string_view packet_name = "tag_query";

    // attributes: #[varint]
    std::int32_t transaction_id;
    ::refrain::net::nbt_blob tag;
};

struct take_item_entity {
    static constexpr std::uint16_t packet_id = 0x7F;
    static constexpr std::string_view packet_name = "take_item_entity";

    // attributes: #[varint]
    std::int32_t item_id;
    // attributes: #[varint]
    std::int32_t player_id;
    // attributes: #[varint]
    std::int32_t amount;
};

struct teleport_entity {
    static constexpr std::uint16_t packet_id = 0x80;
    static constexpr std::string_view packet_name = "teleport_entity";

    // attributes: #[varint]
    std::int32_t id;
    ::refrain::protocol::v777::wire::wire_position_move_rotation change;
    // attributes: #[fixed]
    std::int32_t relatives;
    bool on_ground;
};

struct test_instance_block_status {
    static constexpr std::uint16_t packet_id = 0x81;
    static constexpr std::string_view packet_name = "test_instance_block_status";

    ::refrain::net::text_component status;
    std::optional<::refrain::protocol::v777::wire::wire_vec3i> size;
};

struct ticking_state {
    static constexpr std::uint16_t packet_id = 0x82;
    static constexpr std::string_view packet_name = "ticking_state";

    float tick_rate;
    bool is_frozen;
};

struct ticking_step {
    static constexpr std::uint16_t packet_id = 0x83;
    static constexpr std::string_view packet_name = "ticking_step";

    // attributes: #[varint]
    std::int32_t tick_steps;
};

struct transfer {
    static constexpr std::uint16_t packet_id = 0x84;
    static constexpr std::string_view packet_name = "transfer";

    // attributes: #[max(32767)]
    std::string host;
    // attributes: #[varint]
    std::int32_t port;
};

struct update_advancements {
    static constexpr std::uint16_t packet_id = 0x85;
    static constexpr std::string_view packet_name = "update_advancements";

    bool should_reset;
    std::vector<::refrain::protocol::v777::wire::wire_added> added;
    std::vector<::refrain::data::resource_location> removed;
    std::map<::refrain::data::resource_location, ::refrain::net::advancement_progress> progress;
    bool show_advancements;
};

struct update_attributes {
    static constexpr std::uint16_t packet_id = 0x86;
    static constexpr std::string_view packet_name = "update_attributes";

    // attributes: #[varint]
    std::int32_t get_entity_id;
    std::vector<::refrain::protocol::v777::wire::wire_get_values> get_values;
};

struct update_mob_effect {
    static constexpr std::uint16_t packet_id = 0x87;
    static constexpr std::string_view packet_name = "update_mob_effect";

    // attributes: #[varint]
    std::int32_t entity_id;
    std::uint32_t effect;
    // attributes: #[varint]
    std::int32_t effect_amplifier;
    // attributes: #[varint]
    std::int32_t effect_duration_ticks;
    // attributes: #[fixed]
    std::int8_t flags_;
};

struct update_recipes {
    static constexpr std::uint16_t packet_id = 0x88;
    static constexpr std::string_view packet_name = "update_recipes";

    std::map<::refrain::data::resource_location, std::vector<std::uint32_t>> item_sets;
    ::refrain::net::single_input_set stonecutter_recipes;
};

struct update_tags {
    static constexpr std::uint16_t packet_id = 0x89;
    static constexpr std::string_view packet_name = "update_tags";

    std::map<::refrain::data::resource_location, ::refrain::protocol::v777::wire::wire_network_payload> tags;
};

struct projectile_power {
    static constexpr std::uint16_t packet_id = 0x8A;
    static constexpr std::string_view packet_name = "projectile_power";

    // attributes: #[varint]
    std::int32_t id;
    double acceleration_power;
};

struct custom_report_details {
    static constexpr std::uint16_t packet_id = 0x8B;
    static constexpr std::string_view packet_name = "custom_report_details";

    std::map<std::string, std::string> details;
};

struct server_links {
    static constexpr std::uint16_t packet_id = 0x8C;
    static constexpr std::string_view packet_name = "server_links";

    std::vector<::refrain::protocol::v777::wire::wire_untrusted_entry> links;
};

struct waypoint {
    static constexpr std::uint16_t packet_id = 0x8D;
    static constexpr std::string_view packet_name = "waypoint";

    ::refrain::net::tracked_waypoint payload;
};

struct clear_dialog {
    static constexpr std::uint16_t packet_id = 0x8E;
    static constexpr std::string_view packet_name = "clear_dialog";

};

struct show_dialog {
    static constexpr std::uint16_t packet_id = 0x8F;
    static constexpr std::string_view packet_name = "show_dialog";

    std::variant<std::uint32_t, ::refrain::net::nbt_blob> dialog;
};
[[nodiscard]] inline auto encode_global_pos(::refrain::net::writer &out, const global_pos &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_identifier(value.dimension); !written) {
      return written;
    }
  }
  {
    out.write_i64(static_cast<std::int64_t>(value.position));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_global_pos(::refrain::net::reader &in, global_pos &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_identifier();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.dimension = std::move(*value_0);
  }
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.position = static_cast<decltype(value.position)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_spawn_info(::refrain::net::writer &out, const spawn_info &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.dimension_type));
  }
  {
    if (auto written = out.write_identifier(value.dimension); !written) {
      return written;
    }
  }
  {
    out.write_i64(static_cast<std::int64_t>(value.seed));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.game_type));
  }
  {
    out.write_varint(value.previous_game_type.has_value() ? static_cast<std::uint32_t>(value.previous_game_type.value()) + 1U : 0U);
  }
  {
    out.write_bool(value.debug);
  }
  {
    out.write_bool(value.flat);
  }
  {
    out.write_bool(value.last_death_location.has_value());
    if (value.last_death_location.has_value()) {
      if (auto written = ::refrain::protocol::v777::play::clientbound::encode_global_pos(out, value.last_death_location.value()); !written) {
        return written;
      }
    }
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.portal_cooldown));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.sea_level));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_spawn_info(::refrain::net::reader &in, spawn_info &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.dimension_type = static_cast<decltype(value.dimension_type)>(*value_0);
  }
  {
    auto value_0 = in.read_identifier();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.dimension = std::move(*value_0);
  }
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.seed = static_cast<decltype(value.seed)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.game_type = static_cast<decltype(value.game_type)>(*value_0);
  }
  {
    auto present_0 = in.read_varint();
    if (!present_0) return std::unexpected{std::move(present_0.error())};
    if (*present_0 != 0) {
      value.previous_game_type = static_cast<std::uint32_t>(*present_0 - 1U);
    } else {
      value.previous_game_type.reset();
    }
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.debug = *value_0;
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.flat = *value_0;
  }
  {
    auto present_0 = in.read_bool();
    if (!present_0) return std::unexpected{std::move(present_0.error())};
    if (*present_0) {
      ::refrain::protocol::v777::play::clientbound::global_pos value_0{};
      if (auto decoded = ::refrain::protocol::v777::play::clientbound::decode_global_pos(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.last_death_location = std::move(value_0);
    } else {
      value.last_death_location.reset();
    }
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.portal_cooldown = static_cast<decltype(value.portal_cooldown)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.sea_level = static_cast<decltype(value.sea_level)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_clock_update(::refrain::net::writer &out, const clock_update &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_identifier(value.clock); !written) {
      return written;
    }
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.state));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_clock_update(::refrain::net::reader &in, clock_update &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_identifier();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.clock = std::move(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.state = static_cast<decltype(value.state)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_block_changed_ack(::refrain::net::writer &out, const block_changed_ack &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.sequence));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_block_changed_ack(::refrain::net::reader &in, block_changed_ack &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.sequence = static_cast<decltype(value.sequence)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_block_update(::refrain::net::writer &out, const block_update &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i64(static_cast<std::int64_t>(value.position));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.state));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_block_update(::refrain::net::reader &in, block_update &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.position = static_cast<decltype(value.position)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.state = static_cast<decltype(value.state)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_change_difficulty(::refrain::net::writer &out, const change_difficulty &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.difficulty));
  }
  {
    out.write_bool(value.locked);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_change_difficulty(::refrain::net::reader &in, change_difficulty &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.difficulty = static_cast<decltype(value.difficulty)>(*value_0);
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.locked = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_chunk_batch_finished(::refrain::net::writer &out, const chunk_batch_finished &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.batch_size));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_chunk_batch_finished(::refrain::net::reader &in, chunk_batch_finished &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.batch_size = static_cast<decltype(value.batch_size)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_chunk_batch_start(::refrain::net::writer &out, const chunk_batch_start &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_chunk_batch_start(::refrain::net::reader &in, chunk_batch_start &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_entity_event(::refrain::net::writer &out, const entity_event &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i32(static_cast<std::int32_t>(value.entity_id));
  }
  {
    out.write_u8(value.event);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_entity_event(::refrain::net::reader &in, entity_event &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.entity_id = static_cast<decltype(value.entity_id)>(*value_0);
  }
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.event = static_cast<decltype(value.event)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_game_event(::refrain::net::writer &out, const game_event &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_u8(value.event);
  }
  {
    out.write_f32(value.value);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_game_event(::refrain::net::reader &in, game_event &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.event = static_cast<decltype(value.event)>(*value_0);
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.value = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_initialize_border(::refrain::net::writer &out, const initialize_border &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_f64(value.center_x);
  }
  {
    out.write_f64(value.center_z);
  }
  {
    out.write_f64(value.old_size);
  }
  {
    out.write_f64(value.new_size);
  }
  {
    out.write_varlong(static_cast<std::uint64_t>(value.lerp_time));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.absolute_max_size));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.warning_blocks));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.warning_time));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_initialize_border(::refrain::net::reader &in, initialize_border &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.center_x = *value_0;
  }
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.center_z = *value_0;
  }
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.old_size = *value_0;
  }
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.new_size = *value_0;
  }
  {
    auto value_0 = in.read_varlong();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.lerp_time = static_cast<decltype(value.lerp_time)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.absolute_max_size = static_cast<decltype(value.absolute_max_size)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.warning_blocks = static_cast<decltype(value.warning_blocks)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.warning_time = static_cast<decltype(value.warning_time)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_keep_alive(::refrain::net::writer &out, const keep_alive &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i64(static_cast<std::int64_t>(value.id));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_keep_alive(::refrain::net::reader &in, keep_alive &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.id = static_cast<decltype(value.id)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_login(::refrain::net::writer &out, const login &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i32(static_cast<std::int32_t>(value.player_id));
  }
  {
    out.write_bool(value.hardcore);
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.levels.size()));
    for (const auto &item_0 : value.levels) {
      if (auto written = out.write_identifier(item_0); !written) {
        return written;
      }
    }
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.max_players));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.chunk_radius));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.simulation_distance));
  }
  {
    out.write_bool(value.reduced_debug_info);
  }
  {
    out.write_bool(value.show_death_screen);
  }
  {
    out.write_bool(value.limited_crafting);
  }
  {
    if (auto written = ::refrain::protocol::v777::play::clientbound::encode_spawn_info(out, value.spawn); !written) {
      return written;
    }
  }
  {
    out.write_bool(value.online_mode);
  }
  {
    out.write_bool(value.enforces_secure_chat);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_login(::refrain::net::reader &in, login &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.player_id = static_cast<decltype(value.player_id)>(*value_0);
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.hardcore = *value_0;
  }
  {
    value.levels.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::data::resource_location value_0{};
      auto value_1 = in.read_identifier();
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      value_0 = std::move(*value_1);
      value.levels.push_back(std::move(value_0));
    }
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.max_players = static_cast<decltype(value.max_players)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.chunk_radius = static_cast<decltype(value.chunk_radius)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.simulation_distance = static_cast<decltype(value.simulation_distance)>(*value_0);
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.reduced_debug_info = *value_0;
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.show_death_screen = *value_0;
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.limited_crafting = *value_0;
  }
  {
    if (auto decoded = ::refrain::protocol::v777::play::clientbound::decode_spawn_info(in, value.spawn); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.online_mode = *value_0;
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.enforces_secure_chat = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_player_abilities(::refrain::net::writer &out, const player_abilities &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_u8(value.flags);
  }
  {
    out.write_f32(value.flying_speed);
  }
  {
    out.write_f32(value.walking_speed);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_player_abilities(::refrain::net::reader &in, player_abilities &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.flags = static_cast<decltype(value.flags)>(*value_0);
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.flying_speed = *value_0;
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.walking_speed = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_player_position(::refrain::net::writer &out, const player_position &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.id));
  }
  {
    out.write_f64(value.x);
  }
  {
    out.write_f64(value.y);
  }
  {
    out.write_f64(value.z);
  }
  {
    out.write_f64(value.delta_x);
  }
  {
    out.write_f64(value.delta_y);
  }
  {
    out.write_f64(value.delta_z);
  }
  {
    out.write_f32(value.y_rot);
  }
  {
    out.write_f32(value.x_rot);
  }
  {
    out.write_i32(static_cast<std::int32_t>(value.relatives));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_player_position(::refrain::net::reader &in, player_position &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.id = static_cast<decltype(value.id)>(*value_0);
  }
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.x = *value_0;
  }
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.y = *value_0;
  }
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.z = *value_0;
  }
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.delta_x = *value_0;
  }
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.delta_y = *value_0;
  }
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.delta_z = *value_0;
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.y_rot = *value_0;
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.x_rot = *value_0;
  }
  {
    auto value_0 = in.read_i32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.relatives = static_cast<decltype(value.relatives)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_set_default_spawn_position(::refrain::net::writer &out, const set_default_spawn_position &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_identifier(value.dimension); !written) {
      return written;
    }
  }
  {
    out.write_i64(static_cast<std::int64_t>(value.position));
  }
  {
    out.write_f32(value.yaw);
  }
  {
    out.write_f32(value.pitch);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_set_default_spawn_position(::refrain::net::reader &in, set_default_spawn_position &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_identifier();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.dimension = std::move(*value_0);
  }
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.position = static_cast<decltype(value.position)>(*value_0);
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.yaw = *value_0;
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.pitch = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_set_held_slot(::refrain::net::writer &out, const set_held_slot &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.slot));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_set_held_slot(::refrain::net::reader &in, set_held_slot &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.slot = static_cast<decltype(value.slot)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_set_chunk_cache_center(::refrain::net::writer &out, const set_chunk_cache_center &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.chunk_x));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.chunk_z));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_set_chunk_cache_center(::refrain::net::reader &in, set_chunk_cache_center &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.chunk_x = static_cast<decltype(value.chunk_x)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.chunk_z = static_cast<decltype(value.chunk_z)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_set_chunk_cache_radius(::refrain::net::writer &out, const set_chunk_cache_radius &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.radius));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_set_chunk_cache_radius(::refrain::net::reader &in, set_chunk_cache_radius &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.radius = static_cast<decltype(value.radius)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_set_time(::refrain::net::writer &out, const set_time &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i64(static_cast<std::int64_t>(value.game_time));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.clock_updates.size()));
    for (const auto &item_0 : value.clock_updates) {
      if (auto written = ::refrain::protocol::v777::play::clientbound::encode_clock_update(out, item_0); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_set_time(::refrain::net::reader &in, set_time &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.game_time = static_cast<decltype(value.game_time)>(*value_0);
  }
  {
    value.clock_updates.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::protocol::v777::play::clientbound::clock_update value_0{};
      if (auto decoded = ::refrain::protocol::v777::play::clientbound::decode_clock_update(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.clock_updates.push_back(std::move(value_0));
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_bundle_delimiter(::refrain::net::writer &out, const bundle_delimiter &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_bundle_delimiter(::refrain::net::reader &in, bundle_delimiter &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_add_entity(::refrain::net::writer &out, const add_entity &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.id));
  }
  {
    if (auto written = out.write_uuid(value.uuid); !written) {
      return written;
    }
  }
  {
    out.write_varint(value.type_);
  }
  {
    out.write_f64(value.x);
  }
  {
    out.write_f64(value.y);
  }
  {
    out.write_f64(value.z);
  }
  {
    if (auto written = ::refrain::net::encode_lp_vec3(out, value.movement); !written) {
      return written;
    }
  }
  {
    out.write_u8(static_cast<std::uint8_t>(value.x_rot));
  }
  {
    out.write_u8(static_cast<std::uint8_t>(value.y_rot));
  }
  {
    out.write_u8(static_cast<std::uint8_t>(value.y_head_rot));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.data));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_add_entity(::refrain::net::reader &in, add_entity &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.id = static_cast<decltype(value.id)>(*value_0);
  }
  {
    auto value_0 = in.read_uuid();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.uuid = *value_0;
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.type_ = *value_0;
  }
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.x = *value_0;
  }
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.y = *value_0;
  }
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.z = *value_0;
  }
  {
    if (auto decoded = ::refrain::net::decode_lp_vec3(in, value.movement); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.x_rot = static_cast<decltype(value.x_rot)>(*value_0);
  }
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.y_rot = static_cast<decltype(value.y_rot)>(*value_0);
  }
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.y_head_rot = static_cast<decltype(value.y_head_rot)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.data = static_cast<decltype(value.data)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_animate(::refrain::net::writer &out, const animate &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.id));
  }
  {
    out.write_u8(static_cast<std::uint8_t>(value.action));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_animate(::refrain::net::reader &in, animate &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.id = static_cast<decltype(value.id)>(*value_0);
  }
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.action = static_cast<decltype(value.action)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_award_stats(::refrain::net::writer &out, const award_stats &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.payload.size()));
    for (const auto &entry_0 : value.payload) {
      if (auto written = ::refrain::net::encode_stat_entry(out, entry_0.first); !written) {
        return written;
      }
      out.write_varint(entry_0.second);
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_award_stats(::refrain::net::reader &in, award_stats &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    value.payload.clear();
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::net::stat_entry map_key_0{};
      if (auto decoded = ::refrain::net::decode_stat_entry(in, map_key_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      std::uint32_t map_value_0{};
      auto value_2 = in.read_varint();
      if (!value_2) return std::unexpected{std::move(value_2.error())};
      map_value_0 = *value_2;
      value.payload.emplace(std::move(map_key_0), std::move(map_value_0));
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_block_destruction(::refrain::net::writer &out, const block_destruction &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.id));
  }
  {
    out.write_i64(static_cast<std::int64_t>(value.pos));
  }
  {
    out.write_u8(static_cast<std::uint8_t>(value.progress));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_block_destruction(::refrain::net::reader &in, block_destruction &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.id = static_cast<decltype(value.id)>(*value_0);
  }
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.pos = static_cast<decltype(value.pos)>(*value_0);
  }
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.progress = static_cast<decltype(value.progress)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_block_entity_data(::refrain::net::writer &out, const block_entity_data &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i64(static_cast<std::int64_t>(value.get_pos));
  }
  {
    out.write_varint(value.get_type);
  }
  {
    if (auto written = ::refrain::net::encode_nbt(out, value.get_tag); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_block_entity_data(::refrain::net::reader &in, block_entity_data &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.get_pos = static_cast<decltype(value.get_pos)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.get_type = *value_0;
  }
  {
    if (auto decoded = ::refrain::net::decode_nbt(in, value.get_tag); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_block_event(::refrain::net::writer &out, const block_event &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i64(static_cast<std::int64_t>(value.pos));
  }
  {
    out.write_u8(static_cast<std::uint8_t>(value.b0));
  }
  {
    out.write_u8(static_cast<std::uint8_t>(value.b1));
  }
  {
    out.write_varint(value.block);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_block_event(::refrain::net::reader &in, block_event &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.pos = static_cast<decltype(value.pos)>(*value_0);
  }
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.b0 = static_cast<decltype(value.b0)>(*value_0);
  }
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.b1 = static_cast<decltype(value.b1)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.block = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_boss_event(::refrain::net::writer &out, const boss_event &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_boss_event(::refrain::net::reader &in, boss_event &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_chunks_biomes(::refrain::net::writer &out, const chunks_biomes &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.chunk_biome_data.size()));
    for (const auto &item_0 : value.chunk_biome_data) {
      if (auto written = ::refrain::protocol::v777::wire::encode_wire_chunk_biome_data(out, item_0); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_chunks_biomes(::refrain::net::reader &in, chunks_biomes &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    value.chunk_biome_data.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::protocol::v777::wire::wire_chunk_biome_data value_0{};
      if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_chunk_biome_data(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.chunk_biome_data.push_back(std::move(value_0));
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_clear_titles(::refrain::net::writer &out, const clear_titles &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_bool(value.reset_times);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_clear_titles(::refrain::net::reader &in, clear_titles &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.reset_times = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_command_suggestions(::refrain::net::writer &out, const command_suggestions &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.id));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.start));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.length));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.suggestions.size()));
    for (const auto &item_0 : value.suggestions) {
      if (auto written = ::refrain::protocol::v777::wire::encode_wire_entry(out, item_0); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_command_suggestions(::refrain::net::reader &in, command_suggestions &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.id = static_cast<decltype(value.id)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.start = static_cast<decltype(value.start)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.length = static_cast<decltype(value.length)>(*value_0);
  }
  {
    value.suggestions.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::protocol::v777::wire::wire_entry value_0{};
      if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_entry(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.suggestions.push_back(std::move(value_0));
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_commands(::refrain::net::writer &out, const commands &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = ::refrain::net::encode_command_tree(out, value.tree); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_commands(::refrain::net::reader &in, commands &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    if (auto decoded = ::refrain::net::decode_command_tree(in, value.tree); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_container_close(::refrain::net::writer &out, const container_close &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.container_id));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_container_close(::refrain::net::reader &in, container_close &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.container_id = static_cast<decltype(value.container_id)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_container_set_content(::refrain::net::writer &out, const container_set_content &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.container_id));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.state_id));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.items.size()));
    for (const auto &item_0 : value.items) {
      if (auto written = ::refrain::net::encode_item_stack(out, item_0); !written) {
        return written;
      }
    }
  }
  {
    if (auto written = ::refrain::net::encode_item_stack(out, value.carried_item); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_container_set_content(::refrain::net::reader &in, container_set_content &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.container_id = static_cast<decltype(value.container_id)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.state_id = static_cast<decltype(value.state_id)>(*value_0);
  }
  {
    value.items.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::net::item_stack value_0{};
      if (auto decoded = ::refrain::net::decode_item_stack(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.items.push_back(std::move(value_0));
    }
  }
  {
    if (auto decoded = ::refrain::net::decode_item_stack(in, value.carried_item); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_container_set_data(::refrain::net::writer &out, const container_set_data &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.container_id));
  }
  {
    out.write_u16(static_cast<std::uint16_t>(value.id));
  }
  {
    out.write_u16(static_cast<std::uint16_t>(value.value));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_container_set_data(::refrain::net::reader &in, container_set_data &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.container_id = static_cast<decltype(value.container_id)>(*value_0);
  }
  {
    auto value_0 = in.read_u16();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.id = static_cast<decltype(value.id)>(*value_0);
  }
  {
    auto value_0 = in.read_u16();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.value = static_cast<decltype(value.value)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_container_set_slot(::refrain::net::writer &out, const container_set_slot &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.container_id));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.state_id));
  }
  {
    out.write_u16(static_cast<std::uint16_t>(value.slot));
  }
  {
    if (auto written = ::refrain::net::encode_item_stack(out, value.item_stack); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_container_set_slot(::refrain::net::reader &in, container_set_slot &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.container_id = static_cast<decltype(value.container_id)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.state_id = static_cast<decltype(value.state_id)>(*value_0);
  }
  {
    auto value_0 = in.read_u16();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.slot = static_cast<decltype(value.slot)>(*value_0);
  }
  {
    if (auto decoded = ::refrain::net::decode_item_stack(in, value.item_stack); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_cookie_request(::refrain::net::writer &out, const cookie_request &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_identifier(value.key); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_cookie_request(::refrain::net::reader &in, cookie_request &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_identifier();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.key = std::move(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_cooldown(::refrain::net::writer &out, const cooldown &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_identifier(value.cooldown_group); !written) {
      return written;
    }
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.duration));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_cooldown(::refrain::net::reader &in, cooldown &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_identifier();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.cooldown_group = std::move(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.duration = static_cast<decltype(value.duration)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_custom_chat_completions(::refrain::net::writer &out, const custom_chat_completions &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(value.action);
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.entries.size()));
    for (const auto &item_0 : value.entries) {
      if (auto written = out.write_string(item_0, 32767); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_custom_chat_completions(::refrain::net::reader &in, custom_chat_completions &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.action = *value_0;
  }
  {
    value.entries.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      std::string value_0{};
      auto value_1 = in.read_string(32767);
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      value_0 = std::move(*value_1);
      value.entries.push_back(std::move(value_0));
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_custom_payload(::refrain::net::writer &out, const custom_payload &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_identifier(value.channel); !written) {
      return written;
    }
  }
  {
    out.write_bytes(value.data);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_custom_payload(::refrain::net::reader &in, custom_payload &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_identifier();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.channel = std::move(*value_0);
  }
  {
    auto value_0 = in.read_bytes(in.remaining());
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.data = std::vector<std::byte>{value_0->begin(), value_0->end()};
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_damage_event(::refrain::net::writer &out, const damage_event &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.entity_id));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.source_type));
  }
  {
    out.write_varint(value.field_2.has_value() ? static_cast<std::uint32_t>(value.field_2.value()) + 1U : 0U);
  }
  {
    out.write_varint(value.field_3.has_value() ? static_cast<std::uint32_t>(value.field_3.value()) + 1U : 0U);
  }
  {
    out.write_bool(value.source_position.has_value());
    if (value.source_position.has_value()) {
      if (auto written = ::refrain::protocol::v777::wire::encode_wire_optional_payload(out, value.source_position.value()); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_damage_event(::refrain::net::reader &in, damage_event &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.entity_id = static_cast<decltype(value.entity_id)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.source_type = static_cast<decltype(value.source_type)>(*value_0);
  }
  {
    auto present_0 = in.read_varint();
    if (!present_0) return std::unexpected{std::move(present_0.error())};
    if (*present_0 != 0) {
      value.field_2 = static_cast<std::int32_t>(*present_0 - 1U);
    } else {
      value.field_2.reset();
    }
  }
  {
    auto present_0 = in.read_varint();
    if (!present_0) return std::unexpected{std::move(present_0.error())};
    if (*present_0 != 0) {
      value.field_3 = static_cast<std::int32_t>(*present_0 - 1U);
    } else {
      value.field_3.reset();
    }
  }
  {
    auto present_0 = in.read_bool();
    if (!present_0) return std::unexpected{std::move(present_0.error())};
    if (*present_0) {
      ::refrain::protocol::v777::wire::wire_optional_payload value_0{};
      if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_optional_payload(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.source_position = std::move(value_0);
    } else {
      value.source_position.reset();
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_debug_block_value(::refrain::net::writer &out, const debug_block_value &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i64(static_cast<std::int64_t>(value.block_pos));
  }
  {
    if (auto written = ::refrain::net::encode_debug_subscription_update(out, value.update); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_debug_block_value(::refrain::net::reader &in, debug_block_value &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.block_pos = static_cast<decltype(value.block_pos)>(*value_0);
  }
  {
    if (auto decoded = ::refrain::net::decode_debug_subscription_update(in, value.update); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_debug_chunk_value(::refrain::net::writer &out, const debug_chunk_value &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i64(static_cast<std::int64_t>(value.chunk_pos));
  }
  {
    if (auto written = ::refrain::net::encode_debug_subscription_update(out, value.update); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_debug_chunk_value(::refrain::net::reader &in, debug_chunk_value &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.chunk_pos = static_cast<decltype(value.chunk_pos)>(*value_0);
  }
  {
    if (auto decoded = ::refrain::net::decode_debug_subscription_update(in, value.update); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_debug_entity_value(::refrain::net::writer &out, const debug_entity_value &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.entity_id));
  }
  {
    if (auto written = ::refrain::net::encode_debug_subscription_update(out, value.update); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_debug_entity_value(::refrain::net::reader &in, debug_entity_value &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.entity_id = static_cast<decltype(value.entity_id)>(*value_0);
  }
  {
    if (auto decoded = ::refrain::net::decode_debug_subscription_update(in, value.update); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_debug_event(::refrain::net::writer &out, const debug_event &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = ::refrain::net::encode_debug_subscription_event(out, value.event_); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_debug_event(::refrain::net::reader &in, debug_event &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    if (auto decoded = ::refrain::net::decode_debug_subscription_event(in, value.event_); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_debug_sample(::refrain::net::writer &out, const debug_sample &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.sample.size()));
    for (const auto &item_0 : value.sample) {
      out.write_i64(static_cast<std::int64_t>(item_0));
    }
  }
  {
    out.write_varint(value.debug_sample_type);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_debug_sample(::refrain::net::reader &in, debug_sample &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    value.sample.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      std::int64_t value_0{};
      auto value_1 = in.read_i64();
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      value_0 = static_cast<decltype(value_0)>(*value_1);
      value.sample.push_back(std::move(value_0));
    }
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.debug_sample_type = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_delete_chat(::refrain::net::writer &out, const delete_chat &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = ::refrain::net::encode_message_signature_packed(out, value.message_signature); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_delete_chat(::refrain::net::reader &in, delete_chat &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    if (auto decoded = ::refrain::net::decode_message_signature_packed(in, value.message_signature); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_disconnect(::refrain::net::writer &out, const disconnect &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = ::refrain::net::encode_text_component(out, value.payload); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_disconnect(::refrain::net::reader &in, disconnect &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    if (auto decoded = ::refrain::net::decode_text_component(in, value.payload); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_disguised_chat(::refrain::net::writer &out, const disguised_chat &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = ::refrain::net::encode_text_component(out, value.message); !written) {
      return written;
    }
  }
  {
    if (auto written = ::refrain::protocol::v777::wire::encode_wire_bound(out, value.chat_type); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_disguised_chat(::refrain::net::reader &in, disguised_chat &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    if (auto decoded = ::refrain::net::decode_text_component(in, value.message); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  {
    if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_bound(in, value.chat_type); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_entity_position_sync(::refrain::net::writer &out, const entity_position_sync &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.id));
  }
  {
    if (auto written = ::refrain::net::encode_position_path(out, value.position); !written) {
      return written;
    }
  }
  {
    out.write_f32(value.y_rot);
  }
  {
    out.write_f32(value.x_rot);
  }
  {
    out.write_bool(value.on_ground);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_entity_position_sync(::refrain::net::reader &in, entity_position_sync &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.id = static_cast<decltype(value.id)>(*value_0);
  }
  {
    if (auto decoded = ::refrain::net::decode_position_path(in, value.position); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.y_rot = *value_0;
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.x_rot = *value_0;
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.on_ground = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_explode(::refrain::net::writer &out, const explode &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = ::refrain::protocol::v777::wire::encode_wire_vec3(out, value.center); !written) {
      return written;
    }
  }
  {
    out.write_f32(value.radius);
  }
  {
    out.write_i32(static_cast<std::int32_t>(value.block_count));
  }
  {
    out.write_bool(value.player_knockback.has_value());
    if (value.player_knockback.has_value()) {
      if (auto written = ::refrain::protocol::v777::wire::encode_wire_vec3(out, value.player_knockback.value()); !written) {
        return written;
      }
    }
  }
  {
    if (auto written = ::refrain::net::encode_particle_options(out, value.explosion_particle); !written) {
      return written;
    }
  }
  {
    if (std::holds_alternative<std::uint32_t>(value.explosion_sound)) {
      out.write_varint(std::get<0>(value.explosion_sound));
    } else {
      out.write_varint(0);
      if (auto written = ::refrain::protocol::v777::wire::encode_wire_sound_event(out, std::get<1>(value.explosion_sound)); !written) {
        return written;
      }
    }
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.block_particles.size()));
    for (const auto &item_0 : value.block_particles) {
      if (auto written = ::refrain::protocol::v777::wire::encode_wire_weighted_entry(out, item_0); !written) {
        return written;
      }
    }
  }
  {
    out.write_bool(value.play_sound);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_explode(::refrain::net::reader &in, explode &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_vec3(in, value.center); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.radius = *value_0;
  }
  {
    auto value_0 = in.read_i32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.block_count = static_cast<decltype(value.block_count)>(*value_0);
  }
  {
    auto present_0 = in.read_bool();
    if (!present_0) return std::unexpected{std::move(present_0.error())};
    if (*present_0) {
      ::refrain::protocol::v777::wire::wire_vec3 value_0{};
      if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_vec3(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.player_knockback = std::move(value_0);
    } else {
      value.player_knockback.reset();
    }
  }
  {
    if (auto decoded = ::refrain::net::decode_particle_options(in, value.explosion_particle); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  {
    auto holder_0 = in.read_varint();
    if (!holder_0) return std::unexpected{std::move(holder_0.error())};
    if (*holder_0 != 0) {
      value.explosion_sound.template emplace<0>(*holder_0);
    } else {
      ::refrain::protocol::v777::wire::wire_sound_event value_0{};
      if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_sound_event(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.explosion_sound.template emplace<1>(std::move(value_0));
    }
  }
  {
    value.block_particles.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::protocol::v777::wire::wire_weighted_entry value_0{};
      if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_weighted_entry(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.block_particles.push_back(std::move(value_0));
    }
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.play_sound = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_add_transient_block(::refrain::net::writer &out, const add_transient_block &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i64(static_cast<std::int64_t>(value.pos));
  }
  {
    out.write_varint(value.block_state);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_add_transient_block(::refrain::net::reader &in, add_transient_block &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.pos = static_cast<decltype(value.pos)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.block_state = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_forget_level_chunk(::refrain::net::writer &out, const forget_level_chunk &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i64(static_cast<std::int64_t>(value.pos));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_forget_level_chunk(::refrain::net::reader &in, forget_level_chunk &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.pos = static_cast<decltype(value.pos)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_game_rule_values(::refrain::net::writer &out, const game_rule_values &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.payload.size()));
    for (const auto &entry_0 : value.payload) {
      if (auto written = out.write_identifier(entry_0.first); !written) {
        return written;
      }
      if (auto written = out.write_string(entry_0.second, 32767); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_game_rule_values(::refrain::net::reader &in, game_rule_values &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    value.payload.clear();
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::data::resource_location map_key_0{};
      auto value_1 = in.read_identifier();
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      map_key_0 = std::move(*value_1);
      std::string map_value_0{};
      auto value_2 = in.read_string(32767);
      if (!value_2) return std::unexpected{std::move(value_2.error())};
      map_value_0 = std::move(*value_2);
      value.payload.emplace(std::move(map_key_0), std::move(map_value_0));
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_game_test_highlight_pos(::refrain::net::writer &out, const game_test_highlight_pos &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i64(static_cast<std::int64_t>(value.absolute_pos));
  }
  {
    out.write_i64(static_cast<std::int64_t>(value.relative_pos));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_game_test_highlight_pos(::refrain::net::reader &in, game_test_highlight_pos &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.absolute_pos = static_cast<decltype(value.absolute_pos)>(*value_0);
  }
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.relative_pos = static_cast<decltype(value.relative_pos)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_mount_screen_open(::refrain::net::writer &out, const mount_screen_open &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.container_id));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.inventory_columns));
  }
  {
    out.write_i32(static_cast<std::int32_t>(value.entity_id));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_mount_screen_open(::refrain::net::reader &in, mount_screen_open &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.container_id = static_cast<decltype(value.container_id)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.inventory_columns = static_cast<decltype(value.inventory_columns)>(*value_0);
  }
  {
    auto value_0 = in.read_i32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.entity_id = static_cast<decltype(value.entity_id)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_hurt_animation(::refrain::net::writer &out, const hurt_animation &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.id));
  }
  {
    out.write_f32(value.yaw);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_hurt_animation(::refrain::net::reader &in, hurt_animation &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.id = static_cast<decltype(value.id)>(*value_0);
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.yaw = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_level_chunk_with_light(::refrain::net::writer &out, const level_chunk_with_light &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i32(static_cast<std::int32_t>(value.x));
  }
  {
    out.write_i32(static_cast<std::int32_t>(value.z));
  }
  {
    if (auto written = ::refrain::protocol::v777::wire::encode_wire_clientbound_level_chunk_packet_data(out, value.chunk_data); !written) {
      return written;
    }
  }
  {
    if (auto written = ::refrain::protocol::v777::wire::encode_wire_light_data(out, value.light_data); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_level_chunk_with_light(::refrain::net::reader &in, level_chunk_with_light &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.x = static_cast<decltype(value.x)>(*value_0);
  }
  {
    auto value_0 = in.read_i32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.z = static_cast<decltype(value.z)>(*value_0);
  }
  {
    if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_clientbound_level_chunk_packet_data(in, value.chunk_data); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  {
    if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_light_data(in, value.light_data); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_level_event(::refrain::net::writer &out, const level_event &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i32(static_cast<std::int32_t>(value.type_));
  }
  {
    out.write_i64(static_cast<std::int64_t>(value.pos));
  }
  {
    out.write_i32(static_cast<std::int32_t>(value.data));
  }
  {
    out.write_bool(value.global_event);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_level_event(::refrain::net::reader &in, level_event &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.type_ = static_cast<decltype(value.type_)>(*value_0);
  }
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.pos = static_cast<decltype(value.pos)>(*value_0);
  }
  {
    auto value_0 = in.read_i32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.data = static_cast<decltype(value.data)>(*value_0);
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.global_event = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_level_particles(::refrain::net::writer &out, const level_particles &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = ::refrain::net::encode_particle_options(out, value.particle); !written) {
      return written;
    }
  }
  {
    out.write_bool(value.override_limiter);
  }
  {
    out.write_bool(value.always_show);
  }
  {
    out.write_f64(value.x);
  }
  {
    out.write_f64(value.y);
  }
  {
    out.write_f64(value.z);
  }
  {
    out.write_f32(value.x_dist);
  }
  {
    out.write_f32(value.y_dist);
  }
  {
    out.write_f32(value.z_dist);
  }
  {
    out.write_f32(value.x_max_speed);
  }
  {
    out.write_f32(value.y_max_speed);
  }
  {
    out.write_f32(value.z_max_speed);
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.count));
  }
  {
    out.write_varint(value.randomization_type);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_level_particles(::refrain::net::reader &in, level_particles &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    if (auto decoded = ::refrain::net::decode_particle_options(in, value.particle); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.override_limiter = *value_0;
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.always_show = *value_0;
  }
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.x = *value_0;
  }
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.y = *value_0;
  }
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.z = *value_0;
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.x_dist = *value_0;
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.y_dist = *value_0;
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.z_dist = *value_0;
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.x_max_speed = *value_0;
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.y_max_speed = *value_0;
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.z_max_speed = *value_0;
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.count = static_cast<decltype(value.count)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.randomization_type = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_light_update(::refrain::net::writer &out, const light_update &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.x));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.z));
  }
  {
    if (auto written = ::refrain::protocol::v777::wire::encode_wire_light_data(out, value.light_data); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_light_update(::refrain::net::reader &in, light_update &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.x = static_cast<decltype(value.x)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.z = static_cast<decltype(value.z)>(*value_0);
  }
  {
    if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_light_data(in, value.light_data); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_low_disk_space_warning(::refrain::net::writer &out, const low_disk_space_warning &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_low_disk_space_warning(::refrain::net::reader &in, low_disk_space_warning &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_map_item_data(::refrain::net::writer &out, const map_item_data &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.map_id));
  }
  {
    out.write_u8(static_cast<std::uint8_t>(value.scale));
  }
  {
    out.write_bool(value.locked);
  }
  {
    out.write_bool(value.decorations.has_value());
    if (value.decorations.has_value()) {
      out.write_varint(static_cast<std::uint32_t>(value.decorations.value().size()));
      for (const auto &item_1 : value.decorations.value()) {
        if (auto written = ::refrain::protocol::v777::wire::encode_wire_map_decoration(out, item_1); !written) {
          return written;
        }
      }
    }
  }
  {
    if (auto written = ::refrain::net::encode_map_patch(out, value.color_patch); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_map_item_data(::refrain::net::reader &in, map_item_data &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.map_id = static_cast<decltype(value.map_id)>(*value_0);
  }
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.scale = static_cast<decltype(value.scale)>(*value_0);
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.locked = *value_0;
  }
  {
    auto present_0 = in.read_bool();
    if (!present_0) return std::unexpected{std::move(present_0.error())};
    if (*present_0) {
      std::vector<::refrain::protocol::v777::wire::wire_map_decoration> value_0{};
      value_0.clear();
      auto count_1 = in.read_varint();
      if (!count_1) return std::unexpected{std::move(count_1.error())};
      for (std::uint32_t index_1 = 0; index_1 < *count_1; ++index_1) {
        ::refrain::protocol::v777::wire::wire_map_decoration value_1{};
        if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_map_decoration(in, value_1); !decoded) {
          return std::unexpected{std::move(decoded.error())};
        }
        value_0.push_back(std::move(value_1));
      }
      value.decorations = std::move(value_0);
    } else {
      value.decorations.reset();
    }
  }
  {
    if (auto decoded = ::refrain::net::decode_map_patch(in, value.color_patch); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_merchant_offers(::refrain::net::writer &out, const merchant_offers &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.container_id));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.offers.size()));
    for (const auto &item_0 : value.offers) {
      if (auto written = ::refrain::net::encode_merchant_offer(out, item_0); !written) {
        return written;
      }
    }
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.villager_level));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.villager_xp));
  }
  {
    out.write_bool(value.show_progress);
  }
  {
    out.write_bool(value.can_restock);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_merchant_offers(::refrain::net::reader &in, merchant_offers &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.container_id = static_cast<decltype(value.container_id)>(*value_0);
  }
  {
    value.offers.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::net::merchant_offer value_0{};
      if (auto decoded = ::refrain::net::decode_merchant_offer(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.offers.push_back(std::move(value_0));
    }
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.villager_level = static_cast<decltype(value.villager_level)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.villager_xp = static_cast<decltype(value.villager_xp)>(*value_0);
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.show_progress = *value_0;
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.can_restock = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_move_entity_pos(::refrain::net::writer &out, const move_entity_pos &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(value.entity_id);
  }
  {
    if (auto written = ::refrain::net::encode_vec_delta(out, value.delta); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_move_entity_pos(::refrain::net::reader &in, move_entity_pos &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.entity_id = *value_0;
  }
  {
    if (auto decoded = ::refrain::net::decode_vec_delta(in, value.delta); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_move_entity_pos_rot(::refrain::net::writer &out, const move_entity_pos_rot &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(value.entity_id);
  }
  {
    if (auto written = ::refrain::net::encode_vec_delta(out, value.delta); !written) {
      return written;
    }
  }
  {
    out.write_u8(static_cast<std::uint8_t>(value.y_rot));
  }
  {
    out.write_u8(static_cast<std::uint8_t>(value.x_rot));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_move_entity_pos_rot(::refrain::net::reader &in, move_entity_pos_rot &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.entity_id = *value_0;
  }
  {
    if (auto decoded = ::refrain::net::decode_vec_delta(in, value.delta); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.y_rot = static_cast<decltype(value.y_rot)>(*value_0);
  }
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.x_rot = static_cast<decltype(value.x_rot)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_move_minecart_along_track(::refrain::net::writer &out, const move_minecart_along_track &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.entity_id));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.lerp_steps.size()));
    for (const auto &item_0 : value.lerp_steps) {
      if (auto written = ::refrain::protocol::v777::wire::encode_wire_lerp_steps(out, item_0); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_move_minecart_along_track(::refrain::net::reader &in, move_minecart_along_track &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.entity_id = static_cast<decltype(value.entity_id)>(*value_0);
  }
  {
    value.lerp_steps.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::protocol::v777::wire::wire_lerp_steps value_0{};
      if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_lerp_steps(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.lerp_steps.push_back(std::move(value_0));
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_move_entity_rot(::refrain::net::writer &out, const move_entity_rot &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(value.entity_id);
  }
  {
    out.write_bool(value.on_ground);
  }
  {
    out.write_u8(static_cast<std::uint8_t>(value.y_rot));
  }
  {
    out.write_u8(static_cast<std::uint8_t>(value.x_rot));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_move_entity_rot(::refrain::net::reader &in, move_entity_rot &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.entity_id = *value_0;
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.on_ground = *value_0;
  }
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.y_rot = static_cast<decltype(value.y_rot)>(*value_0);
  }
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.x_rot = static_cast<decltype(value.x_rot)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_move_vehicle(::refrain::net::writer &out, const move_vehicle &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = ::refrain::protocol::v777::wire::encode_wire_position_and_rotation(out, value.payload); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_move_vehicle(::refrain::net::reader &in, move_vehicle &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_position_and_rotation(in, value.payload); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_open_book(::refrain::net::writer &out, const open_book &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(value.hand);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_open_book(::refrain::net::reader &in, open_book &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.hand = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_open_screen(::refrain::net::writer &out, const open_screen &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.get_container_id));
  }
  {
    out.write_varint(value.get_type);
  }
  {
    if (auto written = ::refrain::net::encode_text_component(out, value.get_title); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_open_screen(::refrain::net::reader &in, open_screen &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.get_container_id = static_cast<decltype(value.get_container_id)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.get_type = *value_0;
  }
  {
    if (auto decoded = ::refrain::net::decode_text_component(in, value.get_title); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_open_sign_editor(::refrain::net::writer &out, const open_sign_editor &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i64(static_cast<std::int64_t>(value.pos));
  }
  {
    out.write_varint(value.slot);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_open_sign_editor(::refrain::net::reader &in, open_sign_editor &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.pos = static_cast<decltype(value.pos)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.slot = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_ping(::refrain::net::writer &out, const ping &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i32(static_cast<std::int32_t>(value.id));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_ping(::refrain::net::reader &in, ping &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.id = static_cast<decltype(value.id)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_pong_response(::refrain::net::writer &out, const pong_response &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i64(static_cast<std::int64_t>(value.time));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_pong_response(::refrain::net::reader &in, pong_response &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.time = static_cast<decltype(value.time)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_place_ghost_recipe(::refrain::net::writer &out, const place_ghost_recipe &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.container_id));
  }
  {
    if (auto written = ::refrain::net::encode_recipe_display(out, value.recipe_display); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_place_ghost_recipe(::refrain::net::reader &in, place_ghost_recipe &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.container_id = static_cast<decltype(value.container_id)>(*value_0);
  }
  {
    if (auto decoded = ::refrain::net::decode_recipe_display(in, value.recipe_display); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_player_chat(::refrain::net::writer &out, const player_chat &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.global_index));
  }
  {
    if (auto written = out.write_uuid(value.sender); !written) {
      return written;
    }
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.index));
  }
  {
    out.write_bool(value.signature.has_value());
    if (value.signature.has_value()) {
      if (auto written = ::refrain::protocol::v777::wire::encode_wire_message_signature(out, value.signature.value()); !written) {
        return written;
      }
    }
  }
  {
    if (auto written = ::refrain::protocol::v777::wire::encode_wire_body(out, value.body); !written) {
      return written;
    }
  }
  {
    out.write_bool(value.unsigned_content.has_value());
    if (value.unsigned_content.has_value()) {
      if (auto written = ::refrain::net::encode_text_component(out, value.unsigned_content.value()); !written) {
        return written;
      }
    }
  }
  {
    if (auto written = ::refrain::net::encode_filter_mask(out, value.filter_mask); !written) {
      return written;
    }
  }
  {
    if (auto written = ::refrain::protocol::v777::wire::encode_wire_bound(out, value.chat_type); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_player_chat(::refrain::net::reader &in, player_chat &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.global_index = static_cast<decltype(value.global_index)>(*value_0);
  }
  {
    auto value_0 = in.read_uuid();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.sender = *value_0;
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.index = static_cast<decltype(value.index)>(*value_0);
  }
  {
    auto present_0 = in.read_bool();
    if (!present_0) return std::unexpected{std::move(present_0.error())};
    if (*present_0) {
      ::refrain::protocol::v777::wire::wire_message_signature value_0{};
      if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_message_signature(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.signature = std::move(value_0);
    } else {
      value.signature.reset();
    }
  }
  {
    if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_body(in, value.body); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  {
    auto present_0 = in.read_bool();
    if (!present_0) return std::unexpected{std::move(present_0.error())};
    if (*present_0) {
      ::refrain::net::text_component value_0{};
      if (auto decoded = ::refrain::net::decode_text_component(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.unsigned_content = std::move(value_0);
    } else {
      value.unsigned_content.reset();
    }
  }
  {
    if (auto decoded = ::refrain::net::decode_filter_mask(in, value.filter_mask); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  {
    if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_bound(in, value.chat_type); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_player_combat_end(::refrain::net::writer &out, const player_combat_end &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.duration));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_player_combat_end(::refrain::net::reader &in, player_combat_end &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.duration = static_cast<decltype(value.duration)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_player_combat_enter(::refrain::net::writer &out, const player_combat_enter &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_player_combat_enter(::refrain::net::reader &in, player_combat_enter &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_player_combat_kill(::refrain::net::writer &out, const player_combat_kill &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.player_id));
  }
  {
    if (auto written = ::refrain::net::encode_text_component(out, value.message); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_player_combat_kill(::refrain::net::reader &in, player_combat_kill &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.player_id = static_cast<decltype(value.player_id)>(*value_0);
  }
  {
    if (auto decoded = ::refrain::net::decode_text_component(in, value.message); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_player_info_remove(::refrain::net::writer &out, const player_info_remove &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.profile_ids.size()));
    for (const auto &item_0 : value.profile_ids) {
      if (auto written = out.write_uuid(item_0); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_player_info_remove(::refrain::net::reader &in, player_info_remove &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    value.profile_ids.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::net::uuid value_0{};
      auto value_1 = in.read_uuid();
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      value_0 = *value_1;
      value.profile_ids.push_back(std::move(value_0));
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_player_info_update(::refrain::net::writer &out, const player_info_update &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = ::refrain::net::encode_player_info_update(out, value.payload); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_player_info_update(::refrain::net::reader &in, player_info_update &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    if (auto decoded = ::refrain::net::decode_player_info_update(in, value.payload); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_player_look_at(::refrain::net::writer &out, const player_look_at &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(value.from_anchor);
  }
  {
    out.write_f64(value.x);
  }
  {
    out.write_f64(value.y);
  }
  {
    out.write_f64(value.z);
  }
  {
    out.write_bool(value.at_entity.has_value());
    if (value.at_entity.has_value()) {
      if (auto written = ::refrain::protocol::v777::wire::encode_wire_at_entity_payload(out, value.at_entity.value()); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_player_look_at(::refrain::net::reader &in, player_look_at &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.from_anchor = *value_0;
  }
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.x = *value_0;
  }
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.y = *value_0;
  }
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.z = *value_0;
  }
  {
    auto present_0 = in.read_bool();
    if (!present_0) return std::unexpected{std::move(present_0.error())};
    if (*present_0) {
      ::refrain::protocol::v777::wire::wire_at_entity_payload value_0{};
      if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_at_entity_payload(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.at_entity = std::move(value_0);
    } else {
      value.at_entity.reset();
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_player_rotation(::refrain::net::writer &out, const player_rotation &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_f32(value.y_rot);
  }
  {
    out.write_bool(value.relative_y);
  }
  {
    out.write_f32(value.x_rot);
  }
  {
    out.write_bool(value.relative_x);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_player_rotation(::refrain::net::reader &in, player_rotation &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.y_rot = *value_0;
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.relative_y = *value_0;
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.x_rot = *value_0;
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.relative_x = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_recipe_book_add(::refrain::net::writer &out, const recipe_book_add &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.entries.size()));
    for (const auto &item_0 : value.entries) {
      if (auto written = ::refrain::protocol::v777::wire::encode_wire_entry_2(out, item_0); !written) {
        return written;
      }
    }
  }
  {
    out.write_bool(value.replace);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_recipe_book_add(::refrain::net::reader &in, recipe_book_add &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    value.entries.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::protocol::v777::wire::wire_entry_2 value_0{};
      if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_entry_2(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.entries.push_back(std::move(value_0));
    }
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.replace = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_recipe_book_remove(::refrain::net::writer &out, const recipe_book_remove &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.recipes.size()));
    for (const auto &item_0 : value.recipes) {
      if (auto written = ::refrain::protocol::v777::wire::encode_wire_recipe_display_id(out, item_0); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_recipe_book_remove(::refrain::net::reader &in, recipe_book_remove &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    value.recipes.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::protocol::v777::wire::wire_recipe_display_id value_0{};
      if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_recipe_display_id(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.recipes.push_back(std::move(value_0));
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_recipe_book_settings(::refrain::net::writer &out, const recipe_book_settings &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = ::refrain::protocol::v777::wire::encode_wire_book_settings(out, value.book_settings); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_recipe_book_settings(::refrain::net::reader &in, recipe_book_settings &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_book_settings(in, value.book_settings); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_remove_entities(::refrain::net::writer &out, const remove_entities &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.entity_ids.size()));
    for (const auto &item_0 : value.entity_ids) {
      out.write_varint(item_0);
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_remove_entities(::refrain::net::reader &in, remove_entities &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    value.entity_ids.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      std::uint32_t value_0{};
      auto value_1 = in.read_varint();
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      value_0 = *value_1;
      value.entity_ids.push_back(std::move(value_0));
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_remove_mob_effect(::refrain::net::writer &out, const remove_mob_effect &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.entity_id));
  }
  {
    out.write_varint(value.effect);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_remove_mob_effect(::refrain::net::reader &in, remove_mob_effect &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.entity_id = static_cast<decltype(value.entity_id)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.effect = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_reset_score(::refrain::net::writer &out, const reset_score &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_string(value.owner, 32767); !written) {
      return written;
    }
  }
  {
    out.write_bool(value.objective_name.has_value());
    if (value.objective_name.has_value()) {
      if (auto written = out.write_string(value.objective_name.value(), 32767); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_reset_score(::refrain::net::reader &in, reset_score &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_string(32767);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.owner = std::move(*value_0);
  }
  {
    auto present_0 = in.read_bool();
    if (!present_0) return std::unexpected{std::move(present_0.error())};
    if (*present_0) {
      std::string value_0{};
      auto value_1 = in.read_string(32767);
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      value_0 = std::move(*value_1);
      value.objective_name = std::move(value_0);
    } else {
      value.objective_name.reset();
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_resource_pack_pop(::refrain::net::writer &out, const resource_pack_pop &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_bool(value.id.has_value());
    if (value.id.has_value()) {
      if (auto written = out.write_uuid(value.id.value()); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_resource_pack_pop(::refrain::net::reader &in, resource_pack_pop &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto present_0 = in.read_bool();
    if (!present_0) return std::unexpected{std::move(present_0.error())};
    if (*present_0) {
      ::refrain::net::uuid value_0{};
      auto value_1 = in.read_uuid();
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      value_0 = *value_1;
      value.id = std::move(value_0);
    } else {
      value.id.reset();
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_resource_pack_push(::refrain::net::writer &out, const resource_pack_push &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_uuid(value.id); !written) {
      return written;
    }
  }
  {
    if (auto written = out.write_string(value.url, 32767); !written) {
      return written;
    }
  }
  {
    if (auto written = out.write_string(value.hash, 40); !written) {
      return written;
    }
  }
  {
    out.write_bool(value.required);
  }
  {
    out.write_bool(value.prompt.has_value());
    if (value.prompt.has_value()) {
      if (auto written = ::refrain::net::encode_text_component(out, value.prompt.value()); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_resource_pack_push(::refrain::net::reader &in, resource_pack_push &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_uuid();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.id = *value_0;
  }
  {
    auto value_0 = in.read_string(32767);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.url = std::move(*value_0);
  }
  {
    auto value_0 = in.read_string(40);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.hash = std::move(*value_0);
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.required = *value_0;
  }
  {
    auto present_0 = in.read_bool();
    if (!present_0) return std::unexpected{std::move(present_0.error())};
    if (*present_0) {
      ::refrain::net::text_component value_0{};
      if (auto decoded = ::refrain::net::decode_text_component(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.prompt = std::move(value_0);
    } else {
      value.prompt.reset();
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_post_effects(::refrain::net::writer &out, const post_effects &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.post_effects.size()));
    for (const auto &item_0 : value.post_effects) {
      if (auto written = out.write_identifier(item_0); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_post_effects(::refrain::net::reader &in, post_effects &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    value.post_effects.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::data::resource_location value_0{};
      auto value_1 = in.read_identifier();
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      value_0 = std::move(*value_1);
      value.post_effects.push_back(std::move(value_0));
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_respawn(::refrain::net::writer &out, const respawn &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = ::refrain::protocol::v777::wire::encode_wire_common_player_spawn_info(out, value.common_player_spawn_info); !written) {
      return written;
    }
  }
  {
    out.write_u8(static_cast<std::uint8_t>(value.data_to_keep));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_respawn(::refrain::net::reader &in, respawn &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_common_player_spawn_info(in, value.common_player_spawn_info); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.data_to_keep = static_cast<decltype(value.data_to_keep)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_rotate_head(::refrain::net::writer &out, const rotate_head &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.entity_id));
  }
  {
    out.write_u8(static_cast<std::uint8_t>(value.y_head_rot));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_rotate_head(::refrain::net::reader &in, rotate_head &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.entity_id = static_cast<decltype(value.entity_id)>(*value_0);
  }
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.y_head_rot = static_cast<decltype(value.y_head_rot)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_section_blocks_update(::refrain::net::writer &out, const section_blocks_update &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i64(static_cast<std::int64_t>(value.section_pos));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.positions.size()));
    for (const auto &item_0 : value.positions) {
      out.write_varlong(static_cast<std::uint64_t>(item_0));
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_section_blocks_update(::refrain::net::reader &in, section_blocks_update &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.section_pos = static_cast<decltype(value.section_pos)>(*value_0);
  }
  {
    value.positions.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      std::int64_t value_0{};
      auto value_1 = in.read_varlong();
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      value_0 = static_cast<decltype(value_0)>(*value_1);
      value.positions.push_back(std::move(value_0));
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_select_advancements_tab(::refrain::net::writer &out, const select_advancements_tab &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_bool(value.tab.has_value());
    if (value.tab.has_value()) {
      if (auto written = out.write_identifier(value.tab.value()); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_select_advancements_tab(::refrain::net::reader &in, select_advancements_tab &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto present_0 = in.read_bool();
    if (!present_0) return std::unexpected{std::move(present_0.error())};
    if (*present_0) {
      ::refrain::data::resource_location value_0{};
      auto value_1 = in.read_identifier();
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      value_0 = std::move(*value_1);
      value.tab = std::move(value_0);
    } else {
      value.tab.reset();
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_server_data(::refrain::net::writer &out, const server_data &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = ::refrain::net::encode_text_component(out, value.motd); !written) {
      return written;
    }
  }
  {
    out.write_bool(value.icon_bytes.has_value());
    if (value.icon_bytes.has_value()) {
      out.write_varint(static_cast<std::uint32_t>(value.icon_bytes.value().size()));
      out.write_bytes(std::span<const std::byte>{value.icon_bytes.value() });
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_server_data(::refrain::net::reader &in, server_data &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    if (auto decoded = ::refrain::net::decode_text_component(in, value.motd); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  {
    auto present_0 = in.read_bool();
    if (!present_0) return std::unexpected{std::move(present_0.error())};
    if (*present_0) {
      std::vector<std::byte> value_0{};
      auto length_1 = in.read_varint();
      if (!length_1) return std::unexpected{std::move(length_1.error())};
      auto value_1 = in.read_bytes(*length_1);
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      value_0 = std::vector<std::byte>{value_1->begin(), value_1->end()};
      value.icon_bytes = std::move(value_0);
    } else {
      value.icon_bytes.reset();
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_set_action_bar_text(::refrain::net::writer &out, const set_action_bar_text &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = ::refrain::net::encode_text_component(out, value.text); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_set_action_bar_text(::refrain::net::reader &in, set_action_bar_text &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    if (auto decoded = ::refrain::net::decode_text_component(in, value.text); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_set_border_center(::refrain::net::writer &out, const set_border_center &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_f64(value.new_center_x);
  }
  {
    out.write_f64(value.new_center_z);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_set_border_center(::refrain::net::reader &in, set_border_center &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.new_center_x = *value_0;
  }
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.new_center_z = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_set_border_lerp_size(::refrain::net::writer &out, const set_border_lerp_size &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_f64(value.old_size);
  }
  {
    out.write_f64(value.new_size);
  }
  {
    out.write_varlong(static_cast<std::uint64_t>(value.lerp_time));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_set_border_lerp_size(::refrain::net::reader &in, set_border_lerp_size &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.old_size = *value_0;
  }
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.new_size = *value_0;
  }
  {
    auto value_0 = in.read_varlong();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.lerp_time = static_cast<decltype(value.lerp_time)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_set_border_size(::refrain::net::writer &out, const set_border_size &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_f64(value.size);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_set_border_size(::refrain::net::reader &in, set_border_size &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.size = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_set_border_warning_delay(::refrain::net::writer &out, const set_border_warning_delay &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.warning_delay));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_set_border_warning_delay(::refrain::net::reader &in, set_border_warning_delay &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.warning_delay = static_cast<decltype(value.warning_delay)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_set_border_warning_distance(::refrain::net::writer &out, const set_border_warning_distance &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.warning_blocks));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_set_border_warning_distance(::refrain::net::reader &in, set_border_warning_distance &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.warning_blocks = static_cast<decltype(value.warning_blocks)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_set_camera(::refrain::net::writer &out, const set_camera &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.camera_id));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_set_camera(::refrain::net::reader &in, set_camera &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.camera_id = static_cast<decltype(value.camera_id)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_set_cursor_item(::refrain::net::writer &out, const set_cursor_item &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = ::refrain::net::encode_item_stack(out, value.contents); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_set_cursor_item(::refrain::net::reader &in, set_cursor_item &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    if (auto decoded = ::refrain::net::decode_item_stack(in, value.contents); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_set_display_objective(::refrain::net::writer &out, const set_display_objective &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(value.slot);
  }
  {
    if (auto written = out.write_string(value.objective_name, 32767); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_set_display_objective(::refrain::net::reader &in, set_display_objective &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.slot = *value_0;
  }
  {
    auto value_0 = in.read_string(32767);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.objective_name = std::move(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_set_entity_data(::refrain::net::writer &out, const set_entity_data &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(value.entity_id);
  }
  {
    if (auto written = ::refrain::net::encode_entity_metadata(out, value.values); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_set_entity_data(::refrain::net::reader &in, set_entity_data &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.entity_id = *value_0;
  }
  {
    if (auto decoded = ::refrain::net::decode_entity_metadata(in, value.values); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_set_entity_link(::refrain::net::writer &out, const set_entity_link &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i32(static_cast<std::int32_t>(value.source_id));
  }
  {
    out.write_i32(static_cast<std::int32_t>(value.dest_id));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_set_entity_link(::refrain::net::reader &in, set_entity_link &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.source_id = static_cast<decltype(value.source_id)>(*value_0);
  }
  {
    auto value_0 = in.read_i32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.dest_id = static_cast<decltype(value.dest_id)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_set_entity_motion(::refrain::net::writer &out, const set_entity_motion &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.id));
  }
  {
    if (auto written = ::refrain::net::encode_lp_vec3(out, value.movement); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_set_entity_motion(::refrain::net::reader &in, set_entity_motion &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.id = static_cast<decltype(value.id)>(*value_0);
  }
  {
    if (auto decoded = ::refrain::net::decode_lp_vec3(in, value.movement); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_set_equipment(::refrain::net::writer &out, const set_equipment &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.entries.size()));
    for (const auto &item_0 : value.entries) {
      if (auto written = ::refrain::protocol::v777::wire::encode_wire_entries_entry(out, item_0); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_set_equipment(::refrain::net::reader &in, set_equipment &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    value.entries.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::protocol::v777::wire::wire_entries_entry value_0{};
      if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_entries_entry(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.entries.push_back(std::move(value_0));
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_set_experience(::refrain::net::writer &out, const set_experience &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_f32(value.experience_progress);
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.experience_level));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.total_experience));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_set_experience(::refrain::net::reader &in, set_experience &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.experience_progress = *value_0;
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.experience_level = static_cast<decltype(value.experience_level)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.total_experience = static_cast<decltype(value.total_experience)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_set_health(::refrain::net::writer &out, const set_health &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_f32(value.health);
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.food));
  }
  {
    out.write_f32(value.saturation);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_set_health(::refrain::net::reader &in, set_health &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.health = *value_0;
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.food = static_cast<decltype(value.food)>(*value_0);
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.saturation = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_set_objective(::refrain::net::writer &out, const set_objective &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = ::refrain::net::encode_objective_update(out, value.payload); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_set_objective(::refrain::net::reader &in, set_objective &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    if (auto decoded = ::refrain::net::decode_objective_update(in, value.payload); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_set_passengers(::refrain::net::writer &out, const set_passengers &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.vehicle));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.passengers.size()));
    for (const auto &item_0 : value.passengers) {
      out.write_varint(static_cast<std::uint32_t>(item_0));
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_set_passengers(::refrain::net::reader &in, set_passengers &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.vehicle = static_cast<decltype(value.vehicle)>(*value_0);
  }
  {
    value.passengers.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      std::int32_t value_0{};
      auto value_1 = in.read_varint();
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      value_0 = static_cast<decltype(value_0)>(*value_1);
      value.passengers.push_back(std::move(value_0));
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_set_player_inventory(::refrain::net::writer &out, const set_player_inventory &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.slot));
  }
  {
    if (auto written = ::refrain::net::encode_item_stack(out, value.contents); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_set_player_inventory(::refrain::net::reader &in, set_player_inventory &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.slot = static_cast<decltype(value.slot)>(*value_0);
  }
  {
    if (auto decoded = ::refrain::net::decode_item_stack(in, value.contents); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_set_player_team(::refrain::net::writer &out, const set_player_team &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = ::refrain::net::encode_team_update(out, value.payload); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_set_player_team(::refrain::net::reader &in, set_player_team &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    if (auto decoded = ::refrain::net::decode_team_update(in, value.payload); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_set_score(::refrain::net::writer &out, const set_score &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_string(value.owner, 32767); !written) {
      return written;
    }
  }
  {
    if (auto written = out.write_string(value.objective_name, 32767); !written) {
      return written;
    }
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.score));
  }
  {
    out.write_bool(value.display.has_value());
    if (value.display.has_value()) {
      if (auto written = ::refrain::net::encode_text_component(out, value.display.value()); !written) {
        return written;
      }
    }
  }
  {
    out.write_bool(value.number_format.has_value());
    if (value.number_format.has_value()) {
      if (auto written = ::refrain::net::encode_nbt(out, value.number_format.value()); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_set_score(::refrain::net::reader &in, set_score &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_string(32767);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.owner = std::move(*value_0);
  }
  {
    auto value_0 = in.read_string(32767);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.objective_name = std::move(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.score = static_cast<decltype(value.score)>(*value_0);
  }
  {
    auto present_0 = in.read_bool();
    if (!present_0) return std::unexpected{std::move(present_0.error())};
    if (*present_0) {
      ::refrain::net::text_component value_0{};
      if (auto decoded = ::refrain::net::decode_text_component(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.display = std::move(value_0);
    } else {
      value.display.reset();
    }
  }
  {
    auto present_0 = in.read_bool();
    if (!present_0) return std::unexpected{std::move(present_0.error())};
    if (*present_0) {
      ::refrain::net::nbt_blob value_0{};
      if (auto decoded = ::refrain::net::decode_nbt(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.number_format = std::move(value_0);
    } else {
      value.number_format.reset();
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_set_simulation_distance(::refrain::net::writer &out, const set_simulation_distance &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.simulation_distance));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_set_simulation_distance(::refrain::net::reader &in, set_simulation_distance &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.simulation_distance = static_cast<decltype(value.simulation_distance)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_set_subtitle_text(::refrain::net::writer &out, const set_subtitle_text &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = ::refrain::net::encode_text_component(out, value.text); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_set_subtitle_text(::refrain::net::reader &in, set_subtitle_text &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    if (auto decoded = ::refrain::net::decode_text_component(in, value.text); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_set_title_text(::refrain::net::writer &out, const set_title_text &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = ::refrain::net::encode_text_component(out, value.text); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_set_title_text(::refrain::net::reader &in, set_title_text &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    if (auto decoded = ::refrain::net::decode_text_component(in, value.text); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_set_titles_animation(::refrain::net::writer &out, const set_titles_animation &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_i32(static_cast<std::int32_t>(value.fade_in));
  }
  {
    out.write_i32(static_cast<std::int32_t>(value.stay));
  }
  {
    out.write_i32(static_cast<std::int32_t>(value.fade_out));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_set_titles_animation(::refrain::net::reader &in, set_titles_animation &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_i32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.fade_in = static_cast<decltype(value.fade_in)>(*value_0);
  }
  {
    auto value_0 = in.read_i32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.stay = static_cast<decltype(value.stay)>(*value_0);
  }
  {
    auto value_0 = in.read_i32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.fade_out = static_cast<decltype(value.fade_out)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_sound_entity(::refrain::net::writer &out, const sound_entity &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (std::holds_alternative<std::uint32_t>(value.sound)) {
      out.write_varint(std::get<0>(value.sound));
    } else {
      out.write_varint(0);
      if (auto written = ::refrain::protocol::v777::wire::encode_wire_sound_event(out, std::get<1>(value.sound)); !written) {
        return written;
      }
    }
  }
  {
    out.write_varint(value.source);
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.id));
  }
  {
    out.write_f32(value.volume);
  }
  {
    out.write_f32(value.pitch);
  }
  {
    out.write_i64(static_cast<std::int64_t>(value.seed));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_sound_entity(::refrain::net::reader &in, sound_entity &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto holder_0 = in.read_varint();
    if (!holder_0) return std::unexpected{std::move(holder_0.error())};
    if (*holder_0 != 0) {
      value.sound.template emplace<0>(*holder_0);
    } else {
      ::refrain::protocol::v777::wire::wire_sound_event value_0{};
      if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_sound_event(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.sound.template emplace<1>(std::move(value_0));
    }
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.source = *value_0;
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.id = static_cast<decltype(value.id)>(*value_0);
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.volume = *value_0;
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.pitch = *value_0;
  }
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.seed = static_cast<decltype(value.seed)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_sound(::refrain::net::writer &out, const sound &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (std::holds_alternative<std::uint32_t>(value.sound)) {
      out.write_varint(std::get<0>(value.sound));
    } else {
      out.write_varint(0);
      if (auto written = ::refrain::protocol::v777::wire::encode_wire_sound_event(out, std::get<1>(value.sound)); !written) {
        return written;
      }
    }
  }
  {
    out.write_varint(value.source);
  }
  {
    out.write_i32(static_cast<std::int32_t>(value.x));
  }
  {
    out.write_i32(static_cast<std::int32_t>(value.y));
  }
  {
    out.write_i32(static_cast<std::int32_t>(value.z));
  }
  {
    out.write_f32(value.volume);
  }
  {
    out.write_f32(value.pitch);
  }
  {
    out.write_i64(static_cast<std::int64_t>(value.seed));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_sound(::refrain::net::reader &in, sound &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto holder_0 = in.read_varint();
    if (!holder_0) return std::unexpected{std::move(holder_0.error())};
    if (*holder_0 != 0) {
      value.sound.template emplace<0>(*holder_0);
    } else {
      ::refrain::protocol::v777::wire::wire_sound_event value_0{};
      if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_sound_event(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.sound.template emplace<1>(std::move(value_0));
    }
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.source = *value_0;
  }
  {
    auto value_0 = in.read_i32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.x = static_cast<decltype(value.x)>(*value_0);
  }
  {
    auto value_0 = in.read_i32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.y = static_cast<decltype(value.y)>(*value_0);
  }
  {
    auto value_0 = in.read_i32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.z = static_cast<decltype(value.z)>(*value_0);
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.volume = *value_0;
  }
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.pitch = *value_0;
  }
  {
    auto value_0 = in.read_i64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.seed = static_cast<decltype(value.seed)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_start_configuration(::refrain::net::writer &out, const start_configuration &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_start_configuration(::refrain::net::reader &in, start_configuration &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_stop_sound(::refrain::net::writer &out, const stop_sound &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = ::refrain::net::encode_sound_selector(out, value.payload); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_stop_sound(::refrain::net::reader &in, stop_sound &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    if (auto decoded = ::refrain::net::decode_sound_selector(in, value.payload); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_store_cookie(::refrain::net::writer &out, const store_cookie &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_identifier(value.key); !written) {
      return written;
    }
  }
  {
    if (value.payload.size() > 5120) {
      return std::unexpected{::refrain::make_error(::refrain::error_code::invalid_argument, "field 'payload' exceeds its declared maximum")};
    }
    out.write_varint(static_cast<std::uint32_t>(value.payload.size()));
    out.write_bytes(std::span<const std::byte>{value.payload });
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_store_cookie(::refrain::net::reader &in, store_cookie &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_identifier();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.key = std::move(*value_0);
  }
  {
    auto length_0 = in.read_varint();
    if (!length_0) return std::unexpected{std::move(length_0.error())};
    if (*length_0 > 5120) {
      return std::unexpected{::refrain::make_error(::refrain::error_code::parse_error, "field 'payload' exceeds its declared maximum")};
    }
    auto value_0 = in.read_bytes(*length_0);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.payload = std::vector<std::byte>{value_0->begin(), value_0->end()};
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_swing_animation(::refrain::net::writer &out, const swing_animation &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.entity_id));
  }
  {
    out.write_varint(value.hand);
  }
  {
    if (auto written = ::refrain::protocol::v777::wire::encode_wire_swing_animation(out, value.animation); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_swing_animation(::refrain::net::reader &in, swing_animation &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.entity_id = static_cast<decltype(value.entity_id)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.hand = *value_0;
  }
  {
    if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_swing_animation(in, value.animation); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_system_chat(::refrain::net::writer &out, const system_chat &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = ::refrain::net::encode_text_component(out, value.content); !written) {
      return written;
    }
  }
  {
    out.write_bool(value.overlay);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_system_chat(::refrain::net::reader &in, system_chat &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    if (auto decoded = ::refrain::net::decode_text_component(in, value.content); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.overlay = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_tab_list(::refrain::net::writer &out, const tab_list &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = ::refrain::net::encode_text_component(out, value.header); !written) {
      return written;
    }
  }
  {
    if (auto written = ::refrain::net::encode_text_component(out, value.footer); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_tab_list(::refrain::net::reader &in, tab_list &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    if (auto decoded = ::refrain::net::decode_text_component(in, value.header); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  {
    if (auto decoded = ::refrain::net::decode_text_component(in, value.footer); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_tag_query(::refrain::net::writer &out, const tag_query &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.transaction_id));
  }
  {
    if (auto written = ::refrain::net::encode_nbt(out, value.tag); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_tag_query(::refrain::net::reader &in, tag_query &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.transaction_id = static_cast<decltype(value.transaction_id)>(*value_0);
  }
  {
    if (auto decoded = ::refrain::net::decode_nbt(in, value.tag); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_take_item_entity(::refrain::net::writer &out, const take_item_entity &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.item_id));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.player_id));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.amount));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_take_item_entity(::refrain::net::reader &in, take_item_entity &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.item_id = static_cast<decltype(value.item_id)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.player_id = static_cast<decltype(value.player_id)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.amount = static_cast<decltype(value.amount)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_teleport_entity(::refrain::net::writer &out, const teleport_entity &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.id));
  }
  {
    if (auto written = ::refrain::protocol::v777::wire::encode_wire_position_move_rotation(out, value.change); !written) {
      return written;
    }
  }
  {
    out.write_i32(static_cast<std::int32_t>(value.relatives));
  }
  {
    out.write_bool(value.on_ground);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_teleport_entity(::refrain::net::reader &in, teleport_entity &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.id = static_cast<decltype(value.id)>(*value_0);
  }
  {
    if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_position_move_rotation(in, value.change); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  {
    auto value_0 = in.read_i32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.relatives = static_cast<decltype(value.relatives)>(*value_0);
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.on_ground = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_test_instance_block_status(::refrain::net::writer &out, const test_instance_block_status &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = ::refrain::net::encode_text_component(out, value.status); !written) {
      return written;
    }
  }
  {
    out.write_bool(value.size.has_value());
    if (value.size.has_value()) {
      if (auto written = ::refrain::protocol::v777::wire::encode_wire_vec3i(out, value.size.value()); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_test_instance_block_status(::refrain::net::reader &in, test_instance_block_status &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    if (auto decoded = ::refrain::net::decode_text_component(in, value.status); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  {
    auto present_0 = in.read_bool();
    if (!present_0) return std::unexpected{std::move(present_0.error())};
    if (*present_0) {
      ::refrain::protocol::v777::wire::wire_vec3i value_0{};
      if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_vec3i(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.size = std::move(value_0);
    } else {
      value.size.reset();
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_ticking_state(::refrain::net::writer &out, const ticking_state &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_f32(value.tick_rate);
  }
  {
    out.write_bool(value.is_frozen);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_ticking_state(::refrain::net::reader &in, ticking_state &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_f32();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.tick_rate = *value_0;
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.is_frozen = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_ticking_step(::refrain::net::writer &out, const ticking_step &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.tick_steps));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_ticking_step(::refrain::net::reader &in, ticking_step &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.tick_steps = static_cast<decltype(value.tick_steps)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_transfer(::refrain::net::writer &out, const transfer &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = out.write_string(value.host, 32767); !written) {
      return written;
    }
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.port));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_transfer(::refrain::net::reader &in, transfer &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_string(32767);
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.host = std::move(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.port = static_cast<decltype(value.port)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_update_advancements(::refrain::net::writer &out, const update_advancements &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_bool(value.should_reset);
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.added.size()));
    for (const auto &item_0 : value.added) {
      if (auto written = ::refrain::protocol::v777::wire::encode_wire_added(out, item_0); !written) {
        return written;
      }
    }
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.removed.size()));
    for (const auto &item_0 : value.removed) {
      if (auto written = out.write_identifier(item_0); !written) {
        return written;
      }
    }
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.progress.size()));
    for (const auto &entry_0 : value.progress) {
      if (auto written = out.write_identifier(entry_0.first); !written) {
        return written;
      }
      if (auto written = ::refrain::net::encode_advancement_progress(out, entry_0.second); !written) {
        return written;
      }
    }
  }
  {
    out.write_bool(value.show_advancements);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_update_advancements(::refrain::net::reader &in, update_advancements &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.should_reset = *value_0;
  }
  {
    value.added.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::protocol::v777::wire::wire_added value_0{};
      if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_added(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.added.push_back(std::move(value_0));
    }
  }
  {
    value.removed.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::data::resource_location value_0{};
      auto value_1 = in.read_identifier();
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      value_0 = std::move(*value_1);
      value.removed.push_back(std::move(value_0));
    }
  }
  {
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    value.progress.clear();
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::data::resource_location map_key_0{};
      auto value_1 = in.read_identifier();
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      map_key_0 = std::move(*value_1);
      ::refrain::net::advancement_progress map_value_0{};
      if (auto decoded = ::refrain::net::decode_advancement_progress(in, map_value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.progress.emplace(std::move(map_key_0), std::move(map_value_0));
    }
  }
  {
    auto value_0 = in.read_bool();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.show_advancements = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_update_attributes(::refrain::net::writer &out, const update_attributes &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.get_entity_id));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.get_values.size()));
    for (const auto &item_0 : value.get_values) {
      if (auto written = ::refrain::protocol::v777::wire::encode_wire_get_values(out, item_0); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_update_attributes(::refrain::net::reader &in, update_attributes &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.get_entity_id = static_cast<decltype(value.get_entity_id)>(*value_0);
  }
  {
    value.get_values.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::protocol::v777::wire::wire_get_values value_0{};
      if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_get_values(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.get_values.push_back(std::move(value_0));
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_update_mob_effect(::refrain::net::writer &out, const update_mob_effect &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.entity_id));
  }
  {
    out.write_varint(value.effect);
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.effect_amplifier));
  }
  {
    out.write_varint(static_cast<std::uint32_t>(value.effect_duration_ticks));
  }
  {
    out.write_u8(static_cast<std::uint8_t>(value.flags_));
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_update_mob_effect(::refrain::net::reader &in, update_mob_effect &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.entity_id = static_cast<decltype(value.entity_id)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.effect = *value_0;
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.effect_amplifier = static_cast<decltype(value.effect_amplifier)>(*value_0);
  }
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.effect_duration_ticks = static_cast<decltype(value.effect_duration_ticks)>(*value_0);
  }
  {
    auto value_0 = in.read_u8();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.flags_ = static_cast<decltype(value.flags_)>(*value_0);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_update_recipes(::refrain::net::writer &out, const update_recipes &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.item_sets.size()));
    for (const auto &entry_0 : value.item_sets) {
      if (auto written = out.write_identifier(entry_0.first); !written) {
        return written;
      }
      out.write_varint(static_cast<std::uint32_t>(entry_0.second.size()));
      for (const auto &item_1 : entry_0.second) {
        out.write_varint(item_1);
      }
    }
  }
  {
    if (auto written = ::refrain::net::encode_single_input_set(out, value.stonecutter_recipes); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_update_recipes(::refrain::net::reader &in, update_recipes &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    value.item_sets.clear();
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::data::resource_location map_key_0{};
      auto value_1 = in.read_identifier();
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      map_key_0 = std::move(*value_1);
      std::vector<std::uint32_t> map_value_0{};
      map_value_0.clear();
      auto count_2 = in.read_varint();
      if (!count_2) return std::unexpected{std::move(count_2.error())};
      for (std::uint32_t index_2 = 0; index_2 < *count_2; ++index_2) {
        std::uint32_t value_2{};
        auto value_3 = in.read_varint();
        if (!value_3) return std::unexpected{std::move(value_3.error())};
        value_2 = *value_3;
        map_value_0.push_back(std::move(value_2));
      }
      value.item_sets.emplace(std::move(map_key_0), std::move(map_value_0));
    }
  }
  {
    if (auto decoded = ::refrain::net::decode_single_input_set(in, value.stonecutter_recipes); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_update_tags(::refrain::net::writer &out, const update_tags &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.tags.size()));
    for (const auto &entry_0 : value.tags) {
      if (auto written = out.write_identifier(entry_0.first); !written) {
        return written;
      }
      if (auto written = ::refrain::protocol::v777::wire::encode_wire_network_payload(out, entry_0.second); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_update_tags(::refrain::net::reader &in, update_tags &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    value.tags.clear();
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::data::resource_location map_key_0{};
      auto value_1 = in.read_identifier();
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      map_key_0 = std::move(*value_1);
      ::refrain::protocol::v777::wire::wire_network_payload map_value_0{};
      if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_network_payload(in, map_value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.tags.emplace(std::move(map_key_0), std::move(map_value_0));
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_projectile_power(::refrain::net::writer &out, const projectile_power &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.id));
  }
  {
    out.write_f64(value.acceleration_power);
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_projectile_power(::refrain::net::reader &in, projectile_power &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto value_0 = in.read_varint();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.id = static_cast<decltype(value.id)>(*value_0);
  }
  {
    auto value_0 = in.read_f64();
    if (!value_0) return std::unexpected{std::move(value_0.error())};
    value.acceleration_power = *value_0;
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_custom_report_details(::refrain::net::writer &out, const custom_report_details &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.details.size()));
    for (const auto &entry_0 : value.details) {
      if (auto written = out.write_string(entry_0.first, 32767); !written) {
        return written;
      }
      if (auto written = out.write_string(entry_0.second, 32767); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_custom_report_details(::refrain::net::reader &in, custom_report_details &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    value.details.clear();
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      std::string map_key_0{};
      auto value_1 = in.read_string(32767);
      if (!value_1) return std::unexpected{std::move(value_1.error())};
      map_key_0 = std::move(*value_1);
      std::string map_value_0{};
      auto value_2 = in.read_string(32767);
      if (!value_2) return std::unexpected{std::move(value_2.error())};
      map_value_0 = std::move(*value_2);
      value.details.emplace(std::move(map_key_0), std::move(map_value_0));
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_server_links(::refrain::net::writer &out, const server_links &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    out.write_varint(static_cast<std::uint32_t>(value.links.size()));
    for (const auto &item_0 : value.links) {
      if (auto written = ::refrain::protocol::v777::wire::encode_wire_untrusted_entry(out, item_0); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_server_links(::refrain::net::reader &in, server_links &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    value.links.clear();
    auto count_0 = in.read_varint();
    if (!count_0) return std::unexpected{std::move(count_0.error())};
    for (std::uint32_t index_0 = 0; index_0 < *count_0; ++index_0) {
      ::refrain::protocol::v777::wire::wire_untrusted_entry value_0{};
      if (auto decoded = ::refrain::protocol::v777::wire::decode_wire_untrusted_entry(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.links.push_back(std::move(value_0));
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_waypoint(::refrain::net::writer &out, const waypoint &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (auto written = ::refrain::net::encode_tracked_waypoint(out, value.payload); !written) {
      return written;
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_waypoint(::refrain::net::reader &in, waypoint &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    if (auto decoded = ::refrain::net::decode_tracked_waypoint(in, value.payload); !decoded) {
      return std::unexpected{std::move(decoded.error())};
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_clear_dialog(::refrain::net::writer &out, const clear_dialog &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_clear_dialog(::refrain::net::reader &in, clear_dialog &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto encode_show_dialog(::refrain::net::writer &out, const show_dialog &value) -> ::refrain::status {
  static_cast<void>(out);
  static_cast<void>(value);
  {
    if (std::holds_alternative<std::uint32_t>(value.dialog)) {
      out.write_varint(std::get<0>(value.dialog));
    } else {
      out.write_varint(0);
      if (auto written = ::refrain::net::encode_nbt(out, std::get<1>(value.dialog)); !written) {
        return written;
      }
    }
  }
  return ::refrain::status_ok();
}

[[nodiscard]] inline auto decode_show_dialog(::refrain::net::reader &in, show_dialog &value) -> ::refrain::status {
  static_cast<void>(in);
  static_cast<void>(value);
  {
    auto holder_0 = in.read_varint();
    if (!holder_0) return std::unexpected{std::move(holder_0.error())};
    if (*holder_0 != 0) {
      value.dialog.template emplace<0>(*holder_0);
    } else {
      ::refrain::net::nbt_blob value_0{};
      if (auto decoded = ::refrain::net::decode_nbt(in, value_0); !decoded) {
        return std::unexpected{std::move(decoded.error())};
      }
      value.dialog.template emplace<1>(std::move(value_0));
    }
  }
  return ::refrain::status_ok();
}

} // namespace refrain::protocol::v777::play::clientbound

#endif // REFRAIN_GENERATED_CPP_REFRAIN_SCHEMA_PROTOCOL_777_PLAY_CLIENTBOUND_HPP
