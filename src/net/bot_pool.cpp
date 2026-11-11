// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/log.hpp>
#include <refrain/net/bot_pool.hpp>

#include <refrain/net/base.hpp>
#include <refrain/net/frame.hpp>
#include <refrain/net/wire.hpp>
#include <refrain/protocol/current.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <numbers>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>

namespace refrain::net {

namespace {

namespace common = ::refrain::protocol::current::common;
namespace hs_sb = ::refrain::protocol::current::handshake::serverbound;
namespace login_sb = ::refrain::protocol::current::login::serverbound;
namespace login_cb = ::refrain::protocol::current::login::clientbound;
namespace config_sb = ::refrain::protocol::current::configuration::serverbound;
namespace config_cb = ::refrain::protocol::current::configuration::clientbound;
namespace play_sb = ::refrain::protocol::current::play::serverbound;
namespace play_cb = ::refrain::protocol::current::play::clientbound;

using clock_type = std::chrono::steady_clock;

/// One shared transport serves the whole pool; four threads keep the socket
/// work off the connection dispatch path while staying far below the
/// per-bot thread cost.
constexpr std::size_t pool_io_threads = 4;
/// Failure messages are diagnostics, not a log; the first few are enough.
constexpr std::size_t max_reported_errors = 16;
/// Bytes buffered for one connection whose state object is not registered yet.
constexpr std::size_t max_pending_bytes = 1U * 1024U * 1024U;
/// Bots are laid out on a square grid so their walk origins do not overlap.
constexpr std::size_t walk_grid_width = 32;
constexpr double walk_grid_spacing = 16.0;
constexpr double walk_angle_step = 0.08;
constexpr auto max_timer_tick = std::chrono::milliseconds{50};

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
        low = (low << 8U) | value.bytes[index + 8U];
    }
    return {static_cast<std::int64_t>(high), static_cast<std::int64_t>(low)};
}

enum class bot_phase : std::uint8_t { login, configuration, play };

class bot_pool;

/// One bot's wire state. A slot is touched by the transport I/O thread that
/// owns its connection, by the single timer thread and by the thread that
/// registers the connection. Fields shared between them are guarded by
/// `mutex_`; `encoder_`/`decoder_`/`phase_` are owned by the connection's
/// serialized callback chain.
class bot_slot {
  public:
    bot_slot(bot_pool &owner, std::size_t index,
             const bot_pool_options &options);

    /// Start the join deadline at the connect attempt.
    void begin(clock_type::time_point now);
    /// Report a connection that could not even be started.
    void fail(std::string message);
    void on_open(std::uint64_t connection);
    void on_data(std::span<const std::byte> bytes);
    void on_close(std::string_view reason);
    /// Check the join deadline and send one walk step when it is due.
    void tick(clock_type::time_point now,
              std::chrono::milliseconds walk_period);

    [[nodiscard]] auto finished() const -> bool;
    [[nodiscard]] auto joined() const -> bool;
    [[nodiscard]] auto join_latency() const -> std::chrono::milliseconds;
    [[nodiscard]] auto error() const -> std::string;

  private:
    void send_intention();
    void send_hello();
    void send_client_information();
    void send_move();
    void handle(std::span<const std::byte> body);
    void handle_login(std::uint32_t packet_id, reader &in);
    void handle_configuration(std::uint32_t packet_id, reader &in);
    void handle_play(std::uint32_t packet_id, reader &in);
    void set_joined(clock_type::time_point now);
    void finish(std::string message, bool failed);
    void close_connection();

    template <typename Packet, typename Encode>
    void send_packet(std::uint16_t packet_id, const Packet &packet,
                     Encode encode);

    bot_pool *owner_;
    std::string username_{};
    uuid profile_{};
    endpoint target_{};
    std::uint8_t view_distance_{8};
    std::chrono::milliseconds join_timeout_{0};
    std::chrono::milliseconds linger_{0};
    double walk_radius_{0.0};
    double walk_origin_x_{0.0};
    double walk_origin_z_{0.0};

    std::atomic<std::uint64_t> connection_{0};
    frame_encoder encoder_{-1};
    frame_decoder decoder_{-1};
    std::vector<std::byte> body_{};
    bot_phase phase_{bot_phase::login};

    mutable std::mutex mutex_{};
    std::mutex encode_mutex_{};
    bool finished_{false};
    bool joined_{false};
    std::string error_{};
    clock_type::time_point connect_started_{};
    clock_type::time_point join_deadline_{};
    clock_type::time_point linger_deadline_{};
    clock_type::time_point next_walk_{};
    std::chrono::milliseconds join_latency_{0};
    double spawn_y_{64.0};
    double walk_angle_{0.0};
};

/// Events that arrive before the connection's state object is registered.
struct pending_event {
    enum class kind : std::uint8_t { opened, data, closed };

    kind type{kind::opened};
    std::vector<std::byte> data{};
    std::string message{};
};

class bot_pool {
  public:
    explicit bot_pool(const bot_pool_options &options);
    ~bot_pool();
    bot_pool(const bot_pool &) = delete;
    auto operator=(const bot_pool &) -> bot_pool & = delete;

    auto run() -> bot_pool_report;

    void send(std::uint64_t connection, std::span<const std::byte> frame);
    void close(std::uint64_t connection);
    void count_packet();
    void count_chunk();
    void slot_finished();

  private:
    void attach(std::uint64_t connection, bot_slot &slot);
    void replay(bot_slot &slot, std::uint64_t connection, pending_event &event);
    void queue_pending(std::uint64_t connection, pending_event event);
    void dispatch_open(std::uint64_t connection);
    void dispatch_data(std::uint64_t connection,
                       std::span<const std::byte> bytes);
    void dispatch_close(std::uint64_t connection, std::string_view reason);
    void timer_loop();
    [[nodiscard]] auto take_report() -> bot_pool_report;

    bot_pool_options options_;
    std::unique_ptr<transport> transport_{};
    std::vector<std::unique_ptr<bot_slot>> slots_{};

    std::mutex map_mutex_{};
    std::unordered_map<std::uint64_t, bot_slot *> slots_by_id_{};
    std::unordered_map<std::uint64_t, std::vector<pending_event>>
        pending_events_{};
    std::unordered_map<std::uint64_t, std::size_t> pending_bytes_{};

    std::atomic<std::uint64_t> packets_{0};
    std::atomic<std::uint64_t> chunks_{0};

    std::mutex wait_mutex_{};
    std::condition_variable wait_cv_{};
    std::atomic<std::size_t> remaining_{0};

    std::mutex timer_mutex_{};
    std::condition_variable timer_cv_{};
    bool timer_stop_{false};
    std::thread timer_{};
};

template <typename Packet, typename Encode>
void bot_slot::send_packet(std::uint16_t packet_id, const Packet &packet,
                           Encode encode) {
    std::vector<std::byte> body;
    writer out{body};
    out.write_varint(packet_id);
    if (auto written{encode(out, packet)}; !written) {
        finish(written.error().message, true);
        return;
    }
    std::vector<std::byte> frame;
    status framed{status_ok()};
    {
        const std::lock_guard lock{encode_mutex_};
        framed = encoder_.encode(body, frame);
    }
    if (!framed) {
        finish(framed.error().message, true);
        return;
    }
    owner_->send(connection_.load(std::memory_order_relaxed), frame);
}

bot_slot::bot_slot(bot_pool &owner, std::size_t index,
                   const bot_pool_options &options)
    : owner_{&owner},
      username_{options.username_prefix + std::to_string(index)},
      profile_{random_uuid()}, target_{options.target},
      view_distance_{options.view_distance}, join_timeout_{options.timeout},
      linger_{options.duration}, walk_radius_{options.walk_radius},
      walk_origin_x_{static_cast<double>(index % walk_grid_width) *
                     walk_grid_spacing},
      walk_origin_z_{static_cast<double>(index / walk_grid_width) *
                     walk_grid_spacing},
      walk_angle_{static_cast<double>(index) * 0.37} {}

void bot_slot::begin(clock_type::time_point now) {
    const std::lock_guard lock{mutex_};
    connect_started_ = now;
    join_deadline_ = now + join_timeout_;
}

void bot_slot::fail(std::string message) { finish(std::move(message), true); }

void bot_slot::on_open(std::uint64_t connection) {
    connection_.store(connection, std::memory_order_relaxed);
    if (finished()) {
        close_connection();
        return;
    }
    send_intention();
    send_hello();
}

void bot_slot::on_data(std::span<const std::byte> bytes) {
    if (finished()) {
        return;
    }
    if (auto pushed{decoder_.push(bytes)}; !pushed) {
        finish(pushed.error().message, true);
        return;
    }
    for (;;) {
        auto next{decoder_.next(body_)};
        if (!next) {
            finish(next.error().message, true);
            return;
        }
        if (!*next) {
            return;
        }
        owner_->count_packet();
        handle(body_);
        if (finished()) {
            return;
        }
    }
}

void bot_slot::on_close(std::string_view reason) {
    bool first{false};
    {
        const std::lock_guard lock{mutex_};
        if (!finished_) {
            finished_ = true;
            first = true;
            if (!joined_) {
                error_ = fmt::format("connection closed: {}", reason);
            }
        }
    }
    if (first) {
        close_connection();
        owner_->slot_finished();
    }
}

void bot_slot::tick(clock_type::time_point now,
                    std::chrono::milliseconds walk_period) {
    bool walk{false};
    bool close_now{false};
    {
        const std::lock_guard lock{mutex_};
        if (finished_) {
            return;
        }
        if (joined_) {
            if (now >= linger_deadline_) {
                finished_ = true;
                close_now = true;
            } else if (walk_radius_ > 0.0 && now >= next_walk_) {
                walk = true;
                next_walk_ = now + walk_period;
                walk_angle_ += walk_angle_step;
            }
        } else if (now >= join_deadline_) {
            finished_ = true;
            close_now = true;
            error_ = "timed out waiting for play state";
        }
    }
    if (walk) {
        send_move();
    }
    if (close_now) {
        close_connection();
        owner_->slot_finished();
    }
}

auto bot_slot::finished() const -> bool {
    const std::lock_guard lock{mutex_};
    return finished_;
}

auto bot_slot::joined() const -> bool {
    const std::lock_guard lock{mutex_};
    return joined_;
}

auto bot_slot::join_latency() const -> std::chrono::milliseconds {
    const std::lock_guard lock{mutex_};
    return join_latency_;
}

auto bot_slot::error() const -> std::string {
    const std::lock_guard lock{mutex_};
    return error_;
}

void bot_slot::send_intention() {
    hs_sb::intention packet{};
    packet.protocol = 777;
    packet.host = target_.host;
    packet.port = target_.port;
    packet.next = 2;
    send_packet(hs_sb::intention::packet_id, packet, hs_sb::encode_intention);
}

void bot_slot::send_hello() {
    const auto [high, low]{uuid_halves(profile_)};
    login_sb::hello packet{};
    packet.name = username_;
    packet.profile_high = high;
    packet.profile_low = low;
    send_packet(login_sb::hello::packet_id, packet, login_sb::encode_hello);
}

void bot_slot::send_client_information() {
    config_sb::client_information packet{};
    packet.information.language = "en_us";
    packet.information.view_distance = view_distance_;
    packet.information.visibility = common::chat_visibility::full;
    packet.information.chat_colors = true;
    packet.information.model_customization = 0;
    packet.information.main_hand = common::humanoid_arm::right;
    packet.information.text_filtering = false;
    packet.information.allows_listing = true;
    packet.information.particles = common::particle_status::all;
    send_packet(config_sb::client_information::packet_id, packet,
                config_sb::encode_client_information);
}

void bot_slot::send_move() {
    double x{0.0};
    double y{0.0};
    double z{0.0};
    double angle{0.0};
    {
        const std::lock_guard lock{mutex_};
        if (!joined_ || finished_) {
            return;
        }
        x = walk_origin_x_ + walk_radius_ * std::cos(walk_angle_);
        z = walk_origin_z_ + walk_radius_ * std::sin(walk_angle_);
        y = spawn_y_;
        angle = walk_angle_;
    }
    play_sb::move_player_pos_rot packet{};
    packet.x = x;
    packet.y = y;
    packet.z = z;
    packet.y_rot =
        static_cast<float>(angle * 180.0 / std::numbers::pi_v<double>);
    packet.x_rot = 0.0F;
    packet.flags = 1U;
    send_packet(play_sb::move_player_pos_rot::packet_id, packet,
                play_sb::encode_move_player_pos_rot);
}

void bot_slot::handle(std::span<const std::byte> body) {
    reader in{body};
    auto packet_id{in.read_varint()};
    if (!packet_id) {
        finish("malformed packet", true);
        return;
    }
    if (phase_ == bot_phase::login) {
        handle_login(*packet_id, in);
    } else if (phase_ == bot_phase::configuration) {
        handle_configuration(*packet_id, in);
    } else {
        handle_play(*packet_id, in);
    }
}

void bot_slot::handle_login(std::uint32_t packet_id, reader &in) {
    if (packet_id == login_cb::login_compression::packet_id) {
        login_cb::login_compression packet{};
        if (auto decoded{login_cb::decode_login_compression(in, packet)};
            !decoded) {
            finish("malformed login_compression", true);
            return;
        }
        decoder_.set_threshold(static_cast<int>(packet.threshold));
        {
            const std::lock_guard lock{encode_mutex_};
            encoder_.set_threshold(static_cast<int>(packet.threshold));
        }
        return;
    }
    if (packet_id == login_cb::login_finished::packet_id) {
        login_cb::login_finished packet{};
        if (auto decoded{login_cb::decode_login_finished(in, packet)};
            !decoded) {
            finish("malformed login_finished", true);
            return;
        }
        login_sb::login_acknowledged ack{};
        send_packet(login_sb::login_acknowledged::packet_id, ack,
                    login_sb::encode_login_acknowledged);
        send_client_information();
        phase_ = bot_phase::configuration;
        return;
    }
    if (packet_id == login_cb::login_disconnect::packet_id) {
        login_cb::login_disconnect packet{};
        static_cast<void>(login_cb::decode_login_disconnect(in, packet));
        finish("server rejected login: " + packet.reason, true);
        return;
    }
    if (packet_id == login_cb::hello::packet_id) {
        finish("online-mode server is not supported by the pool client", true);
    }
}

void bot_slot::handle_configuration(std::uint32_t packet_id, reader &in) {
    if (packet_id == config_cb::select_known_packs::packet_id) {
        config_cb::select_known_packs packet{};
        if (auto decoded{config_cb::decode_select_known_packs(in, packet)};
            !decoded) {
            finish("malformed select_known_packs", true);
            return;
        }
        config_sb::select_known_packs response{};
        send_packet(config_sb::select_known_packs::packet_id, response,
                    config_sb::encode_select_known_packs);
        return;
    }
    if (packet_id == config_cb::finish_configuration::packet_id) {
        config_sb::finish_configuration ack{};
        send_packet(config_sb::finish_configuration::packet_id, ack,
                    config_sb::encode_finish_configuration);
        phase_ = bot_phase::play;
        return;
    }
    if (packet_id == config_cb::keep_alive::packet_id) {
        config_cb::keep_alive packet{};
        if (auto decoded{config_cb::decode_keep_alive(in, packet)}; !decoded) {
            finish("malformed configuration keep_alive", true);
            return;
        }
        config_sb::keep_alive response{.id = packet.id};
        send_packet(config_sb::keep_alive::packet_id, response,
                    config_sb::encode_keep_alive);
        return;
    }
    if (packet_id == config_cb::disconnect::packet_id) {
        finish("server disconnected during configuration", true);
    }
}

void bot_slot::handle_play(std::uint32_t packet_id, reader &in) {
    if (packet_id == play_cb::level_chunk_with_light::packet_id) {
        owner_->count_chunk();
        return;
    }
    if (packet_id == play_cb::keep_alive::packet_id) {
        play_cb::keep_alive packet{};
        if (auto decoded{play_cb::decode_keep_alive(in, packet)}; !decoded) {
            finish("malformed play keep_alive", true);
            return;
        }
        play_sb::keep_alive response{.id = packet.id};
        send_packet(play_sb::keep_alive::packet_id, response,
                    play_sb::encode_keep_alive);
        return;
    }
    if (packet_id == play_cb::player_position::packet_id) {
        play_cb::player_position packet{};
        if (auto decoded{play_cb::decode_player_position(in, packet)};
            !decoded) {
            finish("malformed player_position", true);
            return;
        }
        const std::lock_guard lock{mutex_};
        spawn_y_ = packet.y;
        return;
    }
    if (packet_id == play_cb::login::packet_id) {
        play_cb::login packet{};
        if (auto decoded{play_cb::decode_login(in, packet)}; !decoded) {
            finish("malformed play login", true);
            return;
        }
        set_joined(clock_type::now());
    }
}

void bot_slot::set_joined(clock_type::time_point now) {
    {
        const std::lock_guard lock{mutex_};
        if (joined_ || finished_) {
            return;
        }
        joined_ = true;
        join_latency_ = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - connect_started_);
        linger_deadline_ = now + linger_;
        next_walk_ = now;
    }
    play_sb::player_loaded loaded{};
    send_packet(play_sb::player_loaded::packet_id, loaded,
                play_sb::encode_player_loaded);
}

void bot_slot::finish(std::string message, bool failed) {
    bool first{false};
    {
        const std::lock_guard lock{mutex_};
        if (!finished_) {
            finished_ = true;
            first = true;
            if (failed && !joined_) {
                error_ = std::move(message);
            }
        }
    }
    if (first) {
        close_connection();
        owner_->slot_finished();
    }
}

void bot_slot::close_connection() {
    owner_->close(connection_.load(std::memory_order_relaxed));
}

bot_pool::bot_pool(const bot_pool_options &options) : options_{options} {}

bot_pool::~bot_pool() {
    {
        const std::lock_guard lock{timer_mutex_};
        timer_stop_ = true;
    }
    timer_cv_.notify_all();
    if (timer_.joinable()) {
        timer_.join();
    }
    if (transport_) {
        transport_->stop();
    }
}

auto bot_pool::run() -> bot_pool_report {
    bot_pool_report report{};
    report.requested = options_.bots;
    if (options_.bots == 0) {
        return report;
    }

    slots_.reserve(options_.bots);
    for (std::size_t index{0}; index < options_.bots; ++index) {
        slots_.push_back(std::make_unique<bot_slot>(*this, index, options_));
    }
    remaining_.store(slots_.size(), std::memory_order_relaxed);

    auto opened{transport::open(
        transport_handler{
            .on_open =
                [this](std::uint64_t connection) { dispatch_open(connection); },
            .on_data =
                [this](std::uint64_t connection,
                       std::span<const std::byte> bytes) {
                    dispatch_data(connection, bytes);
                },
            .on_close =
                [this](std::uint64_t connection, std::string_view reason) {
                    dispatch_close(connection, reason);
                },
        },
        pool_io_threads)};
    if (!opened) {
        report.failed = options_.bots;
        report.errors.push_back(opened.error().message);
        return report;
    }
    transport_ = std::move(*opened);

    const auto start{clock_type::now()};
    for (auto &slot : slots_) {
        slot->begin(start);
        auto connection{transport_->connect(options_.target)};
        if (!connection) {
            slot->fail(connection.error().message);
            continue;
        }
        attach(*connection, *slot);
    }

    timer_ = std::thread{[this] { timer_loop(); }};
    {
        std::unique_lock lock{wait_mutex_};
        wait_cv_.wait(lock, [this] {
            return remaining_.load(std::memory_order_acquire) == 0;
        });
    }
    {
        const std::lock_guard lock{timer_mutex_};
        timer_stop_ = true;
    }
    timer_cv_.notify_all();
    if (timer_.joinable()) {
        timer_.join();
    }
    transport_->stop();
    transport_.reset();
    return take_report();
}

void bot_pool::send(std::uint64_t connection,
                    std::span<const std::byte> frame) {
    if (transport_ && connection != 0) {
        transport_->send(connection, frame);
    }
}

void bot_pool::close(std::uint64_t connection) {
    if (transport_ && connection != 0) {
        transport_->close(connection);
    }
}

void bot_pool::count_packet() {
    packets_.fetch_add(1, std::memory_order_relaxed);
}

void bot_pool::count_chunk() {
    chunks_.fetch_add(1, std::memory_order_relaxed);
}

void bot_pool::slot_finished() {
    const std::lock_guard lock{wait_mutex_};
    if (remaining_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        wait_cv_.notify_all();
    }
}

void bot_pool::attach(std::uint64_t connection, bot_slot &slot) {
    for (;;) {
        std::vector<pending_event> batch;
        {
            const std::lock_guard lock{map_mutex_};
            auto found{pending_events_.find(connection)};
            if (found == pending_events_.end() || found->second.empty()) {
                slots_by_id_.emplace(connection, &slot);
                if (found != pending_events_.end()) {
                    pending_events_.erase(found);
                }
                pending_bytes_.erase(connection);
                break;
            }
            batch = std::move(found->second);
            found->second.clear();
            pending_bytes_[connection] = 0;
        }
        for (auto &event : batch) {
            replay(slot, connection, event);
        }
    }
}

void bot_pool::replay(bot_slot &slot, std::uint64_t connection,
                      pending_event &event) {
    switch (event.type) {
    case pending_event::kind::opened:
        slot.on_open(connection);
        break;
    case pending_event::kind::data:
        slot.on_data(event.data);
        break;
    case pending_event::kind::closed:
        slot.on_close(event.message);
        break;
    }
}

void bot_pool::queue_pending(std::uint64_t connection, pending_event event) {
    auto &queue{pending_events_[connection]};
    auto &bytes{pending_bytes_[connection]};
    if (event.type == pending_event::kind::data) {
        if (bytes + event.data.size() > max_pending_bytes) {
            pending_event overflow{};
            overflow.type = pending_event::kind::closed;
            overflow.message = "connection data arrived before registration";
            queue.push_back(std::move(overflow));
            return;
        }
        bytes += event.data.size();
    }
    queue.push_back(std::move(event));
}

void bot_pool::dispatch_open(std::uint64_t connection) {
    bot_slot *slot{nullptr};
    {
        const std::lock_guard lock{map_mutex_};
        auto found{slots_by_id_.find(connection)};
        if (found != slots_by_id_.end()) {
            slot = found->second;
        } else {
            pending_event event{};
            event.type = pending_event::kind::opened;
            queue_pending(connection, std::move(event));
        }
    }
    if (slot != nullptr) {
        slot->on_open(connection);
    }
}

void bot_pool::dispatch_data(std::uint64_t connection,
                             std::span<const std::byte> bytes) {
    bot_slot *slot{nullptr};
    {
        const std::lock_guard lock{map_mutex_};
        auto found{slots_by_id_.find(connection)};
        if (found != slots_by_id_.end()) {
            slot = found->second;
        } else {
            pending_event event{};
            event.type = pending_event::kind::data;
            event.data.assign(bytes.begin(), bytes.end());
            queue_pending(connection, std::move(event));
        }
    }
    if (slot != nullptr) {
        slot->on_data(bytes);
    }
}

void bot_pool::dispatch_close(std::uint64_t connection,
                              std::string_view reason) {
    bot_slot *slot{nullptr};
    {
        const std::lock_guard lock{map_mutex_};
        auto found{slots_by_id_.find(connection)};
        if (found != slots_by_id_.end()) {
            slot = found->second;
        } else {
            pending_event event{};
            event.type = pending_event::kind::closed;
            event.message = std::string{reason};
            queue_pending(connection, std::move(event));
        }
    }
    if (slot != nullptr) {
        slot->on_close(reason);
    }
}

void bot_pool::timer_loop() {
    log::set_thread_name("bot-pool-timer");
    const auto walk_period{
        std::max(options_.walk_period, std::chrono::milliseconds{1})};
    const auto tick{std::min(walk_period, max_timer_tick)};
    std::unique_lock lock{timer_mutex_};
    for (;;) {
        lock.unlock();
        const auto now{clock_type::now()};
        for (auto &slot : slots_) {
            slot->tick(now, walk_period);
        }
        lock.lock();
        if (timer_stop_ || remaining_.load(std::memory_order_acquire) == 0) {
            break;
        }
        timer_cv_.wait_for(lock, tick, [this] { return timer_stop_; });
    }
}

auto bot_pool::take_report() -> bot_pool_report {
    bot_pool_report report{};
    report.requested = options_.bots;
    report.packets = packets_.load(std::memory_order_relaxed);
    report.chunks = chunks_.load(std::memory_order_relaxed);

    std::vector<std::chrono::milliseconds> latencies{};
    latencies.reserve(slots_.size());
    for (auto &slot : slots_) {
        if (!slot->joined()) {
            continue;
        }
        ++report.joined;
        latencies.push_back(slot->join_latency());
    }
    report.failed = report.requested - report.joined;
    std::sort(latencies.begin(), latencies.end());
    const auto percentile =
        [&latencies](std::size_t percent) -> std::chrono::milliseconds {
        if (latencies.empty()) {
            return std::chrono::milliseconds{0};
        }
        auto rank{(latencies.size() * percent + 99) / 100};
        rank = std::clamp<std::size_t>(rank, 1, latencies.size());
        return latencies[rank - 1];
    };
    report.join_p50 = percentile(50);
    report.join_p99 = percentile(99);

    for (auto &slot : slots_) {
        if (report.errors.size() >= max_reported_errors) {
            break;
        }
        auto message{slot->error()};
        if (!message.empty()) {
            report.errors.push_back(std::move(message));
        }
    }
    return report;
}

} // namespace

auto run_bot_pool(const bot_pool_options &options) -> bot_pool_report {
    try {
        bot_pool pool{options};
        return pool.run();
    } catch (const std::exception &failure) {
        bot_pool_report report{};
        report.requested = options.bots;
        report.failed = options.bots;
        report.errors.emplace_back(failure.what());
        return report;
    }
}

} // namespace refrain::net
