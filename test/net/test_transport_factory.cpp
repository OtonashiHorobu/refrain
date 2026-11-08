// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/config.hpp>
#include <refrain/net/server.hpp>
#include <refrain/net/transport.hpp>

#include <boost/ut.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace net = refrain::net;

class memory_transport final : public net::transport {
  public:
    [[nodiscard]] auto listen(const net::endpoint &)
        -> refrain::status override {
        listened = true;
        return refrain::status_ok();
    }
    [[nodiscard]] auto connect(const net::endpoint &)
        -> refrain::result<std::uint64_t> override {
        return 1;
    }
    void send(std::uint64_t, std::span<const std::byte>) override {}
    void close(std::uint64_t) override {}
    void stop() noexcept override { stopped = true; }
    [[nodiscard]] auto connection_count() const -> std::size_t override {
        return 0;
    }
    [[nodiscard]] auto local_port() const -> std::uint16_t override {
        return 25565;
    }
    [[nodiscard]] auto descriptor() const
        -> net::transport_descriptor override {
        return net::transport_descriptor{
            .provider = "memory.stream",
            .protocol = net::transport_protocol::memory_stream,
            .reliable = true,
            .ordered = true,
            .message_oriented = false,
            .mtu = 0,
            .backpressure = false,
        };
    }

    bool listened{false};
    bool stopped{false};
};

[[nodiscard]] auto make_temp_dir() -> std::filesystem::path {
    auto path{
        std::filesystem::temp_directory_path() /
        ("refrain-transport-test-" +
         std::to_string(static_cast<unsigned long long>(
             std::chrono::steady_clock::now().time_since_epoch().count())))};
    std::filesystem::create_directories(path);
    return path;
}

suite transport_factory_tests = [] {
    "net/server uses the injected transport factory"_test = [] {
        const auto dir{make_temp_dir()};
        {
            std::ofstream stream{dir / "network.toml",
                                 std::ios::binary | std::ios::trunc};
            stream << "bind_host = \"127.0.0.1\"\nport = 0\n";
        }
        auto store{refrain::core::config::store::empty()};
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
        memory_transport *created{nullptr};
        net::server_inputs inputs{};
        inputs.make_transport = [&created](net::transport_handler,
                                           std::size_t) {
            auto transport{std::make_unique<memory_transport>()};
            created = transport.get();
            return refrain::result<std::unique_ptr<net::transport>>{
                std::move(transport)};
        };
        auto server{net::server::start(std::move(inputs), *view)};
        expect(static_cast<bool>(server));
        if (server) {
            expect(created != nullptr);
            if (created != nullptr) {
                expect(created->listened);
                expect(created->descriptor().protocol ==
                       net::transport_protocol::memory_stream);
            }
            (*server)->stop();
            if (created != nullptr) {
                expect(created->stopped);
            }
        }
        std::error_code error;
        std::filesystem::remove_all(dir, error);
    };
};

} // namespace
