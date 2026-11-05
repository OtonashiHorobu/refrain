// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/platform/mmap.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace boost::ut;
namespace fs = std::filesystem;

using refrain::platform::mapped_file;

/// Removes the scratch directory when the test returns (including early exits
/// on failed preconditions).
struct temp_directory {
    fs::path path{};

    explicit temp_directory(fs::path location) : path{std::move(location)} {}
    temp_directory(const temp_directory &) = delete;
    auto operator=(const temp_directory &) -> temp_directory & = delete;
    ~temp_directory() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

[[nodiscard]] auto make_temp_directory() -> fs::path {
    static int counter{0};
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    const auto path{fs::temp_directory_path() /
                    ("refrain_mmap_test_" + std::to_string(stamp) + "_" +
                     std::to_string(counter++))};
    std::error_code error;
    fs::create_directories(path, error);
    return path;
}

void write_bytes(const fs::path &path, std::span<const std::byte> data) {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    if (!data.empty()) {
        out.write(reinterpret_cast<const char *>(data.data()), // NOLINT
                  static_cast<std::streamsize>(data.size()));
    }
}

[[nodiscard]] auto pattern_blob(std::size_t count) -> std::vector<std::byte> {
    std::vector<std::byte> data(count);
    for (std::size_t index{0}; index < count; ++index) {
        data[index] = static_cast<std::byte>((index * 31U + 7U) % 256U);
    }
    return data;
}

suite<"platform/mmap"> mmap_tests = [] {
    "platform/mmap: empty files map to an empty span"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto path{scratch.path / "empty.bin"};
        write_bytes(path, {});

        auto mapped{mapped_file::open(path.string())};
        expect(mapped.has_value());
        if (!mapped) {
            return;
        }
        expect(mapped->valid());
        expect(mapped->size() == 0U);
        expect(mapped->data().empty());
    };

    "platform/mmap: regular files expose their exact bytes"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto path{scratch.path / "payload.bin"};
        const auto payload{pattern_blob(1024U * 1024U + 37U)};
        write_bytes(path, payload);

        auto mapped{mapped_file::open(path.string())};
        expect(mapped.has_value());
        if (!mapped) {
            return;
        }
        expect(mapped->valid());
        expect(mapped->size() == payload.size());
        expect(mapped->data().size() == payload.size());
        expect(std::equal(mapped->data().begin(), mapped->data().end(),
                          payload.begin()));
    };

    "platform/mmap: missing files are io_error"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto missing{scratch.path / "missing.bin"};
        const auto mapped{mapped_file::open(missing.string())};
        expect(!mapped.has_value());
        if (!mapped.has_value()) {
            expect(mapped.error().code == refrain::error_code::io_error);
        }
    };

    "platform/mmap: directories are io_error"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto mapped{mapped_file::open(scratch.path.string())};
        expect(!mapped.has_value());
        if (!mapped.has_value()) {
            expect(mapped.error().code == refrain::error_code::io_error);
        }
    };

    "platform/mmap: empty paths are invalid_argument"_test = [] {
        const auto mapped{mapped_file::open("")};
        expect(!mapped.has_value());
        if (!mapped.has_value()) {
            expect(mapped.error().code ==
                   refrain::error_code::invalid_argument);
        }
    };

    "platform/mmap: move transfers ownership and unmaps on destruction"_test =
        [] {
            temp_directory scratch{make_temp_directory()};
            const auto path{scratch.path / "moved.bin"};
            const auto payload{pattern_blob(4096U)};
            write_bytes(path, payload);

            {
                auto opened{mapped_file::open(path.string())};
                expect(opened.has_value());
                if (!opened) {
                    return;
                }
                mapped_file moved{std::move(*opened)};
                expect(!opened->valid());
                expect(moved.valid());
                expect(moved.size() == payload.size());
                expect(std::equal(moved.data().begin(), moved.data().end(),
                                  payload.begin()));

                mapped_file assigned{};
                expect(!assigned.valid());
                assigned = std::move(moved);
                expect(!moved.valid());
                expect(assigned.valid());
                expect(assigned.size() == payload.size());
            }

            // The mapping is released by now; the scratch file can be removed
            // (on Windows an outstanding mapped section would keep it busy).
            std::error_code error;
            expect(fs::remove(path, error));
            expect(!error);
        };
};

} // namespace
