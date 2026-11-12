// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/net/server.hpp>

#include <refrain/core/log.hpp>
#include <refrain/data/nbt.hpp>
#include <refrain/data/resource_location.hpp>
#include <refrain/net/auth.hpp>
#include <refrain/net/base.hpp>
#include <refrain/net/crypto.hpp>
#include <refrain/net/frame.hpp>
#include <refrain/net/primitives.hpp>
#include <refrain/net/rate_limit.hpp>
#include <refrain/net/registry.hpp>
#include <refrain/protocol/current.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
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

namespace {

constexpr refrain::log::logger log_net{"net"};

} // namespace

namespace refrain::net {
namespace {

namespace handshake_sb = ::refrain::protocol::current::handshake::serverbound;
namespace status_sb = ::refrain::protocol::current::status::serverbound;
namespace login_cb = ::refrain::protocol::current::login::clientbound;
namespace login_sb = ::refrain::protocol::current::login::serverbound;
namespace config_cb = ::refrain::protocol::current::configuration::clientbound;
namespace config_sb = ::refrain::protocol::current::configuration::serverbound;
namespace play_cb = ::refrain::protocol::current::play::clientbound;
namespace play_sb = ::refrain::protocol::current::play::serverbound;

/// Packets whose payload contains NBT keep hand-written codecs; only their
/// protocol ids are shared with the schema-generated packet table.

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

[[nodiscard]] auto brand_payload() -> std::vector<std::byte> {
    constexpr std::string_view name{"refrain"};
    std::vector<std::byte> bytes;
    writer out{bytes};
    out.write_varint(static_cast<std::uint32_t>(name.size()));
    out.write_bytes(std::as_bytes(std::span{name}));
    return bytes;
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

/// Concrete network settings resolved from the `network` namespace view at
/// startup. The store remains the source of truth; this snapshot keeps the I/O
/// threads independent of it.
struct server_settings {
    endpoint bind{};
    status_info status{};
    int compression_threshold{256};
    std::int32_t view_distance{10};
    std::int32_t simulation_distance{10};
    bool online_mode{false};
    double spawn_x{0.0};
    double spawn_y{64.0};
    double spawn_z{0.0};
    std::int64_t seed{0};
    std::string registry_catalog_path{};
    std::shared_ptr<const std::vector<std::vector<std::byte>>> spawn_chunks{};
    std::function<bool(std::int32_t, std::int32_t, std::int32_t)> break_block{};
    std::function<bool(std::int32_t, std::int32_t, std::int32_t)> place_block{};
    std::string session_server{"https://sessionserver.mojang.com"};
    std::int32_t auth_timeout_ms{5000};
    std::int32_t rate_limit{0};
    std::int32_t idle_timeout_seconds{0};
};

struct server::impl {
    struct connection_state_data {
        connection_state state{connection_state::handshake};
        std::int32_t protocol{0};
        std::string handshake_host{};
        std::uint16_t handshake_port{0};
        std::string name{};
        uuid profile{};
        uuid session{};
        game_mode mode{game_mode::creative};
        bool login_started{false};
        bool login_finished{false};
        bool status_requested{false};
        bool leave_reported{false};
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
        bool knows_core_pack{false};
        bool registries_sent{false};
        std::uint32_t held_slot{0};
        std::vector<std::byte> verify_token{};
        bool online_pending{false};
        token_bucket limiter{};
        std::int64_t last_packet_ns{0};
    };

    /// One live client connection and its write lock.
    ///
    /// `mutex` guards `data` and is the only lock a connection ever needs;
    /// `closed` is atomic so a sender can reject a closed connection without
    /// waiting for the state lock. Two packets for one connection are appended
    /// to the protocol codec in the order the callers acquired `mutex`, so a
    /// caller that sends sequentially keeps its wire order.
    ///
    /// Lock order (strict): `mutex` may be held while calling the protocol
    /// codec, which takes the codec's own internal mutex. Nothing takes a
    /// connection mutex while holding the codec mutex, no path holds two
    /// connection mutexes at once, and the table's shard mutex is never held
    /// while a connection mutex is acquired. The close path publishes `closed`
    /// with an atomic exchange, then takes `mutex` only to snapshot the leave
    /// identity; a sender that already owns `mutex` completes at most one
    /// packet, and no path holds `mutex` while waiting for close.
    struct connection {
        mutable std::mutex mutex{};
        std::atomic<bool> closed{false};
        connection_state_data data{};
    };

    struct live_connection {
        std::uint64_t id{0};
        std::shared_ptr<connection> handle{};
    };

    /// The id-to-connection table is sharded so a lookup contends only with
    /// the connections that hash to the same shard. Membership changes are
    /// rare compared with sends and lookups; the shard lock covers the map
    /// only, never a connection's state or a call into the codec.
    static constexpr std::size_t connection_shard_count{64};
    struct connection_shard {
        mutable std::mutex mutex{};
        std::unordered_map<std::uint64_t, std::shared_ptr<connection>>
            entries{};
    };
    std::array<connection_shard, connection_shard_count> connection_table{};

    [[nodiscard]] static constexpr auto shard_for(std::uint64_t id) noexcept
        -> std::size_t {
        return static_cast<std::size_t>(id) % connection_shard_count;
    }
    [[nodiscard]] auto find_connection(std::uint64_t id) const
        -> std::shared_ptr<connection>;
    void insert_connection(std::uint64_t id, std::shared_ptr<connection> live);
    void erase_connection(std::uint64_t id);
    /// Copy every table entry. The shard locks are released before the caller
    /// inspects a connection, and a caller that keeps a `shared_ptr` stays
    /// valid after the connection leaves the table.
    [[nodiscard]] auto connection_snapshot() const
        -> std::vector<live_connection>;
    /// Ids of connections in play, in table order.
    [[nodiscard]] auto playing_connections() const
        -> std::vector<std::uint64_t>;
    [[nodiscard]] auto playing_count() const -> std::int32_t;

    /// Run `read` against one connection's state while holding its mutex.
    /// Returns false when the connection is gone or closed. `read` must not
    /// send, close, take a hook or look up another connection: those acquire
    /// the same lock or wait for another one.
    template <typename Reader>
    [[nodiscard]] auto with_connection(std::uint64_t id, Reader read) -> bool {
        const auto live{find_connection(id)};
        if (!live || live->closed.load(std::memory_order_acquire)) {
            return false;
        }
        const std::lock_guard lock{live->mutex};
        if (live->closed.load(std::memory_order_relaxed)) {
            return false;
        }
        read(live->data);
        return true;
    }

    server_settings config{};
    server_policy policy{};
    mutable std::mutex policy_mutex{};
    join_state_provider join_state{};
    command_bridge commands{};
    status_provider status_hook{};
    tab_provider tab_hook{};
    tab_encoder tab_encoder_hook{};
    player_event player_join_hook{};
    player_event player_leave_hook{};
    movement_handler movement_hook{};
    attack_handler attack_hook{};
    animation_handler animation_hook{};
    player_event play_ready_hook{};
    respawn_handler respawn_hook{};
    std::function<void(const view_distance_event &)> view_distance_hook{};
    transport_factory make_transport{};
    std::shared_ptr<protocol_codec> protocol{};
    std::mutex command_cache_mutex{};
    std::shared_ptr<const command_tree> cached_tree{};
    std::shared_ptr<const std::vector<std::byte>> cached_commands{};
    session_verifier verify_session{};
    std::unique_ptr<transport> socket{};
    std::optional<rsa_keypair> server_key{};
    std::unique_ptr<http_transport> http{};
    std::unique_ptr<auth_service> auth{};
    std::optional<registry_catalog> catalog{};
    std::vector<std::vector<std::byte>> configuration_packets{};
    std::vector<std::vector<std::byte>> configuration_registries{};
    std::vector<std::byte> finish_packet{};
    std::atomic<bool> stopped{false};
    struct queued_packet {
        std::uint64_t connection{0};
        std::vector<std::byte> body{};
    };
    std::mutex inbound_mutex{};
    std::deque<queued_packet> inbound{};
    std::mutex click_mutex{};
    std::vector<container_click_event> container_clicks{};
    std::vector<container_close_event> container_closes{};
    std::vector<creative_slot_event> creative_slots{};
    std::atomic<std::uint64_t> packets_in{0};
    std::atomic<std::uint64_t> packets_out{0};
    std::atomic<std::uint64_t> bytes_in{0};
    std::atomic<std::uint64_t> bytes_out{0};
    std::thread keepalive_thread{};

    [[nodiscard]] auto start() -> status;
    void stop();
    void on_open(std::uint64_t id);
    void on_data(std::uint64_t id, std::span<const std::byte> bytes);
    void on_close(std::uint64_t id, std::string_view reason);
    void tick();
    void handle(std::uint64_t id, std::span<const std::byte> body);
    void handle_play(std::uint64_t id, std::span<const std::byte> body);
    void send(std::uint64_t id, std::uint16_t packet_id,
              std::span<const std::byte> payload);
    /// Send a body that still carries its VarInt packet id.
    void send(std::uint64_t id, std::span<const std::byte> packet);
    void close(std::uint64_t id);
    void reject(std::uint64_t id, std::string_view reason);
    void send_login_disconnect(std::uint64_t id, std::string_view text);
    void send_login_hello(std::uint64_t id);
    void complete_login(std::uint64_t id);
    void handle_login_key(std::uint64_t id, reader &in);
    void send_login_finished(std::uint64_t id);
    void send_configuration(std::uint64_t id);
    void send_configuration_registries(std::uint64_t id);
    void send_play_login(std::uint64_t id);
    void send_commands(std::uint64_t id);
    void send_tab_list(std::uint64_t id);
    void set_game_mode(std::uint64_t id, game_mode mode);
    void send_system_chat(std::uint64_t id, std::string_view text);
    void execute_chat_command(std::uint64_t id, std::string_view command);
    void broadcast_system_chat(std::string_view text);
    void send_initial_chunks(std::uint64_t id);
    void send_keepalive(std::uint64_t id);
    void keepalive_loop();
    [[nodiscard]] auto build_configuration() -> status;

    template <typename Packet, typename Decoder>
    void parse_only(std::uint64_t id, reader &in, Decoder decode) {
        Packet packet{};
        if (auto decoded{decode(in, packet)}; !decoded) {
            reject(id, "malformed or unexpected packet");
        }
    }

    template <typename Packet, typename Encoder>
    void send_packet(std::uint64_t id, std::uint16_t packet_id,
                     const Packet &packet, Encoder encode) {
        auto body{encode_packet(packet_id, packet, encode)};
        if (!body) {
            log_net.error("packet encode failed connection={} packet={}", id,
                          packet_id);
            close(id);
            return;
        }
        // The id is already known; skip the varint prefix instead of parsing
        // it back out of the body that was just encoded.
        send(id, packet_id,
             std::span<const std::byte>{*body}.subspan(varint_size(packet_id)));
    }
};

auto server::impl::find_connection(std::uint64_t id) const
    -> std::shared_ptr<connection> {
    const auto &shard{connection_table[shard_for(id)]};
    const std::lock_guard lock{shard.mutex};
    const auto found{shard.entries.find(id)};
    if (found == shard.entries.end()) {
        return {};
    }
    return found->second;
}

void server::impl::insert_connection(std::uint64_t id,
                                     std::shared_ptr<connection> live) {
    auto &shard{connection_table[shard_for(id)]};
    const std::lock_guard lock{shard.mutex};
    shard.entries.insert_or_assign(id, std::move(live));
}

void server::impl::erase_connection(std::uint64_t id) {
    auto &shard{connection_table[shard_for(id)]};
    const std::lock_guard lock{shard.mutex};
    shard.entries.erase(id);
}

auto server::impl::connection_snapshot() const -> std::vector<live_connection> {
    std::vector<live_connection> snapshot;
    for (const auto &shard : connection_table) {
        const std::lock_guard lock{shard.mutex};
        snapshot.reserve(snapshot.size() + shard.entries.size());
        for (const auto &[id, live] : shard.entries) {
            snapshot.push_back(live_connection{.id = id, .handle = live});
        }
    }
    return snapshot;
}

auto server::impl::playing_connections() const -> std::vector<std::uint64_t> {
    std::vector<std::uint64_t> ids;
    for (const auto &entry : connection_snapshot()) {
        if (entry.handle->closed.load(std::memory_order_acquire)) {
            continue;
        }
        const std::lock_guard lock{entry.handle->mutex};
        if (entry.handle->data.state == connection_state::play) {
            ids.push_back(entry.id);
        }
    }
    return ids;
}

auto server::impl::playing_count() const -> std::int32_t {
    std::int32_t online{0};
    for (const auto &entry : connection_snapshot()) {
        if (entry.handle->closed.load(std::memory_order_acquire)) {
            continue;
        }
        const std::lock_guard lock{entry.handle->mutex};
        if (entry.handle->data.state == connection_state::play) {
            ++online;
        }
    }
    return online;
}

auto server::impl::start() -> status {
    if (config.online_mode) {
        auto key{rsa_keypair::generate()};
        if (!key) {
            return std::unexpected{std::move(key.error())};
        }
        server_key = std::move(*key);
    }
    if (config.online_mode && !verify_session) {
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
    auto opened{make_transport ? make_transport(std::move(handler), 1)
                               : transport::open(std::move(handler), 1)};
    if (!opened) {
        return std::unexpected{std::move(opened.error())};
    }
    socket = std::move(*opened);
    if (auto listening{socket->listen(config.bind)}; !listening) {
        return listening;
    }
    keepalive_thread = std::thread{[this] {
        log::set_thread_name("net-keepalive");
        keepalive_loop();
    }};
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
    for (auto &shard : connection_table) {
        const std::lock_guard lock{shard.mutex};
        shard.entries.clear();
    }
}

void server::impl::on_open(std::uint64_t id) {
    if (stopped.load(std::memory_order_relaxed)) {
        return;
    }
    log_net.debug("connection opened id={}", id);
    protocol->create_connection(id);
    protocol->notify_state(id, connection_state::handshake);
    auto live{std::make_shared<connection>()};
    live->data.limiter.configure(config.rate_limit);
    live->data.last_packet_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    insert_connection(id, std::move(live));
}

void server::impl::on_close(std::uint64_t id, std::string_view /*reason*/) {
    std::uint64_t left_id{0};
    uuid left_profile{};
    std::string left_name{};
    bool report_leave{false};
    if (const auto live{find_connection(id)}) {
        {
            const std::lock_guard lock{live->mutex};
            auto &data{live->data};
            if (data.state == connection_state::play && !data.name.empty()) {
                left_id = id;
                left_profile = data.profile;
                left_name = data.name;
                report_leave = !data.leave_reported;
                data.leave_reported = true;
            }
        }
        erase_connection(id);
    }
    if (left_id != 0) {
        if (report_leave && player_leave_hook) {
            player_leave_hook(player_identity{
                .connection = left_id,
                .id = left_profile,
                .name = left_name,
                .world = {},
            });
        }
    }
}

void server::impl::on_data(std::uint64_t id, std::span<const std::byte> bytes) {
    bytes_in.fetch_add(bytes.size(), std::memory_order_relaxed);
    const auto live{find_connection(id)};
    if (!live || live->closed.load(std::memory_order_acquire)) {
        return;
    }
    std::vector<std::vector<std::byte>> bodies;
    bool failed{false};
    bool rate_limited{false};
    {
        const std::lock_guard lock{live->mutex};
        auto &connection_data{live->data};
        const auto now_ns = [] {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        };
        if (auto fed{protocol->feed(id, bytes)}; !fed) {
            failed = true;
        } else {
            for (;;) {
                std::uint32_t packet_id{0};
                std::span<const std::byte> payload{};
                auto next{protocol->next_packet(id, packet_id, payload)};
                if (!next) {
                    failed = true;
                    break;
                }
                if (!*next) {
                    break;
                }
                if (connection_data.state == connection_state::play &&
                    !connection_data.limiter.allow(now_ns())) {
                    rate_limited = true;
                    break;
                }
                connection_data.last_packet_ns = now_ns();
                std::vector<std::byte> body;
                writer out{body};
                out.write_varint(packet_id);
                out.write_bytes(payload);
                bodies.push_back(std::move(body));
            }
        }
    }
    if (failed) {
        close(id);
        return;
    }
    if (rate_limited) {
        log_net.warn("rate limit exceeded id={}", id);
        if (socket) {
            socket->close(id);
        }
        return;
    }
    for (const auto &body : bodies) {
        if (live->closed.load(std::memory_order_acquire)) {
            return;
        }
        packets_in.fetch_add(1, std::memory_order_relaxed);
        handle(id, body);
    }
}

void server::impl::handle(std::uint64_t id, std::span<const std::byte> body) {
    connection_state state{connection_state::handshake};
    const auto live{find_connection(id)};
    if (!live || live->closed.load(std::memory_order_acquire)) {
        return;
    }
    {
        const std::lock_guard lock{live->mutex};
        state = live->data.state;
    }
    reader in{body};
    auto packet_id{in.read_varint()};
    if (!packet_id) {
        reject(id, "malformed or unexpected packet");
        return;
    }

    if (state == connection_state::handshake) {
        if (*packet_id != ::refrain::protocol::current::handshake::serverbound::
                              intention::packet_id) {
            reject(id, "malformed or unexpected packet");
            return;
        }
        ::refrain::protocol::current::handshake::serverbound::intention
            intention{};
        if (auto decoded{::refrain::protocol::current::handshake::serverbound::
                             decode_intention(in, intention)};
            !decoded) {
            reject(id, "malformed or unexpected packet");
            return;
        }
        if (!with_connection(id, [&](connection_state_data &data) {
                data.protocol = static_cast<std::int32_t>(intention.protocol);
                data.handshake_host = intention.host;
                data.handshake_port = intention.port;
            })) {
            return;
        }
        const auto event{intention.next == 1 ? protocol_event::status_request
                                             : protocol_event::login_request};
        const auto next_state{protocol->transition(id, event)};
        const auto expected{intention.next == 1 ? connection_state::status
                                                : connection_state::login};
        if (next_state != expected) {
            reject(id, "malformed or unexpected packet");
            return;
        }
        if (!with_connection(id, [next_state](connection_state_data &data) {
                data.state = next_state;
            })) {
            return;
        }
        log_net.debug("handshake id={} protocol={} next={}", id,
                      intention.protocol, intention.next);
        if (intention.next == 1) {
            return;
        }
        if (intention.next == 2) {
            if (intention.protocol != 777) {
                send_login_disconnect(
                    id, "Incompatible client: expected protocol 777");
                reject(id, "malformed or unexpected packet");
            }
            return;
        }
        reject(id, "malformed or unexpected packet");
        return;
    }

    if (state == connection_state::status) {
        if (*packet_id == ::refrain::protocol::current::status::serverbound::
                              status_request::packet_id) {
            bool duplicate{false};
            if (!with_connection(id, [&duplicate](connection_state_data &data) {
                    duplicate = data.status_requested;
                    data.status_requested = true;
                })) {
                return;
            }
            if (duplicate) {
                reject(id, "malformed or unexpected packet");
                return;
            }
            status_request_context context{};
            if (!with_connection(id, [&context](connection_state_data &data) {
                    context.protocol = data.protocol;
                    context.host = data.handshake_host;
                    context.port = data.handshake_port;
                })) {
                return;
            }
            context.online_players = playing_count();
            auto info{status_hook ? status_hook(context) : config.status};
            auto response{encode_status_response(build_status_json(info))};
            if (!response) {
                reject(id, "malformed or unexpected packet");
                return;
            }
            send(id, *response);
            return;
        }
        if (*packet_id == ::refrain::protocol::current::status::serverbound::
                              ping_request::packet_id) {
            status_sb::ping_request packet{};
            if (auto decoded{::refrain::protocol::current::status::serverbound::
                                 decode_ping_request(in, packet)};
                !decoded) {
                reject(id, "malformed or unexpected packet");
                return;
            }
            send(id, encode_pong_response(packet.time));
            return;
        }
        reject(id, "malformed or unexpected packet");
        return;
    }

    if (state == connection_state::login) {
        if (*packet_id == login_sb::hello::packet_id) {
            bool started{false};
            if (!with_connection(id, [&started](connection_state_data &data) {
                    started = data.login_started;
                    data.login_started = true;
                })) {
                return;
            }
            if (started) {
                reject(id, "malformed or unexpected packet");
                return;
            }
            login_sb::hello packet{};
            if (auto decoded{login_sb::decode_hello(in, packet)}; !decoded) {
                send_login_disconnect(id, "Invalid username");
                reject(id, "malformed or unexpected packet");
                return;
            }
            if (!valid_name(packet.name)) {
                send_login_disconnect(id, "Invalid username");
                reject(id, "malformed or unexpected packet");
                return;
            }
            log_net.info("login started id={} name={}", id, packet.name);
            const auto session{random_uuid()};
            if (!with_connection(
                    id, [&packet, session](connection_state_data &data) {
                        data.name = std::move(packet.name);
                        auto profile{uuid_from_halves(packet.profile_high,
                                                      packet.profile_low)};
                        const bool zero_profile{std::ranges::all_of(
                            profile.bytes,
                            [](std::uint8_t byte) { return byte == 0U; })};
                        data.profile = zero_profile ? random_uuid() : profile;
                        data.session = session;
                    })) {
                return;
            }
            if (config.online_mode) {
                std::random_device device{};
                std::vector<std::byte> token(4);
                for (auto &byte : token) {
                    byte = static_cast<std::byte>(device() & 0xFFU);
                }
                if (!with_connection(id, [&token](connection_state_data &data) {
                        data.verify_token = std::move(token);
                    })) {
                    return;
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
            std::string name;
            bool finished{false};
            if (!with_connection(
                    id, [&name, &finished](connection_state_data &data) {
                        name = data.name;
                        finished = data.login_finished;
                    })) {
                return;
            }
            // Login is acknowledged only after the server finished it; without
            // this guard a client could skip hello and encryption entirely.
            if (!finished) {
                reject(id, "malformed or unexpected packet");
                return;
            }
            login_sb::login_acknowledged packet{};
            if (auto decoded{login_sb::decode_login_acknowledged(in, packet)};
                !decoded) {
                reject(id, "malformed or unexpected packet");
                return;
            }
            const auto next_state{
                protocol->transition(id, protocol_event::login_acknowledged)};
            if (next_state != connection_state::configuration) {
                reject(id, "malformed or unexpected packet");
                return;
            }
            if (!with_connection(id, [next_state](connection_state_data &data) {
                    data.state = next_state;
                })) {
                return;
            }
            log_net.info("login complete id={} name={}", id, name);
            send_configuration(id);
            return;
        }
        reject(id, "malformed or unexpected packet");
        return;
    }

    if (state == connection_state::configuration) {
        switch (*packet_id) {
        case config_sb::client_information::packet_id: {
            config_sb::client_information packet{};
            if (auto decoded{config_sb::decode_client_information(in, packet)};
                !decoded) {
                reject(id, "malformed or unexpected packet");
                return;
            }
            return;
        }
        case config_sb::finish_configuration::packet_id: {
            config_sb::finish_configuration packet{};
            if (auto decoded{
                    config_sb::decode_finish_configuration(in, packet)};
                !decoded) {
                reject(id, "malformed or unexpected packet");
                return;
            }
            const auto next_state{protocol->transition(
                id, protocol_event::configuration_acknowledged)};
            if (next_state != connection_state::play) {
                reject(id, "malformed or unexpected packet");
                return;
            }
            if (!with_connection(id, [next_state](connection_state_data &data) {
                    data.state = next_state;
                })) {
                return;
            }
            send_play_login(id);
            return;
        }
        case config_sb::keep_alive::packet_id: {
            config_sb::keep_alive packet{};
            if (auto decoded{config_sb::decode_keep_alive(in, packet)};
                !decoded) {
                reject(id, "malformed or unexpected packet");
            }
            return;
        }
        case config_sb::pong::packet_id: {
            config_sb::pong packet{};
            if (auto decoded{config_sb::decode_pong(in, packet)}; !decoded) {
                reject(id, "malformed or unexpected packet");
            }
            return;
        }
        case config_sb::select_known_packs::packet_id: {
            config_sb::select_known_packs packet{};
            if (auto decoded{config_sb::decode_select_known_packs(in, packet)};
                !decoded) {
                reject(id, "malformed or unexpected packet");
                return;
            }
            const auto knows_core{std::any_of(
                packet.packs.begin(), packet.packs.end(), [](const auto &pack) {
                    return pack.pack_namespace == "minecraft" &&
                           pack.id == "core";
                })};
            bool send_registries{false};
            if (!with_connection(id, [knows_core, &send_registries](
                                         connection_state_data &data) {
                    if (!data.registries_sent) {
                        data.knows_core_pack = knows_core;
                        data.registries_sent = true;
                        send_registries = true;
                    }
                })) {
                return;
            }
            if (send_registries) {
                send_configuration_registries(id);
            }
            return;
        }
        case config_sb::custom_payload::packet_id: {
            config_sb::custom_payload packet{};
            if (auto decoded{config_sb::decode_custom_payload(in, packet)};
                !decoded) {
                reject(id, "malformed or unexpected packet");
            }
            return;
        }
        case config_sb::cookie_response::packet_id:
            parse_only<config_sb::cookie_response>(
                id, in, config_sb::decode_cookie_response);
            return;
        case config_sb::resource_pack::packet_id:
            parse_only<config_sb::resource_pack>(
                id, in, config_sb::decode_resource_pack);
            return;
        case config_sb::custom_click_action::packet_id:
            parse_only<config_sb::custom_click_action>(
                id, in, config_sb::decode_custom_click_action);
            return;
        case config_sb::accept_code_of_conduct::packet_id:
            parse_only<config_sb::accept_code_of_conduct>(
                id, in, config_sb::decode_accept_code_of_conduct);
            return;
        default:
            reject(id, "unknown configuration packet");
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
    if (!with_connection(id, [&token](connection_state_data &data) {
            token = data.verify_token;
        })) {
        return;
    }
    auto public_key{server_key->public_key_der()};
    if (!public_key) {
        send_login_disconnect(id, "Server encryption is unavailable");
        reject(id, "malformed or unexpected packet");
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
        const auto live{find_connection(id)};
        if (!live || live->closed.load(std::memory_order_acquire)) {
            return;
        }
        if (protocol) {
            static_cast<void>(
                protocol->set_compression(id, config.compression_threshold));
        }
    }
    send_login_finished(id);
}

void server::impl::handle_login_key(std::uint64_t id, reader &in) {
    login_sb::key packet{};
    if (auto decoded{login_sb::decode_key(in, packet)}; !decoded) {
        send_login_disconnect(id, "Malformed encryption response");
        reject(id, "malformed or unexpected packet");
        return;
    }
    std::vector<std::byte> expected_token;
    if (!with_connection(id, [&expected_token](connection_state_data &data) {
            expected_token = data.verify_token;
        })) {
        return;
    }
    auto secret{server_key->decrypt_pkcs1_v1_5(packet.keybytes)};
    auto token{server_key->decrypt_pkcs1_v1_5(packet.encrypted_challenge)};
    if (!secret || !token) {
        send_login_disconnect(id, "Malformed encryption response");
        reject(id, "malformed or unexpected packet");
        return;
    }
    if (token->size() != expected_token.size() ||
        !std::equal(token->begin(), token->end(), expected_token.begin())) {
        send_login_disconnect(id, "Invalid verify token");
        reject(id, "malformed or unexpected packet");
        return;
    }
    if (secret->size() != aes128_key_bytes) {
        send_login_disconnect(id, "Invalid shared secret");
        reject(id, "malformed or unexpected packet");
        return;
    }
    auto cipher{aes128_cfb8::create(*secret, *secret)};
    if (!cipher) {
        send_login_disconnect(id, "Encryption is unavailable");
        reject(id, "malformed or unexpected packet");
        return;
    }
    auto public_key{server_key->public_key_der()};
    if (!public_key) {
        send_login_disconnect(id, "Encryption is unavailable");
        reject(id, "malformed or unexpected packet");
        return;
    }
    auto digest{minecraft_server_hash("", *secret, *public_key)};
    if (!digest) {
        send_login_disconnect(id, "Encryption is unavailable");
        reject(id, "malformed or unexpected packet");
        return;
    }
    std::string name;
    const auto live{find_connection(id)};
    if (!live || live->closed.load(std::memory_order_acquire)) {
        return;
    }
    {
        const std::lock_guard lock{live->mutex};
        name = live->data.name;
    }
    if (protocol) {
        static_cast<void>(protocol->set_cipher(
            id, std::span<const std::byte>{secret->data(), secret->size()},
            std::span<const std::byte>{secret->data(), secret->size()}));
    }
    session_profile profile{};
    if (verify_session) {
        auto verified{verify_session(name, *digest)};
        if (!verified) {
            send_login_disconnect(id, "Failed to verify username");
            reject(id, "malformed or unexpected packet");
            return;
        }
        profile = std::move(*verified);
    } else if (auth) {
        auto verified{auth->has_joined(name, *digest)};
        if (!verified) {
            send_login_disconnect(id, "Failed to verify username");
            reject(id, "malformed or unexpected packet");
            return;
        }
        profile.id = verified->id;
        profile.name = verified->name;
    } else {
        send_login_disconnect(id, "Authentication is unavailable");
        reject(id, "malformed or unexpected packet");
        return;
    }
    if (!with_connection(id, [&profile](connection_state_data &data) {
            data.profile = profile.id;
            data.name = std::move(profile.name);
        })) {
        return;
    }
    complete_login(id);
}

void server::impl::send_login_finished(std::uint64_t id) {
    uuid profile{};
    uuid session{};
    std::string name;
    if (!with_connection(
            id, [&profile, &session, &name](connection_state_data &data) {
                profile = data.profile;
                session = data.session;
                name = data.name;
                data.login_finished = true;
            })) {
        return;
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
}

void server::impl::send_configuration_registries(std::uint64_t id) {
    for (const auto &packet : configuration_registries) {
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
                                         .data = brand_payload()};
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
        packet.packs.push_back(::refrain::protocol::current::common::known_pack{
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
            configuration_registries.push_back(std::move(packet));
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
        configuration_registries.push_back(std::move(packet));
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

namespace {

[[nodiscard]] constexpr auto protocol_game_mode(game_mode mode)
    -> std::uint32_t {
    return static_cast<std::uint32_t>(mode);
}

/// Ability flag bits: invulnerable=1, flying=2, allow_flying=4, creative=8.
[[nodiscard]] constexpr auto ability_flags(game_mode mode) -> std::uint8_t {
    switch (mode) {
    case game_mode::creative:
        return 0x0DU; // invulnerable | allow_flying | creative
    case game_mode::spectator:
        return 0x07U; // invulnerable | flying | allow_flying
    case game_mode::survival:
    case game_mode::adventure:
        break;
    }
    return 0x00U;
}

} // namespace

void server::impl::send_play_login(std::uint64_t id) {
    std::string name;
    uuid profile{};
    if (!with_connection(id,
                         [this, &name, &profile](connection_state_data &data) {
                             data.x = config.spawn_x;
                             data.y = config.spawn_y;
                             data.z = config.spawn_z;
                             data.awaiting_teleport = true;
                             name = data.name;
                             profile = data.profile;
                         })) {
        return;
    }
    game_mode mode{game_mode::creative};
    if (join_state) {
        mode = join_state(player_identity{
                              .connection = id,
                              .id = profile,
                              .name = name,
                              .world = {},
                          })
                   .mode;
    }
    if (!with_connection(
            id, [mode](connection_state_data &data) { data.mode = mode; })) {
        return;
    }
    log_net.info("player joined id={} name={}", id, name);
    server_policy policy_snapshot{};
    {
        const std::lock_guard lock{policy_mutex};
        policy_snapshot = policy;
    }

    const auto send_login = [&] {
        play_cb::login packet{};
        packet.player_id = static_cast<std::int32_t>(id);
        packet.hardcore = policy_snapshot.hardcore;
        packet.levels = {overworld()};
        packet.max_players =
            static_cast<std::uint32_t>(config.status.max_players);
        packet.chunk_radius = static_cast<std::uint32_t>(config.view_distance);
        packet.simulation_distance =
            static_cast<std::uint32_t>(config.simulation_distance);
        packet.reduced_debug_info = false;
        packet.show_death_screen = true;
        packet.limited_crafting = false;
        packet.spawn.dimension_type = 0;
        packet.spawn.dimension = overworld();
        packet.spawn.seed = config.seed;
        packet.spawn.game_type = static_cast<std::uint32_t>(mode);
        packet.spawn.previous_game_type = std::nullopt;
        packet.spawn.debug = false;
        packet.spawn.flat = false;
        packet.spawn.last_death_location = std::nullopt;
        packet.spawn.portal_cooldown = 0;
        packet.spawn.sea_level = 63;
        packet.online_mode = config.online_mode;
        packet.enforces_secure_chat = config.status.enforces_secure_chat;
        send_packet(id, play_cb::login::packet_id, packet,
                    play_cb::encode_login);
    };
    const auto send_difficulty = [&] {
        play_cb::change_difficulty difficulty{
            .difficulty = policy_snapshot.difficulty,
            .locked = policy_snapshot.difficulty_locked};
        send_packet(id, play_cb::change_difficulty::packet_id, difficulty,
                    play_cb::encode_change_difficulty);
    };
    const auto send_abilities = [&] {
        play_cb::player_abilities abilities{
            .flags = ability_flags(mode),
            .flying_speed = 0.05F,
            .walking_speed = 0.1F,
        };
        send_packet(id, play_cb::player_abilities::packet_id, abilities,
                    play_cb::encode_player_abilities);
    };
    const auto send_held_slot = [&] {
        play_cb::set_held_slot held{.slot = 0};
        send_packet(id, play_cb::set_held_slot::packet_id, held,
                    play_cb::encode_set_held_slot);
    };
    const auto send_recipes = [&] {
        play_cb::update_recipes recipes{};
        send_packet(id, play_cb::update_recipes::packet_id, recipes,
                    play_cb::encode_update_recipes);
    };
    const auto send_permissions = [&] {
        play_cb::entity_event event{
            .entity_id = static_cast<std::int32_t>(id),
            .event = 24, // permission level: ALL
        };
        send_packet(id, play_cb::entity_event::packet_id, event,
                    play_cb::encode_entity_event);
    };
    const auto send_recipe_book = [&] {
        play_cb::recipe_book_settings settings{};
        send_packet(id, play_cb::recipe_book_settings::packet_id, settings,
                    play_cb::encode_recipe_book_settings);
        play_cb::recipe_book_add recipes{};
        send_packet(id, play_cb::recipe_book_add::packet_id, recipes,
                    play_cb::encode_recipe_book_add);
    };
    const auto send_position = [&] {
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
    };
    const auto send_server_data = [&] {
        play_cb::server_data data{
            .motd = {.text = config.status.description},
            .icon_bytes = std::nullopt,
        };
        send_packet(id, play_cb::server_data::packet_id, data,
                    play_cb::encode_server_data);
    };
    const auto send_border = [&] {
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
    };
    const auto send_time = [&] {
        // TODO: the world clock never advances; set_time is sent once with a
        // fixed time and no periodic update follows.
        play_cb::set_time time{.game_time = 0, .clock_updates = {}};
        send_packet(id, play_cb::set_time::packet_id, time,
                    play_cb::encode_set_time);
    };
    const auto send_spawn = [&] {
        play_cb::set_default_spawn_position spawn{
            .dimension = overworld(),
            .position =
                pack_block_position(static_cast<std::int32_t>(config.spawn_x),
                                    static_cast<std::int32_t>(config.spawn_y),
                                    static_cast<std::int32_t>(config.spawn_z)),
            .yaw = 0.0F,
            .pitch = 0.0F,
        };
        send_packet(id, play_cb::set_default_spawn_position::packet_id, spawn,
                    play_cb::encode_set_default_spawn_position);
    };
    const auto send_level_start = [&] {
        play_cb::game_event event{.event = 13, .value = 0.0F};
        send_packet(id, play_cb::game_event::packet_id, event,
                    play_cb::encode_game_event);
    };
    const auto send_ticking = [&] {
        play_cb::ticking_state ticking{.tick_rate = 20.0F, .is_frozen = false};
        send_packet(id, play_cb::ticking_state::packet_id, ticking,
                    play_cb::encode_ticking_state);
        play_cb::ticking_step step{.tick_steps = 0};
        send_packet(id, play_cb::ticking_step::packet_id, step,
                    play_cb::encode_ticking_step);
    };
    const auto send_view_cache = [&] {
        play_cb::set_chunk_cache_center center{
            .chunk_x =
                static_cast<std::int32_t>(std::floor(config.spawn_x / 16.0)),
            .chunk_z =
                static_cast<std::int32_t>(std::floor(config.spawn_z / 16.0)),
        };
        send_packet(id, play_cb::set_chunk_cache_center::packet_id, center,
                    play_cb::encode_set_chunk_cache_center);
    };
    const auto send_entity_state = [&] {
        play_cb::set_entity_data data{
            .entity_id = static_cast<std::uint32_t>(id),
            .values = {},
        };
        send_packet(id, play_cb::set_entity_data::packet_id, data,
                    play_cb::encode_set_entity_data);
        play_cb::update_attributes attributes{
            .get_entity_id = static_cast<std::int32_t>(id),
            .get_values = {},
        };
        send_packet(id, play_cb::update_attributes::packet_id, attributes,
                    play_cb::encode_update_attributes);
        play_cb::update_advancements advancements{
            .should_reset = true,
            .added = {},
            .removed = {},
            .progress = {},
            .show_advancements = false,
        };
        send_packet(id, play_cb::update_advancements::packet_id, advancements,
                    play_cb::encode_update_advancements);
        play_cb::post_effects effects{};
        send_packet(id, play_cb::post_effects::packet_id, effects,
                    play_cb::encode_post_effects);
    };

    send_login();
    send_difficulty();
    send_abilities();
    send_held_slot();
    send_recipes();
    send_permissions();
    send_commands(id);
    send_recipe_book();
    send_position();
    send_server_data();
    if (player_join_hook) {
        player_join_hook(player_identity{
            .connection = id,
            .id = profile,
            .name = name,
            .world = {},
        });
    }
    send_border();
    send_time();
    send_spawn();
    send_level_start();
    send_ticking();
    send_view_cache();
    send_initial_chunks(id);
    send_entity_state();
    send_tab_list(id);
    if (play_ready_hook) {
        play_ready_hook(player_identity{
            .connection = id,
            .id = profile,
            .name = name,
            .world = {},
        });
    }
}

void server::impl::send_commands(std::uint64_t id) {
    if (!commands.tree) {
        return;
    }
    std::string name;
    uuid profile{};
    if (!with_connection(id, [&name, &profile](connection_state_data &data) {
            name = data.name;
            profile = data.profile;
        })) {
        return;
    }
    const player_identity identity{
        .connection = id,
        .id = profile,
        .name = name,
        .world = {},
    };
    auto tree{commands.tree(identity)};
    if (!tree) {
        return;
    }
    std::shared_ptr<const std::vector<std::byte>> body;
    {
        const std::lock_guard lock{command_cache_mutex};
        if (cached_tree == tree && cached_commands) {
            body = cached_commands;
        }
    }
    if (!body) {
        play_cb::commands packet{};
        packet.tree = *tree;
        auto encoded{encode_packet(play_cb::commands::packet_id, packet,
                                   play_cb::encode_commands)};
        if (!encoded) {
            return;
        }
        body =
            std::make_shared<const std::vector<std::byte>>(std::move(*encoded));
        const std::lock_guard lock{command_cache_mutex};
        cached_tree = std::move(tree);
        cached_commands = body;
    }
    send(id, *body);
}

void server::impl::send_tab_list(std::uint64_t id) {
    if (!tab_hook) {
        return;
    }
    std::string name;
    uuid profile{};
    if (!with_connection(id, [&name, &profile](connection_state_data &data) {
            name = data.name;
            profile = data.profile;
        })) {
        return;
    }
    const player_identity identity{
        .connection = id,
        .id = profile,
        .name = name,
        .world = {},
    };
    const auto view{tab_hook(identity)};
    if (tab_encoder_hook) {
        if (auto encoded{tab_encoder_hook(view.header, view.footer)}; encoded) {
            send(id, *encoded);
            return;
        }
    }
    play_cb::tab_list packet{};
    packet.header.text = view.header;
    packet.footer.text = view.footer;
    send_packet(id, play_cb::tab_list::packet_id, packet,
                play_cb::encode_tab_list);
}

void server::impl::execute_chat_command(std::uint64_t id,
                                        std::string_view command) {
    if (!command.empty() && command.front() == '/') {
        command.remove_prefix(1);
    }
    if (command.empty() || !commands.execute) {
        return;
    }
    std::string name;
    uuid profile{};
    if (!with_connection(id, [&name, &profile](connection_state_data &data) {
            name = data.name;
            profile = data.profile;
        })) {
        return;
    }
    const player_identity identity{
        .connection = id,
        .id = profile,
        .name = name,
        .world = {},
    };
    const auto feedback{commands.execute(identity, command)};
    if (feedback.handled && !feedback.message.empty()) {
        send_system_chat(id, feedback.message);
    }
}

void server::impl::send_system_chat(std::uint64_t id, std::string_view text) {
    play_cb::system_chat packet{};
    packet.content.text = std::string{text};
    packet.overlay = false;
    send_packet(id, play_cb::system_chat::packet_id, packet,
                play_cb::encode_system_chat);
}

void server::impl::broadcast_system_chat(std::string_view text) {
    play_cb::system_chat packet{};
    packet.content.text = std::string{text};
    packet.overlay = false;
    std::vector<std::uint64_t> targets{playing_connections()};
    for (const auto target : targets) {
        send_packet(target, play_cb::system_chat::packet_id, packet,
                    play_cb::encode_system_chat);
    }
}

void server::send_forget_chunk(std::uint64_t connection, std::int32_t chunk_x,
                               std::int32_t chunk_z) {
    if (!impl_) {
        return;
    }
    play_cb::forget_level_chunk packet{};
    // ChunkPos.pack: the low half carries x, the high half carries z.
    const auto low{
        static_cast<std::uint64_t>(static_cast<std::uint32_t>(chunk_x))};
    const auto high{
        static_cast<std::uint64_t>(static_cast<std::uint32_t>(chunk_z)) << 32U};
    const auto packed{low | high};
    packet.pos = static_cast<std::int64_t>(packed);
    impl_->send_packet(connection, play_cb::forget_level_chunk::packet_id,
                       packet, play_cb::encode_forget_level_chunk);
}

void server::send_chunk_batch(std::uint64_t connection, std::int32_t chunk_x,
                              std::int32_t chunk_z,
                              std::span<const std::vector<std::byte>> packets,
                              bool update_center) {
    if (!impl_) {
        return;
    }
    auto &state{*impl_};
    if (update_center) {
        play_cb::set_chunk_cache_center center{
            .chunk_x = chunk_x,
            .chunk_z = chunk_z,
        };
        state.send_packet(connection,
                          play_cb::set_chunk_cache_center::packet_id, center,
                          play_cb::encode_set_chunk_cache_center);
    }
    if (packets.empty()) {
        return;
    }
    play_cb::chunk_batch_start start{};
    state.send_packet(connection, play_cb::chunk_batch_start::packet_id, start,
                      play_cb::encode_chunk_batch_start);
    for (const auto &body : packets) {
        state.send(connection, body);
    }
    play_cb::chunk_batch_finished finished{
        .batch_size = static_cast<std::uint32_t>(packets.size()),
    };
    state.send_packet(connection, play_cb::chunk_batch_finished::packet_id,
                      finished, play_cb::encode_chunk_batch_finished);
}

auto server::backlog(std::uint64_t connection) const -> std::size_t {
    if (!impl_ || !impl_->socket) {
        return 0;
    }
    return impl_->socket->backlog(connection);
}

auto server::stats() const -> server_stats {
    if (!impl_) {
        return {};
    }
    server_stats out{};
    for (const auto &entry : impl_->connection_snapshot()) {
        if (entry.handle->closed.load(std::memory_order_acquire)) {
            continue;
        }
        const std::lock_guard lock{entry.handle->mutex};
        if (entry.handle->data.state == connection_state::play) {
            ++out.connections;
        }
    }
    out.packets_in = impl_->packets_in.load(std::memory_order_relaxed);
    out.packets_out = impl_->packets_out.load(std::memory_order_relaxed);
    out.bytes_in = impl_->bytes_in.load(std::memory_order_relaxed);
    out.bytes_out = impl_->bytes_out.load(std::memory_order_relaxed);
    return out;
}

auto server::take_container_clicks() -> std::vector<container_click_event> {
    if (!impl_) {
        return {};
    }
    std::vector<container_click_event> clicks;
    {
        const std::lock_guard lock{impl_->click_mutex};
        clicks.swap(impl_->container_clicks);
    }
    return clicks;
}

auto server::take_creative_slots() -> std::vector<creative_slot_event> {
    if (!impl_) {
        return {};
    }
    std::vector<creative_slot_event> slots;
    {
        const std::lock_guard lock{impl_->click_mutex};
        slots.swap(impl_->creative_slots);
    }
    return slots;
}

auto server::take_container_closes() -> std::vector<container_close_event> {
    if (!impl_) {
        return {};
    }
    std::vector<container_close_event> closes;
    {
        const std::lock_guard lock{impl_->click_mutex};
        closes.swap(impl_->container_closes);
    }
    return closes;
}

void server::send_container_content(std::uint64_t connection,
                                    std::int32_t container_id,
                                    std::int32_t state_id,
                                    std::span<const item_stack> items,
                                    const item_stack &carried) {
    if (!impl_) {
        return;
    }
    play_cb::container_set_content packet{};
    packet.container_id = container_id;
    packet.state_id = state_id;
    packet.items.assign(items.begin(), items.end());
    packet.carried_item = carried;
    impl_->send_packet(connection, play_cb::container_set_content::packet_id,
                       packet, play_cb::encode_container_set_content);
}

void server::send_container_set_slot(std::uint64_t connection,
                                     std::int32_t container_id,
                                     std::int32_t state_id, std::int16_t slot,
                                     const item_stack &item) {
    if (!impl_) {
        return;
    }
    play_cb::container_set_slot packet{};
    packet.container_id = container_id;
    packet.state_id = state_id;
    packet.slot = slot;
    packet.item_stack = item;
    impl_->send_packet(connection, play_cb::container_set_slot::packet_id,
                       packet, play_cb::encode_container_set_slot);
}

void server::send_cursor_item(std::uint64_t connection,
                              const item_stack &item) {
    if (!impl_) {
        return;
    }
    play_cb::set_cursor_item packet{};
    packet.contents = item;
    impl_->send_packet(connection, play_cb::set_cursor_item::packet_id, packet,
                       play_cb::encode_set_cursor_item);
}

void server::send_raw(std::uint64_t connection,
                      std::span<const std::byte> body) {
    if (impl_) {
        impl_->send(connection, body);
    }
}

void server::broadcast_raw(std::span<const std::byte> body,
                           std::uint64_t except) {
    if (!impl_) {
        return;
    }
    auto targets{impl_->playing_connections()};
    if (except != 0) {
        std::erase(targets, except);
    }
    for (const auto target : targets) {
        impl_->send(target, body);
    }
}

void server::refresh_tab() {
    if (!impl_) {
        return;
    }
    for (const auto id : active_connections()) {
        impl_->send_tab_list(id);
    }
}

void server::refresh_tab(std::uint64_t connection) {
    if (impl_) {
        impl_->send_tab_list(connection);
    }
}

void server::resend_commands() {
    if (!impl_) {
        return;
    }
    {
        const std::lock_guard lock{impl_->command_cache_mutex};
        impl_->cached_tree.reset();
        impl_->cached_commands.reset();
    }
    for (const auto id : active_connections()) {
        impl_->send_commands(id);
    }
}

void server::resend_commands(std::uint64_t connection) {
    if (!impl_) {
        return;
    }
    {
        const std::lock_guard lock{impl_->command_cache_mutex};
        impl_->cached_tree.reset();
        impl_->cached_commands.reset();
    }
    impl_->send_commands(connection);
}

void server::set_difficulty(std::uint32_t difficulty, bool locked) {
    if (!impl_) {
        return;
    }
    {
        const std::lock_guard lock{impl_->policy_mutex};
        impl_->policy.difficulty = difficulty;
        impl_->policy.difficulty_locked = locked;
    }
    play_cb::change_difficulty packet{.difficulty = difficulty,
                                      .locked = locked};
    std::vector<std::byte> body;
    if (auto encoded{encode_packet(play_cb::change_difficulty::packet_id,
                                   packet, play_cb::encode_change_difficulty)};
        encoded) {
        body = std::move(*encoded);
    } else {
        return;
    }
    for (const auto connection : active_connections()) {
        impl_->send(connection, body);
    }
}

void server::set_hardcore(bool hardcore) {
    if (!impl_) {
        return;
    }
    const std::lock_guard lock{impl_->policy_mutex};
    impl_->policy.hardcore = hardcore;
}

void server::send_system_chat(std::uint64_t connection, std::string_view text) {
    if (impl_) {
        impl_->send_system_chat(connection, text);
    }
}

void server::broadcast_system_chat(std::string_view text) {
    if (impl_) {
        impl_->broadcast_system_chat(text);
    }
}

void server::set_player_game_mode(std::uint64_t connection, game_mode mode) {
    if (impl_) {
        impl_->set_game_mode(connection, mode);
    }
}

auto server::player_names() const -> std::vector<player_name> {
    if (!impl_) {
        return {};
    }
    std::vector<player_name> players;
    for (const auto &entry : impl_->connection_snapshot()) {
        if (entry.handle->closed.load(std::memory_order_acquire)) {
            continue;
        }
        const std::lock_guard lock{entry.handle->mutex};
        if (entry.handle->data.state == connection_state::play) {
            players.push_back(
                {.connection = entry.id, .name = entry.handle->data.name});
        }
    }
    return players;
}

auto server::active_connections() const -> std::vector<std::uint64_t> {
    if (!impl_) {
        return {};
    }
    return impl_->playing_connections();
}

namespace {

constexpr std::uint8_t player_info_add{1U};
constexpr std::uint8_t player_info_game_mode{4U};
constexpr std::uint8_t player_info_listed{8U};
constexpr std::uint8_t player_info_latency{16U};
constexpr std::uint32_t player_entity_type{159U};

} // namespace

void server::impl::set_game_mode(std::uint64_t id, game_mode mode) {
    const auto live{find_connection(id)};
    if (!live) {
        return;
    }
    player_info_entry entry{};
    {
        const std::lock_guard lock{live->mutex};
        auto &data{live->data};
        if (live->closed.load(std::memory_order_relaxed) ||
            data.state != connection_state::play) {
            return;
        }
        data.mode = mode;
        entry.id = data.profile;
        entry.name = data.name;
        entry.game_mode = protocol_game_mode(mode);
        entry.listed = true;
        entry.latency = 0U;
    }
    const auto targets{playing_connections()};
    {
        play_cb::player_abilities abilities{
            .flags = ability_flags(mode),
            .flying_speed = 0.05F,
            .walking_speed = 0.1F,
        };
        send_packet(id, play_cb::player_abilities::packet_id, abilities,
                    play_cb::encode_player_abilities);
    }
    play_cb::player_info_update update{};
    update.payload.actions = player_info_game_mode;
    update.payload.entries.push_back(std::move(entry));
    for (const auto target : targets) {
        send_packet(target, play_cb::player_info_update::packet_id, update,
                    play_cb::encode_player_info_update);
    }
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
    if (!with_connection(id, [challenge](connection_state_data &data) {
            data.keepalive_sent_ms = challenge;
            data.keepalive_challenge = challenge;
            data.keepalive_pending = true;
        })) {
        return;
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
        reject(id, "malformed or unexpected packet");
        return;
    }
    switch (*packet_id) {
    case play_sb::accept_teleportation::packet_id: {
        play_sb::accept_teleportation packet{};
        if (auto decoded{play_sb::decode_accept_teleportation(in, packet)};
            !decoded) {
            reject(id, "malformed or unexpected packet");
            return;
        }
        if (packet.id != 1) {
            return;
        }
        bool on_ground{false};
        {
            const auto live{find_connection(id)};
            if (!live) {
                return;
            }
            const std::lock_guard lock{live->mutex};
            auto &data{live->data};
            if (live->closed.load(std::memory_order_relaxed)) {
                return;
            }
            on_ground = data.on_ground;
            data.awaiting_teleport = false;
            data.x = packet.x;
            data.y = packet.y;
            data.z = packet.z;
            data.yaw = packet.y_rot;
            data.pitch = packet.x_rot;
        }
        if (movement_hook) {
            movement_hook(movement_event{.connection = id,
                                         .x = packet.x,
                                         .y = packet.y,
                                         .z = packet.z,
                                         .yaw = packet.y_rot,
                                         .pitch = packet.x_rot,
                                         .on_ground = on_ground});
        }
        return;
    }
    case play_sb::keep_alive::packet_id: {
        play_sb::keep_alive packet{};
        if (auto decoded{play_sb::decode_keep_alive(in, packet)}; !decoded) {
            reject(id, "malformed or unexpected packet");
            return;
        }
        if (!with_connection(id, [&packet](connection_state_data &data) {
                if (data.keepalive_pending &&
                    data.keepalive_challenge == packet.id) {
                    data.keepalive_pending = false;
                }
            })) {
            return;
        }
        return;
    }
    case play_sb::move_player_pos::packet_id: {
        play_sb::move_player_pos packet{};
        if (auto decoded{play_sb::decode_move_player_pos(in, packet)};
            !decoded) {
            reject(id, "malformed or unexpected packet");
            return;
        }
        float yaw{0.0F};
        float pitch{0.0F};
        {
            const auto live{find_connection(id)};
            if (!live) {
                return;
            }
            const std::lock_guard lock{live->mutex};
            auto &data{live->data};
            if (live->closed.load(std::memory_order_relaxed)) {
                return;
            }
            data.x = packet.x;
            data.y = packet.y;
            data.z = packet.z;
            data.on_ground = (packet.flags & 1U) != 0;
            yaw = data.yaw;
            pitch = data.pitch;
        }
        if (movement_hook) {
            movement_hook(
                movement_event{.connection = id,
                               .x = packet.x,
                               .y = packet.y,
                               .z = packet.z,
                               .yaw = yaw,
                               .pitch = pitch,
                               .on_ground = (packet.flags & 1U) != 0});
        }
        return;
    }
    case play_sb::move_player_pos_rot::packet_id: {
        play_sb::move_player_pos_rot packet{};
        if (auto decoded{play_sb::decode_move_player_pos_rot(in, packet)};
            !decoded) {
            reject(id, "malformed or unexpected packet");
            return;
        }
        if (!with_connection(id, [&packet](connection_state_data &data) {
                data.x = packet.x;
                data.y = packet.y;
                data.z = packet.z;
                data.yaw = packet.y_rot;
                data.pitch = packet.x_rot;
                data.on_ground = (packet.flags & 1U) != 0;
            })) {
            return;
        }
        if (movement_hook) {
            movement_hook(
                movement_event{.connection = id,
                               .x = packet.x,
                               .y = packet.y,
                               .z = packet.z,
                               .yaw = packet.y_rot,
                               .pitch = packet.x_rot,
                               .on_ground = (packet.flags & 1U) != 0});
        }
        return;
    }
    case play_sb::move_player_rot::packet_id: {
        play_sb::move_player_rot packet{};
        if (auto decoded{play_sb::decode_move_player_rot(in, packet)};
            !decoded) {
            reject(id, "malformed or unexpected packet");
            return;
        }
        movement_event event{};
        {
            const auto live{find_connection(id)};
            if (!live) {
                return;
            }
            const std::lock_guard lock{live->mutex};
            auto &data{live->data};
            if (live->closed.load(std::memory_order_relaxed)) {
                return;
            }
            data.yaw = packet.y_rot;
            data.pitch = packet.x_rot;
            data.on_ground = (packet.flags & 1U) != 0;
            event = movement_event{.connection = id,
                                   .x = data.x,
                                   .y = data.y,
                                   .z = data.z,
                                   .yaw = data.yaw,
                                   .pitch = data.pitch,
                                   .on_ground = data.on_ground};
        }
        if (movement_hook) {
            movement_hook(event);
        }
        return;
    }
    case play_sb::move_player_status_only::packet_id: {
        play_sb::move_player_status_only packet{};
        if (auto decoded{play_sb::decode_move_player_status_only(in, packet)};
            !decoded) {
            reject(id, "malformed or unexpected packet");
            return;
        }
        movement_event event{};
        {
            const auto live{find_connection(id)};
            if (!live) {
                return;
            }
            const std::lock_guard lock{live->mutex};
            auto &data{live->data};
            if (live->closed.load(std::memory_order_relaxed)) {
                return;
            }
            data.on_ground = (packet.flags & 1U) != 0;
            event = movement_event{.connection = id,
                                   .x = data.x,
                                   .y = data.y,
                                   .z = data.z,
                                   .yaw = data.yaw,
                                   .pitch = data.pitch,
                                   .on_ground = data.on_ground};
        }
        if (movement_hook) {
            movement_hook(event);
        }
        return;
    }
    case play_sb::player_action::packet_id: {
        play_sb::player_action packet{};
        if (auto decoded{play_sb::decode_player_action(in, packet)}; !decoded) {
            reject(id, "malformed or unexpected packet");
            return;
        }
        const auto [x, y, z] = unpack_block_position(packet.position);
        // Only START_DESTROY_BLOCK changes the world; the other actions
        // stop or abort a prediction that the client resolves locally.
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
            reject(id, "malformed or unexpected packet");
            return;
        }
        auto [x, y, z] = unpack_block_position(packet.position);
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
                .position = pack_block_position(x, y, z),
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
            reject(id, "malformed or unexpected packet");
            return;
        }
        if (packet.message.empty()) {
            return;
        }
        std::string name;
        if (!with_connection(id, [&name](connection_state_data &data) {
                name = data.name;
            })) {
            return;
        }
        broadcast_system_chat(fmt::format("<{}> {}", name, packet.message));
        return;
    }
    case play_sb::chat_command::packet_id: {
        play_sb::chat_command packet{};
        if (auto decoded{play_sb::decode_chat_command(in, packet)}; !decoded) {
            reject(id, "malformed or unexpected packet");
            return;
        }
        execute_chat_command(id, packet.command);
        return;
    }
    case play_sb::chat_command_signed::packet_id: {
        play_sb::chat_command_signed packet{};
        if (auto decoded{play_sb::decode_chat_command_signed(in, packet)};
            !decoded) {
            reject(id, "malformed or unexpected packet");
            return;
        }
        execute_chat_command(id, packet.command);
        return;
    }
    case play_sb::command_suggestion::packet_id: {
        play_sb::command_suggestion packet{};
        if (auto decoded{play_sb::decode_command_suggestion(in, packet)};
            !decoded) {
            reject(id, "malformed or unexpected packet");
            return;
        }
        if (!commands.suggest) {
            return;
        }
        std::string name;
        uuid profile{};
        if (!with_connection(id,
                             [&name, &profile](connection_state_data &data) {
                                 name = data.name;
                                 profile = data.profile;
                             })) {
            return;
        }
        const player_identity identity{
            .connection = id,
            .id = profile,
            .name = name,
            .world = {},
        };
        auto result{
            commands.suggest(identity, packet.command,
                             static_cast<std::int32_t>(packet.command.size()))};
        if (result.entries.size() > 1024U) {
            result.entries.resize(1024U);
        }
        play_cb::command_suggestions response{};
        response.id = packet.id;
        response.start = result.start;
        response.length = result.length;
        response.suggestions.reserve(result.entries.size());
        for (const auto &entry : result.entries) {
            refrain::protocol::current::wire::wire_entry suggestion{};
            suggestion.text = std::string{entry.text};
            if (!entry.tooltip.empty()) {
                suggestion.tooltip =
                    text_component{.text = std::string{entry.tooltip}};
            }
            response.suggestions.push_back(std::move(suggestion));
        }
        send_packet(id, play_cb::command_suggestions::packet_id, response,
                    play_cb::encode_command_suggestions);
        return;
    }
    case play_sb::player_loaded::packet_id: {
        play_sb::player_loaded packet{};
        if (auto decoded{play_sb::decode_player_loaded(in, packet)}; !decoded) {
            reject(id, "malformed or unexpected packet");
            return;
        }
        if (!with_connection(id, [](connection_state_data &data) {
                data.client_loaded = true;
            })) {
            return;
        }
        return;
    }
    case play_sb::chunk_batch_received::packet_id: {
        play_sb::chunk_batch_received packet{};
        if (auto decoded{play_sb::decode_chunk_batch_received(in, packet)};
            !decoded) {
            reject(id, "malformed or unexpected packet");
            return;
        }
        static_cast<void>(packet);
        return;
    }
    case play_sb::set_creative_mode_slot::packet_id: {
        play_sb::set_creative_mode_slot packet{};
        if (auto decoded{play_sb::decode_set_creative_mode_slot(in, packet)};
            !decoded) {
            reject(id, "malformed or unexpected packet");
            return;
        }
        const std::lock_guard lock{click_mutex};
        creative_slots.push_back(creative_slot_event{
            .connection = id,
            .slot = packet.slot_num,
            .item_id = packet.item_stack.item_id,
            .count = packet.item_stack.count,
        });
        return;
    }
    case play_sb::client_command::packet_id: {
        play_sb::client_command packet{};
        if (auto decoded{play_sb::decode_client_command(in, packet)};
            !decoded) {
            reject(id, "malformed or unexpected packet");
            return;
        }
        if (packet.action != 0) {
            return;
        }
        game_mode mode{game_mode::creative};
        {
            const auto live{find_connection(id)};
            if (!live) {
                return;
            }
            const std::lock_guard lock{live->mutex};
            auto &data{live->data};
            if (live->closed.load(std::memory_order_relaxed)) {
                return;
            }
            data.x = config.spawn_x;
            data.y = config.spawn_y;
            data.z = config.spawn_z;
            mode = data.mode;
        }
        play_cb::respawn respawn{};
        respawn.common_player_spawn_info.dimension_type = 0;
        respawn.common_player_spawn_info.dimension = overworld();
        respawn.common_player_spawn_info.seed = config.seed;
        respawn.common_player_spawn_info.game_type =
            static_cast<std::uint32_t>(mode);
        respawn.common_player_spawn_info.previous_game_type = std::nullopt;
        respawn.common_player_spawn_info.is_debug = false;
        respawn.common_player_spawn_info.is_flat = false;
        respawn.common_player_spawn_info.last_death_location = std::nullopt;
        respawn.common_player_spawn_info.portal_cooldown = 0;
        respawn.common_player_spawn_info.sea_level = 63;
        respawn.data_to_keep = 0;
        send_packet(id, play_cb::respawn::packet_id, respawn,
                    play_cb::encode_respawn);
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
        if (respawn_hook) {
            respawn_hook(id);
        }
        return;
    }
    case play_sb::container_close::packet_id: {
        play_sb::container_close packet{};
        if (auto decoded{play_sb::decode_container_close(in, packet)};
            !decoded) {
            reject(id, "malformed or unexpected packet");
            return;
        }
        const std::lock_guard lock{click_mutex};
        container_closes.push_back(container_close_event{
            .connection = id,
            .container_id = packet.container_id,
        });
        return;
    }
    case play_sb::container_click::packet_id: {
        play_sb::container_click packet{};
        if (auto decoded{play_sb::decode_container_click(in, packet)};
            !decoded) {
            reject(id, "malformed or unexpected packet");
            return;
        }
        // TODO: clicks for every menu except the player inventory are
        // dropped; block-entity containers need a menu registry first.
        if (packet.payload.container_id != 0) {
            return;
        }
        const std::lock_guard lock{click_mutex};
        container_clicks.push_back(container_click_event{
            .connection = id,
            .container_id = packet.payload.container_id,
            .state_id = packet.payload.state_id,
            .slot = packet.payload.slot_num,
            .button = packet.payload.button_num,
            .mode = packet.payload.container_input,
        });
        return;
    }
    case play_sb::set_carried_item::packet_id: {
        play_sb::set_carried_item packet{};
        if (auto decoded{play_sb::decode_set_carried_item(in, packet)};
            !decoded) {
            reject(id, "malformed or unexpected packet");
            return;
        }
        if (!with_connection(id, [&packet](connection_state_data &data) {
                data.held_slot = static_cast<std::uint32_t>(packet.slot);
            })) {
            return;
        }
        return;
    }
    case play_sb::client_information::packet_id: {
        play_sb::client_information packet{};
        if (auto decoded{play_sb::decode_client_information(in, packet)};
            !decoded) {
            reject(id, "malformed or unexpected packet");
            return;
        }
        const auto live{find_connection(id)};
        if (!live || live->closed.load(std::memory_order_acquire)) {
            return;
        }
        const auto distance =
            static_cast<std::int32_t>(packet.information.view_distance);
        if (view_distance_hook) {
            view_distance_hook(
                view_distance_event{.connection = id, .distance = distance});
        }
        return;
    }
    case play_sb::punch::packet_id: {
        play_sb::punch packet{};
        if (auto decoded{play_sb::decode_punch(in, packet)}; !decoded) {
            reject(id, "malformed or unexpected packet");
            return;
        }
        if (animation_hook) {
            animation_hook(id, 0);
        }
        return;
    }
    case play_sb::interact::packet_id: {
        play_sb::interact packet{};
        if (auto decoded{play_sb::decode_interact(in, packet)}; !decoded) {
            reject(id, "malformed or unexpected packet");
            return;
        }
        if (animation_hook) {
            animation_hook(id, 0);
        }
        return;
    }
    case play_sb::attack::packet_id: {
        play_sb::attack packet{};
        if (auto decoded{play_sb::decode_attack(in, packet)}; !decoded) {
            reject(id, "malformed or unexpected packet");
            return;
        }
        if (attack_hook) {
            attack_hook(attack_event{.connection = id,
                                     .target_entity = packet.entity_id});
        }
        return;
    }
    case play_sb::ping_request::packet_id: {
        play_sb::ping_request packet{};
        if (auto decoded{play_sb::decode_ping_request(in, packet)}; !decoded) {
            reject(id, "malformed or unexpected packet");
            return;
        }
        play_cb::pong_response pong{.time = packet.time};
        send_packet(id, play_cb::pong_response::packet_id, pong,
                    play_cb::encode_pong_response);
        return;
    }
    // Parse-only packets: these carry client presentation/acknowledgement
    // state that the server does not need yet. Decoding still validates frame
    // bounds and field types before the packet is ignored.
    case play_sb::player_abilities::packet_id: {
        play_sb::player_abilities packet{};
        if (auto decoded{play_sb::decode_player_abilities(in, packet)};
            !decoded) {
            reject(id, "malformed or unexpected packet");
        }
        return;
    }
    case play_sb::player_command::packet_id: {
        play_sb::player_command packet{};
        if (auto decoded{play_sb::decode_player_command(in, packet)};
            !decoded) {
            reject(id, "malformed or unexpected packet");
        }
        return;
    }
    case play_sb::player_input::packet_id: {
        play_sb::player_input packet{};
        if (auto decoded{play_sb::decode_player_input(in, packet)}; !decoded) {
            reject(id, "malformed or unexpected packet");
        }
        return;
    }
    case play_sb::bundle_item_selected::packet_id: {
        play_sb::bundle_item_selected packet{};
        if (auto decoded{play_sb::decode_bundle_item_selected(in, packet)};
            !decoded) {
            reject(id, "malformed or unexpected packet");
        }
        return;
    }
    case play_sb::client_tick_end::packet_id: {
        play_sb::client_tick_end packet{};
        if (auto decoded{play_sb::decode_client_tick_end(in, packet)};
            !decoded) {
            reject(id, "malformed or unexpected packet");
        }
        return;
    }
    case play_sb::pong::packet_id: {
        play_sb::pong packet{};
        if (auto decoded{play_sb::decode_pong(in, packet)}; !decoded) {
            reject(id, "malformed or unexpected packet");
        }
        return;
    }
    case play_sb::chat_ack::packet_id: {
        play_sb::chat_ack packet{};
        if (auto decoded{play_sb::decode_chat_ack(in, packet)}; !decoded) {
            reject(id, "malformed or unexpected packet");
        }
        return;
    }
    case play_sb::recipe_book_change_settings::packet_id: {
        play_sb::recipe_book_change_settings packet{};
        if (auto decoded{
                play_sb::decode_recipe_book_change_settings(in, packet)};
            !decoded) {
            reject(id, "malformed or unexpected packet");
        }
        return;
    }
    case play_sb::recipe_book_seen_recipe::packet_id: {
        play_sb::recipe_book_seen_recipe packet{};
        if (auto decoded{play_sb::decode_recipe_book_seen_recipe(in, packet)};
            !decoded) {
            reject(id, "malformed or unexpected packet");
        }
        return;
    }
    case play_sb::seen_advancements::packet_id: {
        play_sb::seen_advancements packet{};
        if (auto decoded{play_sb::decode_seen_advancements(in, packet)};
            !decoded) {
            reject(id, "malformed or unexpected packet");
        }
        return;
    }
    case play_sb::container_button_click::packet_id: {
        play_sb::container_button_click packet{};
        if (auto decoded{play_sb::decode_container_button_click(in, packet)};
            !decoded) {
            reject(id, "malformed or unexpected packet");
        }
        return;
    }
    case play_sb::container_slot_state_changed::packet_id: {
        play_sb::container_slot_state_changed packet{};
        if (auto decoded{
                play_sb::decode_container_slot_state_changed(in, packet)};
            !decoded) {
            reject(id, "malformed or unexpected packet");
        }
        return;
    }
    case play_sb::block_entity_tag_query::packet_id:
        parse_only<play_sb::block_entity_tag_query>(
            id, in, play_sb::decode_block_entity_tag_query);
        return;
    case play_sb::change_difficulty::packet_id:
        parse_only<play_sb::change_difficulty>(
            id, in, play_sb::decode_change_difficulty);
        return;
    case play_sb::change_game_mode::packet_id:
        parse_only<play_sb::change_game_mode>(id, in,
                                              play_sb::decode_change_game_mode);
        return;
    case play_sb::chat_session_update::packet_id:
        parse_only<play_sb::chat_session_update>(
            id, in, play_sb::decode_chat_session_update);
        return;
    case play_sb::configuration_acknowledged::packet_id:
        parse_only<play_sb::configuration_acknowledged>(
            id, in, play_sb::decode_configuration_acknowledged);
        return;
    case play_sb::cookie_response::packet_id:
        parse_only<play_sb::cookie_response>(id, in,
                                             play_sb::decode_cookie_response);
        return;
    case play_sb::custom_payload::packet_id:
        parse_only<play_sb::custom_payload>(id, in,
                                            play_sb::decode_custom_payload);
        return;
    case play_sb::debug_subscription_request::packet_id:
        parse_only<play_sb::debug_subscription_request>(
            id, in, play_sb::decode_debug_subscription_request);
        return;
    case play_sb::edit_book::packet_id:
        parse_only<play_sb::edit_book>(id, in, play_sb::decode_edit_book);
        return;
    case play_sb::entity_tag_query::packet_id:
        parse_only<play_sb::entity_tag_query>(id, in,
                                              play_sb::decode_entity_tag_query);
        return;
    case play_sb::jigsaw_generate::packet_id:
        parse_only<play_sb::jigsaw_generate>(id, in,
                                             play_sb::decode_jigsaw_generate);
        return;
    case play_sb::lock_difficulty::packet_id:
        parse_only<play_sb::lock_difficulty>(id, in,
                                             play_sb::decode_lock_difficulty);
        return;
    case play_sb::move_vehicle::packet_id:
        parse_only<play_sb::move_vehicle>(id, in, play_sb::decode_move_vehicle);
        return;
    case play_sb::paddle_boat::packet_id:
        parse_only<play_sb::paddle_boat>(id, in, play_sb::decode_paddle_boat);
        return;
    case play_sb::pick_item_from_block::packet_id:
        parse_only<play_sb::pick_item_from_block>(
            id, in, play_sb::decode_pick_item_from_block);
        return;
    case play_sb::pick_item_from_entity::packet_id:
        parse_only<play_sb::pick_item_from_entity>(
            id, in, play_sb::decode_pick_item_from_entity);
        return;
    case play_sb::place_recipe::packet_id:
        parse_only<play_sb::place_recipe>(id, in, play_sb::decode_place_recipe);
        return;
    case play_sb::rename_item::packet_id:
        parse_only<play_sb::rename_item>(id, in, play_sb::decode_rename_item);
        return;
    case play_sb::resource_pack::packet_id:
        parse_only<play_sb::resource_pack>(id, in,
                                           play_sb::decode_resource_pack);
        return;
    case play_sb::select_trade::packet_id:
        parse_only<play_sb::select_trade>(id, in, play_sb::decode_select_trade);
        return;
    case play_sb::set_beacon::packet_id:
        parse_only<play_sb::set_beacon>(id, in, play_sb::decode_set_beacon);
        return;
    case play_sb::set_command_block::packet_id:
        parse_only<play_sb::set_command_block>(
            id, in, play_sb::decode_set_command_block);
        return;
    case play_sb::set_command_minecart::packet_id:
        parse_only<play_sb::set_command_minecart>(
            id, in, play_sb::decode_set_command_minecart);
        return;
    case play_sb::set_game_rule::packet_id:
        parse_only<play_sb::set_game_rule>(id, in,
                                           play_sb::decode_set_game_rule);
        return;
    case play_sb::set_jigsaw_block::packet_id:
        parse_only<play_sb::set_jigsaw_block>(id, in,
                                              play_sb::decode_set_jigsaw_block);
        return;
    case play_sb::set_structure_block::packet_id:
        parse_only<play_sb::set_structure_block>(
            id, in, play_sb::decode_set_structure_block);
        return;
    case play_sb::set_test_block::packet_id:
        parse_only<play_sb::set_test_block>(id, in,
                                            play_sb::decode_set_test_block);
        return;
    case play_sb::sign_update::packet_id:
        parse_only<play_sb::sign_update>(id, in, play_sb::decode_sign_update);
        return;
    case play_sb::spectator_action::packet_id:
        parse_only<play_sb::spectator_action>(id, in,
                                              play_sb::decode_spectator_action);
        return;
    case play_sb::teleport_to_entity::packet_id:
        parse_only<play_sb::teleport_to_entity>(
            id, in, play_sb::decode_teleport_to_entity);
        return;
    case play_sb::test_instance_block_action::packet_id:
        parse_only<play_sb::test_instance_block_action>(
            id, in, play_sb::decode_test_instance_block_action);
        return;
    case play_sb::use_item::packet_id:
        parse_only<play_sb::use_item>(id, in, play_sb::decode_use_item);
        return;
    case play_sb::custom_click_action::packet_id:
        parse_only<play_sb::custom_click_action>(
            id, in, play_sb::decode_custom_click_action);
        return;
    default:
        reject(id, "unknown play packet");
        return;
    }
}

void server::impl::send(std::uint64_t id, std::span<const std::byte> packet) {
    reader in{packet};
    const auto packet_id{in.read_varint()};
    if (!packet_id) {
        return;
    }
    send(id, static_cast<std::uint16_t>(*packet_id),
         packet.subspan(in.position()));
}

void server::impl::send(std::uint64_t id, std::uint16_t packet_id,
                        std::span<const std::byte> payload) {
    const auto live{find_connection(id)};
    if (!live || live->closed.load(std::memory_order_acquire)) {
        return;
    }
    // Serialising this connection's codec calls keeps the frame order the
    // callers produced. Sends to other connections use other mutexes, and the
    // table shard lock is not held here.
    const std::lock_guard lock{live->mutex};
    if (live->closed.load(std::memory_order_relaxed)) {
        return;
    }
    packets_out.fetch_add(1, std::memory_order_relaxed);
    bytes_out.fetch_add(payload.size(), std::memory_order_relaxed);
    if (!protocol) {
        return;
    }
    if (auto sent{protocol->send_packet(id, packet_id, payload)}; !sent) {
        return;
    }
    if (socket) {
        std::span<const std::byte> framed{};
        while (protocol->take_outbound(id, framed)) {
            socket->send(id, framed);
        }
    }
}

void server::impl::reject(std::uint64_t id, std::string_view reason) {
    log_net.warn("protocol error id={} reason={}", id, reason);
    close(id);
}

void server::impl::close(std::uint64_t id) {
    const auto live{find_connection(id)};
    if (!live || live->closed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    std::string name;
    connection_state state{connection_state::handshake};
    std::uint64_t leave_id{0};
    uuid leave_profile{};
    bool notify_leave{false};
    {
        const std::lock_guard lock{live->mutex};
        auto &data{live->data};
        name = data.name;
        state = data.state;
        if (state == connection_state::play && !name.empty() &&
            !data.leave_reported) {
            data.leave_reported = true;
            leave_id = id;
            leave_profile = data.profile;
            notify_leave = true;
        }
    }
    if (notify_leave && player_leave_hook) {
        player_leave_hook(player_identity{
            .connection = leave_id,
            .id = leave_profile,
            .name = name,
            .world = {},
        });
    }
    if (name.empty()) {
        log_net.debug("connection closed id={} state={}", id, to_string(state));
    } else {
        log_net.info("player left id={} name={}", id, name);
    }
    protocol->close_connection(id);
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
            for (const auto &entry : connection_snapshot()) {
                const std::lock_guard lock{entry.handle->mutex};
                auto &data{entry.handle->data};
                if (entry.handle->closed.load(std::memory_order_relaxed) ||
                    data.state != connection_state::play) {
                    continue;
                }
                if (now - data.keepalive_sent_ms < interval_ms) {
                    continue;
                }
                if (data.keepalive_pending) {
                    expired.push_back(entry.id);
                } else {
                    due.push_back(entry.id);
                }
            }
        }
        for (const auto id : expired) {
            log_net.warn("keepalive timeout id={}", id);
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
    if (config.idle_timeout_seconds > 0) {
        const auto now{std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count()};
        std::vector<std::uint64_t> idle;
        for (const auto &entry : connection_snapshot()) {
            const std::lock_guard lock{entry.handle->mutex};
            const auto &data{entry.handle->data};
            if (entry.handle->closed.load(std::memory_order_relaxed) ||
                data.state != connection_state::play) {
                continue;
            }
            if (idle_timed_out(
                    data.last_packet_ns, now,
                    static_cast<std::int64_t>(config.idle_timeout_seconds) *
                        1'000'000'000LL)) {
                idle.push_back(entry.id);
            }
        }
        for (const auto id : idle) {
            log_net.info("player idle timeout id={}", id);
            close(id);
        }
    }
}

server::server(std::unique_ptr<impl> state) : impl_{std::move(state)} {}
server::~server() { stop(); }

auto server::start(server_inputs inputs, core::config::view view)
    -> result<std::unique_ptr<server>> {
    const auto int_or = [&view](std::string_view key, std::int64_t fallback) {
        return view.contains(key) ? view.get_int64(key, fallback) : fallback;
    };
    const auto bool_or = [&view](std::string_view key, bool fallback) {
        return view.contains(key) ? view.get_bool(key, fallback) : fallback;
    };
    const auto port{int_or("port", inputs.fallback_port.value_or(25565))};
    if (port < 0 || port > 65535) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "network.port is out of range")};
    }
    const auto compression{int_or("compression_threshold",
                                  inputs.fallback_compression.value_or(256))};
    if (compression < -1 || compression > 2147483647LL) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "network.compression_threshold is out of range")};
    }
    server_settings config{};
    config.bind.host = view.contains("bind_host")
                           ? view.get_string("bind_host", "0.0.0.0")
                           : inputs.fallback_bind_host.value_or("0.0.0.0");
    config.bind.port = static_cast<std::uint16_t>(port);
    config.compression_threshold = static_cast<int>(compression);
    config.view_distance = static_cast<std::int32_t>(
        int_or("view_distance", inputs.fallback_view_distance.value_or(10)));
    config.simulation_distance = static_cast<std::int32_t>(
        int_or("simulation_distance",
               inputs.fallback_simulation_distance.value_or(10)));
    config.online_mode =
        bool_or("online_mode", inputs.fallback_online_mode.value_or(false));
    config.status.description =
        view.contains("status.motd")
            ? view.get_string("status.motd", "A Refrain Server")
            : inputs.fallback_motd.value_or("A Refrain Server");
    config.status.max_players = static_cast<std::int32_t>(
        int_or("status.max_players", inputs.fallback_max_players.value_or(20)));
    config.status.online_players = 0;
    config.status.replies_to_status = view.get_bool("status.enabled", true);
    config.status.enforces_secure_chat = bool_or(
        "enforce_secure_profile", inputs.fallback_secure_chat.value_or(true));
    config.session_server =
        view.get_string("session_server", "https://sessionserver.mojang.com");
    config.auth_timeout_ms =
        static_cast<std::int32_t>(view.get_int64("auth_timeout_ms", 5000));
    config.spawn_x = inputs.spawn_x;
    config.spawn_y = inputs.spawn_y;
    config.spawn_z = inputs.spawn_z;
    config.seed = inputs.seed;
    config.registry_catalog_path = std::move(inputs.registry_catalog_path);
    config.spawn_chunks = std::move(inputs.spawn_chunks);
    config.break_block = std::move(inputs.break_block);
    config.place_block = std::move(inputs.place_block);

    if (!inputs.protocol) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "network server requires a protocol codec")};
    }
    auto state{std::make_unique<impl>()};
    state->policy = std::move(inputs.policy);
    state->join_state = std::move(inputs.join_state);
    state->commands = std::move(inputs.commands);
    state->status_hook = std::move(inputs.status);
    state->tab_hook = std::move(inputs.tab);
    state->tab_encoder_hook = std::move(inputs.encode_tab);
    state->player_join_hook = std::move(inputs.player_join);
    state->player_leave_hook = std::move(inputs.player_leave);
    state->movement_hook = std::move(inputs.movement);
    state->attack_hook = std::move(inputs.attack);
    state->animation_hook = std::move(inputs.animation);
    state->play_ready_hook = std::move(inputs.play_ready);
    state->respawn_hook = std::move(inputs.respawn);
    state->view_distance_hook = std::move(inputs.view_distance);
    state->make_transport = std::move(inputs.make_transport);
    state->protocol = std::move(inputs.protocol);
    state->verify_session = std::move(inputs.verify_session);
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
