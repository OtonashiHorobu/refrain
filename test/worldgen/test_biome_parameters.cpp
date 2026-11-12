// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The loader is exercised against synthetic reports written to a temporary
// directory. The shipped reports are loaded from the content snapshot when the
// checkout contains it; a checkout without content skips those cases with a
// diagnostic instead of failing.

#include <refrain/core/io.hpp>
#include <refrain/worldgen/biome/biome_parameters.hpp>
#include <refrain/worldgen/biome/biome_source.hpp>

#include "support/scratch_directory.hpp"
#include "support/vanilla_data.hpp"

#include <boost/ut.hpp>

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using namespace boost::ut;
using namespace refrain;
using namespace refrain::worldgen;

using scratch_directory = refrain::test_support::scratch_directory;

[[nodiscard]] auto data_root(const scratch_directory &root) -> std::string {
    auto error = std::error_code{};
    std::filesystem::create_directories(root.path() / "data", error);
    expect(!error);
    return (root.path() / "data").string();
}

void write_report(const scratch_directory &root, std::string_view name,
                  std::string_view document) {
    const auto directory = root.path() / "reports" / "biome_parameters";
    auto error = std::error_code{};
    std::filesystem::create_directories(directory, error);
    expect(!error);
    const auto written =
        core::io::write_text((directory / name).string(), document);
    expect(written.has_value()) << written.error().message;
}

auto report(std::string_view dimension, int entry_count,
            std::string_view entries) -> std::string {
    std::string text = "{\n";
    text += "  \"format\": 1,\n";
    text += "  \"game_version\": \"26.3\",\n";
    text += "  \"dimension\": \"";
    text += dimension;
    text += "\",\n";
    text += "  \"entry_count\": ";
    text += std::to_string(entry_count);
    text += ",\n";
    text += "  \"entries\": ";
    text += entries;
    text += "\n}\n";
    return text;
}

constexpr std::string_view overworld_dimension = "minecraft:overworld";

constexpr std::string_view two_entries = R"([
    {"biome": "test:cold", "temperature": [-10000, 0], "humidity": [-10000, 10000], "continentalness": [-10000, 10000], "erosion": [-10000, 10000], "depth": [0, 0], "weirdness": [-10000, 10000], "offset": 0},
    {"biome": "test:warm", "temperature": [1, 10000], "humidity": [5000, 10000], "continentalness": [-10000, 10000], "erosion": [-10000, 10000], "depth": [0, 10000], "weirdness": [-10000, 10000], "offset": 250}
])";

} // namespace

suite<"worldgen/biome-parameters"> biome_parameters_tests = [] {
    "the loader preserves order and quantized intervals"_test = [] {
        const scratch_directory root;
        write_report(root, "overworld.json",
                     report(overworld_dimension, 2, two_entries));
        const auto parameters =
            load_biome_parameters(data_root(root), dimension_kind::overworld);
        expect(parameters.has_value()) << parameters.error().message;
        if (!parameters) {
            return;
        }
        expect(parameters->size() == 2U);
        if (parameters->size() != 2U) {
            return;
        }
        expect((*parameters)[0].biome == "test:cold");
        expect((*parameters)[1].biome == "test:warm");
        expect((*parameters)[0].parameters.temperature ==
               climate::span(-10000, 0));
        expect((*parameters)[1].parameters.temperature ==
               climate::span(1, 10000));
        expect((*parameters)[1].parameters.depth == climate::span(0, 10000));
        expect((*parameters)[1].parameters.offset == 250);
    };

    "a trailing separator still resolves the report"_test = [] {
        const scratch_directory root;
        write_report(root, "overworld.json",
                     report(overworld_dimension, 2, two_entries));
        const auto parameters = load_biome_parameters(
            data_root(root) + "/", dimension_kind::overworld);
        expect(parameters.has_value()) << parameters.error().message;
        if (parameters) {
            expect(parameters->size() == 2U);
        }
    };

    "an unsupported report format is rejected"_test = [] {
        const scratch_directory root;
        auto document = report(overworld_dimension, 1, R"([
    {"biome": "test:cold", "temperature": [0, 0], "humidity": [0, 0], "continentalness": [0, 0], "erosion": [0, 0], "depth": [0, 0], "weirdness": [0, 0], "offset": 0}
])");
        document.replace(document.find("\"format\": 1"),
                         std::string_view{"\"format\": 1"}.size(),
                         "\"format\": 2");
        write_report(root, "overworld.json", document);
        const auto parameters =
            load_biome_parameters(data_root(root), dimension_kind::overworld);
        expect(!parameters.has_value());
        if (parameters) {
            return;
        }
        expect(parameters.error().code == error_code::version_mismatch);
        expect(parameters.error().message.find("overworld.json") !=
               std::string::npos);
    };

    "a missing report names the path it looked for"_test = [] {
        const scratch_directory root;
        const auto parameters =
            load_biome_parameters(data_root(root), dimension_kind::overworld);
        expect(!parameters.has_value());
        if (parameters) {
            return;
        }
        expect(parameters.error().code == error_code::not_found);
        expect(parameters.error().message.find("reports/biome_parameters/"
                                               "overworld.json") !=
               std::string::npos);
    };

    "an entry count that disagrees with the metadata is rejected"_test = [] {
        const scratch_directory root;
        write_report(root, "overworld.json", report(overworld_dimension, 2, R"([
    {"biome": "test:cold", "temperature": [-10000, 0], "humidity": [-10000, 10000], "continentalness": [-10000, 10000], "erosion": [-10000, 10000], "depth": [0, 0], "weirdness": [-10000, 10000], "offset": 0}
])"));
        const auto parameters =
            load_biome_parameters(data_root(root), dimension_kind::overworld);
        expect(!parameters.has_value());
        if (parameters) {
            return;
        }
        expect(parameters.error().code == error_code::parse_error);
        expect(parameters.error().message.find("entry_count") !=
               std::string::npos);
    };

    "an interval with its minimum above its maximum is rejected"_test = [] {
        const scratch_directory root;
        write_report(root, "overworld.json", report(overworld_dimension, 1, R"([
    {"biome": "test:cold", "temperature": [10, -10], "humidity": [-10000, 10000], "continentalness": [-10000, 10000], "erosion": [-10000, 10000], "depth": [0, 0], "weirdness": [-10000, 10000], "offset": 0}
])"));
        const auto parameters =
            load_biome_parameters(data_root(root), dimension_kind::overworld);
        expect(!parameters.has_value());
        if (parameters) {
            return;
        }
        expect(parameters.error().code == error_code::parse_error);
        expect(parameters.error().message.find("temperature") !=
               std::string::npos);
    };

    "an unknown field is rejected instead of ignored"_test = [] {
        const scratch_directory root;
        write_report(root, "overworld.json", report(overworld_dimension, 1, R"([
    {"biome": "test:cold", "temperature": [0, 0], "humidity": [0, 0], "continentalness": [0, 0], "erosion": [0, 0], "depth": [0, 0], "weirdness": [0, 0], "offset": 0, "comment": "ignored?"}
])"));
        const auto parameters =
            load_biome_parameters(data_root(root), dimension_kind::overworld);
        expect(!parameters.has_value());
        if (parameters) {
            return;
        }
        expect(parameters.error().code == error_code::parse_error);
        expect(parameters.error().message.find("comment") != std::string::npos);
    };

    "a report for another dimension is rejected"_test = [] {
        const scratch_directory root;
        write_report(root, "overworld.json",
                     report("minecraft:the_nether", 1, R"([
    {"biome": "test:cold", "temperature": [0, 0], "humidity": [0, 0], "continentalness": [0, 0], "erosion": [0, 0], "depth": [0, 0], "weirdness": [0, 0], "offset": 0}
])"));
        const auto parameters =
            load_biome_parameters(data_root(root), dimension_kind::overworld);
        expect(!parameters.has_value());
        if (parameters) {
            return;
        }
        expect(parameters.error().code == error_code::parse_error);
        expect(parameters.error().message.find("minecraft:the_nether") !=
               std::string::npos);
    };

    "the shipped reports drive a multi-noise source"_test = [] {
        const auto root = refrain::test_support::vanilla_data_root();
        const auto report = (std::filesystem::path{root}.parent_path() /
                             "reports/biome_parameters/overworld.json")
                                .string();
        if (!core::io::is_regular_file(report)) {
            std::fprintf(stderr,
                         "skipping content-backed biome parameter cases: %s "
                         "is absent\n",
                         report.c_str());
            return;
        }

        const auto overworld =
            load_biome_parameters(root, dimension_kind::overworld);
        expect(overworld.has_value()) << overworld.error().message;
        if (!overworld) {
            return;
        }
        expect(overworld->size() > 1U);
        multi_noise_biome_source source{*overworld};
        expect(source.biomes().size() > 1U);
        expect(source.parameters().size() == overworld->size());

        const auto nether = load_biome_parameters(root, dimension_kind::nether);
        expect(nether.has_value()) << nether.error().message;
        if (!nether) {
            return;
        }
        expect(nether->size() > 1U);
        multi_noise_biome_source nether_source{*nether};
        for (const auto &parameter : *nether) {
            const auto &point = parameter.parameters;
            expect((point.temperature.min == point.temperature.max) &&
                   (point.depth.min == point.depth.max));
            const climate::target_point target{
                .temperature = point.temperature.min,
                .humidity = point.humidity.min,
                .continentalness = point.continentalness.min,
                .erosion = point.erosion.min,
                .depth = point.depth.min,
                .weirdness = point.weirdness.min,
            };
            expect(nether_source.biome(nether_source.biome_at(target)) ==
                   parameter.biome);
        }
    };
};
