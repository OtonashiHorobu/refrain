// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 3a0899b8b7e5f861

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
inline constexpr std::array<enum_value_entry, 5> refrain_services_net_protocol_protocol_error_values{
    enum_value_entry{"frame_too_large" , 1},
    enum_value_entry{"malformed_packet" , 2},
    enum_value_entry{"unknown_packet" , 3},
    enum_value_entry{"unsupported_version" , 4},
    enum_value_entry{"compression_failed" , 5},
};
inline constexpr std::array<enum_value_entry, 5> refrain_services_net_protocol_connection_state_values{
    enum_value_entry{"handshake" , 0},
    enum_value_entry{"status" , 1},
    enum_value_entry{"login" , 2},
    enum_value_entry{"configuration" , 3},
    enum_value_entry{"play" , 4},
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
inline constexpr std::array<enum_value_entry, 3> refrain_services_runtime_runtime_error_values{
    enum_value_entry{"invalid_config" , 1},
    enum_value_entry{"io_error" , 2},
    enum_value_entry{"internal" , 3},
};
inline constexpr std::array<field_entry, 23> refrain_services_runtime_runtime_config_fields{
    field_entry{"world_path" , "string", 8, 16, 0},
    field_entry{"server_properties" , "string", 24, 16, 0},
    field_entry{"ticks" , "i64", 40, 8, 0},
    field_entry{"command_file" , "string", 48, 16, 0},
    field_entry{"script_file" , "string", 64, 16, 0},
    field_entry{"hash_every" , "u64", 80, 8, 0},
    field_entry{"world_zstd_level" , "i32", 88, 4, 0},
    field_entry{"zstd_near_level" , "i32", 92, 4, 0},
    field_entry{"zstd_mid_level" , "i32", 96, 4, 0},
    field_entry{"zstd_far_level" , "i32", 100, 4, 0},
    field_entry{"zstd_near_radius" , "i32", 104, 4, 0},
    field_entry{"zstd_mid_radius" , "i32", 108, 4, 0},
    field_entry{"watchdog_timeout_ms" , "u32", 112, 4, 0},
    field_entry{"autosave_interval_ticks" , "u64", 120, 8, 0},
    field_entry{"compact_interval_ticks" , "u64", 128, 8, 0},
    field_entry{"compact_min_bytes" , "u64", 136, 8, 0},
    field_entry{"compact_garbage_percent" , "u32", 144, 4, 0},
    field_entry{"store_compact_interval_ticks" , "u64", 152, 8, 0},
    field_entry{"content_store" , "string", 160, 16, 0},
    field_entry{"content_gc_apply" , "bool", 176, 1, 0},
    field_entry{"content_gc_keep_days" , "i32", 180, 4, 0},
    field_entry{"content_gc_budget_ms" , "u64", 184, 8, 0},
    field_entry{"content_gc_interval_ticks" , "u64", 192, 8, 0},
};
inline constexpr std::array<enum_value_entry, 5> refrain_services_sim_sim_error_values{
    enum_value_entry{"invalid_handle" , 1},
    enum_value_entry{"already_open" , 2},
    enum_value_entry{"not_found" , 3},
    enum_value_entry{"io_error" , 4},
    enum_value_entry{"internal" , 5},
};
inline constexpr std::array<field_entry, 2> refrain_services_sim_sim_config_fields{
    field_entry{"world_path" , "string", 8, 16, 0},
    field_entry{"seed" , "i64", 24, 8, 0},
};
inline constexpr std::array<enum_value_entry, 6> refrain_services_storage_storage_error_values{
    enum_value_entry{"open_failed" , 1},
    enum_value_entry{"not_found" , 2},
    enum_value_entry{"corrupt_data" , 3},
    enum_value_entry{"io_error" , 4},
    enum_value_entry{"version_mismatch" , 5},
    enum_value_entry{"read_only" , 6},
};
inline constexpr std::array<field_entry, 3> refrain_services_storage_world_open_config_fields{
    field_entry{"world_path" , "string", 8, 16, 0},
    field_entry{"gamepack_id" , "string", 24, 16, 0},
    field_entry{"flags" , "u32", 40, 4, 0},
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
inline constexpr std::array<enum_value_entry, 6> refrain_services_world_world_error_values{
    enum_value_entry{"chunk_not_loaded" , 1},
    enum_value_entry{"out_of_bounds" , 2},
    enum_value_entry{"not_implemented" , 3},
    enum_value_entry{"internal" , 4},
    enum_value_entry{"already_open" , 5},
    enum_value_entry{"open_failed" , 6},
};
inline constexpr std::array<field_entry, 2> refrain_services_world_world_config_fields{
    field_entry{"world_path" , "string", 8, 16, 0},
    field_entry{"seed" , "i64", 24, 8, 0},
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

inline constexpr std::array<type_entry, 44> types{
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
    type_entry{"refrain::services::net_protocol::protocol_error", 1, 4, 4, std::span<const field_entry>{}, std::span<const enum_value_entry>{detail::refrain_services_net_protocol_protocol_error_values}},
    type_entry{"refrain::services::net_protocol::connection_state", 1, 4, 4, std::span<const field_entry>{}, std::span<const enum_value_entry>{detail::refrain_services_net_protocol_connection_state_values}},
    type_entry{"refrain::services::net_protocol::protocol_version", 0, 24, 8, std::span<const field_entry>{detail::refrain_services_net_protocol_protocol_version_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::services::net_protocol::frame_header", 0, 12, 4, std::span<const field_entry>{detail::refrain_services_net_protocol_frame_header_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::services::net_protocol::packet_view", 0, 24, 8, std::span<const field_entry>{detail::refrain_services_net_protocol_packet_view_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::services::runtime::runtime_error", 1, 4, 4, std::span<const field_entry>{}, std::span<const enum_value_entry>{detail::refrain_services_runtime_runtime_error_values}},
    type_entry{"refrain::services::runtime::runtime_config", 0, 200, 8, std::span<const field_entry>{detail::refrain_services_runtime_runtime_config_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::services::sim::sim_error", 1, 4, 4, std::span<const field_entry>{}, std::span<const enum_value_entry>{detail::refrain_services_sim_sim_error_values}},
    type_entry{"refrain::services::sim::sim_config", 0, 32, 8, std::span<const field_entry>{detail::refrain_services_sim_sim_config_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::services::storage::storage_error", 1, 4, 4, std::span<const field_entry>{}, std::span<const enum_value_entry>{detail::refrain_services_storage_storage_error_values}},
    type_entry{"refrain::services::storage::world_open_config", 0, 48, 8, std::span<const field_entry>{detail::refrain_services_storage_world_open_config_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::services::storage::chunk_key", 0, 20, 4, std::span<const field_entry>{detail::refrain_services_storage_chunk_key_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::services::storage::chunk_read_result", 0, 24, 8, std::span<const field_entry>{detail::refrain_services_storage_chunk_read_result_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::services::world::world_error", 1, 4, 4, std::span<const field_entry>{}, std::span<const enum_value_entry>{detail::refrain_services_world_world_error_values}},
    type_entry{"refrain::services::world::world_config", 0, 32, 8, std::span<const field_entry>{detail::refrain_services_world_world_config_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::services::worldgen::worldgen_error", 1, 4, 4, std::span<const field_entry>{}, std::span<const enum_value_entry>{detail::refrain_services_worldgen_worldgen_error_values}},
    type_entry{"refrain::services::worldgen::gen_stage", 1, 4, 4, std::span<const field_entry>{}, std::span<const enum_value_entry>{detail::refrain_services_worldgen_gen_stage_values}},
    type_entry{"refrain::services::worldgen::worldgen_config", 0, 40, 8, std::span<const field_entry>{detail::refrain_services_worldgen_worldgen_config_fields}, std::span<const enum_value_entry>{}},
    type_entry{"refrain::services::worldgen::generate_outcome", 0, 24, 8, std::span<const field_entry>{detail::refrain_services_worldgen_generate_outcome_fields}, std::span<const enum_value_entry>{}},
};

inline constexpr std::array<contract_entry, 11> contracts{
    contract_entry{"refrain:host.core", 1, 0, 0, "3a0899b8b7e5f861", 12},
    contract_entry{"refrain:host.kernel", 1, 0, 0, "3a0899b8b7e5f861", 6},
    contract_entry{"refrain:host.metadata", 1, 0, 0, "3a0899b8b7e5f861", 8},
    contract_entry{"refrain:host.platform", 1, 0, 0, "3a0899b8b7e5f861", 12},
    contract_entry{"refrain:data.packs", 1, 0, 0, "3a0899b8b7e5f861", 4},
    contract_entry{"refrain:net.protocol", 1, 0, 0, "3a0899b8b7e5f861", 5},
    contract_entry{"refrain:runtime", 1, 0, 0, "3a0899b8b7e5f861", 1},
    contract_entry{"refrain:sim", 1, 0, 0, "3a0899b8b7e5f861", 5},
    contract_entry{"refrain:storage", 1, 0, 0, "3a0899b8b7e5f861", 6},
    contract_entry{"refrain:world", 1, 0, 0, "3a0899b8b7e5f861", 8},
    contract_entry{"refrain:worldgen", 1, 0, 0, "3a0899b8b7e5f861", 4},
};

inline constexpr std::string_view schema_hash = "3a0899b8b7e5f861";

} // namespace refrain::metadata

#endif // REFRAIN_GENERATED_CPP_REFRAIN_METADATA_HPP
