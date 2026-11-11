// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/net/frame.hpp>
#include <refrain/net/wire.hpp>
#include <refrain/schema/protocol/777/configuration_serverbound.hpp>
#include <refrain/schema/protocol/777/login_serverbound.hpp>
#include <refrain/schema/protocol/777/play_serverbound.hpp>

#include <boost/ut.hpp>

#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace net = refrain::net;
namespace config_sb = refrain::protocol::v777::configuration::serverbound;
namespace login_sb = refrain::protocol::v777::login::serverbound;
namespace play_sb = refrain::protocol::v777::play::serverbound;

[[nodiscard]] auto random_bytes(std::mt19937_64 &engine, std::size_t limit)
    -> std::vector<std::byte> {
    std::uniform_int_distribution<std::size_t> length{0, limit};
    std::vector<std::byte> bytes(length(engine));
    std::uniform_int_distribution<unsigned int> value{0, 255};
    for (auto &byte : bytes) {
        byte = static_cast<std::byte>(value(engine));
    }
    return bytes;
}

template <typename Packet, typename Decoder>
void try_decode(Decoder decode, std::span<const std::byte> bytes) {
    net::reader in{bytes};
    Packet packet{};
    const auto status{decode(in, packet)};
    static_cast<void>(status);
}

/// Encode a default-constructed packet, then decode every truncated prefix and
/// a byte-flipped copy. The decoder sees a valid header and partial payload,
/// which is the shape a truncated or corrupted client packet has.
template <typename Packet, typename Encoder, typename Decoder>
void try_mutated(Encoder encode, Decoder decode, std::mt19937_64 &engine) {
    std::vector<std::byte> body;
    net::writer out{body};
    Packet source{};
    if (!encode(out, source)) {
        return;
    }
    for (std::size_t length{0}; length < body.size(); ++length) {
        net::reader in{std::span<const std::byte>{body}.first(length)};
        Packet packet{};
        static_cast<void>(decode(in, packet));
    }
    if (body.empty()) {
        return;
    }
    std::uniform_int_distribution<std::size_t> offset{0, body.size() - 1};
    std::uniform_int_distribution<unsigned int> value{0, 255};
    for (int round{0}; round < 4; ++round) {
        auto mutated{body};
        for (int flip{0}; flip < 3; ++flip) {
            mutated[offset(engine)] = static_cast<std::byte>(value(engine));
        }
        net::reader in{mutated};
        Packet packet{};
        static_cast<void>(decode(in, packet));
    }
}

} // namespace

suite fuzz_matrix_tests = [] {
    // Every serverbound packet decoder is fed random bytes and truncated or
    // corrupted encodings of itself. The suite asserts only that no input
    // crashes or hangs a decoder; malformed input is expected to return an
    // error.
    "net/every serverbound decoder survives random and mutated input"_test =
        [] {
            std::mt19937_64 engine{0x9E3779B97F4A7C15ULL};
            for (int iteration{0}; iteration < 20; ++iteration) {
                const auto bytes{random_bytes(engine, 128)};
                try_decode<play_sb::accept_teleportation>(
                    play_sb::decode_accept_teleportation, bytes);
                try_mutated<play_sb::accept_teleportation>(
                    play_sb::encode_accept_teleportation,
                    play_sb::decode_accept_teleportation, engine);
                try_decode<play_sb::chat_command>(play_sb::decode_chat_command,
                                                  bytes);
                try_mutated<play_sb::chat_command>(play_sb::encode_chat_command,
                                                   play_sb::decode_chat_command,
                                                   engine);
                try_decode<play_sb::chat>(play_sb::decode_chat, bytes);
                try_mutated<play_sb::chat>(play_sb::encode_chat,
                                           play_sb::decode_chat, engine);
                try_decode<play_sb::chunk_batch_received>(
                    play_sb::decode_chunk_batch_received, bytes);
                try_mutated<play_sb::chunk_batch_received>(
                    play_sb::encode_chunk_batch_received,
                    play_sb::decode_chunk_batch_received, engine);
                try_decode<play_sb::client_information>(
                    play_sb::decode_client_information, bytes);
                try_mutated<play_sb::client_information>(
                    play_sb::encode_client_information,
                    play_sb::decode_client_information, engine);
                try_decode<play_sb::keep_alive>(play_sb::decode_keep_alive,
                                                bytes);
                try_mutated<play_sb::keep_alive>(play_sb::encode_keep_alive,
                                                 play_sb::decode_keep_alive,
                                                 engine);
                try_decode<play_sb::move_player_pos>(
                    play_sb::decode_move_player_pos, bytes);
                try_mutated<play_sb::move_player_pos>(
                    play_sb::encode_move_player_pos,
                    play_sb::decode_move_player_pos, engine);
                try_decode<play_sb::move_player_pos_rot>(
                    play_sb::decode_move_player_pos_rot, bytes);
                try_mutated<play_sb::move_player_pos_rot>(
                    play_sb::encode_move_player_pos_rot,
                    play_sb::decode_move_player_pos_rot, engine);
                try_decode<play_sb::move_player_rot>(
                    play_sb::decode_move_player_rot, bytes);
                try_mutated<play_sb::move_player_rot>(
                    play_sb::encode_move_player_rot,
                    play_sb::decode_move_player_rot, engine);
                try_decode<play_sb::move_player_status_only>(
                    play_sb::decode_move_player_status_only, bytes);
                try_mutated<play_sb::move_player_status_only>(
                    play_sb::encode_move_player_status_only,
                    play_sb::decode_move_player_status_only, engine);
                try_decode<play_sb::player_action>(
                    play_sb::decode_player_action, bytes);
                try_mutated<play_sb::player_action>(
                    play_sb::encode_player_action,
                    play_sb::decode_player_action, engine);
                try_decode<play_sb::player_loaded>(
                    play_sb::decode_player_loaded, bytes);
                try_mutated<play_sb::player_loaded>(
                    play_sb::encode_player_loaded,
                    play_sb::decode_player_loaded, engine);
                try_decode<play_sb::set_carried_item>(
                    play_sb::decode_set_carried_item, bytes);
                try_mutated<play_sb::set_carried_item>(
                    play_sb::encode_set_carried_item,
                    play_sb::decode_set_carried_item, engine);
                try_decode<play_sb::use_item_on>(play_sb::decode_use_item_on,
                                                 bytes);
                try_mutated<play_sb::use_item_on>(play_sb::encode_use_item_on,
                                                  play_sb::decode_use_item_on,
                                                  engine);
                try_decode<play_sb::attack>(play_sb::decode_attack, bytes);
                try_mutated<play_sb::attack>(play_sb::encode_attack,
                                             play_sb::decode_attack, engine);
                try_decode<play_sb::block_entity_tag_query>(
                    play_sb::decode_block_entity_tag_query, bytes);
                try_mutated<play_sb::block_entity_tag_query>(
                    play_sb::encode_block_entity_tag_query,
                    play_sb::decode_block_entity_tag_query, engine);
                try_decode<play_sb::bundle_item_selected>(
                    play_sb::decode_bundle_item_selected, bytes);
                try_mutated<play_sb::bundle_item_selected>(
                    play_sb::encode_bundle_item_selected,
                    play_sb::decode_bundle_item_selected, engine);
                try_decode<play_sb::change_difficulty>(
                    play_sb::decode_change_difficulty, bytes);
                try_mutated<play_sb::change_difficulty>(
                    play_sb::encode_change_difficulty,
                    play_sb::decode_change_difficulty, engine);
                try_decode<play_sb::change_game_mode>(
                    play_sb::decode_change_game_mode, bytes);
                try_mutated<play_sb::change_game_mode>(
                    play_sb::encode_change_game_mode,
                    play_sb::decode_change_game_mode, engine);
                try_decode<play_sb::chat_ack>(play_sb::decode_chat_ack, bytes);
                try_mutated<play_sb::chat_ack>(
                    play_sb::encode_chat_ack, play_sb::decode_chat_ack, engine);
                try_decode<play_sb::chat_command_signed>(
                    play_sb::decode_chat_command_signed, bytes);
                try_mutated<play_sb::chat_command_signed>(
                    play_sb::encode_chat_command_signed,
                    play_sb::decode_chat_command_signed, engine);
                try_decode<play_sb::chat_session_update>(
                    play_sb::decode_chat_session_update, bytes);
                try_mutated<play_sb::chat_session_update>(
                    play_sb::encode_chat_session_update,
                    play_sb::decode_chat_session_update, engine);
                try_decode<play_sb::client_command>(
                    play_sb::decode_client_command, bytes);
                try_mutated<play_sb::client_command>(
                    play_sb::encode_client_command,
                    play_sb::decode_client_command, engine);
                try_decode<play_sb::client_tick_end>(
                    play_sb::decode_client_tick_end, bytes);
                try_mutated<play_sb::client_tick_end>(
                    play_sb::encode_client_tick_end,
                    play_sb::decode_client_tick_end, engine);
                try_decode<play_sb::command_suggestion>(
                    play_sb::decode_command_suggestion, bytes);
                try_mutated<play_sb::command_suggestion>(
                    play_sb::encode_command_suggestion,
                    play_sb::decode_command_suggestion, engine);
                try_decode<play_sb::configuration_acknowledged>(
                    play_sb::decode_configuration_acknowledged, bytes);
                try_mutated<play_sb::configuration_acknowledged>(
                    play_sb::encode_configuration_acknowledged,
                    play_sb::decode_configuration_acknowledged, engine);
                try_decode<play_sb::container_button_click>(
                    play_sb::decode_container_button_click, bytes);
                try_mutated<play_sb::container_button_click>(
                    play_sb::encode_container_button_click,
                    play_sb::decode_container_button_click, engine);
                try_decode<play_sb::container_click>(
                    play_sb::decode_container_click, bytes);
                try_mutated<play_sb::container_click>(
                    play_sb::encode_container_click,
                    play_sb::decode_container_click, engine);
                try_decode<play_sb::container_close>(
                    play_sb::decode_container_close, bytes);
                try_mutated<play_sb::container_close>(
                    play_sb::encode_container_close,
                    play_sb::decode_container_close, engine);
                try_decode<play_sb::container_slot_state_changed>(
                    play_sb::decode_container_slot_state_changed, bytes);
                try_mutated<play_sb::container_slot_state_changed>(
                    play_sb::encode_container_slot_state_changed,
                    play_sb::decode_container_slot_state_changed, engine);
                try_decode<play_sb::cookie_response>(
                    play_sb::decode_cookie_response, bytes);
                try_mutated<play_sb::cookie_response>(
                    play_sb::encode_cookie_response,
                    play_sb::decode_cookie_response, engine);
                try_decode<play_sb::custom_payload>(
                    play_sb::decode_custom_payload, bytes);
                try_mutated<play_sb::custom_payload>(
                    play_sb::encode_custom_payload,
                    play_sb::decode_custom_payload, engine);
                try_decode<play_sb::debug_subscription_request>(
                    play_sb::decode_debug_subscription_request, bytes);
                try_mutated<play_sb::debug_subscription_request>(
                    play_sb::encode_debug_subscription_request,
                    play_sb::decode_debug_subscription_request, engine);
                try_decode<play_sb::edit_book>(play_sb::decode_edit_book,
                                               bytes);
                try_mutated<play_sb::edit_book>(play_sb::encode_edit_book,
                                                play_sb::decode_edit_book,
                                                engine);
                try_decode<play_sb::entity_tag_query>(
                    play_sb::decode_entity_tag_query, bytes);
                try_mutated<play_sb::entity_tag_query>(
                    play_sb::encode_entity_tag_query,
                    play_sb::decode_entity_tag_query, engine);
                try_decode<play_sb::interact>(play_sb::decode_interact, bytes);
                try_mutated<play_sb::interact>(
                    play_sb::encode_interact, play_sb::decode_interact, engine);
                try_decode<play_sb::jigsaw_generate>(
                    play_sb::decode_jigsaw_generate, bytes);
                try_mutated<play_sb::jigsaw_generate>(
                    play_sb::encode_jigsaw_generate,
                    play_sb::decode_jigsaw_generate, engine);
                try_decode<play_sb::lock_difficulty>(
                    play_sb::decode_lock_difficulty, bytes);
                try_mutated<play_sb::lock_difficulty>(
                    play_sb::encode_lock_difficulty,
                    play_sb::decode_lock_difficulty, engine);
                try_decode<play_sb::move_vehicle>(play_sb::decode_move_vehicle,
                                                  bytes);
                try_mutated<play_sb::move_vehicle>(play_sb::encode_move_vehicle,
                                                   play_sb::decode_move_vehicle,
                                                   engine);
                try_decode<play_sb::paddle_boat>(play_sb::decode_paddle_boat,
                                                 bytes);
                try_mutated<play_sb::paddle_boat>(play_sb::encode_paddle_boat,
                                                  play_sb::decode_paddle_boat,
                                                  engine);
                try_decode<play_sb::pick_item_from_block>(
                    play_sb::decode_pick_item_from_block, bytes);
                try_mutated<play_sb::pick_item_from_block>(
                    play_sb::encode_pick_item_from_block,
                    play_sb::decode_pick_item_from_block, engine);
                try_decode<play_sb::pick_item_from_entity>(
                    play_sb::decode_pick_item_from_entity, bytes);
                try_mutated<play_sb::pick_item_from_entity>(
                    play_sb::encode_pick_item_from_entity,
                    play_sb::decode_pick_item_from_entity, engine);
                try_decode<play_sb::ping_request>(play_sb::decode_ping_request,
                                                  bytes);
                try_mutated<play_sb::ping_request>(play_sb::encode_ping_request,
                                                   play_sb::decode_ping_request,
                                                   engine);
                try_decode<play_sb::place_recipe>(play_sb::decode_place_recipe,
                                                  bytes);
                try_mutated<play_sb::place_recipe>(play_sb::encode_place_recipe,
                                                   play_sb::decode_place_recipe,
                                                   engine);
                try_decode<play_sb::player_abilities>(
                    play_sb::decode_player_abilities, bytes);
                try_mutated<play_sb::player_abilities>(
                    play_sb::encode_player_abilities,
                    play_sb::decode_player_abilities, engine);
                try_decode<play_sb::player_command>(
                    play_sb::decode_player_command, bytes);
                try_mutated<play_sb::player_command>(
                    play_sb::encode_player_command,
                    play_sb::decode_player_command, engine);
                try_decode<play_sb::player_input>(play_sb::decode_player_input,
                                                  bytes);
                try_mutated<play_sb::player_input>(play_sb::encode_player_input,
                                                   play_sb::decode_player_input,
                                                   engine);
                try_decode<play_sb::pong>(play_sb::decode_pong, bytes);
                try_mutated<play_sb::pong>(play_sb::encode_pong,
                                           play_sb::decode_pong, engine);
                try_decode<play_sb::punch>(play_sb::decode_punch, bytes);
                try_mutated<play_sb::punch>(play_sb::encode_punch,
                                            play_sb::decode_punch, engine);
                try_decode<play_sb::recipe_book_change_settings>(
                    play_sb::decode_recipe_book_change_settings, bytes);
                try_mutated<play_sb::recipe_book_change_settings>(
                    play_sb::encode_recipe_book_change_settings,
                    play_sb::decode_recipe_book_change_settings, engine);
                try_decode<play_sb::recipe_book_seen_recipe>(
                    play_sb::decode_recipe_book_seen_recipe, bytes);
                try_mutated<play_sb::recipe_book_seen_recipe>(
                    play_sb::encode_recipe_book_seen_recipe,
                    play_sb::decode_recipe_book_seen_recipe, engine);
                try_decode<play_sb::rename_item>(play_sb::decode_rename_item,
                                                 bytes);
                try_mutated<play_sb::rename_item>(play_sb::encode_rename_item,
                                                  play_sb::decode_rename_item,
                                                  engine);
                try_decode<play_sb::resource_pack>(
                    play_sb::decode_resource_pack, bytes);
                try_mutated<play_sb::resource_pack>(
                    play_sb::encode_resource_pack,
                    play_sb::decode_resource_pack, engine);
                try_decode<play_sb::seen_advancements>(
                    play_sb::decode_seen_advancements, bytes);
                try_mutated<play_sb::seen_advancements>(
                    play_sb::encode_seen_advancements,
                    play_sb::decode_seen_advancements, engine);
                try_decode<play_sb::select_trade>(play_sb::decode_select_trade,
                                                  bytes);
                try_mutated<play_sb::select_trade>(play_sb::encode_select_trade,
                                                   play_sb::decode_select_trade,
                                                   engine);
                try_decode<play_sb::set_beacon>(play_sb::decode_set_beacon,
                                                bytes);
                try_mutated<play_sb::set_beacon>(play_sb::encode_set_beacon,
                                                 play_sb::decode_set_beacon,
                                                 engine);
                try_decode<play_sb::set_command_block>(
                    play_sb::decode_set_command_block, bytes);
                try_mutated<play_sb::set_command_block>(
                    play_sb::encode_set_command_block,
                    play_sb::decode_set_command_block, engine);
                try_decode<play_sb::set_command_minecart>(
                    play_sb::decode_set_command_minecart, bytes);
                try_mutated<play_sb::set_command_minecart>(
                    play_sb::encode_set_command_minecart,
                    play_sb::decode_set_command_minecart, engine);
                try_decode<play_sb::set_creative_mode_slot>(
                    play_sb::decode_set_creative_mode_slot, bytes);
                try_mutated<play_sb::set_creative_mode_slot>(
                    play_sb::encode_set_creative_mode_slot,
                    play_sb::decode_set_creative_mode_slot, engine);
                try_decode<play_sb::set_game_rule>(
                    play_sb::decode_set_game_rule, bytes);
                try_mutated<play_sb::set_game_rule>(
                    play_sb::encode_set_game_rule,
                    play_sb::decode_set_game_rule, engine);
                try_decode<play_sb::set_jigsaw_block>(
                    play_sb::decode_set_jigsaw_block, bytes);
                try_mutated<play_sb::set_jigsaw_block>(
                    play_sb::encode_set_jigsaw_block,
                    play_sb::decode_set_jigsaw_block, engine);
                try_decode<play_sb::set_structure_block>(
                    play_sb::decode_set_structure_block, bytes);
                try_mutated<play_sb::set_structure_block>(
                    play_sb::encode_set_structure_block,
                    play_sb::decode_set_structure_block, engine);
                try_decode<play_sb::set_test_block>(
                    play_sb::decode_set_test_block, bytes);
                try_mutated<play_sb::set_test_block>(
                    play_sb::encode_set_test_block,
                    play_sb::decode_set_test_block, engine);
                try_decode<play_sb::sign_update>(play_sb::decode_sign_update,
                                                 bytes);
                try_mutated<play_sb::sign_update>(play_sb::encode_sign_update,
                                                  play_sb::decode_sign_update,
                                                  engine);
                try_decode<play_sb::spectator_action>(
                    play_sb::decode_spectator_action, bytes);
                try_mutated<play_sb::spectator_action>(
                    play_sb::encode_spectator_action,
                    play_sb::decode_spectator_action, engine);
                try_decode<play_sb::teleport_to_entity>(
                    play_sb::decode_teleport_to_entity, bytes);
                try_mutated<play_sb::teleport_to_entity>(
                    play_sb::encode_teleport_to_entity,
                    play_sb::decode_teleport_to_entity, engine);
                try_decode<play_sb::test_instance_block_action>(
                    play_sb::decode_test_instance_block_action, bytes);
                try_mutated<play_sb::test_instance_block_action>(
                    play_sb::encode_test_instance_block_action,
                    play_sb::decode_test_instance_block_action, engine);
                try_decode<play_sb::use_item>(play_sb::decode_use_item, bytes);
                try_mutated<play_sb::use_item>(
                    play_sb::encode_use_item, play_sb::decode_use_item, engine);
                try_decode<play_sb::custom_click_action>(
                    play_sb::decode_custom_click_action, bytes);
                try_mutated<play_sb::custom_click_action>(
                    play_sb::encode_custom_click_action,
                    play_sb::decode_custom_click_action, engine);
                try_decode<config_sb::client_information>(
                    config_sb::decode_client_information, bytes);
                try_mutated<config_sb::client_information>(
                    config_sb::encode_client_information,
                    config_sb::decode_client_information, engine);
                try_decode<config_sb::cookie_response>(
                    config_sb::decode_cookie_response, bytes);
                try_mutated<config_sb::cookie_response>(
                    config_sb::encode_cookie_response,
                    config_sb::decode_cookie_response, engine);
                try_decode<config_sb::custom_payload>(
                    config_sb::decode_custom_payload, bytes);
                try_mutated<config_sb::custom_payload>(
                    config_sb::encode_custom_payload,
                    config_sb::decode_custom_payload, engine);
                try_decode<config_sb::finish_configuration>(
                    config_sb::decode_finish_configuration, bytes);
                try_mutated<config_sb::finish_configuration>(
                    config_sb::encode_finish_configuration,
                    config_sb::decode_finish_configuration, engine);
                try_decode<config_sb::keep_alive>(config_sb::decode_keep_alive,
                                                  bytes);
                try_mutated<config_sb::keep_alive>(config_sb::encode_keep_alive,
                                                   config_sb::decode_keep_alive,
                                                   engine);
                try_decode<config_sb::pong>(config_sb::decode_pong, bytes);
                try_mutated<config_sb::pong>(config_sb::encode_pong,
                                             config_sb::decode_pong, engine);
                try_decode<config_sb::resource_pack>(
                    config_sb::decode_resource_pack, bytes);
                try_mutated<config_sb::resource_pack>(
                    config_sb::encode_resource_pack,
                    config_sb::decode_resource_pack, engine);
                try_decode<config_sb::select_known_packs>(
                    config_sb::decode_select_known_packs, bytes);
                try_mutated<config_sb::select_known_packs>(
                    config_sb::encode_select_known_packs,
                    config_sb::decode_select_known_packs, engine);
                try_decode<config_sb::accept_code_of_conduct>(
                    config_sb::decode_accept_code_of_conduct, bytes);
                try_mutated<config_sb::accept_code_of_conduct>(
                    config_sb::encode_accept_code_of_conduct,
                    config_sb::decode_accept_code_of_conduct, engine);
                try_decode<config_sb::custom_click_action>(
                    config_sb::decode_custom_click_action, bytes);
                try_mutated<config_sb::custom_click_action>(
                    config_sb::encode_custom_click_action,
                    config_sb::decode_custom_click_action, engine);
                try_decode<login_sb::hello>(login_sb::decode_hello, bytes);
                try_mutated<login_sb::hello>(login_sb::encode_hello,
                                             login_sb::decode_hello, engine);
                try_decode<login_sb::key>(login_sb::decode_key, bytes);
                try_mutated<login_sb::key>(login_sb::encode_key,
                                           login_sb::decode_key, engine);
                try_decode<login_sb::login_acknowledged>(
                    login_sb::decode_login_acknowledged, bytes);
                try_mutated<login_sb::login_acknowledged>(
                    login_sb::encode_login_acknowledged,
                    login_sb::decode_login_acknowledged, engine);
                try_decode<login_sb::custom_query_answer>(
                    login_sb::decode_custom_query_answer, bytes);
                try_mutated<login_sb::custom_query_answer>(
                    login_sb::encode_custom_query_answer,
                    login_sb::decode_custom_query_answer, engine);
                try_decode<login_sb::cookie_response>(
                    login_sb::decode_cookie_response, bytes);
                try_mutated<login_sb::cookie_response>(
                    login_sb::encode_cookie_response,
                    login_sb::decode_cookie_response, engine);
            }
            expect(true);
        };
};
