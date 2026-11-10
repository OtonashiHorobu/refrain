// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/json.hpp>
#include <refrain/net/base.hpp>
#include <refrain/schema/protocol/777/catalog.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using namespace boost::ut;

namespace json = refrain::data::json;
namespace net = refrain::net;
namespace v777 = refrain::protocol::v777;
namespace common = refrain::protocol::v777::common;

template <typename Packet, typename Encoder, typename Decoder>
void round_trip(std::uint16_t packet_id, const Packet &packet, Encoder encode,
                Decoder decode) {
    std::vector<std::byte> body;
    net::writer out{body};
    out.write_varint(packet_id);
    auto encoded{encode(out, packet)};
    expect(encoded.has_value());
    if (!encoded) {
        return;
    }
    net::reader in{body};
    auto decoded_id{in.read_varint()};
    expect(decoded_id.has_value());
    if (!decoded_id) {
        return;
    }
    expect(*decoded_id == packet_id);
    Packet decoded{};
    auto status{decode(in, decoded)};
    expect(status.has_value());
    expect(in.empty()) << "codec must consume the whole packet body";
    if (!status) {
        return;
    }
}

[[nodiscard]] auto stage_of(std::string_view name)
    -> std::optional<net::connection_state> {
    for (const auto state :
         {net::connection_state::handshake, net::connection_state::status,
          net::connection_state::login, net::connection_state::configuration,
          net::connection_state::play}) {
        if (net::to_string(state) == name) {
            return state;
        }
    }
    return std::nullopt;
}

[[nodiscard]] auto flow_of(std::string_view name)
    -> std::optional<net::packet_flow> {
    for (const auto flow :
         {net::packet_flow::serverbound, net::packet_flow::clientbound}) {
        if (net::to_string(flow) == name) {
            return flow;
        }
    }
    return std::nullopt;
}

struct catalog_key {
    net::connection_state state;
    net::packet_flow flow;
    std::uint16_t id;

    friend auto operator==(const catalog_key &, const catalog_key &)
        -> bool = default;
};

const suite tests = [] {
    "net/protocol join payload round trips"_test = [] {
        v777::play::clientbound::login packet{};
        packet.player_id = 7;
        packet.hardcore = true;
        packet.levels = {
            *refrain::data::resource_location::parse("minecraft:overworld")};
        packet.max_players = 20;
        packet.chunk_radius = 10;
        packet.simulation_distance = 8;
        packet.reduced_debug_info = false;
        packet.show_death_screen = true;
        packet.limited_crafting = false;
        packet.spawn.dimension_type = 0;
        packet.spawn.dimension =
            *refrain::data::resource_location::parse("minecraft:overworld");
        packet.spawn.seed = -42;
        packet.spawn.game_type = 1;
        packet.spawn.previous_game_type = 0U;
        packet.spawn.debug = false;
        packet.spawn.flat = false;
        packet.spawn.last_death_location = std::nullopt;
        packet.spawn.portal_cooldown = 0;
        packet.spawn.sea_level = 63;
        packet.online_mode = false;
        packet.enforces_secure_chat = false;

        std::vector<std::byte> body;
        net::writer out{body};
        out.write_varint(v777::play::clientbound::login::packet_id);
        const auto encoded{v777::play::clientbound::encode_login(out, packet)};
        expect(encoded.has_value());
        if (!encoded) {
            return;
        }
        net::reader in{body};
        auto id{in.read_varint()};
        expect(id.has_value());
        v777::play::clientbound::login decoded{};
        expect(v777::play::clientbound::decode_login(in, decoded).has_value());
        expect(in.empty());
        expect(decoded.player_id == 7);
        expect(decoded.hardcore);
        expect(decoded.levels.size() == 1U);
        expect(decoded.spawn.seed == -42);
        expect(decoded.spawn.previous_game_type.has_value());
        expect(*decoded.spawn.previous_game_type == 0U);
        expect(decoded.spawn.sea_level == 63);
    };

    "net/protocol chat keeps a fixed signature slot"_test = [] {
        v777::play::serverbound::chat packet{};
        packet.message = "hello";
        packet.timestamp = 1'700'000'000'000;
        packet.salt = 42;
        packet.signature = std::vector<std::byte>(256, std::byte{0xAB});
        packet.offset = 0;
        packet.acknowledged = std::vector<std::byte>(3, std::byte{0xFF});
        packet.checksum = -1;

        std::vector<std::byte> body;
        net::writer out{body};
        out.write_varint(v777::play::serverbound::chat::packet_id);
        expect(v777::play::serverbound::encode_chat(out, packet).has_value());
        net::reader in{body};
        auto id{in.read_varint()};
        expect(id.has_value());
        v777::play::serverbound::chat decoded{};
        expect(v777::play::serverbound::decode_chat(in, decoded).has_value());
        expect(in.empty());
        expect(decoded.message == "hello");
        expect(decoded.signature.has_value());
        expect(decoded.signature->size() == 256U);
        expect(decoded.acknowledged.size() == 3U);
        expect(decoded.checksum == -1);

        packet.signature = std::nullopt;
        std::vector<std::byte> unsigned_body;
        net::writer unsigned_out{unsigned_body};
        unsigned_out.write_varint(v777::play::serverbound::chat::packet_id);
        expect(v777::play::serverbound::encode_chat(unsigned_out, packet)
                   .has_value());
        net::reader unsigned_in{unsigned_body};
        auto unsigned_id{unsigned_in.read_varint()};
        expect(unsigned_id.has_value());
        v777::play::serverbound::chat unsigned_decoded{};
        expect(
            v777::play::serverbound::decode_chat(unsigned_in, unsigned_decoded)
                .has_value());
        expect(unsigned_in.empty());
        expect(!unsigned_decoded.signature.has_value());

        packet.signature = std::vector<std::byte>(255, std::byte{0});
        std::vector<std::byte> invalid;
        net::writer invalid_out{invalid};
        const auto rejected{
            v777::play::serverbound::encode_chat(invalid_out, packet)};
        expect(!rejected.has_value());
        if (!rejected) {
            expect(rejected.error().code ==
                   refrain::error_code::invalid_argument);
        }
    };

    "net/protocol use_item_on round trips its hit result"_test = [] {
        v777::play::serverbound::use_item_on packet{};
        packet.hand = 0;
        packet.position = ((123LL & 0x3FFFFFF) << 38) | ((456LL & 0xFFF)) |
                          ((789LL & 0x3FFFFFF) << 12);
        packet.face = 1;
        packet.click_x = 0.25F;
        packet.click_y = 0.5F;
        packet.click_z = 0.75F;
        packet.inside = false;
        packet.world_border = true;
        packet.sequence = 9;
        round_trip(v777::play::serverbound::use_item_on::packet_id, packet,
                   v777::play::serverbound::encode_use_item_on,
                   v777::play::serverbound::decode_use_item_on);
    };

    "net/protocol client information round trips"_test = [] {
        v777::configuration::serverbound::client_information packet{};
        packet.information.language = "zh_cn";
        packet.information.view_distance = 12;
        packet.information.visibility = common::chat_visibility::system;
        packet.information.chat_colors = true;
        packet.information.model_customization = 1;
        packet.information.main_hand = common::humanoid_arm::right;
        packet.information.text_filtering = false;
        packet.information.allows_listing = true;
        packet.information.particles = common::particle_status::minimal;
        round_trip(
            v777::configuration::serverbound::client_information::packet_id,
            packet, v777::configuration::serverbound::encode_client_information,
            v777::configuration::serverbound::decode_client_information);
    };

    "net/protocol packet table resolves ids by flow and name"_test = [] {
        const auto *login{v777::find_packet(net::connection_state::play,
                                            net::packet_flow::clientbound, 50)};
        expect(login != nullptr);
        if (login != nullptr) {
            expect(login->name == "login");
            expect(login->state == net::connection_state::play);
        }
        const auto *by_name{v777::find_packet(net::connection_state::play,
                                              net::packet_flow::serverbound,
                                              "player_action")};
        expect(by_name != nullptr);
        if (by_name != nullptr) {
            expect(by_name->id == 41);
        }
        expect(v777::find_packet(net::connection_state::play,
                                 net::packet_flow::clientbound,
                                 300) == nullptr);
        expect(v777::find_packet(net::connection_state::play,
                                 net::packet_flow::serverbound,
                                 "not_a_packet") == nullptr);
        expect(v777::find_packet(net::connection_state::configuration,
                                 net::packet_flow::serverbound, 9) != nullptr);
    };

    "net/protocol declares every packet in the datagen catalog"_test = [] {
        const std::filesystem::path path{std::string{REFRAIN_SOURCE_DIR} +
                                         "/content/vanilla/26.3/packets.json"};
        if (!std::filesystem::exists(path)) {
            // The vanilla data directory is optional; the schema itself is
            // still checked by the generated packet tables in that case.
            return;
        }
        auto document{json::value::parse_file(path.string())};
        expect(document.has_value());
        if (!document) {
            return;
        }
        const auto entries{document->as_array()};
        expect(entries.has_value());
        if (!entries) {
            return;
        }

        std::vector<std::pair<catalog_key, std::string>> expected;
        for (const auto &entry : *entries) {
            const auto *stage{entry.find("stage")};
            const auto *flow{entry.find("flow")};
            const auto *id{entry.find("id")};
            const auto *name{entry.find("name")};
            if (stage == nullptr || flow == nullptr || id == nullptr ||
                name == nullptr) {
                continue;
            }
            const auto state{stage_of(*stage->as_string())};
            const auto direction{flow_of(*flow->as_string())};
            const auto numeric{id->as_int64()};
            const auto text{name->as_string()};
            expect(state.has_value());
            expect(direction.has_value());
            expect(numeric.has_value());
            expect(text.has_value());
            if (!state || !direction || !numeric || !text) {
                continue;
            }
            auto canonical{std::string{*text}};
            constexpr std::string_view prefix{"minecraft:"};
            if (canonical.starts_with(prefix)) {
                canonical.erase(0, prefix.size());
            }
            expected.emplace_back(
                catalog_key{.state = *state,
                            .flow = *direction,
                            .id = static_cast<std::uint16_t>(*numeric)},
                std::move(canonical));
        }

        const auto check_table = [&expected](const auto &table) {
            for (const auto &packet : table) {
                const catalog_key lookup{.state = packet.state,
                                         .flow = packet.flow,
                                         .id = packet.id};
                const auto found{std::ranges::find_if(
                    expected, [&lookup](const auto &candidate) {
                        return candidate.first == lookup;
                    })};
                expect(found != expected.end());
                if (found != expected.end()) {
                    expect(packet.name == found->second);
                }
                const auto *by_name{
                    v777::find_packet(packet.state, packet.flow, packet.name)};
                expect(by_name != nullptr);
                if (by_name != nullptr) {
                    expect(by_name->id == packet.id);
                }
            }
        };
        check_table(v777::handshake_packets);
        check_table(v777::status_packets);
        check_table(v777::login_packets);
        check_table(v777::configuration_packets);
        check_table(v777::play_packets);
    };

    "net/protocol round trips either branches in server links"_test = [] {
        v777::play::clientbound::server_links packet{};
        v777::wire::wire_untrusted_entry known{};
        known.type_ = std::uint32_t{3};
        known.link = "https://example.invalid";
        v777::wire::wire_untrusted_entry custom{};
        custom.type_ = refrain::net::text_component{.text = "Support"};
        custom.link = "https://support.invalid";
        packet.links = {known, custom};
        std::vector<std::byte> body;
        net::writer out{body};
        out.write_varint(packet.packet_id);
        const auto encoded{
            v777::play::clientbound::encode_server_links(out, packet)};
        expect(encoded.has_value());
        net::reader in{body};
        auto id{in.read_varint()};
        expect(id.has_value());
        v777::play::clientbound::server_links decoded{};
        const auto status{
            v777::play::clientbound::decode_server_links(in, decoded)};
        expect(status.has_value());
        expect(in.empty());
        expect(decoded.links.size() == 2U);
        if (decoded.links.size() == 2U) {
            expect(
                std::holds_alternative<std::uint32_t>(decoded.links[0].type_));
            expect(std::get<0>(decoded.links[0].type_) == 3U);
            expect(std::holds_alternative<refrain::net::text_component>(
                decoded.links[1].type_));
            expect(std::get<1>(decoded.links[1].type_).text == "Support");
        }
    };

    "net/protocol round trips a registry holder sound"_test = [] {
        v777::play::clientbound::sound packet{};
        packet.sound = std::uint32_t{42};
        packet.source = 1U;
        packet.x = 8;
        packet.y = 64;
        packet.z = 8;
        packet.volume = 1.0F;
        packet.pitch = 1.0F;
        packet.seed = 7;
        std::vector<std::byte> body;
        net::writer out{body};
        out.write_varint(packet.packet_id);
        const auto encoded{v777::play::clientbound::encode_sound(out, packet)};
        expect(encoded.has_value());
        net::reader in{body};
        auto id{in.read_varint()};
        expect(id.has_value());
        v777::play::clientbound::sound decoded{};
        const auto status{v777::play::clientbound::decode_sound(in, decoded)};
        expect(status.has_value());
        expect(in.empty());
        expect(std::holds_alternative<std::uint32_t>(decoded.sound));
        expect(std::get<0>(decoded.sound) == 42U);
    };

    "net/protocol round trips an entity movement delta"_test = [] {
        v777::play::clientbound::move_entity_pos packet{};
        packet.entity_id = 12U;
        packet.delta.on_ground = true;
        packet.delta.steps = {{2U, 1, 2, 3}};
        std::vector<std::byte> body;
        net::writer out{body};
        out.write_varint(packet.packet_id);
        const auto encoded{
            v777::play::clientbound::encode_move_entity_pos(out, packet)};
        expect(encoded.has_value());
        net::reader in{body};
        auto id{in.read_varint()};
        expect(id.has_value());
        v777::play::clientbound::move_entity_pos decoded{};
        const auto status{
            v777::play::clientbound::decode_move_entity_pos(in, decoded)};
        expect(status.has_value());
        expect(in.empty());
        expect(decoded.entity_id == 12U);
        expect(decoded.delta.steps.size() == 1U);
    };

    "net/protocol round trips a login custom query"_test = [] {
        v777::login::clientbound::custom_query packet{};
        packet.transaction_id = 9U;
        const auto channel{
            refrain::data::resource_location::parse("refrain:query")};
        expect(channel.has_value());
        if (!channel) {
            return;
        }
        packet.channel = *channel;
        packet.data = {std::byte{7}, std::byte{8}};
        std::vector<std::byte> body;
        net::writer out{body};
        out.write_varint(packet.packet_id);
        const auto encoded{
            v777::login::clientbound::encode_custom_query(out, packet)};
        expect(encoded.has_value());
        net::reader in{body};
        auto id{in.read_varint()};
        expect(id.has_value());
        v777::login::clientbound::custom_query decoded{};
        const auto status{
            v777::login::clientbound::decode_custom_query(in, decoded)};
        expect(status.has_value());
        expect(in.empty());
        expect(decoded.transaction_id == 9U);
        const bool same{decoded.data == packet.data};
        expect(same);
    };

    "net/protocol round trips a cached chat signature reference"_test = [] {
        v777::play::clientbound::delete_chat packet{};
        packet.message_signature.id = 7U;
        std::vector<std::byte> body;
        net::writer out{body};
        out.write_varint(packet.packet_id);
        const auto encoded{
            v777::play::clientbound::encode_delete_chat(out, packet)};
        expect(encoded.has_value());
        net::reader in{body};
        auto id{in.read_varint()};
        expect(id.has_value());
        v777::play::clientbound::delete_chat decoded{};
        const auto status{
            v777::play::clientbound::decode_delete_chat(in, decoded)};
        expect(status.has_value());
        expect(in.empty());
        expect(!decoded.message_signature.full);
        expect(decoded.message_signature.id == 7U);
    };

    "net/protocol round trips a fixed-size line array"_test = [] {
        v777::play::serverbound::sign_update packet{};
        packet.pos = 1234;
        packet.lines = {"line one", "line two", "", "line four"};
        packet.slot = 2U;
        std::vector<std::byte> body;
        net::writer out{body};
        out.write_varint(packet.packet_id);
        const auto encoded{
            v777::play::serverbound::encode_sign_update(out, packet)};
        expect(encoded.has_value());
        net::reader in{body};
        auto id{in.read_varint()};
        expect(id.has_value());
        v777::play::serverbound::sign_update decoded{};
        const auto status{
            v777::play::serverbound::decode_sign_update(in, decoded)};
        expect(status.has_value());
        expect(in.empty());
        expect(decoded.lines.size() == 4U);
        const bool same{decoded.lines == packet.lines};
        expect(same);
    };

    "net/protocol round trips an opaque custom payload"_test = [] {
        v777::configuration::clientbound::custom_payload packet{};
        const auto channel{
            refrain::data::resource_location::parse("refrain:test")};
        expect(channel.has_value());
        if (!channel) {
            return;
        }
        packet.channel = *channel;
        packet.data = {std::byte{1}, std::byte{2}, std::byte{3}};
        std::vector<std::byte> body;
        net::writer out{body};
        out.write_varint(packet.packet_id);
        const auto encoded{
            v777::configuration::clientbound::encode_custom_payload(out,
                                                                    packet)};
        expect(encoded.has_value());
        expect(body.size() == 17U);
        net::reader in{body};
        auto id{in.read_varint()};
        expect(id.has_value());
        v777::configuration::clientbound::custom_payload decoded{};
        const auto status{
            v777::configuration::clientbound::decode_custom_payload(in,
                                                                    decoded)};
        expect(status.has_value());
        expect(in.empty());
        expect(decoded.channel == packet.channel);
        const bool same{decoded.data == packet.data};
        expect(same);
    };

    "net/protocol round trips a map payload"_test = [] {
        v777::configuration::clientbound::custom_report_details packet{};
        packet.details.emplace("alpha", "one");
        packet.details.emplace("beta", "two");
        std::vector<std::byte> body;
        net::writer out{body};
        out.write_varint(packet.packet_id);
        const auto encoded{
            v777::configuration::clientbound::encode_custom_report_details(
                out, packet)};
        expect(encoded.has_value());
        expect(body.size() >= 2U);
        if (body.size() >= 2U) {
            expect(body[1] == std::byte{2});
        }
        net::reader in{body};
        auto id{in.read_varint()};
        expect(id.has_value());
        v777::configuration::clientbound::custom_report_details decoded{};
        const auto status{
            v777::configuration::clientbound::decode_custom_report_details(
                in, decoded)};
        expect(status.has_value());
        expect(in.empty());
        const bool same{decoded.details == packet.details};
        expect(same);
    };

    "net/protocol round trips a varint array payload"_test = [] {
        v777::play::clientbound::remove_entities packet{};
        packet.entity_ids = {1, 300, 40000};
        std::vector<std::byte> body;
        net::writer out{body};
        out.write_varint(packet.packet_id);
        const auto encoded{
            v777::play::clientbound::encode_remove_entities(out, packet)};
        expect(encoded.has_value());
        net::reader in{body};
        auto id{in.read_varint()};
        expect(id.has_value());
        v777::play::clientbound::remove_entities decoded{};
        const auto status{
            v777::play::clientbound::decode_remove_entities(in, decoded)};
        expect(status.has_value());
        expect(in.empty());
        const bool same{decoded.entity_ids == packet.entity_ids};
        expect(same);
    };

    "net/protocol rejects an overlong chat message"_test = [] {
        std::vector<std::byte> body;
        net::writer out{body};
        out.write_varint(v777::play::serverbound::chat::packet_id);
        out.write_varint(257);
        for (std::uint32_t index{0}; index < 257; ++index) {
            out.write_u8(static_cast<std::uint8_t>('a'));
        }
        net::reader in{body};
        auto id{in.read_varint()};
        expect(id.has_value());
        v777::play::serverbound::chat packet{};
        const auto decoded{v777::play::serverbound::decode_chat(in, packet)};
        expect(!decoded.has_value());
        if (!decoded) {
            expect(decoded.error().code == refrain::error_code::parse_error);
        }
    };
};

} // namespace
