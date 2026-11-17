// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/net/transport.hpp>

#include <boost/ut.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace {

using namespace boost::ut;

namespace net = refrain::net;

const suite tests = [] {
    "net/transport moves bytes over a loopback connection"_test = [] {
        std::mutex mutex;
        std::condition_variable changed;
        std::vector<std::byte> received;
        bool client_open{false};
        bool server_open{false};
        std::string close_reason{};

        auto server = net::transport::open(net::transport_handler{
            .on_open =
                [&](std::uint64_t) {
                    const std::lock_guard lock{mutex};
                    server_open = true;
                    changed.notify_all();
                },
            .on_data =
                [&](std::uint64_t /*id*/, std::span<const std::byte> bytes) {
                    const std::lock_guard lock{mutex};
                    received.assign(bytes.begin(), bytes.end());
                    changed.notify_all();
                },
            .on_close =
                [&](std::uint64_t, std::string_view reason) {
                    const std::lock_guard lock{mutex};
                    close_reason = std::string{reason};
                    changed.notify_all();
                },
            .on_error = [](std::string_view) {},
        });
        expect(server.has_value());
        if (!server) {
            return;
        }
        auto client = net::transport::open(net::transport_handler{
            .on_open =
                [&](std::uint64_t) {
                    const std::lock_guard lock{mutex};
                    client_open = true;
                    changed.notify_all();
                },
            .on_data = [](std::uint64_t, std::span<const std::byte>) {},
            .on_close = [](std::uint64_t, std::string_view) {},
            .on_error = [](std::string_view) {},
        });
        expect(client.has_value());
        if (!client) {
            server->get()->stop();
            return;
        }

        const auto bound{
            (*server)->listen(net::endpoint{.host = "127.0.0.1", .port = 0})};
        expect(bound.has_value());
        if (!bound) {
            return;
        }
        const auto port{(*server)->local_port()};
        expect(port != 0);

        const auto id{(*client)->connect(
            net::endpoint{.host = "127.0.0.1", .port = port})};
        expect(id.has_value());
        if (!id) {
            return;
        }

        {
            std::unique_lock lock{mutex};
            changed.wait_for(lock, std::chrono::seconds{5},
                             [&] { return client_open && server_open; });
            expect(client_open);
            expect(server_open);
        }

        const std::vector<std::byte> payload{static_cast<std::byte>('h'),
                                             static_cast<std::byte>('i')};
        (*client)->send(*id, payload);
        {
            std::unique_lock lock{mutex};
            changed.wait_for(lock, std::chrono::seconds{5},
                             [&] { return received.size() == payload.size(); });
            expect(received == payload);
        }

        (*client)->close(*id);
        {
            std::unique_lock lock{mutex};
            changed.wait_for(lock, std::chrono::seconds{5},
                             [&] { return !close_reason.empty(); });
            expect(!close_reason.empty());
        }

        (*client)->stop();
        (*server)->stop();
    };

    "net/transport closes a connection that passes the write queue cap"_test =
        [] {
            // A single send larger than the per-connection ceiling is dropped
            // and the connection closes; nothing reaches the peer.
            std::mutex mutex;
            std::condition_variable changed;
            std::uint64_t accepted{0};
            std::size_t received{0};
            std::string close_reason;

            auto server = net::transport::open(net::transport_handler{
                .on_open =
                    [&](std::uint64_t id) {
                        const std::lock_guard lock{mutex};
                        accepted = id;
                        changed.notify_all();
                    },
                .on_data = [](std::uint64_t, std::span<const std::byte>) {},
                .on_close =
                    [&](std::uint64_t, std::string_view reason) {
                        const std::lock_guard lock{mutex};
                        close_reason = std::string{reason};
                        changed.notify_all();
                    },
                .on_error = [](std::string_view) {},
            });
            expect(server.has_value());
            if (!server) {
                return;
            }
            (*server)->set_write_queue_limits(1024, 4U * 1024U * 1024U);
            const auto bound{(*server)->listen(
                net::endpoint{.host = "127.0.0.1", .port = 0})};
            expect(bound.has_value());
            if (!bound) {
                (*server)->stop();
                return;
            }
            auto client = net::transport::open(net::transport_handler{
                .on_open = [](std::uint64_t) {},
                .on_data =
                    [&](std::uint64_t, std::span<const std::byte> bytes) {
                        const std::lock_guard lock{mutex};
                        received += bytes.size();
                    },
                .on_close = [](std::uint64_t, std::string_view) {},
                .on_error = [](std::string_view) {},
            });
            expect(client.has_value());
            if (!client) {
                (*server)->stop();
                return;
            }
            const auto connected{(*client)->connect(net::endpoint{
                .host = "127.0.0.1", .port = (*server)->local_port()})};
            expect(connected.has_value());
            if (!connected) {
                (*client)->stop();
                (*server)->stop();
                return;
            }
            {
                std::unique_lock lock{mutex};
                changed.wait_for(lock, std::chrono::seconds{5},
                                 [&] { return accepted != 0; });
                expect(accepted != 0);
            }
            const std::vector<std::byte> oversized(2048, std::byte{0x41});
            (*server)->send(accepted, oversized);
            {
                std::unique_lock lock{mutex};
                changed.wait_for(lock, std::chrono::seconds{5},
                                 [&] { return !close_reason.empty(); });
                expect(close_reason.find("write backlog") != std::string::npos);
                expect(received == 0U);
            }
            expect((*server)->connection_count() == 0U);
            (*client)->stop();
            (*server)->stop();
        };

    "net/transport applies the aggregate write queue cap across sessions"_test =
        [] {
            // The per-connection ceiling is far above this send, so only the
            // aggregate ceiling can close the session.
            std::mutex mutex;
            std::condition_variable changed;
            std::uint64_t accepted{0};
            std::string close_reason;

            auto server = net::transport::open(net::transport_handler{
                .on_open =
                    [&](std::uint64_t id) {
                        const std::lock_guard lock{mutex};
                        accepted = id;
                        changed.notify_all();
                    },
                .on_data = [](std::uint64_t, std::span<const std::byte>) {},
                .on_close =
                    [&](std::uint64_t, std::string_view reason) {
                        const std::lock_guard lock{mutex};
                        close_reason = std::string{reason};
                        changed.notify_all();
                    },
                .on_error = [](std::string_view) {},
            });
            expect(server.has_value());
            if (!server) {
                return;
            }
            (*server)->set_write_queue_limits(4U * 1024U * 1024U, 1024);
            const auto bound{(*server)->listen(
                net::endpoint{.host = "127.0.0.1", .port = 0})};
            expect(bound.has_value());
            if (!bound) {
                (*server)->stop();
                return;
            }
            auto client = net::transport::open(net::transport_handler{
                .on_open = [](std::uint64_t) {},
                .on_data = [](std::uint64_t, std::span<const std::byte>) {},
                .on_close = [](std::uint64_t, std::string_view) {},
                .on_error = [](std::string_view) {},
            });
            expect(client.has_value());
            if (!client) {
                (*server)->stop();
                return;
            }
            const auto connected{(*client)->connect(net::endpoint{
                .host = "127.0.0.1", .port = (*server)->local_port()})};
            expect(connected.has_value());
            if (!connected) {
                (*client)->stop();
                (*server)->stop();
                return;
            }
            {
                std::unique_lock lock{mutex};
                changed.wait_for(lock, std::chrono::seconds{5},
                                 [&] { return accepted != 0; });
                expect(accepted != 0);
            }
            const std::vector<std::byte> oversized(2048, std::byte{0x42});
            (*server)->send(accepted, oversized);
            {
                std::unique_lock lock{mutex};
                changed.wait_for(lock, std::chrono::seconds{5},
                                 [&] { return !close_reason.empty(); });
                expect(close_reason.find("write backlog") != std::string::npos);
            }
            expect((*server)->connection_count() == 0U);
            (*client)->stop();
            (*server)->stop();
        };

    "net/transport refuses an inbound connection above its limit"_test = [] {
        std::mutex mutex;
        std::condition_variable changed;
        std::size_t accepted{0};
        bool second_closed{false};

        auto server = net::transport::open(net::transport_handler{
            .on_open =
                [&](std::uint64_t) {
                    const std::lock_guard lock{mutex};
                    ++accepted;
                    changed.notify_all();
                },
            .on_data = [](std::uint64_t, std::span<const std::byte>) {},
            .on_close = [](std::uint64_t, std::string_view) {},
            .on_error = [](std::string_view) {},
        });
        expect(server.has_value());
        if (!server) {
            return;
        }
        // The ceiling is applied in the accept path, before a session or a
        // receive buffer is committed to the peer.
        (*server)->set_connection_limit(1);
        const auto bound{
            (*server)->listen(net::endpoint{.host = "127.0.0.1", .port = 0})};
        expect(bound.has_value());
        if (!bound) {
            (*server)->stop();
            return;
        }
        const auto port{(*server)->local_port()};

        auto first = net::transport::open(net::transport_handler{
            .on_open = [](std::uint64_t) {},
            .on_data = [](std::uint64_t, std::span<const std::byte>) {},
            .on_close = [](std::uint64_t, std::string_view) {},
            .on_error = [](std::string_view) {},
        });
        expect(first.has_value());
        if (!first) {
            (*server)->stop();
            return;
        }
        const auto first_id{(*first)->connect(
            net::endpoint{.host = "127.0.0.1", .port = port})};
        expect(first_id.has_value());
        {
            std::unique_lock lock{mutex};
            changed.wait_for(lock, std::chrono::seconds{5},
                             [&] { return accepted == 1U; });
            expect(accepted == 1U);
        }

        auto second = net::transport::open(net::transport_handler{
            .on_open = [](std::uint64_t) {},
            .on_data = [](std::uint64_t, std::span<const std::byte>) {},
            .on_close =
                [&](std::uint64_t, std::string_view) {
                    const std::lock_guard lock{mutex};
                    second_closed = true;
                    changed.notify_all();
                },
            .on_error = [](std::string_view) {},
        });
        expect(second.has_value());
        if (!second) {
            (*first)->stop();
            (*server)->stop();
            return;
        }
        const auto second_id{(*second)->connect(
            net::endpoint{.host = "127.0.0.1", .port = port})};
        expect(second_id.has_value());
        {
            std::unique_lock lock{mutex};
            changed.wait_for(lock, std::chrono::seconds{5},
                             [&] { return second_closed; });
            expect(second_closed);
            expect(accepted == 1U);
        }
        expect((*server)->connection_count() == 1U);

        (*first)->stop();
        (*second)->stop();
        (*server)->stop();
    };
};

} // namespace
