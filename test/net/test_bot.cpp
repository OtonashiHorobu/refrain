// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/config.hpp>
#include <refrain/net/bot.hpp>
#include <refrain/net/server.hpp>

#include <boost/ut.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

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
                    ("refrain_bot_" + std::to_string(stamp))};
    std::error_code error;
    std::filesystem::create_directories(path, error);
    return path;
}

suite bot_tests = [] {
    "net/bot joins an offline server and receives a chunk"_test = [] {
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
        net::server_inputs inputs{};
        inputs.spawn_chunks =
            std::make_shared<const std::vector<std::vector<std::byte>>>(
                std::vector<std::vector<std::byte>>{chunk_body});
        auto server{net::server::start(std::move(inputs), *view)};
        expect(static_cast<bool>(server));
        if (!server) {
            std::error_code error;
            std::filesystem::remove_all(dir, error);
            return;
        }
        const auto port{(*server)->local_port()};
        expect(port != 0);

        auto report{net::run_bot(net::bot_options{
            .target = {.host = "127.0.0.1", .port = port},
            .username = "Bot",
            .timeout = std::chrono::milliseconds{5000},
            .wait_chunks = 1,
        })};
        expect(static_cast<bool>(report));
        if (report) {
            expect(report->name == "Bot");
            expect(report->packets > 0);
            expect(report->chunks >= 1);
        }
        (*server)->stop();
        std::error_code error;
        std::filesystem::remove_all(dir, error);
    };
};

} // namespace
