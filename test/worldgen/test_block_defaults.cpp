// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Synthetic coverage of the default-state report. A datapack names a plain
// block when a feature places the block's default state, so the report turns
// that spelling back into the property values the reference state carries.

#include <refrain/core/io.hpp>
#include <refrain/worldgen/chunk/block_defaults.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>

#include <boost/ut.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

using namespace boost::ut;
using namespace refrain;
using namespace refrain::worldgen;

struct temporary_root {
    std::filesystem::path path{};

    temporary_root() {
        const auto stamp{
            std::chrono::steady_clock::now().time_since_epoch().count()};
        path = std::filesystem::temp_directory_path() /
               ("refrain_block_defaults_test_" + std::to_string(stamp));
        auto error = std::error_code{};
        std::filesystem::create_directories(path / "data", error);
        expect(!error);
    }
    temporary_root(const temporary_root &) = delete;
    auto operator=(const temporary_root &) -> temporary_root & = delete;
    ~temporary_root() {
        auto error = std::error_code{};
        std::filesystem::remove_all(path, error);
    }

    [[nodiscard]] auto data_root() const -> std::string {
        return (path / "data").string();
    }

    void write(std::string_view document) const {
        const auto directory = path / "reports" / "block_defaults";
        auto error = std::error_code{};
        std::filesystem::create_directories(directory, error);
        expect(!error);
        const auto written = core::io::write_text(
            (directory / "defaults.json").string(), document);
        expect(written.has_value()) << written.error().message;
    }
};

} // namespace

using namespace boost::ut;
using namespace refrain;
using namespace refrain::worldgen;

suite<"worldgen/block-defaults"> block_defaults_tests = [] {
    "the report loads sorted default properties per block"_test = [] {
        temporary_root root;
        root.write(R"({"game_version":"26.3","data_version":5023,
            "blocks":{"minecraft:tall_grass":{"half":"lower"},
                      "minecraft:oak_stairs":{"waterlogged":"false",
                                              "facing":"north",
                                              "half":"bottom",
                                              "shape":"straight"}}})");
        const auto defaults = load_block_defaults(root.data_root());
        expect(defaults.has_value()) << defaults.error().message;
        if (!defaults) {
            return;
        }
        const auto grass = defaults->find("minecraft:tall_grass");
        expect(grass != defaults->end());
        if (grass == defaults->end()) {
            return;
        }
        expect(grass->second.size() == 1U);
        expect(grass->second[0].name == "half");
        expect(grass->second[0].value == "lower");
        const auto stairs = defaults->find("minecraft:oak_stairs");
        expect(stairs != defaults->end());
        if (stairs == defaults->end()) {
            return;
        }
        const std::array<std::string_view, 4> order{"facing", "half", "shape",
                                                    "waterlogged"};
        expect(stairs->second.size() == order.size());
        for (std::size_t index = 0; index < order.size(); ++index) {
            expect(stairs->second[index].name == order[index]);
        }
    };

    "a missing report is reported as not_found"_test = [] {
        temporary_root root;
        const auto defaults = load_block_defaults(root.data_root());
        expect(!defaults.has_value());
        if (defaults) {
            return;
        }
        expect(defaults.error().code == error_code::not_found);
    };

    "a malformed report is rejected"_test = [] {
        temporary_root root;
        root.write(R"({"game_version":"26.3","blocks":[]})");
        const auto defaults = load_block_defaults(root.data_root());
        expect(!defaults.has_value());
        if (defaults) {
            return;
        }
        expect(defaults.error().code == error_code::parse_error);
    };

    "the registry answers a bare block name with its default properties"_test =
        [] {
            block_state_registry registry;
            const auto grass = registry.intern("minecraft:tall_grass", {});
            expect(grass.has_value());
            if (!grass) {
                return;
            }
            block_default_map defaults;
            defaults.emplace(
                "minecraft:tall_grass",
                block_default_properties{{.name = "half", .value = "lower"}});
            registry.set_block_defaults(std::move(defaults));
            const auto properties = registry.default_properties(*grass);
            expect(properties.size() == 1U);
            if (properties.size() != 1U) {
                return;
            }
            expect(properties[0].name == "half");
            expect(properties[0].value == "lower");
            const auto unknown = registry.intern("minecraft:stone", {});
            expect(unknown.has_value());
            if (unknown) {
                expect(registry.default_properties(*unknown).empty());
            }
        };
};
