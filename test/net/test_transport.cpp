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
};

} // namespace
