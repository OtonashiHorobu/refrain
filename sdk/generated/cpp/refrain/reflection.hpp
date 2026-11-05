// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 21bfdf943ae85aa9

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_REFLECTION_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_REFLECTION_HPP

#if defined(REFRAIN_USE_REFLECTION) && REFRAIN_USE_REFLECTION

// Optional reflection metadata over the generated C++ model: one
// custom_traits<T> specialization per generated struct/record (RSC
// + RIDL) with the qualified type name and the schema fields in
// declaration order. ABI-only members (`struct_size`, `reserved_N`
// padding) are not schema fields, so they are not reflected. With
// REFRAIN_USE_REFLECTION disabled this header is empty; normal
// consumers of the generated model never depend on refl-cpp.

#include <refrain/contracts.hpp>
#include <refrain/schema/storage/chunk.hpp>

#include <string_view>
#include <tuple>

#include <refrain/reflect.hpp>

namespace refrain::reflect {

/// Generated RIDL struct 'refrain::core::chunk_pos'.
template <> struct custom_traits<::refrain::core::chunk_pos> {
    static constexpr std::string_view type_name = "refrain::core::chunk_pos";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"x", &::refrain::core::chunk_pos::x},
        refrain::reflect::custom_field{"z", &::refrain::core::chunk_pos::z});
};

/// Generated RIDL struct 'refrain::core::block_pos'.
template <> struct custom_traits<::refrain::core::block_pos> {
    static constexpr std::string_view type_name = "refrain::core::block_pos";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"x", &::refrain::core::block_pos::x},
        refrain::reflect::custom_field{"y", &::refrain::core::block_pos::y},
        refrain::reflect::custom_field{"z", &::refrain::core::block_pos::z});
};

/// Generated RIDL struct 'refrain::core::chunk_section_view'.
template <> struct custom_traits<::refrain::core::chunk_section_view> {
    static constexpr std::string_view type_name = "refrain::core::chunk_section_view";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"section_y", &::refrain::core::chunk_section_view::section_y},
        refrain::reflect::custom_field{"block_count", &::refrain::core::chunk_section_view::block_count},
        refrain::reflect::custom_field{"bits_per_entry", &::refrain::core::chunk_section_view::bits_per_entry},
        refrain::reflect::custom_field{"flags", &::refrain::core::chunk_section_view::flags},
        refrain::reflect::custom_field{"block_entities", &::refrain::core::chunk_section_view::block_entities});
};

/// Generated RIDL struct 'refrain::core::chunk_view'.
template <> struct custom_traits<::refrain::core::chunk_view> {
    static constexpr std::string_view type_name = "refrain::core::chunk_view";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"pos", &::refrain::core::chunk_view::pos},
        refrain::reflect::custom_field{"status", &::refrain::core::chunk_view::status},
        refrain::reflect::custom_field{"section_count", &::refrain::core::chunk_view::section_count},
        refrain::reflect::custom_field{"sections", &::refrain::core::chunk_view::sections});
};

/// Generated RIDL struct 'refrain::core::block_change'.
template <> struct custom_traits<::refrain::core::block_change> {
    static constexpr std::string_view type_name = "refrain::core::block_change";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"pos", &::refrain::core::block_change::pos},
        refrain::reflect::custom_field{"old_state", &::refrain::core::block_change::old_state},
        refrain::reflect::custom_field{"new_state", &::refrain::core::block_change::new_state});
};

/// Generated RIDL struct 'refrain::core::chunk_buffer'.
template <> struct custom_traits<::refrain::core::chunk_buffer> {
    static constexpr std::string_view type_name = "refrain::core::chunk_buffer";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"pos", &::refrain::core::chunk_buffer::pos},
        refrain::reflect::custom_field{"generation", &::refrain::core::chunk_buffer::generation},
        refrain::reflect::custom_field{"word_count", &::refrain::core::chunk_buffer::word_count},
        refrain::reflect::custom_field{"data", &::refrain::core::chunk_buffer::data});
};

/// Generated RIDL struct 'refrain::services::data_packs::pack_source'.
template <> struct custom_traits<::refrain::services::data_packs::pack_source> {
    static constexpr std::string_view type_name = "refrain::services::data_packs::pack_source";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"path", &::refrain::services::data_packs::pack_source::path},
        refrain::reflect::custom_field{"kind", &::refrain::services::data_packs::pack_source::kind},
        refrain::reflect::custom_field{"pack_format", &::refrain::services::data_packs::pack_source::pack_format});
};

/// Generated RIDL struct 'refrain::services::data_packs::pack_info'.
template <> struct custom_traits<::refrain::services::data_packs::pack_info> {
    static constexpr std::string_view type_name = "refrain::services::data_packs::pack_info";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"pack", &::refrain::services::data_packs::pack_info::pack},
        refrain::reflect::custom_field{"id", &::refrain::services::data_packs::pack_info::id},
        refrain::reflect::custom_field{"version", &::refrain::services::data_packs::pack_info::version},
        refrain::reflect::custom_field{"pack_format", &::refrain::services::data_packs::pack_info::pack_format},
        refrain::reflect::custom_field{"file_count", &::refrain::services::data_packs::pack_info::file_count});
};

/// Generated RIDL struct 'refrain::services::net_protocol::protocol_version'.
template <> struct custom_traits<::refrain::services::net_protocol::protocol_version> {
    static constexpr std::string_view type_name = "refrain::services::net_protocol::protocol_version";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"protocol", &::refrain::services::net_protocol::protocol_version::protocol},
        refrain::reflect::custom_field{"name", &::refrain::services::net_protocol::protocol_version::name});
};

/// Generated RIDL struct 'refrain::services::net_protocol::frame_header'.
template <> struct custom_traits<::refrain::services::net_protocol::frame_header> {
    static constexpr std::string_view type_name = "refrain::services::net_protocol::frame_header";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"length", &::refrain::services::net_protocol::frame_header::length},
        refrain::reflect::custom_field{"uncompressed_length", &::refrain::services::net_protocol::frame_header::uncompressed_length});
};

/// Generated RIDL struct 'refrain::services::net_protocol::packet_view'.
template <> struct custom_traits<::refrain::services::net_protocol::packet_view> {
    static constexpr std::string_view type_name = "refrain::services::net_protocol::packet_view";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"packet_id", &::refrain::services::net_protocol::packet_view::packet_id},
        refrain::reflect::custom_field{"payload", &::refrain::services::net_protocol::packet_view::payload});
};

/// Generated RIDL struct 'refrain::services::runtime::runtime_config'.
template <> struct custom_traits<::refrain::services::runtime::runtime_config> {
    static constexpr std::string_view type_name = "refrain::services::runtime::runtime_config";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"world_path", &::refrain::services::runtime::runtime_config::world_path},
        refrain::reflect::custom_field{"server_properties", &::refrain::services::runtime::runtime_config::server_properties},
        refrain::reflect::custom_field{"ticks", &::refrain::services::runtime::runtime_config::ticks},
        refrain::reflect::custom_field{"command_file", &::refrain::services::runtime::runtime_config::command_file},
        refrain::reflect::custom_field{"script_file", &::refrain::services::runtime::runtime_config::script_file},
        refrain::reflect::custom_field{"hash_every", &::refrain::services::runtime::runtime_config::hash_every},
        refrain::reflect::custom_field{"world_zstd_level", &::refrain::services::runtime::runtime_config::world_zstd_level},
        refrain::reflect::custom_field{"zstd_near_level", &::refrain::services::runtime::runtime_config::zstd_near_level},
        refrain::reflect::custom_field{"zstd_mid_level", &::refrain::services::runtime::runtime_config::zstd_mid_level},
        refrain::reflect::custom_field{"zstd_far_level", &::refrain::services::runtime::runtime_config::zstd_far_level},
        refrain::reflect::custom_field{"zstd_near_radius", &::refrain::services::runtime::runtime_config::zstd_near_radius},
        refrain::reflect::custom_field{"zstd_mid_radius", &::refrain::services::runtime::runtime_config::zstd_mid_radius},
        refrain::reflect::custom_field{"watchdog_timeout_ms", &::refrain::services::runtime::runtime_config::watchdog_timeout_ms},
        refrain::reflect::custom_field{"autosave_interval_ticks", &::refrain::services::runtime::runtime_config::autosave_interval_ticks},
        refrain::reflect::custom_field{"compact_interval_ticks", &::refrain::services::runtime::runtime_config::compact_interval_ticks},
        refrain::reflect::custom_field{"compact_min_bytes", &::refrain::services::runtime::runtime_config::compact_min_bytes},
        refrain::reflect::custom_field{"compact_garbage_percent", &::refrain::services::runtime::runtime_config::compact_garbage_percent},
        refrain::reflect::custom_field{"store_compact_interval_ticks", &::refrain::services::runtime::runtime_config::store_compact_interval_ticks},
        refrain::reflect::custom_field{"content_store", &::refrain::services::runtime::runtime_config::content_store},
        refrain::reflect::custom_field{"content_gc_apply", &::refrain::services::runtime::runtime_config::content_gc_apply},
        refrain::reflect::custom_field{"content_gc_keep_days", &::refrain::services::runtime::runtime_config::content_gc_keep_days},
        refrain::reflect::custom_field{"content_gc_budget_ms", &::refrain::services::runtime::runtime_config::content_gc_budget_ms},
        refrain::reflect::custom_field{"content_gc_interval_ticks", &::refrain::services::runtime::runtime_config::content_gc_interval_ticks});
};

/// Generated RIDL struct 'refrain::services::sim::sim_config'.
template <> struct custom_traits<::refrain::services::sim::sim_config> {
    static constexpr std::string_view type_name = "refrain::services::sim::sim_config";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"world_path", &::refrain::services::sim::sim_config::world_path},
        refrain::reflect::custom_field{"seed", &::refrain::services::sim::sim_config::seed});
};

/// Generated RIDL struct 'refrain::services::storage::world_open_config'.
template <> struct custom_traits<::refrain::services::storage::world_open_config> {
    static constexpr std::string_view type_name = "refrain::services::storage::world_open_config";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"world_path", &::refrain::services::storage::world_open_config::world_path},
        refrain::reflect::custom_field{"gamepack_id", &::refrain::services::storage::world_open_config::gamepack_id},
        refrain::reflect::custom_field{"flags", &::refrain::services::storage::world_open_config::flags});
};

/// Generated RIDL struct 'refrain::services::storage::chunk_key'.
template <> struct custom_traits<::refrain::services::storage::chunk_key> {
    static constexpr std::string_view type_name = "refrain::services::storage::chunk_key";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"pos", &::refrain::services::storage::chunk_key::pos},
        refrain::reflect::custom_field{"format_version", &::refrain::services::storage::chunk_key::format_version});
};

/// Generated RIDL struct 'refrain::services::storage::chunk_read_result'.
template <> struct custom_traits<::refrain::services::storage::chunk_read_result> {
    static constexpr std::string_view type_name = "refrain::services::storage::chunk_read_result";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"bytes_read", &::refrain::services::storage::chunk_read_result::bytes_read},
        refrain::reflect::custom_field{"format_version", &::refrain::services::storage::chunk_read_result::format_version},
        refrain::reflect::custom_field{"record_version", &::refrain::services::storage::chunk_read_result::record_version});
};

/// Generated RIDL struct 'refrain::services::world::world_config'.
template <> struct custom_traits<::refrain::services::world::world_config> {
    static constexpr std::string_view type_name = "refrain::services::world::world_config";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"world_path", &::refrain::services::world::world_config::world_path},
        refrain::reflect::custom_field{"seed", &::refrain::services::world::world_config::seed});
};

/// Generated RIDL struct 'refrain::services::worldgen::worldgen_config'.
template <> struct custom_traits<::refrain::services::worldgen::worldgen_config> {
    static constexpr std::string_view type_name = "refrain::services::worldgen::worldgen_config";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"seed", &::refrain::services::worldgen::worldgen_config::seed},
        refrain::reflect::custom_field{"view_distance", &::refrain::services::worldgen::worldgen_config::view_distance},
        refrain::reflect::custom_field{"flags", &::refrain::services::worldgen::worldgen_config::flags},
        refrain::reflect::custom_field{"preset", &::refrain::services::worldgen::worldgen_config::preset});
};

/// Generated RIDL struct 'refrain::services::worldgen::generate_outcome'.
template <> struct custom_traits<::refrain::services::worldgen::generate_outcome> {
    static constexpr std::string_view type_name = "refrain::services::worldgen::generate_outcome";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"stage", &::refrain::services::worldgen::generate_outcome::stage},
        refrain::reflect::custom_field{"generated_blocks", &::refrain::services::worldgen::generate_outcome::generated_blocks},
        refrain::reflect::custom_field{"elapsed_us", &::refrain::services::worldgen::generate_outcome::elapsed_us});
};

/// Generated RSC struct 'refrain::storage::block_pos'.
template <> struct custom_traits<::refrain::storage::block_pos> {
    static constexpr std::string_view type_name = "refrain::storage::block_pos";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"x", &::refrain::storage::block_pos::x},
        refrain::reflect::custom_field{"y", &::refrain::storage::block_pos::y},
        refrain::reflect::custom_field{"z", &::refrain::storage::block_pos::z});
};

/// Generated RSC struct 'refrain::storage::palette_entry'.
template <> struct custom_traits<::refrain::storage::palette_entry> {
    static constexpr std::string_view type_name = "refrain::storage::palette_entry";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"key", &::refrain::storage::palette_entry::key},
        refrain::reflect::custom_field{"count", &::refrain::storage::palette_entry::count},
        refrain::reflect::custom_field{"state", &::refrain::storage::palette_entry::state});
};

/// Generated RSC struct 'refrain::storage::section_record'.
template <> struct custom_traits<::refrain::storage::section_record> {
    static constexpr std::string_view type_name = "refrain::storage::section_record";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"y", &::refrain::storage::section_record::y},
        refrain::reflect::custom_field{"block_count", &::refrain::storage::section_record::block_count},
        refrain::reflect::custom_field{"kind", &::refrain::storage::section_record::kind},
        refrain::reflect::custom_field{"bits_per_entry", &::refrain::storage::section_record::bits_per_entry},
        refrain::reflect::custom_field{"palette", &::refrain::storage::section_record::palette},
        refrain::reflect::custom_field{"data", &::refrain::storage::section_record::data},
        refrain::reflect::custom_field{"biome_kind", &::refrain::storage::section_record::biome_kind},
        refrain::reflect::custom_field{"biome_bits_per_entry", &::refrain::storage::section_record::biome_bits_per_entry},
        refrain::reflect::custom_field{"biome_palette", &::refrain::storage::section_record::biome_palette},
        refrain::reflect::custom_field{"biome_data", &::refrain::storage::section_record::biome_data});
};

/// Generated RSC struct 'refrain::storage::light_section'.
template <> struct custom_traits<::refrain::storage::light_section> {
    static constexpr std::string_view type_name = "refrain::storage::light_section";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"y", &::refrain::storage::light_section::y},
        refrain::reflect::custom_field{"block_light", &::refrain::storage::light_section::block_light},
        refrain::reflect::custom_field{"sky_light", &::refrain::storage::light_section::sky_light});
};

/// Generated RSC struct 'refrain::storage::heightmap_record'.
template <> struct custom_traits<::refrain::storage::heightmap_record> {
    static constexpr std::string_view type_name = "refrain::storage::heightmap_record";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"name", &::refrain::storage::heightmap_record::name},
        refrain::reflect::custom_field{"bits_per_entry", &::refrain::storage::heightmap_record::bits_per_entry},
        refrain::reflect::custom_field{"data", &::refrain::storage::heightmap_record::data});
};

/// Generated RSC struct 'refrain::storage::saved_tick'.
template <> struct custom_traits<::refrain::storage::saved_tick> {
    static constexpr std::string_view type_name = "refrain::storage::saved_tick";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"id", &::refrain::storage::saved_tick::id},
        refrain::reflect::custom_field{"pos", &::refrain::storage::saved_tick::pos},
        refrain::reflect::custom_field{"priority", &::refrain::storage::saved_tick::priority},
        refrain::reflect::custom_field{"kind", &::refrain::storage::saved_tick::kind},
        refrain::reflect::custom_field{"remaining_ticks", &::refrain::storage::saved_tick::remaining_ticks});
};

/// Generated RSC record 'refrain::storage::chunk'.
template <> struct custom_traits<::refrain::storage::chunk> {
    static constexpr std::string_view type_name = "refrain::storage::chunk";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"format_version", &::refrain::storage::chunk::format_version},
        refrain::reflect::custom_field{"status", &::refrain::storage::chunk::status},
        refrain::reflect::custom_field{"last_update", &::refrain::storage::chunk::last_update},
        refrain::reflect::custom_field{"inhabited_time", &::refrain::storage::chunk::inhabited_time},
        refrain::reflect::custom_field{"sections", &::refrain::storage::chunk::sections},
        refrain::reflect::custom_field{"heightmaps", &::refrain::storage::chunk::heightmaps},
        refrain::reflect::custom_field{"block_ticks", &::refrain::storage::chunk::block_ticks},
        refrain::reflect::custom_field{"fluid_ticks", &::refrain::storage::chunk::fluid_ticks},
        refrain::reflect::custom_field{"block_entities", &::refrain::storage::chunk::block_entities},
        refrain::reflect::custom_field{"structures", &::refrain::storage::chunk::structures},
        refrain::reflect::custom_field{"post_processing", &::refrain::storage::chunk::post_processing},
        refrain::reflect::custom_field{"blending", &::refrain::storage::chunk::blending},
        refrain::reflect::custom_field{"extra", &::refrain::storage::chunk::extra},
        refrain::reflect::custom_field{"extensions", &::refrain::storage::chunk::extensions},
        refrain::reflect::custom_field{"y_pos", &::refrain::storage::chunk::y_pos},
        refrain::reflect::custom_field{"light_sections", &::refrain::storage::chunk::light_sections});
};

} // namespace refrain::reflect

#endif // REFRAIN_USE_REFLECTION

#endif // REFRAIN_GENERATED_CPP_REFRAIN_REFLECTION_HPP
