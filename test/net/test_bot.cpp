// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/config.hpp>
#include <refrain/net/bot.hpp>
#include <refrain/net/server.hpp>

#include <boost/ut.hpp>

#include <atomic>

#include "test_protocol.hpp"

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
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
        inputs.protocol = refrain::test::make_test_protocol();
        inputs.spawn_chunks =
            std::make_shared<const std::vector<std::vector<std::byte>>>(
                std::vector<std::vector<std::byte>>{chunk_body});
        auto tree{std::make_shared<net::command_tree>()};
        tree->nodes.push_back(net::command_node{});
        tree->root_index = 0;
        inputs.commands.tree = [tree](const net::player_identity &) {
            return tree;
        };
        inputs.tab = [](const net::player_identity &) {
            return net::tab_view{.header = "Header", .footer = "Footer"};
        };
        std::atomic<int> joins{0};
        std::atomic<int> leaves{0};
        inputs.player_join = [&joins](const net::player_identity &identity) {
            if (!identity.name.empty()) {
                ++joins;
            }
        };
        inputs.player_leave = [&leaves](const net::player_identity &) {
            ++leaves;
        };
        net::server *server_ptr{nullptr};
        inputs.commands.execute =
            [&server_ptr](const net::player_identity &identity,
                          std::string_view input) -> net::command_feedback {
            if (input == "help") {
                return {.handled = true,
                        .success = true,
                        .message = "Commands: /help, /list, /say <message>"};
            }
            if (input.starts_with("say ")) {
                std::string message{"["};
                message += identity.name;
                message += "] ";
                message += input.substr(4);
                server_ptr->broadcast_system_chat(message);
                return {.handled = true, .success = true};
            }
            return {.handled = true,
                    .success = false,
                    .message = "Unknown command"};
        };
        auto server{net::server::start(std::move(inputs), *view)};
        server_ptr = server ? server->get() : nullptr;
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
        auto chat_report{net::run_bot(net::bot_options{
            .target = {.host = "127.0.0.1", .port = port},
            .username = "ChatBot",
            .timeout = std::chrono::milliseconds{5000},
            .command = "say hello",
            .wait_messages = 1,
        })};
        if (!chat_report) {
            std::fprintf(stderr, "chat bot error: %s\n",
                         chat_report.error().message.c_str());
        }
        expect(static_cast<bool>(chat_report));
        if (chat_report) {
            expect(chat_report->messages.size() == 1);
            if (!chat_report->messages.empty()) {
                expect(chat_report->messages.front().find("hello") !=
                       std::string::npos);
            }
        }

        auto capture_report{net::run_bot(net::bot_options{
            .target = {.host = "127.0.0.1", .port = port},
            .username = "CaptureBot",
            .timeout = std::chrono::milliseconds{5000},
            .linger = std::chrono::milliseconds{200},
            .capture = true,
        })};
        expect(static_cast<bool>(capture_report));
        if (capture_report) {
            const auto has_id = [&](std::uint32_t id) {
                for (const auto &packet : capture_report->capture) {
                    if (packet.id == id) {
                        return true;
                    }
                }
                return false;
            };
            expect(has_id(16U));  // commands
            expect(has_id(71U));  // player_info_update
            expect(has_id(88U));  // server_data
            expect(has_id(106U)); // set_health
            expect(has_id(136U)); // update_recipes
            expect(has_id(125U)); // tab_list
        }
        const auto stats{(*server)->stats()};
        expect(stats.connections == 1U);
        expect(stats.packets_out > 0U);
        expect(stats.bytes_out > 0U);
        expect(stats.packets_in > 0U);
        expect(stats.bytes_in > 0U);
        ticking.store(false, std::memory_order_relaxed);
        ticker.join();
        (*server)->stop();
        expect(joins.load() >= 1);
        expect(leaves.load() >= 1);
        std::error_code error;
        std::filesystem::remove_all(dir, error);
    };

    "net/server announces a second player as an entity"_test = [] {
        const auto dir{make_temp_dir()};
        {
            std::ofstream stream{dir / "network.toml",
                                 std::ios::binary | std::ios::trunc};
            stream << "bind_host = \"127.0.0.1\"\nport = 0\n";
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
        net::server_inputs inputs{};
        inputs.protocol = refrain::test::make_test_protocol();
        auto server{net::server::start(std::move(inputs), *view)};
        expect(static_cast<bool>(server));
        if (!server) {
            std::error_code error;
            std::filesystem::remove_all(dir, error);
            return;
        }
        const auto target{net::endpoint{.host = "127.0.0.1",
                                        .port = (*server)->local_port()}};
        std::thread first{[&] {
            static_cast<void>(net::run_bot(net::bot_options{
                .target = target,
                .username = "BotOne",
                .timeout = std::chrono::milliseconds{5000},
                .linger = std::chrono::milliseconds{3000},
            }));
        }};
        std::this_thread::sleep_for(std::chrono::milliseconds{800});
        auto second{net::run_bot(net::bot_options{
            .target = target,
            .username = "BotTwo",
            .timeout = std::chrono::milliseconds{5000},
            .linger = std::chrono::milliseconds{300},
            .capture = true,
        })};
        expect(static_cast<bool>(second));
        if (second) {
            const auto has_id = [&](std::uint32_t id) {
                for (const auto &packet : second->capture) {
                    if (packet.id == id) {
                        return true;
                    }
                }
                return false;
            };
            expect(has_id(1U));  // add_entity
            expect(has_id(71U)); // player_info_update
        }
        first.join();
        (*server)->stop();
        std::error_code error;
        std::filesystem::remove_all(dir, error);
    };

    "net/bot completes online-mode encryption with a fake verifier"_test = [] {
        const auto dir{make_temp_dir()};
        {
            std::ofstream stream{dir / "network.toml",
                                 std::ios::binary | std::ios::trunc};
            stream << "bind_host = \"127.0.0.1\"\nport = 0\n"
                      "online_mode = true\n";
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
        net::server_inputs inputs{};
        inputs.protocol = refrain::test::make_test_protocol();
        inputs.verify_session =
            [](std::string_view,
               std::string_view) -> refrain::result<net::session_profile> {
            net::uuid id{};
            for (std::size_t index{0}; index < id.bytes.size(); ++index) {
                id.bytes[index] = static_cast<std::uint8_t>(index + 1U);
            }
            return net::session_profile{.id = id, .name = "Verified"};
        };
        auto server{net::server::start(std::move(inputs), *view)};
        expect(static_cast<bool>(server));
        if (!server) {
            std::error_code error;
            std::filesystem::remove_all(dir, error);
            return;
        }
        const auto port{(*server)->local_port()};
        auto report{net::run_bot(net::bot_options{
            .target = {.host = "127.0.0.1", .port = port},
            .username = "OnlineBot",
            .timeout = std::chrono::milliseconds{5000},
        })};
        expect(static_cast<bool>(report));
        if (report) {
            expect(report->name == "Verified");
        }
        (*server)->stop();
        std::error_code error;
        std::filesystem::remove_all(dir, error);
    };

    "net/bot rejects a failed session verification"_test = [] {
        const auto dir{make_temp_dir()};
        {
            std::ofstream stream{dir / "network.toml",
                                 std::ios::binary | std::ios::trunc};
            stream << "bind_host = \"127.0.0.1\"\nport = 0\n"
                      "online_mode = true\n";
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
        net::server_inputs inputs{};
        inputs.protocol = refrain::test::make_test_protocol();
        inputs.verify_session =
            [](std::string_view,
               std::string_view) -> refrain::result<net::session_profile> {
            return std::unexpected{refrain::make_error(
                refrain::error_code::not_found, "unknown session")};
        };
        auto server{net::server::start(std::move(inputs), *view)};
        expect(static_cast<bool>(server));
        if (!server) {
            std::error_code error;
            std::filesystem::remove_all(dir, error);
            return;
        }
        auto report{net::run_bot(net::bot_options{
            .target = {.host = "127.0.0.1", .port = (*server)->local_port()},
            .username = "Rejected",
            .timeout = std::chrono::milliseconds{5000},
        })};
        expect(!report);
        if (!report) {
            expect(report.error().code == refrain::error_code::io_error);
        }
        (*server)->stop();
        std::error_code error;
        std::filesystem::remove_all(dir, error);
    };
};

} // namespace
