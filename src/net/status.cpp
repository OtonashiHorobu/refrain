// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/net/status.hpp>

#include <refrain/data/json.hpp>
#include <refrain/schema/protocol/777/handshake_serverbound.hpp>
#include <refrain/schema/protocol/777/login_clientbound.hpp>
#include <refrain/schema/protocol/777/login_serverbound.hpp>
#include <refrain/schema/protocol/777/status_clientbound.hpp>
#include <refrain/schema/protocol/777/status_serverbound.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

namespace refrain::net {
namespace {

namespace json = refrain::data::json;

[[nodiscard]] auto status_failure(std::string_view message) -> error {
    return make_error(error_code::parse_error, message);
}

[[nodiscard]] auto object_of() -> json::value::object_type {
    return json::value::object_type{};
}

void add(json::value::object_type &object, std::string key, json::value value) {
    object.emplace(std::move(key), std::move(value));
}

[[nodiscard]] auto description_text(const json::value &description)
    -> std::string {
    if (description.is_string()) {
        const auto text{description.as_string()};
        return text ? std::string{*text} : std::string{};
    }
    if (description.is_object()) {
        const auto *text{description.find("text")};
        if (text != nullptr && text->is_string()) {
            const auto value{text->as_string()};
            if (value) {
                return std::string{*value};
            }
        }
    }
    return description.dump();
}

} // namespace

auto build_status_json(const status_info &info) -> std::string {
    auto root{object_of()};

    auto description{object_of()};
    add(description, "text", json::value{info.description});
    add(root, "description", json::value{std::move(description)});

    auto players{object_of()};
    add(players, "max", json::value{info.max_players});
    add(players, "online", json::value{info.online_players});
    json::value::array_type sample{};
    for (const auto &entry : info.sample) {
        auto member{object_of()};
        add(member, "name", json::value{entry.name});
        add(member, "id", json::value{entry.id.to_string()});
        sample.emplace_back(std::move(member));
    }
    add(players, "sample", json::value{std::move(sample)});
    add(root, "players", json::value{std::move(players)});

    auto version{object_of()};
    add(version, "name", json::value{info.version_name});
    add(version, "protocol", json::value{static_cast<std::int64_t>(
                                 info.protocol)});
    add(root, "version", json::value{std::move(version)});

    if (!info.favicon.empty()) {
        add(root, "favicon", json::value{info.favicon});
    }
    add(root, "enforcesSecureChat", json::value{info.enforces_secure_chat});
    return json::value{std::move(root)}.dump();
}

auto parse_status_json(std::string_view text) -> result<status_info> {
    auto parsed{json::value::parse(text)};
    if (!parsed) {
        return std::unexpected{std::move(parsed.error())};
    }
    if (!parsed->is_object()) {
        return std::unexpected{status_failure("status response is not an object")};
    }
    status_info info{};
    if (const auto *description{parsed->find("description")};
        description != nullptr) {
        info.description = description_text(*description);
    }
    if (const auto *players{parsed->find("players")};
        players != nullptr && players->is_object()) {
        if (const auto *max{players->find("max")}; max != nullptr) {
            const auto value{max->as_int64()};
            if (value) {
                info.max_players = static_cast<std::int32_t>(*value);
            }
        }
        if (const auto *online{players->find("online")}; online != nullptr) {
            const auto value{online->as_int64()};
            if (value) {
                info.online_players = static_cast<std::int32_t>(*value);
            }
        }
        if (const auto *sample{players->find("sample")};
            sample != nullptr && sample->is_array()) {
            const auto entries{sample->as_array()};
            if (entries) {
                for (const auto &entry : *entries) {
                    status_sample member{};
                    if (const auto *name{entry.find("name")};
                        name != nullptr && name->is_string()) {
                        const auto value{name->as_string()};
                        if (value) {
                            member.name = *value;
                        }
                    }
                    if (const auto *id{entry.find("id")};
                        id != nullptr && id->is_string()) {
                        const auto value{id->as_string()};
                        if (value) {
                            auto parsed_id{uuid::parse(*value)};
                            if (parsed_id) {
                                member.id = *parsed_id;
                            }
                        }
                    }
                    info.sample.push_back(std::move(member));
                }
            }
        }
    }
    if (const auto *version{parsed->find("version")};
        version != nullptr && version->is_object()) {
        if (const auto *name{version->find("name")}; name != nullptr) {
            const auto value{name->as_string()};
            if (value) {
                info.version_name = *value;
            }
        }
        if (const auto *protocol{version->find("protocol")};
            protocol != nullptr) {
            const auto value{protocol->as_int64()};
            if (value) {
                info.protocol = static_cast<std::int32_t>(*value);
            }
        }
    }
    if (const auto *favicon{parsed->find("favicon")};
        favicon != nullptr && favicon->is_string()) {
        const auto value{favicon->as_string()};
        if (value) {
            info.favicon = *value;
        }
    }
    if (const auto *enforces{parsed->find("enforcesSecureChat")};
        enforces != nullptr && enforces->is_boolean()) {
        const auto value{enforces->as_bool()};
        if (value) {
            info.enforces_secure_chat = *value;
        }
    }
    return info;
}

auto encode_handshake(const handshake_intention &intention)
    -> result<std::vector<std::byte>> {
    std::vector<std::byte> body;
    writer out{body};
    out.write_varint(
        protocol::v777::handshake::serverbound::intention::packet_id);
    out.write_varint(static_cast<std::uint32_t>(intention.protocol));
    if (auto written{out.write_string(intention.host, 255)}; !written) {
        return std::unexpected{std::move(written.error())};
    }
    out.write_u16(intention.port);
    out.write_varint(static_cast<std::uint32_t>(intention.next));
    return body;
}

auto decode_handshake(std::span<const std::byte> body)
    -> result<handshake_intention> {
    reader in{body};
    auto packet_id{in.read_varint()};
    if (!packet_id) {
        return std::unexpected{packet_id.error()};
    }
    if (*packet_id !=
        protocol::v777::handshake::serverbound::intention::packet_id) {
        return std::unexpected{status_failure(fmt::format(
            "handshake packet id is {}, expected {}",
            *packet_id,
            protocol::v777::handshake::serverbound::intention::packet_id))};
    }
    protocol::v777::handshake::serverbound::intention packet{};
    if (auto decoded{
            protocol::v777::handshake::serverbound::decode_intention(in,
                                                                     packet)};
        !decoded) {
        return std::unexpected{std::move(decoded.error())};
    }
    return handshake_intention{
        .protocol = static_cast<std::int32_t>(packet.protocol),
        .host = std::move(packet.host),
        .port = packet.port,
        .next = static_cast<std::int32_t>(packet.next),
    };
}

auto encode_status_request() -> std::vector<std::byte> {
    std::vector<std::byte> body;
    writer out{body};
    out.write_varint(
        protocol::v777::status::serverbound::status_request::packet_id);
    protocol::v777::status::serverbound::status_request packet{};
    static_cast<void>(
        protocol::v777::status::serverbound::encode_status_request(out, packet));
    return body;
}

auto encode_ping_request(std::int64_t time) -> std::vector<std::byte> {
    std::vector<std::byte> body;
    writer out{body};
    out.write_varint(
        protocol::v777::status::serverbound::ping_request::packet_id);
    protocol::v777::status::serverbound::ping_request packet{.time = time};
    static_cast<void>(
        protocol::v777::status::serverbound::encode_ping_request(out, packet));
    return body;
}

auto encode_status_response(std::string_view text)
    -> result<std::vector<std::byte>> {
    std::vector<std::byte> body;
    writer out{body};
    out.write_varint(
        protocol::v777::status::clientbound::status_response::packet_id);
    protocol::v777::status::clientbound::status_response packet{
        .json = std::string{text}};
    if (auto written{
            protocol::v777::status::clientbound::encode_status_response(out,
                                                                        packet)};
        !written) {
        return std::unexpected{std::move(written.error())};
    }
    return body;
}

auto encode_pong_response(std::int64_t time) -> std::vector<std::byte> {
    std::vector<std::byte> body;
    writer out{body};
    out.write_varint(
        protocol::v777::status::clientbound::pong_response::packet_id);
    protocol::v777::status::clientbound::pong_response packet{.time = time};
    static_cast<void>(
        protocol::v777::status::clientbound::encode_pong_response(out, packet));
    return body;
}

} // namespace refrain::net
