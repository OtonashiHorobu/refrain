// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/world/anvil.hpp>
#include <refrain/world/create.hpp>
#include <refrain/world/store.hpp>

#include <boost/ut.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace world = refrain::world;

[[nodiscard]] auto scratch_directory() -> std::filesystem::path {
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    const auto path{std::filesystem::temp_directory_path() /
                    ("refrain_world_create_" + std::to_string(stamp))};
    std::error_code error;
    std::filesystem::create_directories(path, error);
    return path;
}

suite world_create_tests = [] {
    "world/create writes a world that opens with its seed and name"_test = [] {
        const auto directory = scratch_directory();
        const auto root = (directory / "new-world").string();

        expect(static_cast<bool>(world::create_world(root, 4242, "probe")));

        auto store{world::world_store::open(root)};
        if (!store) {
            std::fprintf(stderr, "create probe open failed: %s\n",
                         store.error().message.c_str());
        }
        expect(static_cast<bool>(store));
        if (store) {
            auto level{store->validate_level()};
            if (!level) {
                std::fprintf(stderr, "create probe level failed: %s\n",
                             level.error().message.c_str());
            }
            expect(static_cast<bool>(level));
            if (level) {
                expect(level->seed == 4242);
                expect(level->level_name == "probe");
                expect(level->data_version ==
                       world::anvil::supported_data_version);
            }
            auto chunks{store->chunk_positions()};
            expect(static_cast<bool>(chunks));
            if (chunks) {
                expect(chunks->empty());
            }
        }

        std::error_code error;
        std::filesystem::remove_all(directory, error);
    };

    "world/create refuses an existing world"_test = [] {
        const auto directory = scratch_directory();
        const auto root = (directory / "existing").string();

        expect(static_cast<bool>(world::create_world(root, 1, "first")));
        const auto second{world::create_world(root, 2, "second")};
        expect(!second);
        if (!second) {
            expect(second.error().code == refrain::error_code::already_exists);
        }

        std::error_code error;
        std::filesystem::remove_all(directory, error);
    };

    "world/create rejects an empty path"_test = [] {
        const auto created{world::create_world("", 1, "name")};
        expect(!created);
        if (!created) {
            expect(created.error().code ==
                   refrain::error_code::invalid_argument);
        }
    };
};

} // namespace
