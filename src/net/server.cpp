// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/net/server.hpp>

#include <refrain/data/nbt.hpp>
#include <refrain/data/resource_location.hpp>
#include <refrain/net/auth.hpp>
#include <refrain/net/base.hpp>
#include <refrain/net/crypto.hpp>
#include <refrain/net/frame.hpp>
#include <refrain/net/registry.hpp>
#include <refrain/schema/protocol/777/configuration_clientbound.hpp>
#include <refrain/schema/protocol/777/configuration_serverbound.hpp>
#include <refrain/schema/protocol/777/handshake_serverbound.hpp>
#include <refrain/schema/protocol/777/login_clientbound.hpp>
#include <refrain/schema/protocol/777/login_serverbound.hpp>
#include <refrain/schema/protocol/777/play_clientbound.hpp>
#include <refrain/schema/protocol/777/play_serverbound.hpp>
#include <refrain/schema/protocol/777/status_serverbound.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>

namespace refrain::net {
namespace {

namespace handshake_sb = ::refrain::protocol::v777::handshake::serverbound;
namespace status_sb = ::refrain::protocol::v777::status::serverbound;
namespace login_cb = ::refrain::protocol::v777::login::clientbound;
namespace login_sb = ::refrain::protocol::v777::login::serverbound;
namespace config_cb = ::refrain::protocol::v777::configuration::clientbound;
namespace config_sb = ::refrain::protocol::v777::configuration::serverbound;
namespace play_cb = ::refrain::protocol::v777::play::clientbound;
namespace play_sb = ::refrain::protocol::v777::play::serverbound;

/// Packets whose payload contains NBT keep hand-written codecs; only their
/// protocol ids are shared with the schema-generated packet table.
inline constexpr std::uint16_t raw_system_chat_id = 124;

[[nodiscard]] auto overworld() -> const data::resource_location & {
    static const auto location{
        *data::resource_location::parse("minecraft:overworld")};
    return location;
}

[[nodiscard]] auto brand_channel() -> const data::resource_location & {
    static const auto location{
        *data::resource_location::parse("minecraft:brand")};
    return location;
}

[[nodiscard]] auto vanilla_feature() -> const data::resource_location & {
    static const auto location{
        *data::resource_location::parse("minecraft:vanilla")};
    return location;
}

[[nodiscard]] auto random_uuid() -> uuid {
    std::random_device device{};
    uuid value{};
    for (auto &byte : value.bytes) {
        byte = static_cast<std::uint8_t>(device() & 0xFFU);
    }
    value.bytes[6] =
        static_cast<std::uint8_t>((value.bytes[6] & 0x0FU) | 0x40U);
    value.bytes[8] =
        static_cast<std::uint8_t>((value.bytes[8] & 0x3FU) | 0x80U);
    return value;
}

[[nodiscard]] auto uuid_halves(const uuid &value)
    -> std::pair<std::int64_t, std::int64_t> {
    std::uint64_t high{0};
    std::uint64_t low{0};
    for (std::size_t index{0}; index < 8; ++index) {
        high = (high << 8U) | value.bytes[index];
        low = (low << 8U) | value.bytes[index + 8];
    }
    return {std::bit_cast<std::int64_t>(high),
            std::bit_cast<std::int64_t>(low)};
}

[[nodiscard]] auto uuid_from_halves(std::int64_t high, std::int64_t low)
    -> uuid {
    const auto high_bits{std::bit_cast<std::uint64_t>(high)};
    const auto low_bits{std::bit_cast<std::uint64_t>(low)};
    uuid value{};
    for (std::size_t index{0}; index < 8; ++index) {
        value.bytes[index] = static_cast<std::uint8_t>(
            (high_bits >> (56U - index * 8U)) & 0xFFU);
        value.bytes[index + 8] =
            static_cast<std::uint8_t>((low_bits >> (56U - index * 8U)) & 0xFFU);
    }
    return value;
}

[[nodiscard]] auto component_nbt(std::string_view text)
    -> result<std::vector<std::byte>> {
    data::nbt::compound members{};
    members.emplace("text", data::nbt::value{std::string{text}});
    return data::nbt::write(data::nbt::make_compound(std::move(members)), "");
}

[[nodiscard]] auto valid_name(std::string_view name) noexcept -> bool {
    if (name.size() < 3 || name.size() > 16) {
        return false;
    }
    for (const char character : name) {
        const bool valid = (character >= 'A' && character <= 'Z') ||
                           (character >= 'a' && character <= 'z') ||
                           (character >= '0' && character <= '9') ||
                           character == '_';
        if (!valid) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] auto block_position(std::int64_t packed)
    -> std::array<std::int32_t, 3> {
    return {static_cast<std::int32_t>((packed >> 38) & 0x3FFFFFF),
            static_cast<std::int32_t>(packed & 0xFFF),
            static_cast<std::int32_t>((packed >> 12) & 0x3FFFFFF)};
}

[[nodiscard]] auto packed_block_position(std::int32_t x, std::int32_t y,
                                         std::int32_t z) -> std::int64_t {
    return ((static_cast<std::int64_t>(x) & 0x3FFFFFF) << 38) |
           ((static_cast<std::int64_t>(z) & 0x3FFFFFF) << 12) |
           (static_cast<std::int64_t>(y) & 0xFFF);
}

[[nodiscard]] auto bytes_of(std::string_view text) -> std::vector<std::byte> {
    std::vector<std::byte> result;
    result.reserve(text.size());
    for (const char character : text) {
        result.push_back(
            static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
    return result;
}

/// Encode one generated packet with its wire id in front. The body is what the
/// frame encoder consumes; keep this the only place that knows packet bodies
/// start with a VarInt id.
template <typename Packet>
[[nodiscard]] auto encode_packet(std::uint16_t packet_id, const Packet &packet,
                                 auto (*encode)(writer &, const Packet &))
    -> result<std::vector<std::byte>> {
    std::vector<std::byte> body;
    writer out{body};
    out.write_varint(packet_id);
    if (auto written = encode(out, packet); !written) {
        return std::unexpected{std::move(written.error())};
    }
    return body;
}

} // namespace

struct server::impl {
    struct connection_state_data {
        frame_decoder decoder{-1};
        frame_encoder encoder{-1};
        connection_state state{connection_state::handshake};
        std::int32_t protocol{0};
        std::string name{};
        uuid profile{};
        uuid session{};
        bool login_started{false};
        bool status_requested{false};
        bool closed{false};
        std::int64_t keepalive_sent_ms{0};
        std::int64_t keepalive_challenge{0};
        bool keepalive_pending{false};
        double x{0.0};
        double y{0.0};
        double z{0.0};
        float yaw{0.0F};
        float pitch{0.0F};
        bool on_ground{false};
        bool awaiting_teleport{true};
        bool client_loaded{false};
        std::int32_t client_view_distance{0};
        std::uint32_t held_slot{0};
        std::unique_ptr<aes128_cfb8> cipher{};
        std::vector<std::byte> verify_token{};
        bool online_pending{false};
    };

    server_config config{};
    std::unique_ptr<transport> socket{};
    std::unordered_map<std::uint64_t, connection_state_data> connections{};
    std::string status_json{};
    std::optional<rsa_keypair> server_key{};
    std::unique_ptr<http_transport> http{};
    std::unique_ptr<auth_service> auth{};
    std::optional<registry_catalog> catalog{};
    std::vector<std::vector<std::byte>> configuration_packets{};
    std::vector<std::byte> finish_packet{};
    std::atomic<bool> stopped{false};
    struct queued_packet {
        std::uint64_t connection{0};
        std::vector<std::byte> body{};
    };
    std::mutex connections_mutex{};
    std::mutex inbound_mutex{};
    std::deque<queued_packet> inbound{};
    std::thread keepalive_thread{};

    [[nodiscard]] auto start() -> status;
    void stop();
    void on_open(std::uint64_t id);
    void on_data(std::uint64_t id, std::span<const std::byte> bytes);
    void on_close(std::uint64_t id, std::string_view reason);
    void tick();
    void handle(std::uint64_t id, std::span<const std::byte> body);
    void handle_play(std::uint64_t id, std::span<const std::byte> body);
    void send(std::uint64_t id, std::span<const std::byte> packet);
    void close(std::uint64_t id);
    void send_login_disconnect(std::uint64_t id, std::string_view text);
    void send_login_hello(std::uint64_t id);
    void complete_login(std::uint64_t id);
    void handle_login_key(std::uint64_t id, reader &in);
    void send_login_finished(std::uint64_t id);
    void send_configuration(std::uint64_t id);
    void send_play_login(std::uint64_t id);
    void send_initial_chunks(std::uint64_t id);
    void send_keepalive(std::uint64_t id);
    void keepalive_loop();
    [[nodiscard]] auto build_configuration() -> status;

    template <typename Packet, typename Encoder>
    void send_packet(std::uint64_t id, std::uint16_t packet_id,
                     const Packet &packet, Encoder encode) {
        auto body{encode_packet(packet_id, packet, encode)};
        if (!body) {
            close(id);
            return;
        }
        send(id, *body);
    }
};

auto server::impl::start() -> status {
    status_json = build_status_json(config.status);
    if (config.online_mode) {
        auto key{rsa_keypair::generate()};
        if (!key) {
            return std::unexpected{std::move(key.error())};
        }
        server_key = std::move(*key);
        auto transport{make_curl_transport()};
        if (!transport) {
            return std::unexpected{std::move(transport.error())};
        }
        http = std::move(*transport);
        auth_options options{};
        options.session_server = config.session_server;
        options.timeout = std::chrono::milliseconds{config.auth_timeout_ms};
        auth = std::make_unique<auth_service>(*http, options);
    }
    if (!config.registry_catalog_path.empty()) {
        auto loaded{registry_catalog::load(config.registry_catalog_path)};
        if (!loaded) {
            return std::unexpected{std::move(loaded.error())};
        }
        catalog = std::move(*loaded);
    }
    if (auto built{build_configuration()}; !built) {
        return built;
    }
    transport_handler handler{
        .on_open = [this](std::uint64_t id) { on_open(id); },
        .on_data =
            [this](std::uint64_t id, std::span<const std::byte> bytes) {
                on_data(id, bytes);
            },
        .on_close = [this](std::uint64_t id,
                           std::string_view reason) { on_close(id, reason); },
        .on_error = [](std::string_view) {},
    };
    auto opened{transport::open(std::move(handler), 1)};
    if (!opened) {
        return std::unexpected{std::move(opened.error())};
    }
    socket = std::move(*opened);
    if (auto listening{socket->listen(config.bind)}; !listening) {
        return listening;
    }
    keepalive_thread = std::thread{[this] { keepalive_loop(); }};
    return status_ok();
}

void server::impl::stop() {
    if (stopped.exchange(true)) {
        return;
    }
    if (keepalive_thread.joinable()) {
        keepalive_thread.join();
    }
    if (socket) {
        socket->stop();
        socket.reset();
    }
    const std::lock_guard lock{connections_mutex};
    connections.clear();
}

void server::impl::on_open(std::uint64_t id) {
    if (stopped.load(std::memory_order_relaxed)) {
        return;
    }
    const std::lock_guard lock{connections_mutex};
    connections.emplace(id, connection_state_data{});
}

void server::impl::on_close(std::uint64_t id, std::string_view /*reason*/) {
    const std::lock_guard lock{connections_mutex};
    connections.erase(id);
}

void server::impl::on_data(std::uint64_t id, std::span<const std::byte> bytes) {
    std::vector<std::vector<std::byte>> bodies;
    bool failed{false};
    {
        const std::lock_guard lock{connections_mutex};
        const auto found{connections.find(id)};
        if (found == connections.end() || found->second.closed) {
            return;
        }
        auto &connection{found->second};
        std::vector<std::byte> plain{bytes.begin(), bytes.end()};
        if (connection.cipher) {
            auto decrypted{connection.cipher->decrypt(bytes)};
            if (!decrypted) {
                connection.closed = true;
                failed = true;
            } else {
                plain = std::move(*decrypted);
            }
        }
        if (!failed) {
            if (auto pushed{connection.decoder.push(plain)}; !pushed) {
                connection.closed = true;
                failed = true;
            } else {
                for (;;) {
                    std::vector<std::byte> body;
                    auto next{connection.decoder.next(body)};
                    if (!next) {
                        connection.closed = true;
                        failed = true;
                        break;
                    }
                    if (!*next) {
                        break;
                    }
                    bodies.push_back(std::move(body));
                }
            }
        }
    }
    if (failed) {
        close(id);
        return;
    }
    for (const auto &body : bodies) {
        {
            const std::lock_guard lock{connections_mutex};
            const auto found{connections.find(id)};
            if (found == connections.end() || found->second.closed) {
                return;
            }
        }
        handle(id, body);
    }
}

void server::impl::handle(std::uint64_t id, std::span<const std::byte> body) {
    connection_state state{connection_state::handshake};
    {
        const std::lock_guard lock{connections_mutex};
        const auto found{connections.find(id)};
        if (found == connections.end() || found->second.closed) {
            return;
        }
        state = found->second.state;
    }
    reader in{body};
    auto packet_id{in.read_varint()};
    if (!packet_id) {
        close(id);
        return;
    }

    if (state == connection_state::handshake) {
        if (*packet_id != ::refrain::protocol::v777::handshake::serverbound::
                              intention::packet_id) {
            close(id);
            return;
        }
        ::refrain::protocol::v777::handshake::serverbound::intention
            intention{};
        if (auto decoded{::refrain::protocol::v777::handshake::serverbound::
                             decode_intention(in, intention)};
            !decoded) {
            close(id);
            return;
        }
        {
            const std::lock_guard lock{connections_mutex};
            const auto found{connections.find(id)};
            if (found == connections.end() || found->second.closed) {
                return;
            }
            auto &live{found->second};
            live.protocol = static_cast<std::int32_t>(intention.protocol);
            if (intention.next == 1) {
                live.state = connection_state::status;
            } else if (intention.next == 2) {
                live.state = connection_state::login;
            }
        }
        if (intention.next == 1) {
            return;
        }
        if (intention.next == 2) {
            if (intention.protocol != 777) {
                send_login_disconnect(
                    id, "Incompatible client: expected protocol 777");
                close(id);
            }
            return;
        }
        close(id);
        return;
    }

    if (state == connection_state::status) {
        if (*packet_id == ::refrain::protocol::v777::status::serverbound::
                              status_request::packet_id) {
            bool duplicate{false};
            {
                const std::lock_guard lock{connections_mutex};
                const auto found{connections.find(id)};
                if (found == connections.end() || found->second.closed) {
                    return;
                }
                auto &live{found->second};
                duplicate = live.status_requested;
                live.status_requested = true;
            }
            if (duplicate) {
                close(id);
                return;
            }
            auto response{encode_status_response(status_json)};
            if (!response) {
                close(id);
                return;
            }
            send(id, *response);
            return;
        }
        if (*packet_id == ::refrain::protocol::v777::status::serverbound::
                              ping_request::packet_id) {
            status_sb::ping_request packet{};
            if (auto decoded{::refrain::protocol::v777::status::serverbound::
                                 decode_ping_request(in, packet)};
                !decoded) {
                close(id);
                return;
            }
            send(id, encode_pong_response(packet.time));
            return;
        }
        close(id);
        return;
    }

    if (state == connection_state::login) {
        if (*packet_id == login_sb::hello::packet_id) {
            bool started{false};
            {
                const std::lock_guard lock{connections_mutex};
                const auto found{connections.find(id)};
                if (found == connections.end() || found->second.closed) {
                    return;
                }
                auto &live{found->second};
                started = live.login_started;
                live.login_started = true;
            }
            if (started) {
                close(id);
                return;
            }
            login_sb::hello packet{};
            if (auto decoded{login_sb::decode_hello(in, packet)}; !decoded) {
                send_login_disconnect(id, "Invalid username");
                close(id);
                return;
            }
            if (!valid_name(packet.name)) {
                send_login_disconnect(id, "Invalid username");
                close(id);
                return;
            }
            const auto session{random_uuid()};
            {
                const std::lock_guard lock{connections_mutex};
                const auto found{connections.find(id)};
                if (found == connections.end() || found->second.closed) {
                    return;
                }
                auto &live{found->second};
                live.name = std::move(packet.name);
                live.profile =
                    uuid_from_halves(packet.profile_high, packet.profile_low);
                live.session = session;
            }
            if (config.online_mode) {
                std::random_device device{};
                std::vector<std::byte> token(4);
                for (auto &byte : token) {
                    byte = static_cast<std::byte>(device() & 0xFFU);
                }
                {
                    const std::lock_guard lock{connections_mutex};
                    const auto found{connections.find(id)};
                    if (found == connections.end() || found->second.closed) {
                        return;
                    }
                    found->second.verify_token = std::move(token);
                }
                send_login_hello(id);
                return;
            }
            complete_login(id);
            return;
        }
        if (*packet_id == login_sb::key::packet_id && config.online_mode) {
            handle_login_key(id, in);
            return;
        }
        if (*packet_id == login_sb::login_acknowledged::packet_id) {
            login_sb::login_acknowledged packet{};
            if (auto decoded{login_sb::decode_login_acknowledged(in, packet)};
                !decoded) {
                close(id);
                return;
            }
            {
                const std::lock_guard lock{connections_mutex};
                const auto found{connections.find(id)};
                if (found == connections.end() || found->second.closed) {
                    return;
                }
                found->second.state = connection_state::configuration;
            }
            send_configuration(id);
            return;
        }
        close(id);
        return;
    }

    if (state == connection_state::configuration) {
        switch (*packet_id) {
        case config_sb::client_information::packet_id: {
            config_sb::client_information packet{};
            if (auto decoded{config_sb::decode_client_information(in, packet)};
                !decoded) {
                close(id);
                return;
            }
            const std::lock_guard lock{connections_mutex};
            const auto found{connections.find(id)};
            if (found == connections.end() || found->second.closed) {
                return;
            }
            found->second.client_view_distance =
                packet.information.view_distance;
            return;
        }
        case config_sb::finish_configuration::packet_id: {
            config_sb::finish_configuration packet{};
            if (auto decoded{
                    config_sb::decode_finish_configuration(in, packet)};
                !decoded) {
                close(id);
                return;
            }
            {
                const std::lock_guard lock{connections_mutex};
                const auto found{connections.find(id)};
                if (found == connections.end() || found->second.closed) {
                    return;
                }
                found->second.state = connection_state::play;
            }
            send_play_login(id);
            return;
        }
        case config_sb::keep_alive::packet_id: {
            config_sb::keep_alive packet{};
            if (auto decoded{config_sb::decode_keep_alive(in, packet)};
                !decoded) {
                close(id);
            }
            return;
        }
        case config_sb::pong::packet_id: {
            config_sb::pong packet{};
            if (auto decoded{config_sb::decode_pong(in, packet)}; !decoded) {
                close(id);
            }
            return;
        }
        case config_sb::select_known_packs::packet_id: {
            config_sb::select_known_packs packet{};
            if (auto decoded{config_sb::decode_select_known_packs(in, packet)};
                !decoded) {
                close(id);
            }
            return;
        }
        case config_sb::custom_payload::packet_id: {
            config_sb::custom_payload packet{};
            if (auto decoded{config_sb::decode_custom_payload(in, packet)};
                !decoded) {
                close(id);
            }
            return;
        }
        default:
            // Cookie and resource-pack replies are only expected after the
            // server sends the matching request, which it does not yet do.
            return;
        }
    }

    if (state == connection_state::play) {
        std::vector<std::byte> copy{body.begin(), body.end()};
        const std::lock_guard lock{inbound_mutex};
        inbound.push_back(
            queued_packet{.connection = id, .body = std::move(copy)});
    }
}

void server::impl::send_login_disconnect(std::uint64_t id,
                                         std::string_view text) {
    login_cb::login_disconnect packet{
        .reason = fmt::format(R"({{"text":"{}"}})", text)};
    send_packet(id, login_cb::login_disconnect::packet_id, packet,
                login_cb::encode_login_disconnect);
}

void server::impl::send_login_hello(std::uint64_t id) {
    std::vector<std::byte> token;
    {
        const std::lock_guard lock{connections_mutex};
        const auto found{connections.find(id)};
        if (found == connections.end() || found->second.closed) {
            return;
        }
        token = found->second.verify_token;
    }
    auto public_key{server_key->public_key_der()};
    if (!public_key) {
        send_login_disconnect(id, "Server encryption is unavailable");
        close(id);
        return;
    }
    login_cb::hello packet{
        .server_id = {},
        .public_key = std::move(*public_key),
        .challenge = std::move(token),
        .should_authenticate = true,
    };
    send_packet(id, login_cb::hello::packet_id, packet, login_cb::encode_hello);
}

void server::impl::complete_login(std::uint64_t id) {
    if (config.compression_threshold >= 0) {
        login_cb::login_compression packet{
            .threshold =
                static_cast<std::uint32_t>(config.compression_threshold)};
        send_packet(id, login_cb::login_compression::packet_id, packet,
                    login_cb::encode_login_compression);
        const std::lock_guard lock{connections_mutex};
        const auto found{connections.find(id)};
        if (found == connections.end() || found->second.closed) {
            return;
        }
        found->second.encoder.set_threshold(config.compression_threshold);
        found->second.decoder.set_threshold(config.compression_threshold);
    }
    send_login_finished(id);
}

void server::impl::handle_login_key(std::uint64_t id, reader &in) {
    login_sb::key packet{};
    if (auto decoded{login_sb::decode_key(in, packet)}; !decoded) {
        send_login_disconnect(id, "Malformed encryption response");
        close(id);
        return;
    }
    std::vector<std::byte> expected_token;
    {
        const std::lock_guard lock{connections_mutex};
        const auto found{connections.find(id)};
        if (found == connections.end() || found->second.closed) {
            return;
        }
        expected_token = found->second.verify_token;
    }
    auto secret{server_key->decrypt_pkcs1_v1_5(packet.keybytes)};
    auto token{server_key->decrypt_pkcs1_v1_5(packet.encrypted_challenge)};
    if (!secret || !token) {
        send_login_disconnect(id, "Malformed encryption response");
        close(id);
        return;
    }
    if (token->size() != expected_token.size() ||
        !std::equal(token->begin(), token->end(), expected_token.begin())) {
        send_login_disconnect(id, "Invalid verify token");
        close(id);
        return;
    }
    if (secret->size() != aes128_key_bytes) {
        send_login_disconnect(id, "Invalid shared secret");
        close(id);
        return;
    }
    auto cipher{aes128_cfb8::create(*secret, *secret)};
    if (!cipher) {
        send_login_disconnect(id, "Encryption is unavailable");
        close(id);
        return;
    }
    auto public_key{server_key->public_key_der()};
    if (!public_key) {
        send_login_disconnect(id, "Encryption is unavailable");
        close(id);
        return;
    }
    auto digest{minecraft_server_hash("", *secret, *public_key)};
    if (!digest) {
        send_login_disconnect(id, "Encryption is unavailable");
        close(id);
        return;
    }
    std::string name;
    {
        const std::lock_guard lock{connections_mutex};
        const auto found{connections.find(id)};
        if (found == connections.end() || found->second.closed) {
            return;
        }
        auto &live{found->second};
        live.cipher = std::make_unique<aes128_cfb8>(std::move(*cipher));
        name = live.name;
    }
    if (!auth) {
        send_login_disconnect(id, "Authentication is unavailable");
        close(id);
        return;
    }
    auto profile{auth->has_joined(name, *digest)};
    if (!profile) {
        send_login_disconnect(id, "Failed to verify username");
        close(id);
        return;
    }
    {
        const std::lock_guard lock{connections_mutex};
        const auto found{connections.find(id)};
        if (found == connections.end() || found->second.closed) {
            return;
        }
        auto &live{found->second};
        live.profile = profile->id;
        live.name = profile->name;
    }
    complete_login(id);
}

void server::impl::send_login_finished(std::uint64_t id) {
    uuid profile{};
    uuid session{};
    std::string name;
    {
        const std::lock_guard lock{connections_mutex};
        const auto found{connections.find(id)};
        if (found == connections.end() || found->second.closed) {
            return;
        }
        profile = found->second.profile;
        session = found->second.session;
        name = found->second.name;
    }
    const auto [profile_high, profile_low] = uuid_halves(profile);
    const auto [session_high, session_low] = uuid_halves(session);
    login_cb::login_finished packet{
        .profile_high = profile_high,
        .profile_low = profile_low,
        .name = std::move(name),
        .properties = {},
        .session_high = session_high,
        .session_low = session_low,
    };
    send_packet(id, login_cb::login_finished::packet_id, packet,
                login_cb::encode_login_finished);
}

void server::impl::send_configuration(std::uint64_t id) {
    for (const auto &packet : configuration_packets) {
        send(id, packet);
    }
    send(id, finish_packet);
}

auto server::impl::build_configuration() -> status {
    const auto add_packet = [this](std::vector<std::byte> packet) {
        configuration_packets.push_back(std::move(packet));
    };
    {
        config_cb::custom_payload packet{.channel = brand_channel(),
                                         .data = bytes_of("refrain")};
        auto built{encode_packet(config_cb::custom_payload::packet_id, packet,
                                 config_cb::encode_custom_payload)};
        if (!built) {
            return std::unexpected{std::move(built.error())};
        }
        add_packet(std::move(*built));
    }
    {
        config_cb::update_enabled_features packet{
            .features = {vanilla_feature()}};
        auto built{encode_packet(config_cb::update_enabled_features::packet_id,
                                 packet,
                                 config_cb::encode_update_enabled_features)};
        if (!built) {
            return std::unexpected{std::move(built.error())};
        }
        add_packet(std::move(*built));
    }
    {
        config_cb::select_known_packs packet{};
        packet.packs.push_back(::refrain::protocol::v777::common::known_pack{
            .pack_namespace = "minecraft",
            .id = "core",
            .version = "26.3",
        });
        auto built{encode_packet(config_cb::select_known_packs::packet_id,
                                 packet, config_cb::encode_select_known_packs)};
        if (!built) {
            return std::unexpected{std::move(built.error())};
        }
        add_packet(std::move(*built));
    }
    if (catalog) {
        for (const auto &registry : catalog->registries()) {
            auto key{data::resource_location::parse(registry.key)};
            if (!key) {
                return std::unexpected{std::move(key.error())};
            }
            // Hand-written registry_data payload: each entry carries an
            // optional NBT tag, which the core pack never provides.
            std::vector<std::byte> packet;
            writer out{packet};
            out.write_varint(7); // registry_data
            static_cast<void>(out.write_identifier(*key));
            out.write_varint(
                static_cast<std::uint32_t>(registry.entries.size()));
            for (const auto &entry : registry.entries) {
                auto name{data::resource_location::parse(entry.name)};
                if (!name) {
                    return std::unexpected{std::move(name.error())};
                }
                if (auto written{out.write_identifier(*name)}; !written) {
                    return written;
                }
                out.write_bool(false); // payload comes from the core pack
            }
            if (packet.size() > max_frame_length) {
                return std::unexpected{make_error(
                    error_code::invalid_argument,
                    fmt::format("registry_data '{}' exceeds the frame limit",
                                registry.key))};
            }
            add_packet(std::move(packet));
        }

        std::map<std::string, std::vector<const tag_definition *>> grouped{};
        for (const auto &tag : catalog->tags()) {
            grouped[tag.registry].push_back(&tag);
        }
        // Hand-written update_tags payload: entry payloads are optional NBT
        // tag lists, and the core pack only contributes empty tags.
        std::vector<std::byte> packet;
        writer out{packet};
        out.write_varint(14); // update_tags
        out.write_varint(static_cast<std::uint32_t>(grouped.size()));
        for (const auto &[registry_name, tags] : grouped) {
            auto registry{data::resource_location::parse(registry_name)};
            if (!registry) {
                return std::unexpected{std::move(registry.error())};
            }
            if (auto written{out.write_identifier(*registry)}; !written) {
                return written;
            }
            out.write_varint(static_cast<std::uint32_t>(tags.size()));
            for (const auto *tag : tags) {
                auto name{data::resource_location::parse(tag->tag)};
                if (!name) {
                    return std::unexpected{std::move(name.error())};
                }
                if (auto written{out.write_identifier(*name)}; !written) {
                    return written;
                }
                out.write_varint(
                    static_cast<std::uint32_t>(tag->entries.size()));
                for (const auto id : tag->entries) {
                    out.write_varint(id);
                }
            }
        }
        add_packet(std::move(packet));
    }

    config_cb::finish_configuration packet{};
    auto built{encode_packet(config_cb::finish_configuration::packet_id, packet,
                             config_cb::encode_finish_configuration)};
    if (!built) {
        return std::unexpected{std::move(built.error())};
    }
    finish_packet = std::move(*built);
    return status_ok();
}

void server::impl::send_play_login(std::uint64_t id) {
    {
        const std::lock_guard lock{connections_mutex};
        const auto found{connections.find(id)};
        if (found == connections.end() || found->second.closed) {
            return;
        }
        auto &live{found->second};
        live.x = config.spawn_x;
        live.y = config.spawn_y;
        live.z = config.spawn_z;
        live.awaiting_teleport = true;
    }
    play_cb::login packet{};
    packet.player_id = static_cast<std::int32_t>(id);
    packet.hardcore = config.hardcore;
    packet.levels = {overworld()};
    packet.max_players = static_cast<std::uint32_t>(config.status.max_players);
    packet.chunk_radius = static_cast<std::uint32_t>(config.view_distance);
    packet.simulation_distance =
        static_cast<std::uint32_t>(config.simulation_distance);
    packet.reduced_debug_info = false;
    packet.show_death_screen = true;
    packet.limited_crafting = false;
    packet.spawn.dimension_type = 0;
    packet.spawn.dimension = overworld();
    packet.spawn.seed = config.seed;
    packet.spawn.game_type = static_cast<std::uint32_t>(config.game_mode);
    packet.spawn.previous_game_type = std::nullopt;
    packet.spawn.debug = false;
    packet.spawn.flat = false;
    packet.spawn.last_death_location = std::nullopt;
    packet.spawn.portal_cooldown = 0;
    packet.spawn.sea_level = 63;
    packet.online_mode = config.online_mode;
    packet.enforces_secure_chat = config.status.enforces_secure_chat;
    send_packet(id, play_cb::login::packet_id, packet, play_cb::encode_login);

    {
        play_cb::change_difficulty difficulty{.difficulty = 2, .locked = false};
        send_packet(id, play_cb::change_difficulty::packet_id, difficulty,
                    play_cb::encode_change_difficulty);
    }
    {
        play_cb::player_abilities abilities{
            .flags =
                static_cast<std::uint8_t>(config.game_mode == 1 ? 0x0D : 0x00),
            .flying_speed = 0.05F,
            .walking_speed = 0.1F,
        };
        send_packet(id, play_cb::player_abilities::packet_id, abilities,
                    play_cb::encode_player_abilities);
    }
    {
        play_cb::set_held_slot held{.slot = 0};
        send_packet(id, play_cb::set_held_slot::packet_id, held,
                    play_cb::encode_set_held_slot);
    }
    {
        play_cb::entity_event event{
            .entity_id = static_cast<std::int32_t>(id),
            .event = 24, // permission level: ALL
        };
        send_packet(id, play_cb::entity_event::packet_id, event,
                    play_cb::encode_entity_event);
    }
    {
        play_cb::set_time time{.game_time = 0, .clock_updates = {}};
        send_packet(id, play_cb::set_time::packet_id, time,
                    play_cb::encode_set_time);
    }
    {
        play_cb::initialize_border border{
            .center_x = 0.0,
            .center_z = 0.0,
            .old_size = 29'999'984.0,
            .new_size = 29'999'984.0,
            .lerp_time = 0,
            .absolute_max_size = 29'999'984,
            .warning_blocks = 5,
            .warning_time = 15,
        };
        send_packet(id, play_cb::initialize_border::packet_id, border,
                    play_cb::encode_initialize_border);
    }
    {
        play_cb::set_default_spawn_position spawn{
            .dimension = overworld(),
            .position = packed_block_position(config.spawn_x, config.spawn_y,
                                              config.spawn_z),
            .yaw = 0.0F,
            .pitch = 0.0F,
        };
        send_packet(id, play_cb::set_default_spawn_position::packet_id, spawn,
                    play_cb::encode_set_default_spawn_position);
    }

    // The client needs a spawn position and a first chunk batch before it can
    // leave the loading screen.
    {
        play_cb::player_position position{
            .id = 1,
            .x = config.spawn_x,
            .y = config.spawn_y,
            .z = config.spawn_z,
            .delta_x = 0.0,
            .delta_y = 0.0,
            .delta_z = 0.0,
            .y_rot = 0.0F,
            .x_rot = 0.0F,
            .relatives = 0,
        };
        send_packet(id, play_cb::player_position::packet_id, position,
                    play_cb::encode_player_position);
    }
    {
        play_cb::game_event event{.event = 13, .value = 0.0F};
        send_packet(id, play_cb::game_event::packet_id, event,
                    play_cb::encode_game_event);
    }
    send_initial_chunks(id);
    send_keepalive(id);
}

void server::impl::send_initial_chunks(std::uint64_t id) {
    play_cb::chunk_batch_start start{};
    send_packet(id, play_cb::chunk_batch_start::packet_id, start,
                play_cb::encode_chunk_batch_start);

    std::uint32_t count{0};
    if (config.spawn_chunks) {
        for (const auto &packet : *config.spawn_chunks) {
            send(id, packet);
            ++count;
        }
    }
    play_cb::chunk_batch_finished finished{.batch_size = count};
    send_packet(id, play_cb::chunk_batch_finished::packet_id, finished,
                play_cb::encode_chunk_batch_finished);
}

void server::impl::send_keepalive(std::uint64_t id) {
    const auto challenge{
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count()};
    {
        const std::lock_guard lock{connections_mutex};
        const auto found{connections.find(id)};
        if (found == connections.end() || found->second.closed) {
            return;
        }
        found->second.keepalive_sent_ms = challenge;
        found->second.keepalive_challenge = challenge;
        found->second.keepalive_pending = true;
    }
    play_cb::keep_alive packet{.id = challenge};
    send_packet(id, play_cb::keep_alive::packet_id, packet,
                play_cb::encode_keep_alive);
}

void server::impl::handle_play(std::uint64_t id,
                               std::span<const std::byte> body) {
    reader in{body};
    auto packet_id{in.read_varint()};
    if (!packet_id) {
        close(id);
        return;
    }
    switch (*packet_id) {
    case play_sb::accept_teleportation::packet_id: {
        play_sb::accept_teleportation packet{};
        if (auto decoded{play_sb::decode_accept_teleportation(in, packet)};
            !decoded) {
            close(id);
            return;
        }
        if (packet.id != 1) {
            return;
        }
        const std::lock_guard lock{connections_mutex};
        const auto found{connections.find(id)};
        if (found == connections.end() || found->second.closed) {
            return;
        }
        auto &live{found->second};
        live.awaiting_teleport = false;
        live.x = packet.x;
        live.y = packet.y;
        live.z = packet.z;
        live.yaw = packet.y_rot;
        live.pitch = packet.x_rot;
        return;
    }
    case play_sb::keep_alive::packet_id: {
        play_sb::keep_alive packet{};
        if (auto decoded{play_sb::decode_keep_alive(in, packet)}; !decoded) {
            close(id);
            return;
        }
        const std::lock_guard lock{connections_mutex};
        const auto found{connections.find(id)};
        if (found == connections.end() || found->second.closed) {
            return;
        }
        auto &live{found->second};
        if (live.keepalive_pending && live.keepalive_challenge == packet.id) {
            live.keepalive_pending = false;
        }
        return;
    }
    case play_sb::move_player_pos::packet_id: {
        play_sb::move_player_pos packet{};
        if (auto decoded{play_sb::decode_move_player_pos(in, packet)};
            !decoded) {
            close(id);
            return;
        }
        const std::lock_guard lock{connections_mutex};
        const auto found{connections.find(id)};
        if (found == connections.end() || found->second.closed) {
            return;
        }
        auto &live{found->second};
        live.x = packet.x;
        live.y = packet.y;
        live.z = packet.z;
        live.on_ground = (packet.flags & 1U) != 0;
        return;
    }
    case play_sb::move_player_pos_rot::packet_id: {
        play_sb::move_player_pos_rot packet{};
        if (auto decoded{play_sb::decode_move_player_pos_rot(in, packet)};
            !decoded) {
            close(id);
            return;
        }
        const std::lock_guard lock{connections_mutex};
        const auto found{connections.find(id)};
        if (found == connections.end() || found->second.closed) {
            return;
        }
        auto &live{found->second};
        live.x = packet.x;
        live.y = packet.y;
        live.z = packet.z;
        live.yaw = packet.y_rot;
        live.pitch = packet.x_rot;
        live.on_ground = (packet.flags & 1U) != 0;
        return;
    }
    case play_sb::move_player_rot::packet_id: {
        play_sb::move_player_rot packet{};
        if (auto decoded{play_sb::decode_move_player_rot(in, packet)};
            !decoded) {
            close(id);
            return;
        }
        const std::lock_guard lock{connections_mutex};
        const auto found{connections.find(id)};
        if (found == connections.end() || found->second.closed) {
            return;
        }
        auto &live{found->second};
        live.yaw = packet.y_rot;
        live.pitch = packet.x_rot;
        live.on_ground = (packet.flags & 1U) != 0;
        return;
    }
    case play_sb::move_player_status_only::packet_id: {
        play_sb::move_player_status_only packet{};
        if (auto decoded{play_sb::decode_move_player_status_only(in, packet)};
            !decoded) {
            close(id);
            return;
        }
        const std::lock_guard lock{connections_mutex};
        const auto found{connections.find(id)};
        if (found == connections.end() || found->second.closed) {
            return;
        }
        found->second.on_ground = (packet.flags & 1U) != 0;
        return;
    }
    case play_sb::player_action::packet_id: {
        play_sb::player_action packet{};
        if (auto decoded{play_sb::decode_player_action(in, packet)}; !decoded) {
            close(id);
            return;
        }
        const auto [x, y, z] = block_position(packet.position);
        // Only START_DESTROY_BLOCK changes the world; the other actions stop
        // or abort a prediction that the client resolves locally.
        if (packet.action == 0 && config.break_block &&
            config.break_block(x, y, z)) {
            play_cb::block_update update{.position = packet.position,
                                         .state = 0};
            send_packet(id, play_cb::block_update::packet_id, update,
                        play_cb::encode_block_update);
        }
        play_cb::block_changed_ack ack{.sequence = packet.sequence};
        send_packet(id, play_cb::block_changed_ack::packet_id, ack,
                    play_cb::encode_block_changed_ack);
        return;
    }
    case play_sb::use_item_on::packet_id: {
        play_sb::use_item_on packet{};
        if (auto decoded{play_sb::decode_use_item_on(in, packet)}; !decoded) {
            close(id);
            return;
        }
        auto [x, y, z] = block_position(packet.position);
        switch (packet.face) {
        case 0:
            --y;
            break;
        case 1:
            ++y;
            break;
        case 2:
            --z;
            break;
        case 3:
            ++z;
            break;
        case 4:
            --x;
            break;
        case 5:
            ++x;
            break;
        default:
            break;
        }
        if (packet.face <= 5 && config.place_block &&
            config.place_block(x, y, z)) {
            play_cb::block_update update{
                .position = packed_block_position(x, y, z),
                .state = 1, // minecraft:stone
            };
            send_packet(id, play_cb::block_update::packet_id, update,
                        play_cb::encode_block_update);
        }
        play_cb::block_changed_ack ack{.sequence = packet.sequence};
        send_packet(id, play_cb::block_changed_ack::packet_id, ack,
                    play_cb::encode_block_changed_ack);
        return;
    }
    case play_sb::chat::packet_id: {
        play_sb::chat packet{};
        if (auto decoded{play_sb::decode_chat(in, packet)}; !decoded) {
            close(id);
            return;
        }
        if (packet.message.empty()) {
            return;
        }
        std::string name;
        {
            const std::lock_guard lock{connections_mutex};
            const auto found{connections.find(id)};
            if (found == connections.end() || found->second.closed) {
                return;
            }
            name = found->second.name;
        }
        auto component{
            component_nbt(fmt::format("<{}> {}", name, packet.message))};
        if (!component) {
            return;
        }
        // system_chat carries an NBT component, so its body stays
        // hand-written; the packet id is fixed by protocol 777.
        std::vector<std::byte> chat;
        writer out{chat};
        out.write_varint(raw_system_chat_id);
        out.write_bytes(std::span<const std::byte>{*component});
        out.write_bool(false);
        send(id, chat);
        return;
    }
    case play_sb::chat_command::packet_id: {
        play_sb::chat_command packet{};
        if (auto decoded{play_sb::decode_chat_command(in, packet)}; !decoded) {
            close(id);
            return;
        }
        auto component{component_nbt(fmt::format(
            "Refrain: command '/{}' is not implemented", packet.command))};
        if (!component) {
            return;
        }
        std::vector<std::byte> chat;
        writer out{chat};
        out.write_varint(raw_system_chat_id);
        out.write_bytes(std::span<const std::byte>{*component});
        out.write_bool(false);
        send(id, chat);
        return;
    }
    case play_sb::player_loaded::packet_id: {
        play_sb::player_loaded packet{};
        if (auto decoded{play_sb::decode_player_loaded(in, packet)}; !decoded) {
            close(id);
            return;
        }
        const std::lock_guard lock{connections_mutex};
        const auto found{connections.find(id)};
        if (found == connections.end() || found->second.closed) {
            return;
        }
        found->second.client_loaded = true;
        return;
    }
    case play_sb::chunk_batch_received::packet_id: {
        play_sb::chunk_batch_received packet{};
        if (auto decoded{play_sb::decode_chunk_batch_received(in, packet)};
            !decoded) {
            close(id);
            return;
        }
        static_cast<void>(packet);
        return;
    }
    case play_sb::set_carried_item::packet_id: {
        play_sb::set_carried_item packet{};
        if (auto decoded{play_sb::decode_set_carried_item(in, packet)};
            !decoded) {
            close(id);
            return;
        }
        const std::lock_guard lock{connections_mutex};
        const auto found{connections.find(id)};
        if (found == connections.end() || found->second.closed) {
            return;
        }
        found->second.held_slot = static_cast<std::uint32_t>(packet.slot);
        return;
    }
    case play_sb::client_information::packet_id: {
        play_sb::client_information packet{};
        if (auto decoded{play_sb::decode_client_information(in, packet)};
            !decoded) {
            close(id);
            return;
        }
        const std::lock_guard lock{connections_mutex};
        const auto found{connections.find(id)};
        if (found == connections.end() || found->second.closed) {
            return;
        }
        found->second.client_view_distance = packet.information.view_distance;
        return;
    }
    default:
        // Packets without server-side state (animation, input, containers)
        // are ignored until their handlers land.
        return;
    }
}

void server::impl::send(std::uint64_t id, std::span<const std::byte> packet) {
    std::vector<std::byte> framed;
    {
        const std::lock_guard lock{connections_mutex};
        const auto found{connections.find(id)};
        if (found == connections.end() || found->second.closed) {
            return;
        }
        if (auto encoded{found->second.encoder.encode(packet, framed)};
            !encoded) {
            return;
        }
        if (found->second.cipher) {
            auto encrypted{found->second.cipher->encrypt(framed)};
            if (!encrypted) {
                return;
            }
            framed = std::move(*encrypted);
        }
    }
    if (socket) {
        socket->send(id, framed);
    }
}

void server::impl::close(std::uint64_t id) {
    {
        const std::lock_guard lock{connections_mutex};
        const auto found{connections.find(id)};
        if (found == connections.end() || found->second.closed) {
            return;
        }
        found->second.closed = true;
    }
    if (socket) {
        socket->close(id);
    }
}

void server::impl::keepalive_loop() {
    constexpr std::int64_t interval_ms{15'000};
    while (!stopped.load(std::memory_order_acquire)) {
        for (int tick{0}; tick < 10 && !stopped.load(std::memory_order_acquire);
             ++tick) {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
        if (stopped.load(std::memory_order_acquire)) {
            break;
        }
        std::vector<std::uint64_t> expired;
        std::vector<std::uint64_t> due;
        const auto now{std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count()};
        {
            const std::lock_guard lock{connections_mutex};
            for (const auto &[id, connection] : connections) {
                if (connection.closed ||
                    connection.state != connection_state::play) {
                    continue;
                }
                if (now - connection.keepalive_sent_ms < interval_ms) {
                    continue;
                }
                if (connection.keepalive_pending) {
                    expired.push_back(id);
                } else {
                    due.push_back(id);
                }
            }
        }
        for (const auto id : expired) {
            close(id);
        }
        for (const auto id : due) {
            send_keepalive(id);
        }
    }
}

void server::impl::tick() {
    std::deque<queued_packet> local{};
    {
        const std::lock_guard lock{inbound_mutex};
        local.swap(inbound);
    }
    for (const auto &packet : local) {
        handle_play(packet.connection, packet.body);
    }
}

server::server(std::unique_ptr<impl> state) : impl_{std::move(state)} {}
server::~server() { stop(); }

auto server::start(server_config config) -> result<std::unique_ptr<server>> {
    auto state{std::make_unique<impl>()};
    state->config = std::move(config);
    if (auto started{state->start()}; !started) {
        return std::unexpected{std::move(started.error())};
    }
    return std::unique_ptr<server>{new server{std::move(state)}};
}

void server::stop() noexcept {
    if (impl_) {
        impl_->stop();
    }
}

void server::tick() {
    if (impl_) {
        impl_->tick();
    }
}

auto server::local_port() const -> std::uint16_t {
    return impl_ && impl_->socket ? impl_->socket->local_port() : 0;
}

} // namespace refrain::net
