// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// `refrain:host.platform` adapter tests over the app host context: file
// read/write/sync/close, mmap, invalid flags/handles and crash annotations.

#include <refrain/app/host_context.hpp>
#include <refrain/host/platform_api.hpp>
#include <refrain/mod/host.hpp>

#include <boost/ut.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace host = refrain::host;
namespace platform = refrain::host::platform;
namespace app = refrain::app;
namespace mod = refrain::mod;

const suite host_platform_tests = [] {
    "host/platform file and mmap round trip"_test = [] {
        namespace fs = std::filesystem;
        const auto path{fs::temp_directory_path() /
                        "refrain-host-platform-test.bin"};
        std::error_code error;
        fs::remove(path, error);

        mod::host owner{};
        app::host_context context{owner};
        const auto api{host::make_platform_provider(context)};

        const auto flags{platform::file_open_flags::read_write |
                         platform::file_open_flags::create |
                         platform::file_open_flags::truncate};
        const auto file{api.file_open(path.string(), flags)};
        expect(file.has_value());
        if (!file) {
            fs::remove(path, error);
            return;
        }

        const std::array<std::uint8_t, 5> payload{'h', 'e', 'l', 'l', 'o'};
        const auto written{api.file_write(*file, 0, payload)};
        expect(written.has_value());
        expect(*written == 5U);

        const auto size{api.file_size(*file)};
        expect(size.has_value());
        expect(*size == 5U);

        std::array<std::uint8_t, 5> buffer{};
        const auto read{api.file_read(*file, 0, buffer)};
        expect(read.has_value());
        expect(*read == 5U);
        expect(buffer == payload);

        expect(api.file_sync(*file).has_value());
        expect(api.file_close(*file).has_value());
        expect(api.file_exists(path.string()));

        const auto map{api.mmap_open(path.string())};
        expect(map.has_value());
        if (map) {
            const auto bytes{api.mmap_data(*map)};
            expect(bytes.has_value());
            if (bytes) {
                expect(bytes->size() == 5U);
                expect((*bytes)[0] == 'h');
            }
            expect(api.mmap_close(*map).has_value());
        }

        const auto reopened{
            api.file_open(path.string(), platform::file_open_flags::read_only)};
        expect(reopened.has_value());
        if (reopened) {
            expect(api.file_close(*reopened).has_value());
        }

        const auto invalid_flags{
            api.file_open(path.string(), platform::file_open_flags::create)};
        expect(!invalid_flags.has_value());
        expect(invalid_flags.error() ==
               platform::platform_error::invalid_argument);

        std::array<std::uint8_t, 1> tiny{};
        const auto missing{api.file_read(platform::file{9999}, 0, tiny)};
        expect(!missing.has_value());
        expect(missing.error() == platform::platform_error::not_found);

        expect(!api.temp_directory().empty());
        api.crash_annotation("test-key", "test-value");

        fs::remove(path, error);
    };
};

} // namespace
