// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/net/ping.hpp>

#include <refrain/net/base.hpp>
#include <refrain/net/frame.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

namespace refrain::net {

auto ping(const endpoint &target, std::chrono::milliseconds timeout)
    -> result<ping_result> {
    struct state {
        std::mutex mutex{};
        std::condition_variable changed{};
        frame_decoder decoder{-1};
        status_info status{};
        std::int64_t sent_at{0};
        std::chrono::milliseconds latency{0};
        bool done{false};
        bool have_status{false};
        std::string failure{};
    };

    auto shared{std::make_shared<state>()};
    auto socket{transport::open(
        transport_handler{
            .on_open = [](std::uint64_t) {},
            .on_data =
                [shared](std::uint64_t, std::span<const std::byte> bytes) {
                    const std::lock_guard lock{shared->mutex};
                    if (shared->done) {
                        return;
                    }
                    if (auto pushed{shared->decoder.push(bytes)}; !pushed) {
                        shared->failure = pushed.error().message;
                        shared->done = true;
                        shared->changed.notify_all();
                        return;
                    }
                    for (;;) {
                        std::vector<std::byte> body;
                        auto next{shared->decoder.next(body)};
                        if (!next) {
                            shared->failure = next.error().message;
                            shared->done = true;
                            shared->changed.notify_all();
                            return;
                        }
                        if (!*next) {
                            return;
                        }
                        reader in{body};
                        auto id{in.read_varint()};
                        if (!id) {
                            shared->failure = id.error().message;
                            shared->done = true;
                            shared->changed.notify_all();
                            return;
                        }
                        if (*id == 0) {
                            auto json{in.read_string(32'767)};
                            if (!json) {
                                shared->failure = json.error().message;
                                shared->done = true;
                                shared->changed.notify_all();
                                return;
                            }
                            auto parsed{parse_status_json(*json)};
                            if (!parsed) {
                                shared->failure = parsed.error().message;
                                shared->done = true;
                                shared->changed.notify_all();
                                return;
                            }
                            shared->status = std::move(*parsed);
                            shared->have_status = true;
                            shared->sent_at =
                                std::chrono::duration_cast<
                                    std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now()
                                        .time_since_epoch())
                                    .count();
                            // The ping request is queued on the transport from
                            // the I/O thread; the send copy keeps it alive.
                            shared->changed.notify_all();
                        } else if (*id == 1) {
                            auto time{in.read_i64()};
                            if (!time) {
                                shared->failure = time.error().message;
                                shared->done = true;
                                shared->changed.notify_all();
                                return;
                            }
                            const auto now{std::chrono::duration_cast<
                                               std::chrono::milliseconds>(
                                               std::chrono::steady_clock::now()
                                                   .time_since_epoch())
                                               .count()};
                            shared->latency =
                                std::chrono::milliseconds{now - *time};
                            shared->done = true;
                            shared->changed.notify_all();
                            return;
                        }
                    }
                },
            .on_close =
                [shared](std::uint64_t, std::string_view reason) {
                    const std::lock_guard lock{shared->mutex};
                    if (!shared->done) {
                        shared->failure =
                            fmt::format("connection closed: {}", reason);
                        shared->done = true;
                        shared->changed.notify_all();
                    }
                },
            .on_error = [](std::string_view) {},
        },
        1)};
    if (!socket) {
        return std::unexpected{std::move(socket.error())};
    }

    auto connection{(*socket)->connect(target)};
    if (!connection) {
        (*socket)->stop();
        return std::unexpected{std::move(connection.error())};
    }

    const auto handshake{encode_handshake(handshake_intention{
        .protocol = 777, .host = target.host, .port = target.port, .next = 1})};
    if (!handshake) {
        (*socket)->stop();
        return std::unexpected{std::move(handshake.error())};
    }
    net::frame_encoder encoder{-1};
    const auto send_packet = [&](std::span<const std::byte> body) {
        std::vector<std::byte> framed;
        if (auto encoded{encoder.encode(body, framed)}; encoded) {
            (*socket)->send(*connection, framed);
        }
    };
    send_packet(*handshake);
    send_packet(encode_status_request());

    bool ping_sent{false};
    {
        std::unique_lock lock{shared->mutex};
        const auto ready{shared->changed.wait_for(lock, timeout, [&] {
            return shared->done || shared->have_status;
        })};
        if (!ready) {
            lock.unlock();
            (*socket)->stop();
            return std::unexpected{
                make_error(error_code::timeout, "status ping timed out")};
        }
        if (shared->done) {
            lock.unlock();
            (*socket)->stop();
            return std::unexpected{
                make_error(error_code::io_error, shared->failure)};
        }
        ping_sent = shared->have_status;
    }

    if (ping_sent) {
        send_packet(encode_ping_request(shared->sent_at));
    }

    {
        std::unique_lock lock{shared->mutex};
        shared->changed.wait_for(lock, timeout, [&] { return shared->done; });
        if (!shared->done) {
            lock.unlock();
            (*socket)->stop();
            return std::unexpected{
                make_error(error_code::timeout, "pong response timed out")};
        }
        if (!shared->failure.empty() && !shared->have_status) {
            const auto message{shared->failure};
            lock.unlock();
            (*socket)->stop();
            return std::unexpected{make_error(error_code::io_error, message)};
        }
        auto result{
            ping_result{.status = shared->status, .latency = shared->latency}};
        lock.unlock();
        (*socket)->stop();
        return result;
    }
}

} // namespace refrain::net
