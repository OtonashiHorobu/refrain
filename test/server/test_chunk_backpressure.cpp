// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/net/transport.hpp>

#include <boost/ut.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace net = refrain::net;

/// The chunk stream's last-resort backpressure is the transport's write-queue
/// cap: a client that stops reading must be closed once its queue reaches
/// 16 MiB instead of buffering without limit. A fake transport cannot show
/// the close, so the test drives the real loopback provider.
suite<"server/chunk-backpressure"> backpressure_tests = [] {
    "server/transport closes a client that stops reading"_test = [] {
        std::mutex mutex;
        std::condition_variable changed;
        std::string close_reason{};
        std::uint64_t server_connection{0};
        bool server_open{false};
        std::atomic<bool> release_reader{false};

        auto server = net::transport::open(net::transport_handler{
            .on_open =
                [&](std::uint64_t id) {
                    const std::lock_guard lock{mutex};
                    server_connection = id;
                    server_open = true;
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
        // The client's read callback blocks until the server has closed the
        // connection: the client stops draining its socket, both kernel
        // buffers fill, and the server's write queue starts to grow.
        auto client = net::transport::open(net::transport_handler{
            .on_open = [](std::uint64_t) {},
            .on_data =
                [&](std::uint64_t, std::span<const std::byte>) {
                    std::unique_lock lock{mutex};
                    changed.wait_for(lock, std::chrono::seconds{20},
                                     [&] { return release_reader.load(); });
                },
            .on_close = [](std::uint64_t, std::string_view) {},
            .on_error = [](std::string_view) {},
        });
        expect(client.has_value());
        if (!client) {
            (*server)->stop();
            return;
        }

        const auto bound{
            (*server)->listen(net::endpoint{.host = "127.0.0.1", .port = 0})};
        expect(bound.has_value());
        if (!bound) {
            (*client)->stop();
            (*server)->stop();
            return;
        }
        const auto port{(*server)->local_port()};
        expect(port != 0);
        const auto id{(*client)->connect(
            net::endpoint{.host = "127.0.0.1", .port = port})};
        expect(id.has_value());
        if (!id) {
            (*client)->stop();
            (*server)->stop();
            return;
        }
        {
            std::unique_lock lock{mutex};
            changed.wait_for(lock, std::chrono::seconds{5},
                             [&] { return server_open; });
            expect(server_open);
        }

        // Each send queues another megabyte the server cannot write out, so
        // the queue reaches the cap after about sixteen of them.
        const std::vector<std::byte> payload(1U * 1024U * 1024U,
                                             std::byte{0x5a});
        for (int index{0}; index < 64; ++index) {
            (*server)->send(server_connection, payload);
        }
        {
            std::unique_lock lock{mutex};
            changed.wait_for(lock, std::chrono::seconds{20},
                             [&] { return !close_reason.empty(); });
            expect(close_reason == "write backlog");
        }
        expect((*server)->connection_count() == 0U);

        release_reader.store(true);
        changed.notify_all();
        (*client)->stop();
        (*server)->stop();
    };
};

} // namespace
