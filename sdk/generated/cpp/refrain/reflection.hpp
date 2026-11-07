// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 37ad437ddc6c0e0a

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
#include <refrain/schema/protocol/777/common.hpp>
#include <refrain/schema/protocol/777/configuration_clientbound.hpp>
#include <refrain/schema/protocol/777/configuration_serverbound.hpp>
#include <refrain/schema/protocol/777/handshake_serverbound.hpp>
#include <refrain/schema/protocol/777/login_clientbound.hpp>
#include <refrain/schema/protocol/777/login_serverbound.hpp>
#include <refrain/schema/protocol/777/play_clientbound.hpp>
#include <refrain/schema/protocol/777/play_serverbound.hpp>
#include <refrain/schema/protocol/777/status_clientbound.hpp>
#include <refrain/schema/protocol/777/status_serverbound.hpp>
#include <refrain/schema/protocol/777/wire.hpp>
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

/// Generated RSC struct 'refrain::protocol::v777::common::known_pack'.
template <> struct custom_traits<::refrain::protocol::v777::common::known_pack> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::common::known_pack";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"pack_namespace", &::refrain::protocol::v777::common::known_pack::pack_namespace},
        refrain::reflect::custom_field{"id", &::refrain::protocol::v777::common::known_pack::id},
        refrain::reflect::custom_field{"version", &::refrain::protocol::v777::common::known_pack::version});
};

/// Generated RSC struct 'refrain::protocol::v777::common::client_information_data'.
template <> struct custom_traits<::refrain::protocol::v777::common::client_information_data> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::common::client_information_data";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"language", &::refrain::protocol::v777::common::client_information_data::language},
        refrain::reflect::custom_field{"view_distance", &::refrain::protocol::v777::common::client_information_data::view_distance},
        refrain::reflect::custom_field{"visibility", &::refrain::protocol::v777::common::client_information_data::visibility},
        refrain::reflect::custom_field{"chat_colors", &::refrain::protocol::v777::common::client_information_data::chat_colors},
        refrain::reflect::custom_field{"model_customization", &::refrain::protocol::v777::common::client_information_data::model_customization},
        refrain::reflect::custom_field{"main_hand", &::refrain::protocol::v777::common::client_information_data::main_hand},
        refrain::reflect::custom_field{"text_filtering", &::refrain::protocol::v777::common::client_information_data::text_filtering},
        refrain::reflect::custom_field{"allows_listing", &::refrain::protocol::v777::common::client_information_data::allows_listing},
        refrain::reflect::custom_field{"particles", &::refrain::protocol::v777::common::client_information_data::particles});
};

/// Generated RSC packet 'refrain::protocol::v777::configuration::clientbound::custom_payload'.
template <> struct custom_traits<::refrain::protocol::v777::configuration::clientbound::custom_payload> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::configuration::clientbound::custom_payload";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"channel", &::refrain::protocol::v777::configuration::clientbound::custom_payload::channel},
        refrain::reflect::custom_field{"data", &::refrain::protocol::v777::configuration::clientbound::custom_payload::data});
};

/// Generated RSC packet 'refrain::protocol::v777::configuration::clientbound::finish_configuration'.
template <> struct custom_traits<::refrain::protocol::v777::configuration::clientbound::finish_configuration> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::configuration::clientbound::finish_configuration";
    static constexpr auto fields = std::make_tuple();
};

/// Generated RSC packet 'refrain::protocol::v777::configuration::clientbound::keep_alive'.
template <> struct custom_traits<::refrain::protocol::v777::configuration::clientbound::keep_alive> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::configuration::clientbound::keep_alive";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"id", &::refrain::protocol::v777::configuration::clientbound::keep_alive::id});
};

/// Generated RSC packet 'refrain::protocol::v777::configuration::clientbound::ping'.
template <> struct custom_traits<::refrain::protocol::v777::configuration::clientbound::ping> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::configuration::clientbound::ping";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"id", &::refrain::protocol::v777::configuration::clientbound::ping::id});
};

/// Generated RSC packet 'refrain::protocol::v777::configuration::clientbound::update_enabled_features'.
template <> struct custom_traits<::refrain::protocol::v777::configuration::clientbound::update_enabled_features> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::configuration::clientbound::update_enabled_features";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"features", &::refrain::protocol::v777::configuration::clientbound::update_enabled_features::features});
};

/// Generated RSC packet 'refrain::protocol::v777::configuration::clientbound::select_known_packs'.
template <> struct custom_traits<::refrain::protocol::v777::configuration::clientbound::select_known_packs> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::configuration::clientbound::select_known_packs";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"packs", &::refrain::protocol::v777::configuration::clientbound::select_known_packs::packs});
};

/// Generated RSC packet 'refrain::protocol::v777::configuration::clientbound::cookie_request'.
template <> struct custom_traits<::refrain::protocol::v777::configuration::clientbound::cookie_request> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::configuration::clientbound::cookie_request";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"key", &::refrain::protocol::v777::configuration::clientbound::cookie_request::key});
};

/// Generated RSC packet 'refrain::protocol::v777::configuration::clientbound::disconnect'.
template <> struct custom_traits<::refrain::protocol::v777::configuration::clientbound::disconnect> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::configuration::clientbound::disconnect";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"payload", &::refrain::protocol::v777::configuration::clientbound::disconnect::payload});
};

/// Generated RSC packet 'refrain::protocol::v777::configuration::clientbound::reset_chat'.
template <> struct custom_traits<::refrain::protocol::v777::configuration::clientbound::reset_chat> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::configuration::clientbound::reset_chat";
    static constexpr auto fields = std::make_tuple();
};

/// Generated RSC packet 'refrain::protocol::v777::configuration::clientbound::registry_data'.
template <> struct custom_traits<::refrain::protocol::v777::configuration::clientbound::registry_data> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::configuration::clientbound::registry_data";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"payload", &::refrain::protocol::v777::configuration::clientbound::registry_data::payload});
};

/// Generated RSC packet 'refrain::protocol::v777::configuration::clientbound::resource_pack_pop'.
template <> struct custom_traits<::refrain::protocol::v777::configuration::clientbound::resource_pack_pop> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::configuration::clientbound::resource_pack_pop";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"id", &::refrain::protocol::v777::configuration::clientbound::resource_pack_pop::id});
};

/// Generated RSC packet 'refrain::protocol::v777::configuration::clientbound::resource_pack_push'.
template <> struct custom_traits<::refrain::protocol::v777::configuration::clientbound::resource_pack_push> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::configuration::clientbound::resource_pack_push";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"id", &::refrain::protocol::v777::configuration::clientbound::resource_pack_push::id},
        refrain::reflect::custom_field{"url", &::refrain::protocol::v777::configuration::clientbound::resource_pack_push::url},
        refrain::reflect::custom_field{"hash", &::refrain::protocol::v777::configuration::clientbound::resource_pack_push::hash},
        refrain::reflect::custom_field{"required", &::refrain::protocol::v777::configuration::clientbound::resource_pack_push::required},
        refrain::reflect::custom_field{"prompt", &::refrain::protocol::v777::configuration::clientbound::resource_pack_push::prompt});
};

/// Generated RSC packet 'refrain::protocol::v777::configuration::clientbound::post_effects'.
template <> struct custom_traits<::refrain::protocol::v777::configuration::clientbound::post_effects> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::configuration::clientbound::post_effects";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"post_effects", &::refrain::protocol::v777::configuration::clientbound::post_effects::post_effects});
};

/// Generated RSC packet 'refrain::protocol::v777::configuration::clientbound::store_cookie'.
template <> struct custom_traits<::refrain::protocol::v777::configuration::clientbound::store_cookie> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::configuration::clientbound::store_cookie";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"key", &::refrain::protocol::v777::configuration::clientbound::store_cookie::key},
        refrain::reflect::custom_field{"payload", &::refrain::protocol::v777::configuration::clientbound::store_cookie::payload});
};

/// Generated RSC packet 'refrain::protocol::v777::configuration::clientbound::transfer'.
template <> struct custom_traits<::refrain::protocol::v777::configuration::clientbound::transfer> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::configuration::clientbound::transfer";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"host", &::refrain::protocol::v777::configuration::clientbound::transfer::host},
        refrain::reflect::custom_field{"port", &::refrain::protocol::v777::configuration::clientbound::transfer::port});
};

/// Generated RSC packet 'refrain::protocol::v777::configuration::clientbound::custom_report_details'.
template <> struct custom_traits<::refrain::protocol::v777::configuration::clientbound::custom_report_details> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::configuration::clientbound::custom_report_details";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"payload", &::refrain::protocol::v777::configuration::clientbound::custom_report_details::payload});
};

/// Generated RSC packet 'refrain::protocol::v777::configuration::clientbound::clear_dialog'.
template <> struct custom_traits<::refrain::protocol::v777::configuration::clientbound::clear_dialog> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::configuration::clientbound::clear_dialog";
    static constexpr auto fields = std::make_tuple();
};

/// Generated RSC packet 'refrain::protocol::v777::configuration::clientbound::code_of_conduct'.
template <> struct custom_traits<::refrain::protocol::v777::configuration::clientbound::code_of_conduct> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::configuration::clientbound::code_of_conduct";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"code_of_conduct", &::refrain::protocol::v777::configuration::clientbound::code_of_conduct::code_of_conduct});
};

/// Generated RSC packet 'refrain::protocol::v777::configuration::serverbound::client_information'.
template <> struct custom_traits<::refrain::protocol::v777::configuration::serverbound::client_information> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::configuration::serverbound::client_information";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"information", &::refrain::protocol::v777::configuration::serverbound::client_information::information});
};

/// Generated RSC packet 'refrain::protocol::v777::configuration::serverbound::cookie_response'.
template <> struct custom_traits<::refrain::protocol::v777::configuration::serverbound::cookie_response> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::configuration::serverbound::cookie_response";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"key", &::refrain::protocol::v777::configuration::serverbound::cookie_response::key},
        refrain::reflect::custom_field{"payload", &::refrain::protocol::v777::configuration::serverbound::cookie_response::payload});
};

/// Generated RSC packet 'refrain::protocol::v777::configuration::serverbound::custom_payload'.
template <> struct custom_traits<::refrain::protocol::v777::configuration::serverbound::custom_payload> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::configuration::serverbound::custom_payload";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"channel", &::refrain::protocol::v777::configuration::serverbound::custom_payload::channel},
        refrain::reflect::custom_field{"data", &::refrain::protocol::v777::configuration::serverbound::custom_payload::data});
};

/// Generated RSC packet 'refrain::protocol::v777::configuration::serverbound::finish_configuration'.
template <> struct custom_traits<::refrain::protocol::v777::configuration::serverbound::finish_configuration> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::configuration::serverbound::finish_configuration";
    static constexpr auto fields = std::make_tuple();
};

/// Generated RSC packet 'refrain::protocol::v777::configuration::serverbound::keep_alive'.
template <> struct custom_traits<::refrain::protocol::v777::configuration::serverbound::keep_alive> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::configuration::serverbound::keep_alive";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"id", &::refrain::protocol::v777::configuration::serverbound::keep_alive::id});
};

/// Generated RSC packet 'refrain::protocol::v777::configuration::serverbound::pong'.
template <> struct custom_traits<::refrain::protocol::v777::configuration::serverbound::pong> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::configuration::serverbound::pong";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"id", &::refrain::protocol::v777::configuration::serverbound::pong::id});
};

/// Generated RSC packet 'refrain::protocol::v777::configuration::serverbound::resource_pack'.
template <> struct custom_traits<::refrain::protocol::v777::configuration::serverbound::resource_pack> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::configuration::serverbound::resource_pack";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"id", &::refrain::protocol::v777::configuration::serverbound::resource_pack::id},
        refrain::reflect::custom_field{"action", &::refrain::protocol::v777::configuration::serverbound::resource_pack::action});
};

/// Generated RSC packet 'refrain::protocol::v777::configuration::serverbound::select_known_packs'.
template <> struct custom_traits<::refrain::protocol::v777::configuration::serverbound::select_known_packs> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::configuration::serverbound::select_known_packs";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"packs", &::refrain::protocol::v777::configuration::serverbound::select_known_packs::packs});
};

/// Generated RSC packet 'refrain::protocol::v777::configuration::serverbound::accept_code_of_conduct'.
template <> struct custom_traits<::refrain::protocol::v777::configuration::serverbound::accept_code_of_conduct> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::configuration::serverbound::accept_code_of_conduct";
    static constexpr auto fields = std::make_tuple();
};

/// Generated RSC packet 'refrain::protocol::v777::handshake::serverbound::intention'.
template <> struct custom_traits<::refrain::protocol::v777::handshake::serverbound::intention> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::handshake::serverbound::intention";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"protocol", &::refrain::protocol::v777::handshake::serverbound::intention::protocol},
        refrain::reflect::custom_field{"host", &::refrain::protocol::v777::handshake::serverbound::intention::host},
        refrain::reflect::custom_field{"port", &::refrain::protocol::v777::handshake::serverbound::intention::port},
        refrain::reflect::custom_field{"next", &::refrain::protocol::v777::handshake::serverbound::intention::next});
};

/// Generated RSC struct 'refrain::protocol::v777::login::clientbound::profile_property'.
template <> struct custom_traits<::refrain::protocol::v777::login::clientbound::profile_property> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::login::clientbound::profile_property";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"name", &::refrain::protocol::v777::login::clientbound::profile_property::name},
        refrain::reflect::custom_field{"value", &::refrain::protocol::v777::login::clientbound::profile_property::value},
        refrain::reflect::custom_field{"signature", &::refrain::protocol::v777::login::clientbound::profile_property::signature});
};

/// Generated RSC packet 'refrain::protocol::v777::login::clientbound::login_disconnect'.
template <> struct custom_traits<::refrain::protocol::v777::login::clientbound::login_disconnect> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::login::clientbound::login_disconnect";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"reason", &::refrain::protocol::v777::login::clientbound::login_disconnect::reason});
};

/// Generated RSC packet 'refrain::protocol::v777::login::clientbound::hello'.
template <> struct custom_traits<::refrain::protocol::v777::login::clientbound::hello> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::login::clientbound::hello";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"server_id", &::refrain::protocol::v777::login::clientbound::hello::server_id},
        refrain::reflect::custom_field{"public_key", &::refrain::protocol::v777::login::clientbound::hello::public_key},
        refrain::reflect::custom_field{"challenge", &::refrain::protocol::v777::login::clientbound::hello::challenge},
        refrain::reflect::custom_field{"should_authenticate", &::refrain::protocol::v777::login::clientbound::hello::should_authenticate});
};

/// Generated RSC packet 'refrain::protocol::v777::login::clientbound::login_finished'.
template <> struct custom_traits<::refrain::protocol::v777::login::clientbound::login_finished> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::login::clientbound::login_finished";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"profile_high", &::refrain::protocol::v777::login::clientbound::login_finished::profile_high},
        refrain::reflect::custom_field{"profile_low", &::refrain::protocol::v777::login::clientbound::login_finished::profile_low},
        refrain::reflect::custom_field{"name", &::refrain::protocol::v777::login::clientbound::login_finished::name},
        refrain::reflect::custom_field{"properties", &::refrain::protocol::v777::login::clientbound::login_finished::properties},
        refrain::reflect::custom_field{"session_high", &::refrain::protocol::v777::login::clientbound::login_finished::session_high},
        refrain::reflect::custom_field{"session_low", &::refrain::protocol::v777::login::clientbound::login_finished::session_low});
};

/// Generated RSC packet 'refrain::protocol::v777::login::clientbound::login_compression'.
template <> struct custom_traits<::refrain::protocol::v777::login::clientbound::login_compression> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::login::clientbound::login_compression";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"threshold", &::refrain::protocol::v777::login::clientbound::login_compression::threshold});
};

/// Generated RSC packet 'refrain::protocol::v777::login::clientbound::cookie_request'.
template <> struct custom_traits<::refrain::protocol::v777::login::clientbound::cookie_request> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::login::clientbound::cookie_request";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"key", &::refrain::protocol::v777::login::clientbound::cookie_request::key});
};

/// Generated RSC packet 'refrain::protocol::v777::login::serverbound::hello'.
template <> struct custom_traits<::refrain::protocol::v777::login::serverbound::hello> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::login::serverbound::hello";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"name", &::refrain::protocol::v777::login::serverbound::hello::name},
        refrain::reflect::custom_field{"profile_high", &::refrain::protocol::v777::login::serverbound::hello::profile_high},
        refrain::reflect::custom_field{"profile_low", &::refrain::protocol::v777::login::serverbound::hello::profile_low});
};

/// Generated RSC packet 'refrain::protocol::v777::login::serverbound::key'.
template <> struct custom_traits<::refrain::protocol::v777::login::serverbound::key> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::login::serverbound::key";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"keybytes", &::refrain::protocol::v777::login::serverbound::key::keybytes},
        refrain::reflect::custom_field{"encrypted_challenge", &::refrain::protocol::v777::login::serverbound::key::encrypted_challenge});
};

/// Generated RSC packet 'refrain::protocol::v777::login::serverbound::login_acknowledged'.
template <> struct custom_traits<::refrain::protocol::v777::login::serverbound::login_acknowledged> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::login::serverbound::login_acknowledged";
    static constexpr auto fields = std::make_tuple();
};

/// Generated RSC packet 'refrain::protocol::v777::login::serverbound::cookie_response'.
template <> struct custom_traits<::refrain::protocol::v777::login::serverbound::cookie_response> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::login::serverbound::cookie_response";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"key", &::refrain::protocol::v777::login::serverbound::cookie_response::key},
        refrain::reflect::custom_field{"payload", &::refrain::protocol::v777::login::serverbound::cookie_response::payload});
};

/// Generated RSC struct 'refrain::protocol::v777::play::clientbound::global_pos'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::global_pos> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::global_pos";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"dimension", &::refrain::protocol::v777::play::clientbound::global_pos::dimension},
        refrain::reflect::custom_field{"position", &::refrain::protocol::v777::play::clientbound::global_pos::position});
};

/// Generated RSC struct 'refrain::protocol::v777::play::clientbound::spawn_info'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::spawn_info> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::spawn_info";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"dimension_type", &::refrain::protocol::v777::play::clientbound::spawn_info::dimension_type},
        refrain::reflect::custom_field{"dimension", &::refrain::protocol::v777::play::clientbound::spawn_info::dimension},
        refrain::reflect::custom_field{"seed", &::refrain::protocol::v777::play::clientbound::spawn_info::seed},
        refrain::reflect::custom_field{"game_type", &::refrain::protocol::v777::play::clientbound::spawn_info::game_type},
        refrain::reflect::custom_field{"previous_game_type", &::refrain::protocol::v777::play::clientbound::spawn_info::previous_game_type},
        refrain::reflect::custom_field{"debug", &::refrain::protocol::v777::play::clientbound::spawn_info::debug},
        refrain::reflect::custom_field{"flat", &::refrain::protocol::v777::play::clientbound::spawn_info::flat},
        refrain::reflect::custom_field{"last_death_location", &::refrain::protocol::v777::play::clientbound::spawn_info::last_death_location},
        refrain::reflect::custom_field{"portal_cooldown", &::refrain::protocol::v777::play::clientbound::spawn_info::portal_cooldown},
        refrain::reflect::custom_field{"sea_level", &::refrain::protocol::v777::play::clientbound::spawn_info::sea_level});
};

/// Generated RSC struct 'refrain::protocol::v777::play::clientbound::clock_update'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::clock_update> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::clock_update";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"clock", &::refrain::protocol::v777::play::clientbound::clock_update::clock},
        refrain::reflect::custom_field{"state", &::refrain::protocol::v777::play::clientbound::clock_update::state});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::block_changed_ack'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::block_changed_ack> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::block_changed_ack";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"sequence", &::refrain::protocol::v777::play::clientbound::block_changed_ack::sequence});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::block_update'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::block_update> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::block_update";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"position", &::refrain::protocol::v777::play::clientbound::block_update::position},
        refrain::reflect::custom_field{"state", &::refrain::protocol::v777::play::clientbound::block_update::state});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::change_difficulty'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::change_difficulty> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::change_difficulty";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"difficulty", &::refrain::protocol::v777::play::clientbound::change_difficulty::difficulty},
        refrain::reflect::custom_field{"locked", &::refrain::protocol::v777::play::clientbound::change_difficulty::locked});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::chunk_batch_finished'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::chunk_batch_finished> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::chunk_batch_finished";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"batch_size", &::refrain::protocol::v777::play::clientbound::chunk_batch_finished::batch_size});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::chunk_batch_start'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::chunk_batch_start> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::chunk_batch_start";
    static constexpr auto fields = std::make_tuple();
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::entity_event'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::entity_event> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::entity_event";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"entity_id", &::refrain::protocol::v777::play::clientbound::entity_event::entity_id},
        refrain::reflect::custom_field{"event", &::refrain::protocol::v777::play::clientbound::entity_event::event});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::game_event'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::game_event> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::game_event";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"event", &::refrain::protocol::v777::play::clientbound::game_event::event},
        refrain::reflect::custom_field{"value", &::refrain::protocol::v777::play::clientbound::game_event::value});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::initialize_border'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::initialize_border> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::initialize_border";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"center_x", &::refrain::protocol::v777::play::clientbound::initialize_border::center_x},
        refrain::reflect::custom_field{"center_z", &::refrain::protocol::v777::play::clientbound::initialize_border::center_z},
        refrain::reflect::custom_field{"old_size", &::refrain::protocol::v777::play::clientbound::initialize_border::old_size},
        refrain::reflect::custom_field{"new_size", &::refrain::protocol::v777::play::clientbound::initialize_border::new_size},
        refrain::reflect::custom_field{"lerp_time", &::refrain::protocol::v777::play::clientbound::initialize_border::lerp_time},
        refrain::reflect::custom_field{"absolute_max_size", &::refrain::protocol::v777::play::clientbound::initialize_border::absolute_max_size},
        refrain::reflect::custom_field{"warning_blocks", &::refrain::protocol::v777::play::clientbound::initialize_border::warning_blocks},
        refrain::reflect::custom_field{"warning_time", &::refrain::protocol::v777::play::clientbound::initialize_border::warning_time});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::keep_alive'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::keep_alive> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::keep_alive";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"id", &::refrain::protocol::v777::play::clientbound::keep_alive::id});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::login'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::login> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::login";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"player_id", &::refrain::protocol::v777::play::clientbound::login::player_id},
        refrain::reflect::custom_field{"hardcore", &::refrain::protocol::v777::play::clientbound::login::hardcore},
        refrain::reflect::custom_field{"levels", &::refrain::protocol::v777::play::clientbound::login::levels},
        refrain::reflect::custom_field{"max_players", &::refrain::protocol::v777::play::clientbound::login::max_players},
        refrain::reflect::custom_field{"chunk_radius", &::refrain::protocol::v777::play::clientbound::login::chunk_radius},
        refrain::reflect::custom_field{"simulation_distance", &::refrain::protocol::v777::play::clientbound::login::simulation_distance},
        refrain::reflect::custom_field{"reduced_debug_info", &::refrain::protocol::v777::play::clientbound::login::reduced_debug_info},
        refrain::reflect::custom_field{"show_death_screen", &::refrain::protocol::v777::play::clientbound::login::show_death_screen},
        refrain::reflect::custom_field{"limited_crafting", &::refrain::protocol::v777::play::clientbound::login::limited_crafting},
        refrain::reflect::custom_field{"spawn", &::refrain::protocol::v777::play::clientbound::login::spawn},
        refrain::reflect::custom_field{"online_mode", &::refrain::protocol::v777::play::clientbound::login::online_mode},
        refrain::reflect::custom_field{"enforces_secure_chat", &::refrain::protocol::v777::play::clientbound::login::enforces_secure_chat});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::player_abilities'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::player_abilities> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::player_abilities";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"flags", &::refrain::protocol::v777::play::clientbound::player_abilities::flags},
        refrain::reflect::custom_field{"flying_speed", &::refrain::protocol::v777::play::clientbound::player_abilities::flying_speed},
        refrain::reflect::custom_field{"walking_speed", &::refrain::protocol::v777::play::clientbound::player_abilities::walking_speed});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::player_position'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::player_position> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::player_position";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"id", &::refrain::protocol::v777::play::clientbound::player_position::id},
        refrain::reflect::custom_field{"x", &::refrain::protocol::v777::play::clientbound::player_position::x},
        refrain::reflect::custom_field{"y", &::refrain::protocol::v777::play::clientbound::player_position::y},
        refrain::reflect::custom_field{"z", &::refrain::protocol::v777::play::clientbound::player_position::z},
        refrain::reflect::custom_field{"delta_x", &::refrain::protocol::v777::play::clientbound::player_position::delta_x},
        refrain::reflect::custom_field{"delta_y", &::refrain::protocol::v777::play::clientbound::player_position::delta_y},
        refrain::reflect::custom_field{"delta_z", &::refrain::protocol::v777::play::clientbound::player_position::delta_z},
        refrain::reflect::custom_field{"y_rot", &::refrain::protocol::v777::play::clientbound::player_position::y_rot},
        refrain::reflect::custom_field{"x_rot", &::refrain::protocol::v777::play::clientbound::player_position::x_rot},
        refrain::reflect::custom_field{"relatives", &::refrain::protocol::v777::play::clientbound::player_position::relatives});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::set_default_spawn_position'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::set_default_spawn_position> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::set_default_spawn_position";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"dimension", &::refrain::protocol::v777::play::clientbound::set_default_spawn_position::dimension},
        refrain::reflect::custom_field{"position", &::refrain::protocol::v777::play::clientbound::set_default_spawn_position::position},
        refrain::reflect::custom_field{"yaw", &::refrain::protocol::v777::play::clientbound::set_default_spawn_position::yaw},
        refrain::reflect::custom_field{"pitch", &::refrain::protocol::v777::play::clientbound::set_default_spawn_position::pitch});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::set_held_slot'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::set_held_slot> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::set_held_slot";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"slot", &::refrain::protocol::v777::play::clientbound::set_held_slot::slot});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::set_chunk_cache_center'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::set_chunk_cache_center> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::set_chunk_cache_center";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"chunk_x", &::refrain::protocol::v777::play::clientbound::set_chunk_cache_center::chunk_x},
        refrain::reflect::custom_field{"chunk_z", &::refrain::protocol::v777::play::clientbound::set_chunk_cache_center::chunk_z});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::set_chunk_cache_radius'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::set_chunk_cache_radius> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::set_chunk_cache_radius";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"radius", &::refrain::protocol::v777::play::clientbound::set_chunk_cache_radius::radius});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::set_time'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::set_time> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::set_time";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"game_time", &::refrain::protocol::v777::play::clientbound::set_time::game_time},
        refrain::reflect::custom_field{"clock_updates", &::refrain::protocol::v777::play::clientbound::set_time::clock_updates});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::bundle_delimiter'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::bundle_delimiter> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::bundle_delimiter";
    static constexpr auto fields = std::make_tuple();
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::add_entity'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::add_entity> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::add_entity";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"id", &::refrain::protocol::v777::play::clientbound::add_entity::id},
        refrain::reflect::custom_field{"uuid", &::refrain::protocol::v777::play::clientbound::add_entity::uuid},
        refrain::reflect::custom_field{"type_", &::refrain::protocol::v777::play::clientbound::add_entity::type_},
        refrain::reflect::custom_field{"x", &::refrain::protocol::v777::play::clientbound::add_entity::x},
        refrain::reflect::custom_field{"y", &::refrain::protocol::v777::play::clientbound::add_entity::y},
        refrain::reflect::custom_field{"z", &::refrain::protocol::v777::play::clientbound::add_entity::z},
        refrain::reflect::custom_field{"movement", &::refrain::protocol::v777::play::clientbound::add_entity::movement},
        refrain::reflect::custom_field{"x_rot", &::refrain::protocol::v777::play::clientbound::add_entity::x_rot},
        refrain::reflect::custom_field{"y_rot", &::refrain::protocol::v777::play::clientbound::add_entity::y_rot},
        refrain::reflect::custom_field{"y_head_rot", &::refrain::protocol::v777::play::clientbound::add_entity::y_head_rot},
        refrain::reflect::custom_field{"data", &::refrain::protocol::v777::play::clientbound::add_entity::data});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::animate'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::animate> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::animate";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"id", &::refrain::protocol::v777::play::clientbound::animate::id},
        refrain::reflect::custom_field{"action", &::refrain::protocol::v777::play::clientbound::animate::action});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::block_destruction'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::block_destruction> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::block_destruction";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"id", &::refrain::protocol::v777::play::clientbound::block_destruction::id},
        refrain::reflect::custom_field{"pos", &::refrain::protocol::v777::play::clientbound::block_destruction::pos},
        refrain::reflect::custom_field{"progress", &::refrain::protocol::v777::play::clientbound::block_destruction::progress});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::block_event'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::block_event> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::block_event";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"pos", &::refrain::protocol::v777::play::clientbound::block_event::pos},
        refrain::reflect::custom_field{"b0", &::refrain::protocol::v777::play::clientbound::block_event::b0},
        refrain::reflect::custom_field{"b1", &::refrain::protocol::v777::play::clientbound::block_event::b1},
        refrain::reflect::custom_field{"block", &::refrain::protocol::v777::play::clientbound::block_event::block});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::boss_event'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::boss_event> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::boss_event";
    static constexpr auto fields = std::make_tuple();
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::chunks_biomes'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::chunks_biomes> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::chunks_biomes";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"chunk_biome_data", &::refrain::protocol::v777::play::clientbound::chunks_biomes::chunk_biome_data});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::clear_titles'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::clear_titles> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::clear_titles";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"reset_times", &::refrain::protocol::v777::play::clientbound::clear_titles::reset_times});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::command_suggestions'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::command_suggestions> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::command_suggestions";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"id", &::refrain::protocol::v777::play::clientbound::command_suggestions::id},
        refrain::reflect::custom_field{"start", &::refrain::protocol::v777::play::clientbound::command_suggestions::start},
        refrain::reflect::custom_field{"length", &::refrain::protocol::v777::play::clientbound::command_suggestions::length},
        refrain::reflect::custom_field{"suggestions", &::refrain::protocol::v777::play::clientbound::command_suggestions::suggestions});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::container_close'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::container_close> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::container_close";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"container_id", &::refrain::protocol::v777::play::clientbound::container_close::container_id});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::container_set_content'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::container_set_content> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::container_set_content";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"container_id", &::refrain::protocol::v777::play::clientbound::container_set_content::container_id},
        refrain::reflect::custom_field{"state_id", &::refrain::protocol::v777::play::clientbound::container_set_content::state_id},
        refrain::reflect::custom_field{"items", &::refrain::protocol::v777::play::clientbound::container_set_content::items},
        refrain::reflect::custom_field{"carried_item", &::refrain::protocol::v777::play::clientbound::container_set_content::carried_item});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::container_set_data'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::container_set_data> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::container_set_data";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"container_id", &::refrain::protocol::v777::play::clientbound::container_set_data::container_id},
        refrain::reflect::custom_field{"id", &::refrain::protocol::v777::play::clientbound::container_set_data::id},
        refrain::reflect::custom_field{"value", &::refrain::protocol::v777::play::clientbound::container_set_data::value});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::container_set_slot'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::container_set_slot> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::container_set_slot";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"container_id", &::refrain::protocol::v777::play::clientbound::container_set_slot::container_id},
        refrain::reflect::custom_field{"state_id", &::refrain::protocol::v777::play::clientbound::container_set_slot::state_id},
        refrain::reflect::custom_field{"slot", &::refrain::protocol::v777::play::clientbound::container_set_slot::slot},
        refrain::reflect::custom_field{"item_stack", &::refrain::protocol::v777::play::clientbound::container_set_slot::item_stack});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::cookie_request'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::cookie_request> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::cookie_request";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"key", &::refrain::protocol::v777::play::clientbound::cookie_request::key});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::cooldown'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::cooldown> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::cooldown";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"cooldown_group", &::refrain::protocol::v777::play::clientbound::cooldown::cooldown_group},
        refrain::reflect::custom_field{"duration", &::refrain::protocol::v777::play::clientbound::cooldown::duration});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::custom_chat_completions'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::custom_chat_completions> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::custom_chat_completions";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"action", &::refrain::protocol::v777::play::clientbound::custom_chat_completions::action},
        refrain::reflect::custom_field{"entries", &::refrain::protocol::v777::play::clientbound::custom_chat_completions::entries});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::custom_payload'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::custom_payload> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::custom_payload";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"channel", &::refrain::protocol::v777::play::clientbound::custom_payload::channel},
        refrain::reflect::custom_field{"data", &::refrain::protocol::v777::play::clientbound::custom_payload::data});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::debug_sample'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::debug_sample> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::debug_sample";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"sample", &::refrain::protocol::v777::play::clientbound::debug_sample::sample},
        refrain::reflect::custom_field{"debug_sample_type", &::refrain::protocol::v777::play::clientbound::debug_sample::debug_sample_type});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::disconnect'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::disconnect> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::disconnect";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"payload", &::refrain::protocol::v777::play::clientbound::disconnect::payload});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::add_transient_block'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::add_transient_block> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::add_transient_block";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"pos", &::refrain::protocol::v777::play::clientbound::add_transient_block::pos},
        refrain::reflect::custom_field{"block_state", &::refrain::protocol::v777::play::clientbound::add_transient_block::block_state});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::forget_level_chunk'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::forget_level_chunk> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::forget_level_chunk";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"pos", &::refrain::protocol::v777::play::clientbound::forget_level_chunk::pos});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::game_rule_values'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::game_rule_values> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::game_rule_values";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"payload", &::refrain::protocol::v777::play::clientbound::game_rule_values::payload});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::game_test_highlight_pos'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::game_test_highlight_pos> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::game_test_highlight_pos";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"absolute_pos", &::refrain::protocol::v777::play::clientbound::game_test_highlight_pos::absolute_pos},
        refrain::reflect::custom_field{"relative_pos", &::refrain::protocol::v777::play::clientbound::game_test_highlight_pos::relative_pos});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::mount_screen_open'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::mount_screen_open> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::mount_screen_open";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"container_id", &::refrain::protocol::v777::play::clientbound::mount_screen_open::container_id},
        refrain::reflect::custom_field{"inventory_columns", &::refrain::protocol::v777::play::clientbound::mount_screen_open::inventory_columns},
        refrain::reflect::custom_field{"entity_id", &::refrain::protocol::v777::play::clientbound::mount_screen_open::entity_id});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::hurt_animation'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::hurt_animation> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::hurt_animation";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"id", &::refrain::protocol::v777::play::clientbound::hurt_animation::id},
        refrain::reflect::custom_field{"yaw", &::refrain::protocol::v777::play::clientbound::hurt_animation::yaw});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::level_event'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::level_event> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::level_event";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"type_", &::refrain::protocol::v777::play::clientbound::level_event::type_},
        refrain::reflect::custom_field{"pos", &::refrain::protocol::v777::play::clientbound::level_event::pos},
        refrain::reflect::custom_field{"data", &::refrain::protocol::v777::play::clientbound::level_event::data},
        refrain::reflect::custom_field{"global_event", &::refrain::protocol::v777::play::clientbound::level_event::global_event});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::low_disk_space_warning'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::low_disk_space_warning> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::low_disk_space_warning";
    static constexpr auto fields = std::make_tuple();
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::move_minecart_along_track'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::move_minecart_along_track> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::move_minecart_along_track";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"entity_id", &::refrain::protocol::v777::play::clientbound::move_minecart_along_track::entity_id},
        refrain::reflect::custom_field{"lerp_steps", &::refrain::protocol::v777::play::clientbound::move_minecart_along_track::lerp_steps});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::move_vehicle'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::move_vehicle> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::move_vehicle";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"payload", &::refrain::protocol::v777::play::clientbound::move_vehicle::payload});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::open_book'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::open_book> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::open_book";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"hand", &::refrain::protocol::v777::play::clientbound::open_book::hand});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::open_screen'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::open_screen> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::open_screen";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"get_container_id", &::refrain::protocol::v777::play::clientbound::open_screen::get_container_id},
        refrain::reflect::custom_field{"get_type", &::refrain::protocol::v777::play::clientbound::open_screen::get_type},
        refrain::reflect::custom_field{"get_title", &::refrain::protocol::v777::play::clientbound::open_screen::get_title});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::open_sign_editor'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::open_sign_editor> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::open_sign_editor";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"pos", &::refrain::protocol::v777::play::clientbound::open_sign_editor::pos},
        refrain::reflect::custom_field{"slot", &::refrain::protocol::v777::play::clientbound::open_sign_editor::slot});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::ping'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::ping> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::ping";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"id", &::refrain::protocol::v777::play::clientbound::ping::id});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::pong_response'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::pong_response> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::pong_response";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"time", &::refrain::protocol::v777::play::clientbound::pong_response::time});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::place_ghost_recipe'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::place_ghost_recipe> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::place_ghost_recipe";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"container_id", &::refrain::protocol::v777::play::clientbound::place_ghost_recipe::container_id},
        refrain::reflect::custom_field{"recipe_display", &::refrain::protocol::v777::play::clientbound::place_ghost_recipe::recipe_display});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::player_combat_end'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::player_combat_end> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::player_combat_end";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"duration", &::refrain::protocol::v777::play::clientbound::player_combat_end::duration});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::player_combat_enter'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::player_combat_enter> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::player_combat_enter";
    static constexpr auto fields = std::make_tuple();
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::player_combat_kill'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::player_combat_kill> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::player_combat_kill";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"player_id", &::refrain::protocol::v777::play::clientbound::player_combat_kill::player_id},
        refrain::reflect::custom_field{"message", &::refrain::protocol::v777::play::clientbound::player_combat_kill::message});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::player_info_remove'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::player_info_remove> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::player_info_remove";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"profile_ids", &::refrain::protocol::v777::play::clientbound::player_info_remove::profile_ids});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::player_rotation'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::player_rotation> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::player_rotation";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"y_rot", &::refrain::protocol::v777::play::clientbound::player_rotation::y_rot},
        refrain::reflect::custom_field{"relative_y", &::refrain::protocol::v777::play::clientbound::player_rotation::relative_y},
        refrain::reflect::custom_field{"x_rot", &::refrain::protocol::v777::play::clientbound::player_rotation::x_rot},
        refrain::reflect::custom_field{"relative_x", &::refrain::protocol::v777::play::clientbound::player_rotation::relative_x});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::recipe_book_remove'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::recipe_book_remove> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::recipe_book_remove";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"recipes", &::refrain::protocol::v777::play::clientbound::recipe_book_remove::recipes});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::recipe_book_settings'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::recipe_book_settings> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::recipe_book_settings";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"book_settings", &::refrain::protocol::v777::play::clientbound::recipe_book_settings::book_settings});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::remove_entities'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::remove_entities> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::remove_entities";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"payload", &::refrain::protocol::v777::play::clientbound::remove_entities::payload});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::remove_mob_effect'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::remove_mob_effect> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::remove_mob_effect";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"entity_id", &::refrain::protocol::v777::play::clientbound::remove_mob_effect::entity_id},
        refrain::reflect::custom_field{"effect", &::refrain::protocol::v777::play::clientbound::remove_mob_effect::effect});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::resource_pack_pop'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::resource_pack_pop> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::resource_pack_pop";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"id", &::refrain::protocol::v777::play::clientbound::resource_pack_pop::id});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::resource_pack_push'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::resource_pack_push> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::resource_pack_push";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"id", &::refrain::protocol::v777::play::clientbound::resource_pack_push::id},
        refrain::reflect::custom_field{"url", &::refrain::protocol::v777::play::clientbound::resource_pack_push::url},
        refrain::reflect::custom_field{"hash", &::refrain::protocol::v777::play::clientbound::resource_pack_push::hash},
        refrain::reflect::custom_field{"required", &::refrain::protocol::v777::play::clientbound::resource_pack_push::required},
        refrain::reflect::custom_field{"prompt", &::refrain::protocol::v777::play::clientbound::resource_pack_push::prompt});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::post_effects'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::post_effects> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::post_effects";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"post_effects", &::refrain::protocol::v777::play::clientbound::post_effects::post_effects});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::respawn'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::respawn> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::respawn";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"common_player_spawn_info", &::refrain::protocol::v777::play::clientbound::respawn::common_player_spawn_info},
        refrain::reflect::custom_field{"data_to_keep", &::refrain::protocol::v777::play::clientbound::respawn::data_to_keep});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::rotate_head'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::rotate_head> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::rotate_head";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"entity_id", &::refrain::protocol::v777::play::clientbound::rotate_head::entity_id},
        refrain::reflect::custom_field{"y_head_rot", &::refrain::protocol::v777::play::clientbound::rotate_head::y_head_rot});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::section_blocks_update'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::section_blocks_update> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::section_blocks_update";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"section_pos", &::refrain::protocol::v777::play::clientbound::section_blocks_update::section_pos},
        refrain::reflect::custom_field{"positions", &::refrain::protocol::v777::play::clientbound::section_blocks_update::positions});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::server_data'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::server_data> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::server_data";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"motd", &::refrain::protocol::v777::play::clientbound::server_data::motd},
        refrain::reflect::custom_field{"icon_bytes", &::refrain::protocol::v777::play::clientbound::server_data::icon_bytes});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::set_action_bar_text'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::set_action_bar_text> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::set_action_bar_text";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"text", &::refrain::protocol::v777::play::clientbound::set_action_bar_text::text});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::set_border_center'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::set_border_center> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::set_border_center";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"new_center_x", &::refrain::protocol::v777::play::clientbound::set_border_center::new_center_x},
        refrain::reflect::custom_field{"new_center_z", &::refrain::protocol::v777::play::clientbound::set_border_center::new_center_z});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::set_border_lerp_size'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::set_border_lerp_size> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::set_border_lerp_size";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"old_size", &::refrain::protocol::v777::play::clientbound::set_border_lerp_size::old_size},
        refrain::reflect::custom_field{"new_size", &::refrain::protocol::v777::play::clientbound::set_border_lerp_size::new_size},
        refrain::reflect::custom_field{"lerp_time", &::refrain::protocol::v777::play::clientbound::set_border_lerp_size::lerp_time});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::set_border_size'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::set_border_size> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::set_border_size";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"size", &::refrain::protocol::v777::play::clientbound::set_border_size::size});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::set_border_warning_delay'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::set_border_warning_delay> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::set_border_warning_delay";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"warning_delay", &::refrain::protocol::v777::play::clientbound::set_border_warning_delay::warning_delay});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::set_border_warning_distance'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::set_border_warning_distance> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::set_border_warning_distance";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"warning_blocks", &::refrain::protocol::v777::play::clientbound::set_border_warning_distance::warning_blocks});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::set_camera'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::set_camera> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::set_camera";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"camera_id", &::refrain::protocol::v777::play::clientbound::set_camera::camera_id});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::set_cursor_item'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::set_cursor_item> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::set_cursor_item";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"contents", &::refrain::protocol::v777::play::clientbound::set_cursor_item::contents});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::set_display_objective'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::set_display_objective> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::set_display_objective";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"slot", &::refrain::protocol::v777::play::clientbound::set_display_objective::slot},
        refrain::reflect::custom_field{"objective_name", &::refrain::protocol::v777::play::clientbound::set_display_objective::objective_name});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::set_entity_link'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::set_entity_link> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::set_entity_link";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"source_id", &::refrain::protocol::v777::play::clientbound::set_entity_link::source_id},
        refrain::reflect::custom_field{"dest_id", &::refrain::protocol::v777::play::clientbound::set_entity_link::dest_id});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::set_entity_motion'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::set_entity_motion> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::set_entity_motion";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"id", &::refrain::protocol::v777::play::clientbound::set_entity_motion::id},
        refrain::reflect::custom_field{"movement", &::refrain::protocol::v777::play::clientbound::set_entity_motion::movement});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::set_equipment'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::set_equipment> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::set_equipment";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"entries", &::refrain::protocol::v777::play::clientbound::set_equipment::entries});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::set_experience'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::set_experience> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::set_experience";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"experience_progress", &::refrain::protocol::v777::play::clientbound::set_experience::experience_progress},
        refrain::reflect::custom_field{"experience_level", &::refrain::protocol::v777::play::clientbound::set_experience::experience_level},
        refrain::reflect::custom_field{"total_experience", &::refrain::protocol::v777::play::clientbound::set_experience::total_experience});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::set_health'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::set_health> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::set_health";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"health", &::refrain::protocol::v777::play::clientbound::set_health::health},
        refrain::reflect::custom_field{"food", &::refrain::protocol::v777::play::clientbound::set_health::food},
        refrain::reflect::custom_field{"saturation", &::refrain::protocol::v777::play::clientbound::set_health::saturation});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::set_passengers'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::set_passengers> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::set_passengers";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"vehicle", &::refrain::protocol::v777::play::clientbound::set_passengers::vehicle},
        refrain::reflect::custom_field{"passengers", &::refrain::protocol::v777::play::clientbound::set_passengers::passengers});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::set_player_inventory'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::set_player_inventory> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::set_player_inventory";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"slot", &::refrain::protocol::v777::play::clientbound::set_player_inventory::slot},
        refrain::reflect::custom_field{"contents", &::refrain::protocol::v777::play::clientbound::set_player_inventory::contents});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::set_player_team'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::set_player_team> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::set_player_team";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"payload", &::refrain::protocol::v777::play::clientbound::set_player_team::payload});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::set_simulation_distance'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::set_simulation_distance> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::set_simulation_distance";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"simulation_distance", &::refrain::protocol::v777::play::clientbound::set_simulation_distance::simulation_distance});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::set_subtitle_text'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::set_subtitle_text> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::set_subtitle_text";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"text", &::refrain::protocol::v777::play::clientbound::set_subtitle_text::text});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::set_title_text'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::set_title_text> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::set_title_text";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"text", &::refrain::protocol::v777::play::clientbound::set_title_text::text});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::set_titles_animation'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::set_titles_animation> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::set_titles_animation";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"fade_in", &::refrain::protocol::v777::play::clientbound::set_titles_animation::fade_in},
        refrain::reflect::custom_field{"stay", &::refrain::protocol::v777::play::clientbound::set_titles_animation::stay},
        refrain::reflect::custom_field{"fade_out", &::refrain::protocol::v777::play::clientbound::set_titles_animation::fade_out});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::start_configuration'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::start_configuration> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::start_configuration";
    static constexpr auto fields = std::make_tuple();
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::store_cookie'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::store_cookie> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::store_cookie";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"key", &::refrain::protocol::v777::play::clientbound::store_cookie::key},
        refrain::reflect::custom_field{"payload", &::refrain::protocol::v777::play::clientbound::store_cookie::payload});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::swing_animation'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::swing_animation> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::swing_animation";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"entity_id", &::refrain::protocol::v777::play::clientbound::swing_animation::entity_id},
        refrain::reflect::custom_field{"hand", &::refrain::protocol::v777::play::clientbound::swing_animation::hand},
        refrain::reflect::custom_field{"animation", &::refrain::protocol::v777::play::clientbound::swing_animation::animation});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::system_chat'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::system_chat> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::system_chat";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"content", &::refrain::protocol::v777::play::clientbound::system_chat::content},
        refrain::reflect::custom_field{"overlay", &::refrain::protocol::v777::play::clientbound::system_chat::overlay});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::tab_list'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::tab_list> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::tab_list";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"header", &::refrain::protocol::v777::play::clientbound::tab_list::header},
        refrain::reflect::custom_field{"footer", &::refrain::protocol::v777::play::clientbound::tab_list::footer});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::take_item_entity'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::take_item_entity> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::take_item_entity";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"item_id", &::refrain::protocol::v777::play::clientbound::take_item_entity::item_id},
        refrain::reflect::custom_field{"player_id", &::refrain::protocol::v777::play::clientbound::take_item_entity::player_id},
        refrain::reflect::custom_field{"amount", &::refrain::protocol::v777::play::clientbound::take_item_entity::amount});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::teleport_entity'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::teleport_entity> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::teleport_entity";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"id", &::refrain::protocol::v777::play::clientbound::teleport_entity::id},
        refrain::reflect::custom_field{"change", &::refrain::protocol::v777::play::clientbound::teleport_entity::change},
        refrain::reflect::custom_field{"relatives", &::refrain::protocol::v777::play::clientbound::teleport_entity::relatives},
        refrain::reflect::custom_field{"on_ground", &::refrain::protocol::v777::play::clientbound::teleport_entity::on_ground});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::test_instance_block_status'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::test_instance_block_status> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::test_instance_block_status";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"status", &::refrain::protocol::v777::play::clientbound::test_instance_block_status::status},
        refrain::reflect::custom_field{"size", &::refrain::protocol::v777::play::clientbound::test_instance_block_status::size});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::ticking_state'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::ticking_state> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::ticking_state";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"tick_rate", &::refrain::protocol::v777::play::clientbound::ticking_state::tick_rate},
        refrain::reflect::custom_field{"is_frozen", &::refrain::protocol::v777::play::clientbound::ticking_state::is_frozen});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::ticking_step'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::ticking_step> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::ticking_step";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"tick_steps", &::refrain::protocol::v777::play::clientbound::ticking_step::tick_steps});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::transfer'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::transfer> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::transfer";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"host", &::refrain::protocol::v777::play::clientbound::transfer::host},
        refrain::reflect::custom_field{"port", &::refrain::protocol::v777::play::clientbound::transfer::port});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::update_mob_effect'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::update_mob_effect> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::update_mob_effect";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"entity_id", &::refrain::protocol::v777::play::clientbound::update_mob_effect::entity_id},
        refrain::reflect::custom_field{"effect", &::refrain::protocol::v777::play::clientbound::update_mob_effect::effect},
        refrain::reflect::custom_field{"effect_amplifier", &::refrain::protocol::v777::play::clientbound::update_mob_effect::effect_amplifier},
        refrain::reflect::custom_field{"effect_duration_ticks", &::refrain::protocol::v777::play::clientbound::update_mob_effect::effect_duration_ticks},
        refrain::reflect::custom_field{"flags_", &::refrain::protocol::v777::play::clientbound::update_mob_effect::flags_});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::projectile_power'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::projectile_power> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::projectile_power";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"id", &::refrain::protocol::v777::play::clientbound::projectile_power::id},
        refrain::reflect::custom_field{"acceleration_power", &::refrain::protocol::v777::play::clientbound::projectile_power::acceleration_power});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::custom_report_details'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::custom_report_details> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::custom_report_details";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"payload", &::refrain::protocol::v777::play::clientbound::custom_report_details::payload});
};

/// Generated RSC packet 'refrain::protocol::v777::play::clientbound::clear_dialog'.
template <> struct custom_traits<::refrain::protocol::v777::play::clientbound::clear_dialog> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::clientbound::clear_dialog";
    static constexpr auto fields = std::make_tuple();
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::accept_teleportation'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::accept_teleportation> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::accept_teleportation";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"id", &::refrain::protocol::v777::play::serverbound::accept_teleportation::id},
        refrain::reflect::custom_field{"x", &::refrain::protocol::v777::play::serverbound::accept_teleportation::x},
        refrain::reflect::custom_field{"y", &::refrain::protocol::v777::play::serverbound::accept_teleportation::y},
        refrain::reflect::custom_field{"z", &::refrain::protocol::v777::play::serverbound::accept_teleportation::z},
        refrain::reflect::custom_field{"y_rot", &::refrain::protocol::v777::play::serverbound::accept_teleportation::y_rot},
        refrain::reflect::custom_field{"x_rot", &::refrain::protocol::v777::play::serverbound::accept_teleportation::x_rot});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::chat_command'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::chat_command> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::chat_command";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"command", &::refrain::protocol::v777::play::serverbound::chat_command::command});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::chat'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::chat> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::chat";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"message", &::refrain::protocol::v777::play::serverbound::chat::message},
        refrain::reflect::custom_field{"timestamp", &::refrain::protocol::v777::play::serverbound::chat::timestamp},
        refrain::reflect::custom_field{"salt", &::refrain::protocol::v777::play::serverbound::chat::salt},
        refrain::reflect::custom_field{"signature", &::refrain::protocol::v777::play::serverbound::chat::signature},
        refrain::reflect::custom_field{"offset", &::refrain::protocol::v777::play::serverbound::chat::offset},
        refrain::reflect::custom_field{"acknowledged", &::refrain::protocol::v777::play::serverbound::chat::acknowledged},
        refrain::reflect::custom_field{"checksum", &::refrain::protocol::v777::play::serverbound::chat::checksum});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::chunk_batch_received'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::chunk_batch_received> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::chunk_batch_received";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"desired_chunks", &::refrain::protocol::v777::play::serverbound::chunk_batch_received::desired_chunks});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::client_information'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::client_information> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::client_information";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"information", &::refrain::protocol::v777::play::serverbound::client_information::information});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::keep_alive'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::keep_alive> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::keep_alive";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"id", &::refrain::protocol::v777::play::serverbound::keep_alive::id});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::move_player_pos'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::move_player_pos> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::move_player_pos";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"x", &::refrain::protocol::v777::play::serverbound::move_player_pos::x},
        refrain::reflect::custom_field{"y", &::refrain::protocol::v777::play::serverbound::move_player_pos::y},
        refrain::reflect::custom_field{"z", &::refrain::protocol::v777::play::serverbound::move_player_pos::z},
        refrain::reflect::custom_field{"flags", &::refrain::protocol::v777::play::serverbound::move_player_pos::flags});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::move_player_pos_rot'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::move_player_pos_rot> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::move_player_pos_rot";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"x", &::refrain::protocol::v777::play::serverbound::move_player_pos_rot::x},
        refrain::reflect::custom_field{"y", &::refrain::protocol::v777::play::serverbound::move_player_pos_rot::y},
        refrain::reflect::custom_field{"z", &::refrain::protocol::v777::play::serverbound::move_player_pos_rot::z},
        refrain::reflect::custom_field{"y_rot", &::refrain::protocol::v777::play::serverbound::move_player_pos_rot::y_rot},
        refrain::reflect::custom_field{"x_rot", &::refrain::protocol::v777::play::serverbound::move_player_pos_rot::x_rot},
        refrain::reflect::custom_field{"flags", &::refrain::protocol::v777::play::serverbound::move_player_pos_rot::flags});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::move_player_rot'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::move_player_rot> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::move_player_rot";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"y_rot", &::refrain::protocol::v777::play::serverbound::move_player_rot::y_rot},
        refrain::reflect::custom_field{"x_rot", &::refrain::protocol::v777::play::serverbound::move_player_rot::x_rot},
        refrain::reflect::custom_field{"flags", &::refrain::protocol::v777::play::serverbound::move_player_rot::flags});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::move_player_status_only'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::move_player_status_only> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::move_player_status_only";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"flags", &::refrain::protocol::v777::play::serverbound::move_player_status_only::flags});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::player_action'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::player_action> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::player_action";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"action", &::refrain::protocol::v777::play::serverbound::player_action::action},
        refrain::reflect::custom_field{"position", &::refrain::protocol::v777::play::serverbound::player_action::position},
        refrain::reflect::custom_field{"direction", &::refrain::protocol::v777::play::serverbound::player_action::direction},
        refrain::reflect::custom_field{"sequence", &::refrain::protocol::v777::play::serverbound::player_action::sequence});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::player_loaded'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::player_loaded> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::player_loaded";
    static constexpr auto fields = std::make_tuple();
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::set_carried_item'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::set_carried_item> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::set_carried_item";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"slot", &::refrain::protocol::v777::play::serverbound::set_carried_item::slot});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::use_item_on'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::use_item_on> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::use_item_on";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"hand", &::refrain::protocol::v777::play::serverbound::use_item_on::hand},
        refrain::reflect::custom_field{"position", &::refrain::protocol::v777::play::serverbound::use_item_on::position},
        refrain::reflect::custom_field{"face", &::refrain::protocol::v777::play::serverbound::use_item_on::face},
        refrain::reflect::custom_field{"click_x", &::refrain::protocol::v777::play::serverbound::use_item_on::click_x},
        refrain::reflect::custom_field{"click_y", &::refrain::protocol::v777::play::serverbound::use_item_on::click_y},
        refrain::reflect::custom_field{"click_z", &::refrain::protocol::v777::play::serverbound::use_item_on::click_z},
        refrain::reflect::custom_field{"inside", &::refrain::protocol::v777::play::serverbound::use_item_on::inside},
        refrain::reflect::custom_field{"world_border", &::refrain::protocol::v777::play::serverbound::use_item_on::world_border},
        refrain::reflect::custom_field{"sequence", &::refrain::protocol::v777::play::serverbound::use_item_on::sequence});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::attack'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::attack> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::attack";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"entity_id", &::refrain::protocol::v777::play::serverbound::attack::entity_id});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::block_entity_tag_query'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::block_entity_tag_query> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::block_entity_tag_query";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"transaction_id", &::refrain::protocol::v777::play::serverbound::block_entity_tag_query::transaction_id},
        refrain::reflect::custom_field{"pos", &::refrain::protocol::v777::play::serverbound::block_entity_tag_query::pos});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::bundle_item_selected'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::bundle_item_selected> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::bundle_item_selected";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"slot_id", &::refrain::protocol::v777::play::serverbound::bundle_item_selected::slot_id},
        refrain::reflect::custom_field{"selected_item_index", &::refrain::protocol::v777::play::serverbound::bundle_item_selected::selected_item_index});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::change_difficulty'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::change_difficulty> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::change_difficulty";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"difficulty", &::refrain::protocol::v777::play::serverbound::change_difficulty::difficulty});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::change_game_mode'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::change_game_mode> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::change_game_mode";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"mode", &::refrain::protocol::v777::play::serverbound::change_game_mode::mode});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::chat_ack'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::chat_ack> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::chat_ack";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"offset", &::refrain::protocol::v777::play::serverbound::chat_ack::offset});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::client_command'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::client_command> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::client_command";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"action", &::refrain::protocol::v777::play::serverbound::client_command::action});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::client_tick_end'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::client_tick_end> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::client_tick_end";
    static constexpr auto fields = std::make_tuple();
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::configuration_acknowledged'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::configuration_acknowledged> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::configuration_acknowledged";
    static constexpr auto fields = std::make_tuple();
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::container_button_click'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::container_button_click> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::container_button_click";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"container_id", &::refrain::protocol::v777::play::serverbound::container_button_click::container_id},
        refrain::reflect::custom_field{"button_id", &::refrain::protocol::v777::play::serverbound::container_button_click::button_id});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::container_close'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::container_close> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::container_close";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"container_id", &::refrain::protocol::v777::play::serverbound::container_close::container_id});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::container_slot_state_changed'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::container_slot_state_changed> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::container_slot_state_changed";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"slot_id", &::refrain::protocol::v777::play::serverbound::container_slot_state_changed::slot_id},
        refrain::reflect::custom_field{"container_id", &::refrain::protocol::v777::play::serverbound::container_slot_state_changed::container_id},
        refrain::reflect::custom_field{"new_state", &::refrain::protocol::v777::play::serverbound::container_slot_state_changed::new_state});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::cookie_response'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::cookie_response> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::cookie_response";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"key", &::refrain::protocol::v777::play::serverbound::cookie_response::key},
        refrain::reflect::custom_field{"payload", &::refrain::protocol::v777::play::serverbound::cookie_response::payload});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::custom_payload'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::custom_payload> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::custom_payload";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"channel", &::refrain::protocol::v777::play::serverbound::custom_payload::channel},
        refrain::reflect::custom_field{"data", &::refrain::protocol::v777::play::serverbound::custom_payload::data});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::debug_subscription_request'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::debug_subscription_request> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::debug_subscription_request";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"payload", &::refrain::protocol::v777::play::serverbound::debug_subscription_request::payload});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::edit_book'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::edit_book> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::edit_book";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"slot", &::refrain::protocol::v777::play::serverbound::edit_book::slot},
        refrain::reflect::custom_field{"pages", &::refrain::protocol::v777::play::serverbound::edit_book::pages},
        refrain::reflect::custom_field{"title", &::refrain::protocol::v777::play::serverbound::edit_book::title});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::entity_tag_query'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::entity_tag_query> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::entity_tag_query";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"transaction_id", &::refrain::protocol::v777::play::serverbound::entity_tag_query::transaction_id},
        refrain::reflect::custom_field{"entity_id", &::refrain::protocol::v777::play::serverbound::entity_tag_query::entity_id});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::interact'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::interact> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::interact";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"entity_id", &::refrain::protocol::v777::play::serverbound::interact::entity_id},
        refrain::reflect::custom_field{"hand", &::refrain::protocol::v777::play::serverbound::interact::hand},
        refrain::reflect::custom_field{"location", &::refrain::protocol::v777::play::serverbound::interact::location},
        refrain::reflect::custom_field{"using_secondary_action", &::refrain::protocol::v777::play::serverbound::interact::using_secondary_action});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::jigsaw_generate'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::jigsaw_generate> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::jigsaw_generate";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"pos", &::refrain::protocol::v777::play::serverbound::jigsaw_generate::pos},
        refrain::reflect::custom_field{"levels", &::refrain::protocol::v777::play::serverbound::jigsaw_generate::levels},
        refrain::reflect::custom_field{"keep_jigsaws", &::refrain::protocol::v777::play::serverbound::jigsaw_generate::keep_jigsaws});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::lock_difficulty'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::lock_difficulty> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::lock_difficulty";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"locked", &::refrain::protocol::v777::play::serverbound::lock_difficulty::locked});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::move_vehicle'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::move_vehicle> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::move_vehicle";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"moving_to", &::refrain::protocol::v777::play::serverbound::move_vehicle::moving_to},
        refrain::reflect::custom_field{"on_ground", &::refrain::protocol::v777::play::serverbound::move_vehicle::on_ground});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::paddle_boat'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::paddle_boat> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::paddle_boat";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"left", &::refrain::protocol::v777::play::serverbound::paddle_boat::left},
        refrain::reflect::custom_field{"right", &::refrain::protocol::v777::play::serverbound::paddle_boat::right});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::pick_item_from_block'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::pick_item_from_block> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::pick_item_from_block";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"pos", &::refrain::protocol::v777::play::serverbound::pick_item_from_block::pos},
        refrain::reflect::custom_field{"include_data", &::refrain::protocol::v777::play::serverbound::pick_item_from_block::include_data});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::pick_item_from_entity'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::pick_item_from_entity> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::pick_item_from_entity";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"id", &::refrain::protocol::v777::play::serverbound::pick_item_from_entity::id},
        refrain::reflect::custom_field{"include_data", &::refrain::protocol::v777::play::serverbound::pick_item_from_entity::include_data});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::ping_request'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::ping_request> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::ping_request";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"time", &::refrain::protocol::v777::play::serverbound::ping_request::time});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::place_recipe'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::place_recipe> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::place_recipe";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"container_id", &::refrain::protocol::v777::play::serverbound::place_recipe::container_id},
        refrain::reflect::custom_field{"recipe", &::refrain::protocol::v777::play::serverbound::place_recipe::recipe},
        refrain::reflect::custom_field{"use_max_items", &::refrain::protocol::v777::play::serverbound::place_recipe::use_max_items});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::player_abilities'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::player_abilities> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::player_abilities";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"is_flying", &::refrain::protocol::v777::play::serverbound::player_abilities::is_flying});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::player_command'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::player_command> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::player_command";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"id", &::refrain::protocol::v777::play::serverbound::player_command::id},
        refrain::reflect::custom_field{"action", &::refrain::protocol::v777::play::serverbound::player_command::action},
        refrain::reflect::custom_field{"data", &::refrain::protocol::v777::play::serverbound::player_command::data});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::player_input'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::player_input> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::player_input";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"input", &::refrain::protocol::v777::play::serverbound::player_input::input});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::pong'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::pong> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::pong";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"id", &::refrain::protocol::v777::play::serverbound::pong::id});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::punch'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::punch> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::punch";
    static constexpr auto fields = std::make_tuple();
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::recipe_book_change_settings'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::recipe_book_change_settings> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::recipe_book_change_settings";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"book_type", &::refrain::protocol::v777::play::serverbound::recipe_book_change_settings::book_type},
        refrain::reflect::custom_field{"is_open", &::refrain::protocol::v777::play::serverbound::recipe_book_change_settings::is_open},
        refrain::reflect::custom_field{"is_filtering", &::refrain::protocol::v777::play::serverbound::recipe_book_change_settings::is_filtering});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::recipe_book_seen_recipe'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::recipe_book_seen_recipe> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::recipe_book_seen_recipe";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"recipe", &::refrain::protocol::v777::play::serverbound::recipe_book_seen_recipe::recipe});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::rename_item'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::rename_item> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::rename_item";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"name", &::refrain::protocol::v777::play::serverbound::rename_item::name});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::resource_pack'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::resource_pack> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::resource_pack";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"id", &::refrain::protocol::v777::play::serverbound::resource_pack::id},
        refrain::reflect::custom_field{"action", &::refrain::protocol::v777::play::serverbound::resource_pack::action});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::select_trade'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::select_trade> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::select_trade";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"item", &::refrain::protocol::v777::play::serverbound::select_trade::item});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::set_beacon'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::set_beacon> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::set_beacon";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"primary", &::refrain::protocol::v777::play::serverbound::set_beacon::primary},
        refrain::reflect::custom_field{"secondary", &::refrain::protocol::v777::play::serverbound::set_beacon::secondary});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::set_command_block'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::set_command_block> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::set_command_block";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"pos", &::refrain::protocol::v777::play::serverbound::set_command_block::pos},
        refrain::reflect::custom_field{"command", &::refrain::protocol::v777::play::serverbound::set_command_block::command},
        refrain::reflect::custom_field{"mode", &::refrain::protocol::v777::play::serverbound::set_command_block::mode},
        refrain::reflect::custom_field{"track_output", &::refrain::protocol::v777::play::serverbound::set_command_block::track_output});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::set_command_minecart'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::set_command_minecart> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::set_command_minecart";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"entity", &::refrain::protocol::v777::play::serverbound::set_command_minecart::entity},
        refrain::reflect::custom_field{"command", &::refrain::protocol::v777::play::serverbound::set_command_minecart::command},
        refrain::reflect::custom_field{"track_output", &::refrain::protocol::v777::play::serverbound::set_command_minecart::track_output});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::set_creative_mode_slot'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::set_creative_mode_slot> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::set_creative_mode_slot";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"slot_num", &::refrain::protocol::v777::play::serverbound::set_creative_mode_slot::slot_num},
        refrain::reflect::custom_field{"item_stack", &::refrain::protocol::v777::play::serverbound::set_creative_mode_slot::item_stack});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::set_game_rule'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::set_game_rule> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::set_game_rule";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"entries", &::refrain::protocol::v777::play::serverbound::set_game_rule::entries});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::set_jigsaw_block'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::set_jigsaw_block> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::set_jigsaw_block";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"pos", &::refrain::protocol::v777::play::serverbound::set_jigsaw_block::pos},
        refrain::reflect::custom_field{"name", &::refrain::protocol::v777::play::serverbound::set_jigsaw_block::name},
        refrain::reflect::custom_field{"target", &::refrain::protocol::v777::play::serverbound::set_jigsaw_block::target},
        refrain::reflect::custom_field{"pool", &::refrain::protocol::v777::play::serverbound::set_jigsaw_block::pool},
        refrain::reflect::custom_field{"final_state", &::refrain::protocol::v777::play::serverbound::set_jigsaw_block::final_state},
        refrain::reflect::custom_field{"joint", &::refrain::protocol::v777::play::serverbound::set_jigsaw_block::joint},
        refrain::reflect::custom_field{"selection_priority", &::refrain::protocol::v777::play::serverbound::set_jigsaw_block::selection_priority},
        refrain::reflect::custom_field{"placement_priority", &::refrain::protocol::v777::play::serverbound::set_jigsaw_block::placement_priority});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::set_structure_block'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::set_structure_block> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::set_structure_block";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"pos", &::refrain::protocol::v777::play::serverbound::set_structure_block::pos},
        refrain::reflect::custom_field{"update_type", &::refrain::protocol::v777::play::serverbound::set_structure_block::update_type},
        refrain::reflect::custom_field{"mode", &::refrain::protocol::v777::play::serverbound::set_structure_block::mode},
        refrain::reflect::custom_field{"name", &::refrain::protocol::v777::play::serverbound::set_structure_block::name},
        refrain::reflect::custom_field{"offset", &::refrain::protocol::v777::play::serverbound::set_structure_block::offset},
        refrain::reflect::custom_field{"size", &::refrain::protocol::v777::play::serverbound::set_structure_block::size},
        refrain::reflect::custom_field{"mirror", &::refrain::protocol::v777::play::serverbound::set_structure_block::mirror},
        refrain::reflect::custom_field{"rotation", &::refrain::protocol::v777::play::serverbound::set_structure_block::rotation},
        refrain::reflect::custom_field{"data", &::refrain::protocol::v777::play::serverbound::set_structure_block::data},
        refrain::reflect::custom_field{"integrity", &::refrain::protocol::v777::play::serverbound::set_structure_block::integrity},
        refrain::reflect::custom_field{"mirror_2", &::refrain::protocol::v777::play::serverbound::set_structure_block::mirror_2},
        refrain::reflect::custom_field{"rotation_2", &::refrain::protocol::v777::play::serverbound::set_structure_block::rotation_2},
        refrain::reflect::custom_field{"data_2", &::refrain::protocol::v777::play::serverbound::set_structure_block::data_2},
        refrain::reflect::custom_field{"integrity_2", &::refrain::protocol::v777::play::serverbound::set_structure_block::integrity_2},
        refrain::reflect::custom_field{"seed", &::refrain::protocol::v777::play::serverbound::set_structure_block::seed},
        refrain::reflect::custom_field{"field_15", &::refrain::protocol::v777::play::serverbound::set_structure_block::field_15});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::set_test_block'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::set_test_block> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::set_test_block";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"position", &::refrain::protocol::v777::play::serverbound::set_test_block::position},
        refrain::reflect::custom_field{"mode", &::refrain::protocol::v777::play::serverbound::set_test_block::mode},
        refrain::reflect::custom_field{"message", &::refrain::protocol::v777::play::serverbound::set_test_block::message});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::sign_update'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::sign_update> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::sign_update";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"pos", &::refrain::protocol::v777::play::serverbound::sign_update::pos},
        refrain::reflect::custom_field{"lines", &::refrain::protocol::v777::play::serverbound::sign_update::lines},
        refrain::reflect::custom_field{"slot", &::refrain::protocol::v777::play::serverbound::sign_update::slot});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::spectator_action'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::spectator_action> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::spectator_action";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"spectate_entity_id", &::refrain::protocol::v777::play::serverbound::spectator_action::spectate_entity_id});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::teleport_to_entity'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::teleport_to_entity> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::teleport_to_entity";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"uuid", &::refrain::protocol::v777::play::serverbound::teleport_to_entity::uuid});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::test_instance_block_action'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::test_instance_block_action> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::test_instance_block_action";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"pos", &::refrain::protocol::v777::play::serverbound::test_instance_block_action::pos},
        refrain::reflect::custom_field{"action", &::refrain::protocol::v777::play::serverbound::test_instance_block_action::action},
        refrain::reflect::custom_field{"data", &::refrain::protocol::v777::play::serverbound::test_instance_block_action::data});
};

/// Generated RSC packet 'refrain::protocol::v777::play::serverbound::use_item'.
template <> struct custom_traits<::refrain::protocol::v777::play::serverbound::use_item> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::play::serverbound::use_item";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"hand", &::refrain::protocol::v777::play::serverbound::use_item::hand},
        refrain::reflect::custom_field{"sequence", &::refrain::protocol::v777::play::serverbound::use_item::sequence},
        refrain::reflect::custom_field{"y_rot", &::refrain::protocol::v777::play::serverbound::use_item::y_rot},
        refrain::reflect::custom_field{"x_rot", &::refrain::protocol::v777::play::serverbound::use_item::x_rot});
};

/// Generated RSC packet 'refrain::protocol::v777::status::clientbound::status_response'.
template <> struct custom_traits<::refrain::protocol::v777::status::clientbound::status_response> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::status::clientbound::status_response";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"json", &::refrain::protocol::v777::status::clientbound::status_response::json});
};

/// Generated RSC packet 'refrain::protocol::v777::status::clientbound::pong_response'.
template <> struct custom_traits<::refrain::protocol::v777::status::clientbound::pong_response> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::status::clientbound::pong_response";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"time", &::refrain::protocol::v777::status::clientbound::pong_response::time});
};

/// Generated RSC packet 'refrain::protocol::v777::status::serverbound::status_request'.
template <> struct custom_traits<::refrain::protocol::v777::status::serverbound::status_request> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::status::serverbound::status_request";
    static constexpr auto fields = std::make_tuple();
};

/// Generated RSC packet 'refrain::protocol::v777::status::serverbound::ping_request'.
template <> struct custom_traits<::refrain::protocol::v777::status::serverbound::ping_request> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::status::serverbound::ping_request";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"time", &::refrain::protocol::v777::status::serverbound::ping_request::time});
};

/// Generated RSC struct 'refrain::protocol::v777::wire::wire_entries_entry'.
template <> struct custom_traits<::refrain::protocol::v777::wire::wire_entries_entry> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::wire::wire_entries_entry";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"field_0", &::refrain::protocol::v777::wire::wire_entries_entry::field_0},
        refrain::reflect::custom_field{"field_1", &::refrain::protocol::v777::wire::wire_entries_entry::field_1});
};

/// Generated RSC struct 'refrain::protocol::v777::wire::wire_entry_4'.
template <> struct custom_traits<::refrain::protocol::v777::wire::wire_entry_4> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::wire::wire_entry_4";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"game_rule_key", &::refrain::protocol::v777::wire::wire_entry_4::game_rule_key},
        refrain::reflect::custom_field{"value", &::refrain::protocol::v777::wire::wire_entry_4::value});
};

/// Generated RSC struct 'refrain::protocol::v777::wire::wire_data_3'.
template <> struct custom_traits<::refrain::protocol::v777::wire::wire_data_3> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::wire::wire_data_3";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"test", &::refrain::protocol::v777::wire::wire_data_3::test},
        refrain::reflect::custom_field{"size", &::refrain::protocol::v777::wire::wire_data_3::size},
        refrain::reflect::custom_field{"rotation", &::refrain::protocol::v777::wire::wire_data_3::rotation},
        refrain::reflect::custom_field{"ignore_entities", &::refrain::protocol::v777::wire::wire_data_3::ignore_entities},
        refrain::reflect::custom_field{"status", &::refrain::protocol::v777::wire::wire_data_3::status},
        refrain::reflect::custom_field{"error_message", &::refrain::protocol::v777::wire::wire_data_3::error_message});
};

/// Generated RSC struct 'refrain::protocol::v777::wire::wire_chunk_biome_data'.
template <> struct custom_traits<::refrain::protocol::v777::wire::wire_chunk_biome_data> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::wire::wire_chunk_biome_data";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"pos", &::refrain::protocol::v777::wire::wire_chunk_biome_data::pos},
        refrain::reflect::custom_field{"buffer", &::refrain::protocol::v777::wire::wire_chunk_biome_data::buffer});
};

/// Generated RSC struct 'refrain::protocol::v777::wire::wire_entry'.
template <> struct custom_traits<::refrain::protocol::v777::wire::wire_entry> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::wire::wire_entry";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"text", &::refrain::protocol::v777::wire::wire_entry::text},
        refrain::reflect::custom_field{"tooltip", &::refrain::protocol::v777::wire::wire_entry::tooltip});
};

/// Generated RSC struct 'refrain::protocol::v777::wire::wire_global_pos'.
template <> struct custom_traits<::refrain::protocol::v777::wire::wire_global_pos> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::wire::wire_global_pos";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"dimension", &::refrain::protocol::v777::wire::wire_global_pos::dimension},
        refrain::reflect::custom_field{"pos", &::refrain::protocol::v777::wire::wire_global_pos::pos});
};

/// Generated RSC struct 'refrain::protocol::v777::wire::wire_common_player_spawn_info'.
template <> struct custom_traits<::refrain::protocol::v777::wire::wire_common_player_spawn_info> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::wire::wire_common_player_spawn_info";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"dimension_type", &::refrain::protocol::v777::wire::wire_common_player_spawn_info::dimension_type},
        refrain::reflect::custom_field{"dimension", &::refrain::protocol::v777::wire::wire_common_player_spawn_info::dimension},
        refrain::reflect::custom_field{"seed", &::refrain::protocol::v777::wire::wire_common_player_spawn_info::seed},
        refrain::reflect::custom_field{"game_type", &::refrain::protocol::v777::wire::wire_common_player_spawn_info::game_type},
        refrain::reflect::custom_field{"previous_game_type", &::refrain::protocol::v777::wire::wire_common_player_spawn_info::previous_game_type},
        refrain::reflect::custom_field{"is_debug", &::refrain::protocol::v777::wire::wire_common_player_spawn_info::is_debug},
        refrain::reflect::custom_field{"is_flat", &::refrain::protocol::v777::wire::wire_common_player_spawn_info::is_flat},
        refrain::reflect::custom_field{"last_death_location", &::refrain::protocol::v777::wire::wire_common_player_spawn_info::last_death_location},
        refrain::reflect::custom_field{"portal_cooldown", &::refrain::protocol::v777::wire::wire_common_player_spawn_info::portal_cooldown},
        refrain::reflect::custom_field{"sea_level", &::refrain::protocol::v777::wire::wire_common_player_spawn_info::sea_level});
};

/// Generated RSC struct 'refrain::protocol::v777::wire::wire_lerp_steps'.
template <> struct custom_traits<::refrain::protocol::v777::wire::wire_lerp_steps> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::wire::wire_lerp_steps";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"position", &::refrain::protocol::v777::wire::wire_lerp_steps::position},
        refrain::reflect::custom_field{"movement", &::refrain::protocol::v777::wire::wire_lerp_steps::movement},
        refrain::reflect::custom_field{"y_rot", &::refrain::protocol::v777::wire::wire_lerp_steps::y_rot},
        refrain::reflect::custom_field{"x_rot", &::refrain::protocol::v777::wire::wire_lerp_steps::x_rot},
        refrain::reflect::custom_field{"weight", &::refrain::protocol::v777::wire::wire_lerp_steps::weight});
};

/// Generated RSC struct 'refrain::protocol::v777::wire::wire_recipe_display_id'.
template <> struct custom_traits<::refrain::protocol::v777::wire::wire_recipe_display_id> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::wire::wire_recipe_display_id";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"index", &::refrain::protocol::v777::wire::wire_recipe_display_id::index});
};

/// Generated RSC struct 'refrain::protocol::v777::wire::wire_type_settings'.
template <> struct custom_traits<::refrain::protocol::v777::wire::wire_type_settings> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::wire::wire_type_settings";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"open", &::refrain::protocol::v777::wire::wire_type_settings::open},
        refrain::reflect::custom_field{"filtering", &::refrain::protocol::v777::wire::wire_type_settings::filtering});
};

/// Generated RSC struct 'refrain::protocol::v777::wire::wire_book_settings'.
template <> struct custom_traits<::refrain::protocol::v777::wire::wire_book_settings> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::wire::wire_book_settings";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"o_o_crafting", &::refrain::protocol::v777::wire::wire_book_settings::o_o_crafting},
        refrain::reflect::custom_field{"o_o_furnace", &::refrain::protocol::v777::wire::wire_book_settings::o_o_furnace},
        refrain::reflect::custom_field{"o_o_blast_furnace", &::refrain::protocol::v777::wire::wire_book_settings::o_o_blast_furnace},
        refrain::reflect::custom_field{"o_o_smoker", &::refrain::protocol::v777::wire::wire_book_settings::o_o_smoker});
};

/// Generated RSC struct 'refrain::protocol::v777::wire::wire_swing_animation'.
template <> struct custom_traits<::refrain::protocol::v777::wire::wire_swing_animation> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::wire::wire_swing_animation";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"type_", &::refrain::protocol::v777::wire::wire_swing_animation::type_},
        refrain::reflect::custom_field{"duration", &::refrain::protocol::v777::wire::wire_swing_animation::duration});
};

/// Generated RSC struct 'refrain::protocol::v777::wire::wire_position_and_rotation'.
template <> struct custom_traits<::refrain::protocol::v777::wire::wire_position_and_rotation> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::wire::wire_position_and_rotation";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"position", &::refrain::protocol::v777::wire::wire_position_and_rotation::position},
        refrain::reflect::custom_field{"y_rot", &::refrain::protocol::v777::wire::wire_position_and_rotation::y_rot},
        refrain::reflect::custom_field{"x_rot", &::refrain::protocol::v777::wire::wire_position_and_rotation::x_rot});
};

/// Generated RSC struct 'refrain::protocol::v777::wire::wire_position_move_rotation'.
template <> struct custom_traits<::refrain::protocol::v777::wire::wire_position_move_rotation> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::wire::wire_position_move_rotation";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"position", &::refrain::protocol::v777::wire::wire_position_move_rotation::position},
        refrain::reflect::custom_field{"delta_movement", &::refrain::protocol::v777::wire::wire_position_move_rotation::delta_movement},
        refrain::reflect::custom_field{"y_rot", &::refrain::protocol::v777::wire::wire_position_move_rotation::y_rot},
        refrain::reflect::custom_field{"x_rot", &::refrain::protocol::v777::wire::wire_position_move_rotation::x_rot});
};

/// Generated RSC struct 'refrain::protocol::v777::wire::wire_vec3'.
template <> struct custom_traits<::refrain::protocol::v777::wire::wire_vec3> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::wire::wire_vec3";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"x", &::refrain::protocol::v777::wire::wire_vec3::x},
        refrain::reflect::custom_field{"y", &::refrain::protocol::v777::wire::wire_vec3::y},
        refrain::reflect::custom_field{"z", &::refrain::protocol::v777::wire::wire_vec3::z});
};

/// Generated RSC struct 'refrain::protocol::v777::wire::wire_vec3i'.
template <> struct custom_traits<::refrain::protocol::v777::wire::wire_vec3i> {
    static constexpr std::string_view type_name = "refrain::protocol::v777::wire::wire_vec3i";
    static constexpr auto fields = std::make_tuple(
        refrain::reflect::custom_field{"x", &::refrain::protocol::v777::wire::wire_vec3i::x},
        refrain::reflect::custom_field{"y", &::refrain::protocol::v777::wire::wire_vec3i::y},
        refrain::reflect::custom_field{"z", &::refrain::protocol::v777::wire::wire_vec3i::z});
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
