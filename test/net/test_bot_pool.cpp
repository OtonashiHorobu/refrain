// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/config.hpp>
#include <refrain/net/bot_pool.hpp>
#include <refrain/net/server.hpp>

#include <boost/ut.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "test_protocol.hpp"

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace config = refrain::core::config;
namespace net = refrain::net;

[[nodiscard]] auto make_temp_dir() -> std::filesystem::path {
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    const auto path{std::filesystem::temp_directory_path() /
                    ("refrain_bot_pool_" + std::to_string(stamp))};
    std::error_code error;
    std::filesystem::create_directories(path, error);
    return path;
}

suite bot_pool_tests = [] {
    "net/bot pool reports an empty run without a transport"_test = [] {
        net::bot_pool_options options{};
        options.bots = 0;
        const auto report{net::run_bot_pool(options)};
        expect(report.requested == 0U);
        expect(report.joined == 0U);
        expect(report.failed == 0U);
    };

    "net/bot pool joins a server and keeps every bot in play"_test = [] {
        const auto dir{make_temp_dir()};
        {
            std::ofstream stream{dir / "network.toml",
                                 std::ios::binary | std::ios::trunc};
            stream << "bind_host = \"127.0.0.1\"\nport = 0\n"
                      "online_mode = false\nview_distance = 3\n";
        }
        auto store{config::store::empty()};
        expect(static_cast<bool>(store.mount(
            {.name = "network", .path = (dir / "network.toml").string()})));
        expect(static_cast<bool>(store.load_all()));
        auto view{store.module("network")};
        expect(static_cast<bool>(view));
        if (!view) {
            std::error_code error;
            std::filesystem::remove_all(dir, error);
            return;
        }

        std::vector<std::byte> chunk_body;
        {
            net::writer out{chunk_body};
            constexpr std::uint16_t chunk_id{0x2E}; // level_chunk_with_light
            out.write_varint(chunk_id);
        }
        std::atomic<int> moves{0};
        net::server_inputs inputs{};
        inputs.protocol = refrain::test::make_test_protocol();
        inputs.spawn_chunks =
            std::make_shared<const std::vector<std::vector<std::byte>>>(
                std::vector<std::vector<std::byte>>{chunk_body});
        inputs.movement = [&moves](const net::movement_event &) { ++moves; };
        auto server{net::server::start(std::move(inputs), *view)};
        expect(static_cast<bool>(server));
        if (!server) {
            std::error_code error;
            std::filesystem::remove_all(dir, error);
            return;
        }
        const auto port{(*server)->local_port()};
        expect(port != 0);

        std::atomic<bool> ticking{true};
        std::thread ticker{[&] {
            while (ticking.load(std::memory_order_relaxed)) {
                (*server)->tick();
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            }
        }};

        const auto report{net::run_bot_pool(net::bot_pool_options{
            .target = {.host = "127.0.0.1", .port = port},
            .bots = 4,
            .duration = std::chrono::milliseconds{800},
            .timeout = std::chrono::milliseconds{5000},
            .walk_radius = 2.0,
            .walk_period = std::chrono::milliseconds{50},
            .username_prefix = "Pool",
            .view_distance = 3,
        })};
        expect(report.requested == 4U);
        expect(report.joined == 4U);
        expect(report.failed == 0U);
        expect(report.packets > 0U);
        expect(report.chunks >= 4U);
        expect(report.join_p99 >= report.join_p50);

        // Play packets are applied on the simulation thread, so let the
        // ticker drain the walk that the bots sent while they lingered.
        std::size_t live_connections{(*server)->stats().connections};
        for (int attempt{0}; attempt < 200 && live_connections != 0U;
             ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
            live_connections = (*server)->stats().connections;
        }
        expect(live_connections == 0U);
        expect(moves.load() > 0);

        ticking.store(false, std::memory_order_relaxed);
        ticker.join();
        (*server)->stop();
        std::error_code error;
        std::filesystem::remove_all(dir, error);
    };
};

} // namespace
