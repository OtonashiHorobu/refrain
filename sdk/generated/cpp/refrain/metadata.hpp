// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 996e38144c4577ab

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_METADATA_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_METADATA_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// Generated runtime schema metadata. This is data for the
// sealed `refrain:host.metadata` service, not C++ `reflect<T>`.
namespace refrain::metadata {

struct enum_value_entry {
    std::string_view name;
    std::int64_t value;
};

struct field_entry {
    std::string_view name;
    std::string_view type_id;
    std::uint32_t offset;
    std::uint32_t size;
    std::uint32_t flags;
};

struct type_entry {
    std::string_view id;
    std::uint32_t kind;
    std::uint32_t size;
    std::uint32_t alignment;
    std::span<const field_entry> fields;
    std::span<const enum_value_entry> values;
};

struct contract_entry {
    std::string_view id;
    std::uint32_t abi_major;
    std::uint32_t abi_minor;
    std::uint32_t abi_patch;
    std::string_view schema_hash;
    std::uint32_t method_count;
};

namespace detail {
inline constexpr std::array<enum_value_entry, 8> refrain_core_core_error_values{
    enum_value_entry{"invalid_argument" , 1},
    enum_value_entry{"not_found" , 2},
    enum_value_entry{"already_exists" , 3},
    enum_value_entry{"permission_denied" , 4},
    enum_value_entry{"io_error" , 5},
    enum_value_entry{"out_of_memory" , 6},
    enum_value_entry{"unsupported" , 7},
    enum_value_entry{"version_mismatch" , 8},
};
inline constexpr std::array<enum_value_entry, 11> refrain_core_chunk_status_values{
    enum_value_entry{"empty" , 0},
    enum_value_entry{"structure_starts" , 1},
    enum_value_entry{"structure_references" , 2},
    enum_value_entry{"biomes" , 3},
    enum_value_entry{"noise" , 4},
    enum_value_entry{"surface" , 5},
    enum_value_entry{"carvers" , 6},
    enum_value_entry{"features" , 7},
    enum_value_entry{"light" , 8},
    enum_value_entry{"spawn" , 9},
    enum_value_entry{"full" , 10},
};
inline constexpr std::array<enum_value_entry, 6> refrain_core_block_face_values{
    enum_value_entry{"north" , 0},
    enum_value_entry{"east" , 1},
    enum_value_entry{"south" , 2},
    enum_value_entry{"west" , 3},
    enum_value_entry{"up" , 4},
    enum_value_entry{"down" , 5},
};
inline constexpr std::array<field_entry, 2> refrain_core_chunk_pos_fields{
    field_entry{"x" , "i32", 4, 4, 0},
    field_entry{"z" , "i32", 8, 4, 0},
};
inline constexpr std::array<field_entry, 3> refrain_core_block_pos_fields{
    field_entry{"x" , "i32", 4, 4, 0},
    field_entry{"y" , "i32", 8, 4, 0},
    field_entry{"z" , "i32", 12, 4, 0},
};
inline constexpr std::array<field_entry, 5> refrain_core_chunk_section_view_fields{
    field_entry{"section_y" , "i32", 4, 4, 0},
    field_entry{"block_count" , "u16", 8, 2, 0},
    field_entry{"bits_per_entry" , "u8", 10, 1, 0},
    field_entry{"flags" , "u8", 11, 1, 0},
    field_entry{"block_entities" , "slice<refrain::core::block_pos>", 16, 16, 0},
};
inline constexpr std::array<field_entry, 4> refrain_core_chunk_view_fields{
    field_entry{"pos" , "refrain::core::chunk_pos", 4, 12, 0},
    field_entry{"status" , "refrain::core::chunk_status", 16, 4, 0},
    field_entry{"section_count" , "u32", 20, 4, 0},
    field_entry{"sections" , "slice<refrain::core::chunk_section_view>", 24, 16, 0},
};
inline constexpr std::array<field_entry, 3> refrain_core_block_change_fields{
    field_entry{"pos" , "refrain::core::block_pos", 4, 16, 0},
    field_entry{"old_state" , "u32", 20, 4, 0},
    field_entry{"new_state" , "u32", 24, 4, 0},
};
inline constexpr std::array<field_entry, 4> refrain_core_chunk_buffer_fields{
    field_entry{"pos" , "refrain::core::chunk_pos", 4, 12, 0},
    field_entry{"generation" , "u32", 16, 4, 0},
    field_entry{"word_count" , "u32", 20, 4, 0},
    field_entry{"data" , "bytes", 24, 16, 0},
};
inline constexpr std::array<enum_value_entry, 12> refrain_host_core_host_error_values{
    enum_value_entry{"invalid_argument" , 1},
    enum_value_entry{"not_found" , 2},
    enum_value_entry{"permission_denied" , 3},
    enum_value_entry{"version_mismatch" , 4},
    enum_value_entry{"unsupported" , 5},
    enum_value_entry{"not_initialized" , 6},
    enum_value_entry{"out_of_memory" , 7},
    enum_value_entry{"limit_exceeded" , 8},
    enum_value_entry{"thread_violation" , 9},
    enum_value_entry{"internal" , 10},
    enum_value_entry{"already_exists" , 11},
    enum_value_entry{"io_error" , 12},
};
inline constexpr std::array<field_entry, 3> refrain_host_core_host_version_fields{
    field_entry{"major" , "u32", 4, 4, 0},
    field_entry{"minor" , "u32", 8, 4, 0},
    field_entry{"patch" , "u32", 12, 4, 0},
};
inline constexpr std::array<field_entry, 4> refrain_host_core_provider_ref_fields{
    field_entry{"vtable" , "opaque_ptr", 8, 8, 0},
    field_entry{"self" , "opaque_ptr", 16, 8, 0},
    field_entry{"abi_major" , "u32", 24, 4, 0},
    field_entry{"reserved" , "u32", 28, 4, 0},
};
inline constexpr std::array<enum_value_entry, 9> refrain_host_kernel_kernel_error_values{
    enum_value_entry{"invalid_argument" , 1},
    enum_value_entry{"not_found" , 2},
    enum_value_entry{"permission_denied" , 3},
    enum_value_entry{"unsupported" , 4},
    enum_value_entry{"out_of_memory" , 5},
    enum_value_entry{"cancelled" , 6},
    enum_value_entry{"timeout" , 7},
    enum_value_entry{"thread_violation" , 8},
    enum_value_entry{"internal" , 9},
};
inline constexpr std::array<enum_value_entry, 3> refrain_host_metadata_metadata_error_values{
    enum_value_entry{"invalid_argument" , 1},
    enum_value_entry{"not_found" , 2},
    enum_value_entry{"internal" , 3},
};
inline constexpr std::array<enum_value_entry, 5> refrain_host_metadata_type_kind_values{
    enum_value_entry{"structure" , 0},
    enum_value_entry{"enumeration" , 1},
    enum_value_entry{"flags" , 2},
    enum_value_entry{"record" , 3},
    enum_value_entry{"packet" , 4},
};
inline constexpr std::array<field_entry, 6> refrain_host_metadata_type_info_fields{
    field_entry{"id" , "string", 8, 16, 0},
    field_entry{"kind" , "refrain::host::metadata::type_kind", 24, 4, 0},
    field_entry{"size" , "u32", 28, 4, 0},
    field_entry{"alignment" , "u32", 32, 4, 0},
    field_entry{"field_count" , "u32", 36, 4, 0},
    field_entry{"enum_count" , "u32", 40, 4, 0},
};
inline constexpr std::array<field_entry, 5> refrain_host_metadata_field_info_fields{
    field_entry{"name" , "string", 8, 16, 0},
    field_entry{"type_id" , "string", 24, 16, 0},
    field_entry{"offset" , "u32", 40, 4, 0},
    field_entry{"size" , "u32", 44, 4, 0},
    field_entry{"flags" , "u32", 48, 4, 0},
};
inline constexpr std::array<field_entry, 2> refrain_host_metadata_enum_value_info_fields{
    field_entry{"name" , "string", 8, 16, 0},
    field_entry{"value" , "i64", 24, 8, 0},
};
inline constexpr std::array<field_entry, 6> refrain_host_metadata_contract_info_fields{
    field_entry{"id" , "string", 8, 16, 0},
    field_entry{"abi_major" , "u32", 24, 4, 0},
    field_entry{"abi_minor" , "u32", 28, 4, 0},
    field_entry{"abi_patch" , "u32", 32, 4, 0},
    field_entry{"schema_hash" , "string", 40, 16, 0},
    field_entry{"method_count" , "u32", 56, 4, 0},
};
inline constexpr std::array<enum_value_entry, 10> refrain_host_platform_platform_error_values{
    enum_value_entry{"invalid_argument" , 1},
    enum_value_entry{"not_found" , 2},
    enum_value_entry{"permission_denied" , 3},
    enum_value_entry{"io_error" , 4},
    enum_value_entry{"out_of_memory" , 5},
    enum_value_entry{"unsupported" , 6},
    enum_value_entry{"already_exists" , 7},
    enum_value_entry{"read_only" , 8},
    enum_value_entry{"limit_exceeded" , 9},
    enum_value_entry{"internal" , 10},
};
inline constexpr std::array<enum_value_entry, 6> refrain_host_platform_file_open_flags_values{
    enum_value_entry{"read_only" , 1},
    enum_value_entry{"read_write" , 2},
    enum_value_entry{"create" , 4},
    enum_value_entry{"exclusive" , 8},
    enum_value_entry{"truncate" , 16},
    enum_value_entry{"no_follow" , 32},
};
inline constexpr std::array<enum_value_entry, 3> refrain_services_data_packs_pack_kind_values{
    enum_value_entry{"data" , 0},
    enum_value_entry{"resource" , 1},
    enum_value_entry{"mixed" , 2},
};
inline constexpr std::array<enum_value_entry, 5> refrain_services_data_packs_pack_error_values{
    enum_value_entry{"not_found" , 1},
    enum_value_entry{"invalid_format" , 2},
    enum_value_entry{"version_mismatch" , 3},
    enum_value_entry{"io_error" , 4},
    enum_value_entry{"already_mounted" , 5},
};
inline constexpr std::array<field_entry, 3> refrain_services_data_packs_pack_source_fields{
    field_entry{"path" , "string", 8, 16, 0},
    field_entry{"kind" , "refrain::services::data_packs::pack_kind", 24, 4, 0},
    field_entry{"pack_format" , "u32", 28, 4, 0},
};
inline constexpr std::array<field_entry, 5> refrain_services_data_packs_pack_info_fields{
    field_entry{"pack" , "handle:pack", 8, 8, 0},
    field_entry{"id" , "string", 16, 16, 0},
    field_entry{"version" , "string", 32, 16, 0},
    field_entry{"pack_format" , "u32", 48, 4, 0},
    field_entry{"file_count" , "u64", 56, 8, 0},
};
inline constexpr std::array<enum_value_entry, 4> refrain_services_net_protocol_protocol_edition_values{
    enum_value_entry{"java" , 0},
    enum_value_entry{"bedrock" , 1},
    enum_value_entry{"refrain" , 2},
    enum_value_entry{"custom" , 3},
};
inline constexpr std::array<enum_value_entry, 5> refrain_services_net_protocol_protocol_error_values{
    enum_value_entry{"frame_too_large" , 1},
    enum_value_entry{"malformed_packet" , 2},
    enum_value_entry{"unknown_packet" , 3},
    enum_value_entry{"unsupported_version" , 4},
    enum_value_entry{"compression_failed" , 5},
};
inline constexpr std::array<enum_value_entry, 4> refrain_services_net_protocol_protocol_event_values{
    enum_value_entry{"status_request" , 0},
    enum_value_entry{"login_request" , 1},
    enum_value_entry{"login_acknowledged" , 2},
    enum_value_entry{"configuration_acknowledged" , 3},
};
inline constexpr std::array<enum_value_entry, 5> refrain_services_net_protocol_connection_state_values{
    enum_value_entry{"handshake" , 0},
    enum_value_entry{"status" , 1},
    enum_value_entry{"login" , 2},
    enum_value_entry{"configuration" , 3},
    enum_value_entry{"play" , 4},
};
inline constexpr std::array<field_entry, 7> refrain_services_net_protocol_protocol_descriptor_fields{
    field_entry{"edition" , "refrain::services::net_protocol::protocol_edition", 4, 4, 0},
    field_entry{"family" , "string", 8, 16, 0},
    field_entry{"canonical_version" , "u32", 24, 4, 0},
    field_entry{"accepted_versions" , "string", 32, 16, 0},
    field_entry{"transport_protocol" , "u32", 48, 4, 0},
    field_entry{"requires_stream" , "bool", 52, 1, 0},
    field_entry{"capabilities" , "string", 56, 16, 0},
};
inline constexpr std::array<field_entry, 1> refrain_services_net_protocol_frame_chunk_fields{
    field_entry{"data" , "bytes", 8, 16, 0},
};
inline constexpr std::array<field_entry, 2> refrain_services_net_protocol_decoded_packet_fields{
    field_entry{"header" , "refrain::services::net_protocol::frame_header", 4, 12, 0},
    field_entry{"packet" , "refrain::services::net_protocol::packet_view", 16, 24, 0},
};
inline constexpr std::array<field_entry, 2> refrain_services_net_protocol_protocol_version_fields{
    field_entry{"protocol" , "u32", 4, 4, 0},
    field_entry{"name" , "string", 8, 16, 0},
};
inline constexpr std::array<field_entry, 2> refrain_services_net_protocol_frame_header_fields{
    field_entry{"length" , "u32", 4, 4, 0},
    field_entry{"uncompressed_length" , "u32", 8, 4, 0},
};
inline constexpr std::array<field_entry, 2> refrain_services_net_protocol_packet_view_fields{
    field_entry{"packet_id" , "u32", 4, 4, 0},
    field_entry{"payload" , "bytes", 8, 16, 0},
};
inline constexpr std::array<field_entry, 1> refrain_services_net_protocol_encoded_packet_fields{
    field_entry{"data" , "bytes", 8, 16, 0},
};
inline constexpr std::array<field_entry, 1> refrain_services_net_protocol_connection_ref_fields{
    field_entry{"connection" , "u64", 8, 8, 0},
};
inline constexpr std::array<enum_value_entry, 7> refrain_services_net_session_session_error_values{
    enum_value_entry{"invalid_argument" , 1},
    enum_value_entry{"not_joined" , 2},
    enum_value_entry{"unavailable" , 3},
    enum_value_entry{"malformed_profile" , 4},
    enum_value_entry{"not_found" , 5},
    enum_value_entry{"already_open" , 6},
    enum_value_entry{"unsupported" , 7},
};
inline constexpr std::array<field_entry, 3> refrain_services_net_session_session_profile_fields{
    field_entry{"id" , "string", 8, 16, 0},
    field_entry{"name" , "string", 24, 16, 0},
    field_entry{"properties" , "bytes", 40, 16, 0},
};
inline constexpr std::array<field_entry, 7> refrain_services_net_session_menu_click_result_fields{
    field_entry{"slot" , "i32", 4, 4, 0},
    field_entry{"button" , "i32", 8, 4, 0},
    field_entry{"accepted" , "bool", 12, 1, 0},
    field_entry{"slot_count" , "u32", 16, 4, 0},
    field_entry{"slot_item" , "string", 24, 16, 0},
    field_entry{"carried_count" , "u32", 40, 4, 0},
    field_entry{"carried_item" , "string", 48, 16, 0},
};
inline constexpr std::array<enum_value_entry, 3> refrain_services_net_translator_translation_error_values{
    enum_value_entry{"unsupported" , 1},
    enum_value_entry{"malformed_packet" , 2},
    enum_value_entry{"internal" , 3},
};
inline constexpr std::array<field_entry, 4> refrain_services_net_translator_translator_descriptor_fields{
    field_entry{"name" , "string", 8, 16, 0},
    field_entry{"source_family" , "string", 24, 16, 0},
    field_entry{"target_family" , "string", 40, 16, 0},
    field_entry{"capabilities" , "string", 56, 16, 0},
};
inline constexpr std::array<field_entry, 5> refrain_services_net_translator_translation_request_fields{
    field_entry{"connection" , "u64", 8, 8, 0},
    field_entry{"source_protocol" , "u32", 16, 4, 0},
    field_entry{"target_protocol" , "u32", 20, 4, 0},
    field_entry{"packet_id" , "u32", 24, 4, 0},
    field_entry{"payload" , "bytes", 32, 16, 0},
};
inline constexpr std::array<field_entry, 2> refrain_services_net_translator_translation_result_fields{
    field_entry{"packet_id" , "u32", 4, 4, 0},
    field_entry{"payload" , "bytes", 8, 16, 0},
};
inline constexpr std::array<enum_value_entry, 3> refrain_services_runtime_runtime_error_values{
    enum_value_entry{"invalid_config" , 1},
    enum_value_entry{"io_error" , 2},
    enum_value_entry{"internal" , 3},
};
inline constexpr std::array<field_entry, 22> refrain_services_runtime_runtime_config_fields{
    field_entry{"world_path" , "string", 8, 16, 0},
    field_entry{"ticks" , "i64", 24, 8, 0},
    field_entry{"command_file" , "string", 32, 16, 0},
    field_entry{"script_file" , "string", 48, 16, 0},
    field_entry{"hash_every" , "u64", 64, 8, 0},
    field_entry{"world_zstd_level" , "i32", 72, 4, 0},
    field_entry{"zstd_near_level" , "i32", 76, 4, 0},
    field_entry{"zstd_mid_level" , "i32", 80, 4, 0},
    field_entry{"zstd_far_level" , "i32", 84, 4, 0},
    field_entry{"zstd_near_radius" , "i32", 88, 4, 0},
    field_entry{"zstd_mid_radius" , "i32", 92, 4, 0},
    field_entry{"watchdog_timeout_ms" , "u32", 96, 4, 0},
    field_entry{"autosave_interval_ticks" , "u64", 104, 8, 0},
    field_entry{"compact_interval_ticks" , "u64", 112, 8, 0},
    field_entry{"compact_min_bytes" , "u64", 120, 8, 0},
    field_entry{"compact_garbage_percent" , "u32", 128, 4, 0},
    field_entry{"store_compact_interval_ticks" , "u64", 136, 8, 0},
    field_entry{"content_store" , "string", 144, 16, 0},
    field_entry{"content_gc_apply" , "bool", 160, 1, 0},
    field_entry{"content_gc_keep_days" , "i32", 164, 4, 0},
    field_entry{"content_gc_budget_ms" , "u64", 168, 8, 0},
    field_entry{"content_gc_interval_ticks" , "u64", 176, 8, 0},
};
inline constexpr std::array<field_entry, 3> refrain_services_session_lifecycle_player_info_fields{
    field_entry{"connection" , "u64", 8, 8, 0},
    field_entry{"name" , "string", 16, 16, 0},
    field_entry{"world" , "string", 32, 16, 0},
};
inline constexpr std::array<field_entry, 4> refrain_services_session_status_status_request_fields{
    field_entry{"protocol" , "u32", 4, 4, 0},
    field_entry{"host" , "string", 8, 16, 0},
    field_entry{"port" , "u16", 24, 2, 0},
    field_entry{"online_players" , "u32", 28, 4, 0},
};
inline constexpr std::array<field_entry, 3> refrain_services_session_status_status_view_fields{
    field_entry{"description" , "string", 8, 16, 0},
    field_entry{"online_players" , "u32", 24, 4, 0},
    field_entry{"max_players" , "u32", 28, 4, 0},
};
inline constexpr std::array<field_entry, 2> refrain_services_session_tab_tab_request_fields{
    field_entry{"connection" , "u64", 8, 8, 0},
    field_entry{"player_name" , "string", 16, 16, 0},
};
inline constexpr std::array<field_entry, 2> refrain_services_session_tab_tab_view_fields{
    field_entry{"header" , "string", 8, 16, 0},
    field_entry{"footer" , "string", 24, 16, 0},
};
inline constexpr std::array<enum_value_entry, 8> refrain_services_sim_sim_error_values{
    enum_value_entry{"invalid_handle" , 1},
    enum_value_entry{"already_open" , 2},
    enum_value_entry{"not_found" , 3},
    enum_value_entry{"io_error" , 4},
    enum_value_entry{"internal" , 5},
    enum_value_entry{"out_of_bounds" , 6},
    enum_value_entry{"invalid_argument" , 7},
    enum_value_entry{"storage_unavailable" , 8},
};
inline constexpr std::array<field_entry, 2> refrain_services_sim_sim_config_fields{
    field_entry{"storage" , "refrain::services::storage::storage_handle", 8, 56, 0},
    field_entry{"seed" , "i64", 64, 8, 0},
};
inline constexpr std::array<enum_value_entry, 7> refrain_services_storage_storage_error_values{
    enum_value_entry{"open_failed" , 1},
    enum_value_entry{"not_found" , 2},
    enum_value_entry{"corrupt_data" , 3},
    enum_value_entry{"io_error" , 4},
    enum_value_entry{"version_mismatch" , 5},
    enum_value_entry{"read_only" , 6},
    enum_value_entry{"already_open" , 7},
};
inline constexpr std::array<field_entry, 3> refrain_services_storage_world_open_config_fields{
    field_entry{"world_path" , "string", 8, 16, 0},
    field_entry{"gamepack_id" , "string", 24, 16, 0},
    field_entry{"flags" , "u32", 40, 4, 0},
};
inline constexpr std::array<field_entry, 3> refrain_services_storage_storage_handle_fields{
    field_entry{"world" , "handle:storage_world", 8, 8, 0},
    field_entry{"owner" , "refrain::host::core::provider_ref", 16, 32, 0},
    field_entry{"store" , "opaque_ptr", 48, 8, 0},
};
inline constexpr std::array<field_entry, 2> refrain_services_storage_chunk_key_fields{
    field_entry{"pos" , "refrain::core::chunk_pos", 4, 12, 0},
    field_entry{"format_version" , "u32", 16, 4, 0},
};
inline constexpr std::array<field_entry, 3> refrain_services_storage_chunk_read_result_fields{
    field_entry{"bytes_read" , "u32", 4, 4, 0},
    field_entry{"format_version" , "u32", 8, 4, 0},
    field_entry{"record_version" , "u64", 16, 8, 0},
};
inline constexpr std::array<enum_value_entry, 8> refrain_services_transport_transport_error_values{
    enum_value_entry{"invalid_argument" , 1},
    enum_value_entry{"not_found" , 2},
    enum_value_entry{"already_exists" , 3},
    enum_value_entry{"permission_denied" , 4},
    enum_value_entry{"io_error" , 5},
    enum_value_entry{"out_of_memory" , 6},
    enum_value_entry{"unsupported" , 7},
    enum_value_entry{"closed" , 8},
};
inline constexpr std::array<enum_value_entry, 7> refrain_services_transport_transport_protocol_values{
    enum_value_entry{"tcp" , 0},
    enum_value_entry{"tls_tcp" , 1},
    enum_value_entry{"raknet" , 2},
    enum_value_entry{"unix_stream" , 3},
    enum_value_entry{"memory_stream" , 4},
    enum_value_entry{"memory_message" , 5},
    enum_value_entry{"custom" , 6},
};
inline constexpr std::array<enum_value_entry, 4> refrain_services_transport_transport_event_kind_values{
    enum_value_entry{"opened" , 1},
    enum_value_entry{"data" , 2},
    enum_value_entry{"closed" , 3},
    enum_value_entry{"error" , 4},
};
inline constexpr std::array<field_entry, 2> refrain_services_transport_transport_endpoint_fields{
    field_entry{"host" , "string", 8, 16, 0},
    field_entry{"port" , "u16", 24, 2, 0},
};
inline constexpr std::array<field_entry, 7> refrain_services_transport_transport_descriptor_fields{
    field_entry{"provider" , "string", 8, 16, 0},
    field_entry{"protocol" , "refrain::services::transport::transport_protocol", 24, 4, 0},
    field_entry{"reliable" , "bool", 28, 1, 0},
    field_entry{"ordered" , "bool", 29, 1, 0},
    field_entry{"message_oriented" , "bool", 30, 1, 0},
    field_entry{"mtu" , "u32", 32, 4, 0},
    field_entry{"backpressure" , "bool", 36, 1, 0},
};
inline constexpr std::array<field_entry, 4> refrain_services_transport_transport_event_fields{
    field_entry{"kind" , "refrain::services::transport::transport_event_kind", 4, 4, 0},
    field_entry{"connection" , "u64", 8, 8, 0},
    field_entry{"data" , "bytes", 16, 16, 0},
    field_entry{"error" , "string", 32, 16, 0},
};
inline constexpr std::array<enum_value_entry, 7> refrain_services_world_world_error_values{
    enum_value_entry{"chunk_not_loaded" , 1},
    enum_value_entry{"out_of_bounds" , 2},
    enum_value_entry{"not_implemented" , 3},
    enum_value_entry{"internal" , 4},
    enum_value_entry{"already_open" , 5},
    enum_value_entry{"open_failed" , 6},
    enum_value_entry{"storage_unavailable" , 7},
};
inline constexpr std::array<field_entry, 2> refrain_services_world_world_config_fields{
    field_entry{"storage" , "refrain::services::storage::storage_handle", 8, 56, 0},
    field_entry{"seed" , "i64", 64, 8, 0},
};
inline constexpr std::array<enum_value_entry, 4> refrain_services_worldgen_worldgen_error_values{
    enum_value_entry{"invalid_config" , 1},
    enum_value_entry{"cancelled" , 2},
    enum_value_entry{"provider_unavailable" , 3},
    enum_value_entry{"internal" , 4},
};
inline constexpr std::array<enum_value_entry, 6> refrain_services_worldgen_gen_stage_values{
    enum_value_entry{"empty" , 0},
    enum_value_entry{"biomes" , 1},
    enum_value_entry{"noise" , 2},
    enum_value_entry{"surface" , 3},
    enum_value_entry{"features" , 4},
    enum_value_entry{"full" , 5},
};
inline constexpr std::array<field_entry, 4> refrain_services_worldgen_worldgen_config_fields{
    field_entry{"seed" , "u64", 8, 8, 0},
    field_entry{"view_distance" , "i32", 16, 4, 0},
    field_entry{"flags" , "u32", 20, 4, 0},
    field_entry{"preset" , "string", 24, 16, 0},
};
inline constexpr std::array<field_entry, 3> refrain_services_worldgen_generate_outcome_fields{
    field_entry{"stage" , "refrain::services::worldgen::gen_stage", 4, 4, 0},
    field_entry{"generated_blocks" , "u32", 8, 4, 0},
    field_entry{"elapsed_us" , "u64", 16, 8, 0},
};
} // namespace detail

inline constexpr std::array<type_entry, 70> types{
    type_entry{"refrain::core::core_error", 1, 4, 4, std::span<const field_entry>{}, std::span<const enum_value_entry>{detail::refrain_core_core_error_values}},
    type_entry{"refrain::core::chunk_status", 1, 4, 4, std::span<const field_entry>{}, std::span<const enum_value_entry>{detail::refrain_core_chunk_status_values}},
    type_entry{"refrain::core::block_face", 1, 1, 1, std::span<const field_entry>{}, std::span<const enum_value_entry>{detail::refrain_core_block_face_values}},
    type_entry{"refrain::core::chunk_pos", 0, 12, 4, std::span<const field_entry>{detail::refrain_core_chunk_pos_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::core::block_pos", 0, 16, 4, std::span<const field_entry>{detail::refrain_core_block_pos_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::core::chunk_section_view", 0, 32, 8, std::span<const field_entry>{detail::refrain_core_chunk_section_view_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::core::chunk_view", 0, 40, 8, std::span<const field_entry>{detail::refrain_core_chunk_view_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::core::block_change", 0, 28, 4, std::span<const field_entry>{detail::refrain_core_block_change_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::core::chunk_buffer", 0, 40, 8, std::span<const field_entry>{detail::refrain_core_chunk_buffer_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::host::core::host_error", 1, 4, 4, std::span<const field_entry>{}, std::span<const enum_value_entry>{detail::refrain_host_core_host_error_values}},
    type_entry{"refrain::host::core::host_version", 0, 16, 4, std::span<const field_entry>{detail::refrain_host_core_host_version_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::host::core::provider_ref", 0, 32, 8, std::span<const field_entry>{detail::refrain_host_core_provider_ref_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::host::kernel::kernel_error", 1, 4, 4, std::span<const field_entry>{}, std::span<const enum_value_entry>{detail::refrain_host_kernel_kernel_error_values}},
    type_entry{"refrain::host::metadata::metadata_error", 1, 4, 4, std::span<const field_entry>{}, std::span<const enum_value_entry>{detail::refrain_host_metadata_metadata_error_values}},
    type_entry{"refrain::host::metadata::type_kind", 1, 4, 4, std::span<const field_entry>{}, std::span<const enum_value_entry>{detail::refrain_host_metadata_type_kind_values}},
    type_entry{"refrain::host::metadata::type_info", 0, 48, 8, std::span<const field_entry>{detail::refrain_host_metadata_type_info_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::host::metadata::field_info", 0, 56, 8, std::span<const field_entry>{detail::refrain_host_metadata_field_info_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::host::metadata::enum_value_info", 0, 32, 8, std::span<const field_entry>{detail::refrain_host_metadata_enum_value_info_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::host::metadata::contract_info", 0, 64, 8, std::span<const field_entry>{detail::refrain_host_metadata_contract_info_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::host::platform::platform_error", 1, 4, 4, std::span<const field_entry>{}, std::span<const enum_value_entry>{detail::refrain_host_platform_platform_error_values}},
    type_entry{"refrain::host::platform::file_open_flags", 2, 4, 4, std::span<const field_entry>{}, std::span<const enum_value_entry>{detail::refrain_host_platform_file_open_flags_values}},
    type_entry{"refrain::services::data_packs::pack_kind", 1, 4, 4, std::span<const field_entry>{}, std::span<const enum_value_entry>{detail::refrain_services_data_packs_pack_kind_values}},
    type_entry{"refrain::services::data_packs::pack_error", 1, 4, 4, std::span<const field_entry>{}, std::span<const enum_value_entry>{detail::refrain_services_data_packs_pack_error_values}},
    type_entry{"refrain::services::data_packs::pack_source", 0, 32, 8, std::span<const field_entry>{detail::refrain_services_data_packs_pack_source_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::services::data_packs::pack_info", 0, 64, 8, std::span<const field_entry>{detail::refrain_services_data_packs_pack_info_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::services::net_protocol::protocol_edition", 1, 4, 4, std::span<const field_entry>{}, std::span<const enum_value_entry>{detail::refrain_services_net_protocol_protocol_edition_values}},
    type_entry{"refrain::services::net_protocol::protocol_error", 1, 4, 4, std::span<const field_entry>{}, std::span<const enum_value_entry>{detail::refrain_services_net_protocol_protocol_error_values}},
    type_entry{"refrain::services::net_protocol::protocol_event", 1, 4, 4, std::span<const field_entry>{}, std::span<const enum_value_entry>{detail::refrain_services_net_protocol_protocol_event_values}},
    type_entry{"refrain::services::net_protocol::connection_state", 1, 4, 4, std::span<const field_entry>{}, std::span<const enum_value_entry>{detail::refrain_services_net_protocol_connection_state_values}},
    type_entry{"refrain::services::net_protocol::protocol_descriptor", 0, 72, 8, std::span<const field_entry>{detail::refrain_services_net_protocol_protocol_descriptor_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::services::net_protocol::frame_chunk", 0, 24, 8, std::span<const field_entry>{detail::refrain_services_net_protocol_frame_chunk_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::services::net_protocol::decoded_packet", 0, 40, 8, std::span<const field_entry>{detail::refrain_services_net_protocol_decoded_packet_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::services::net_protocol::protocol_version", 0, 24, 8, std::span<const field_entry>{detail::refrain_services_net_protocol_protocol_version_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::services::net_protocol::frame_header", 0, 12, 4, std::span<const field_entry>{detail::refrain_services_net_protocol_frame_header_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::services::net_protocol::packet_view", 0, 24, 8, std::span<const field_entry>{detail::refrain_services_net_protocol_packet_view_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::services::net_protocol::encoded_packet", 0, 24, 8, std::span<const field_entry>{detail::refrain_services_net_protocol_encoded_packet_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::services::net_protocol::connection_ref", 0, 16, 8, std::span<const field_entry>{detail::refrain_services_net_protocol_connection_ref_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::services::net_session::session_error", 1, 4, 4, std::span<const field_entry>{}, std::span<const enum_value_entry>{detail::refrain_services_net_session_session_error_values}},
    type_entry{"refrain::services::net_session::session_profile", 0, 56, 8, std::span<const field_entry>{detail::refrain_services_net_session_session_profile_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::services::net_session::menu_click_result", 0, 64, 8, std::span<const field_entry>{detail::refrain_services_net_session_menu_click_result_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::services::net_translator::translation_error", 1, 4, 4, std::span<const field_entry>{}, std::span<const enum_value_entry>{detail::refrain_services_net_translator_translation_error_values}},
    type_entry{"refrain::services::net_translator::translator_descriptor", 0, 72, 8, std::span<const field_entry>{detail::refrain_services_net_translator_translator_descriptor_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::services::net_translator::translation_request", 0, 48, 8, std::span<const field_entry>{detail::refrain_services_net_translator_translation_request_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::services::net_translator::translation_result", 0, 24, 8, std::span<const field_entry>{detail::refrain_services_net_translator_translation_result_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::services::runtime::runtime_error", 1, 4, 4, std::span<const field_entry>{}, std::span<const enum_value_entry>{detail::refrain_services_runtime_runtime_error_values}},
    type_entry{"refrain::services::runtime::runtime_config", 0, 184, 8, std::span<const field_entry>{detail::refrain_services_runtime_runtime_config_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::services::session_lifecycle::player_info", 0, 48, 8, std::span<const field_entry>{detail::refrain_services_session_lifecycle_player_info_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::services::session_status::status_request", 0, 32, 8, std::span<const field_entry>{detail::refrain_services_session_status_status_request_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::services::session_status::status_view", 0, 32, 8, std::span<const field_entry>{detail::refrain_services_session_status_status_view_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::services::session_tab::tab_request", 0, 32, 8, std::span<const field_entry>{detail::refrain_services_session_tab_tab_request_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::services::session_tab::tab_view", 0, 40, 8, std::span<const field_entry>{detail::refrain_services_session_tab_tab_view_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::services::sim::sim_error", 1, 4, 4, std::span<const field_entry>{}, std::span<const enum_value_entry>{detail::refrain_services_sim_sim_error_values}},
    type_entry{"refrain::services::sim::sim_config", 0, 72, 8, std::span<const field_entry>{detail::refrain_services_sim_sim_config_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::services::storage::storage_error", 1, 4, 4, std::span<const field_entry>{}, std::span<const enum_value_entry>{detail::refrain_services_storage_storage_error_values}},
    type_entry{"refrain::services::storage::world_open_config", 0, 48, 8, std::span<const field_entry>{detail::refrain_services_storage_world_open_config_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::services::storage::storage_handle", 0, 56, 8, std::span<const field_entry>{detail::refrain_services_storage_storage_handle_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::services::storage::chunk_key", 0, 20, 4, std::span<const field_entry>{detail::refrain_services_storage_chunk_key_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::services::storage::chunk_read_result", 0, 24, 8, std::span<const field_entry>{detail::refrain_services_storage_chunk_read_result_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::services::transport::transport_error", 1, 4, 4, std::span<const field_entry>{}, std::span<const enum_value_entry>{detail::refrain_services_transport_transport_error_values}},
    type_entry{"refrain::services::transport::transport_protocol", 1, 4, 4, std::span<const field_entry>{}, std::span<const enum_value_entry>{detail::refrain_services_transport_transport_protocol_values}},
    type_entry{"refrain::services::transport::transport_event_kind", 1, 4, 4, std::span<const field_entry>{}, std::span<const enum_value_entry>{detail::refrain_services_transport_transport_event_kind_values}},
    type_entry{"refrain::services::transport::transport_endpoint", 0, 32, 8, std::span<const field_entry>{detail::refrain_services_transport_transport_endpoint_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::services::transport::transport_descriptor", 0, 40, 8, std::span<const field_entry>{detail::refrain_services_transport_transport_descriptor_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::services::transport::transport_event", 0, 48, 8, std::span<const field_entry>{detail::refrain_services_transport_transport_event_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::services::world::world_error", 1, 4, 4, std::span<const field_entry>{}, std::span<const enum_value_entry>{detail::refrain_services_world_world_error_values}},
    type_entry{"refrain::services::world::world_config", 0, 72, 8, std::span<const field_entry>{detail::refrain_services_world_world_config_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::services::worldgen::worldgen_error", 1, 4, 4, std::span<const field_entry>{}, std::span<const enum_value_entry>{detail::refrain_services_worldgen_worldgen_error_values}},
    type_entry{"refrain::services::worldgen::gen_stage", 1, 4, 4, std::span<const field_entry>{}, std::span<const enum_value_entry>{detail::refrain_services_worldgen_gen_stage_values}},
    type_entry{"refrain::services::worldgen::worldgen_config", 0, 40, 8, std::span<const field_entry>{detail::refrain_services_worldgen_worldgen_config_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::services::worldgen::generate_outcome", 0, 24, 8, std::span<const field_entry>{detail::refrain_services_worldgen_generate_outcome_fields}, std::span<const enum_value_entry>{}},
};

inline constexpr std::array<contract_entry, 17> contracts{
    contract_entry{"refrain:host.core", 1, 0, 0, "996e38144c4577ab", 12},
    contract_entry{"refrain:host.kernel", 1, 0, 0, "996e38144c4577ab", 6},
    contract_entry{"refrain:host.metadata", 1, 0, 0, "996e38144c4577ab", 8},
    contract_entry{"refrain:host.platform", 1, 0, 0, "996e38144c4577ab", 12},
    contract_entry{"refrain:data.packs", 1, 0, 0, "996e38144c4577ab", 4},
    contract_entry{"refrain:net.protocol", 2, 1, 0, "996e38144c4577ab", 14},
    contract_entry{"refrain:net.session", 1, 0, 0, "996e38144c4577ab", 6},
    contract_entry{"refrain:net.translator", 1, 0, 0, "996e38144c4577ab", 2},
    contract_entry{"refrain:runtime", 2, 0, 0, "996e38144c4577ab", 1},
    contract_entry{"refrain:session.lifecycle", 1, 0, 0, "996e38144c4577ab", 3},
    contract_entry{"refrain:session.status", 1, 0, 0, "996e38144c4577ab", 1},
    contract_entry{"refrain:session.tab", 1, 0, 0, "996e38144c4577ab", 1},
    contract_entry{"refrain:sim", 1, 0, 0, "996e38144c4577ab", 8},
    contract_entry{"refrain:storage", 1, 0, 0, "996e38144c4577ab", 6},
    contract_entry{"refrain:transport", 1, 0, 0, "996e38144c4577ab", 9},
    contract_entry{"refrain:world", 1, 0, 0, "996e38144c4577ab", 8},
    contract_entry{"refrain:worldgen", 1, 0, 0, "996e38144c4577ab", 4},
};

inline constexpr std::string_view schema_hash = "996e38144c4577ab";

} // namespace refrain::metadata

#endif // REFRAIN_GENERATED_CPP_REFRAIN_METADATA_HPP
