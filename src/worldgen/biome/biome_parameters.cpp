// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/biome/biome_parameters.hpp>

#include <refrain/core/io.hpp>
#include <refrain/data/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::worldgen {

namespace {

using json = data::json::value;

constexpr std::string_view entry_fields[] = {
    "biome",   "temperature", "humidity",  "continentalness",
    "erosion", "depth",       "weirdness", "offset",
};

constexpr std::string_view document_fields[] = {
    "format",    "game_version", "data_version",
    "dimension", "entry_count",  "entries",
};

auto document_error(std::string_view path, std::string_view message) -> error {
    std::string text{"biome parameter report "};
    text += path;
    text += ": ";
    text += message;
    return make_error(error_code::parse_error, text);
}

auto supported_format(const json &document, std::string_view path)
    -> result<std::int64_t> {
    const auto *field = document.find("format");
    if (field == nullptr) {
        return std::unexpected{document_error(path, "format is missing")};
    }
    const auto format = field->as_int64();
    if (!format) {
        return std::unexpected{
            document_error(path, "format is not an integer")};
    }
    if (*format != 1) {
        return std::unexpected{make_error(error_code::version_mismatch,
                                          "biome parameter report " +
                                              std::string{path} +
                                              ": format is not supported")};
    }
    return *format;
}

auto required(const json &document, std::string_view key) -> const json * {
    return document.find(key);
}

auto decode_interval(const json &entry, std::string_view key, std::size_t index)
    -> result<climate::parameter> {
    const auto *field = entry.find(key);
    if (field == nullptr) {
        return std::unexpected{make_error(error_code::parse_error,
                                          "biome parameter entry " +
                                              std::to_string(index) +
                                              " has no " + std::string{key})};
    }
    const auto values = field->as_array();
    if (!values || values->size() != 2) {
        return std::unexpected{make_error(
            error_code::parse_error,
            "biome parameter entry " + std::to_string(index) + " field " +
                std::string{key} + " is not a two-element interval")};
    }
    const auto minimum = (*values)[0].as_int64();
    const auto maximum = (*values)[1].as_int64();
    if (!minimum || !maximum) {
        return std::unexpected{make_error(
            error_code::parse_error,
            "biome parameter entry " + std::to_string(index) + " field " +
                std::string{key} + " is not an integer interval")};
    }
    if (*minimum > *maximum) {
        return std::unexpected{make_error(
            error_code::parse_error,
            "biome parameter entry " + std::to_string(index) + " field " +
                std::string{key} + " has a minimum above its maximum")};
    }
    return climate::span(static_cast<climate::coord>(*minimum),
                         static_cast<climate::coord>(*maximum));
}

auto decode_offset(const json &entry, std::size_t index)
    -> result<climate::coord> {
    const auto *field = entry.find("offset");
    if (field == nullptr) {
        return std::unexpected{
            make_error(error_code::parse_error, "biome parameter entry " +
                                                    std::to_string(index) +
                                                    " has no offset")};
    }
    const auto offset = field->as_int64();
    if (!offset) {
        return std::unexpected{make_error(error_code::parse_error,
                                          "biome parameter entry " +
                                              std::to_string(index) +
                                              " offset is not an integer")};
    }
    return static_cast<climate::coord>(*offset);
}

auto decode_biome(const json &entry, std::size_t index) -> result<std::string> {
    const auto *field = entry.find("biome");
    if (field == nullptr) {
        return std::unexpected{
            make_error(error_code::parse_error, "biome parameter entry " +
                                                    std::to_string(index) +
                                                    " has no biome")};
    }
    const auto biome = field->as_string();
    if (!biome) {
        return std::unexpected{
            make_error(error_code::parse_error, "biome parameter entry " +
                                                    std::to_string(index) +
                                                    " biome is not a string")};
    }
    const auto separator = biome->find(':');
    if (separator == std::string_view::npos || separator == 0 ||
        separator + 1 >= biome->size()) {
        return std::unexpected{make_error(error_code::parse_error,
                                          "biome parameter entry " +
                                              std::to_string(index) +
                                              " biome is not a namespaced id")};
    }
    return std::string{*biome};
}

auto reject_unknown_fields(const json &object, std::string_view context,
                           std::span<const std::string_view> known) -> status {
    for (const auto &[key, value] : object.as_object()->get()) {
        (void)value;
        bool found = false;
        for (const auto field : known) {
            found = found || key == field;
        }
        if (!found) {
            std::string message{context};
            message += " has an unknown field ";
            message += key;
            return std::unexpected{
                make_error(error_code::parse_error, std::move(message))};
        }
    }
    return status_ok();
}

auto report_stem(dimension_kind dimension) -> std::string_view {
    switch (dimension) {
    case dimension_kind::nether:
        return "nether";
    case dimension_kind::end:
        return "end";
    case dimension_kind::overworld:
    default:
        return "overworld";
    }
}

auto report_path(std::string_view data_root, dimension_kind dimension)
    -> std::string {
    while (data_root.size() > 1 &&
           (data_root.back() == '/' || data_root.back() == '\\')) {
        data_root.remove_suffix(1);
    }
    std::string path = core::io::parent_directory(data_root);
    path += "/reports/biome_parameters/";
    path += report_stem(dimension);
    path += ".json";
    return path;
}

} // namespace

auto load_biome_parameters(std::string_view data_root, dimension_kind dimension)
    -> result<std::vector<biome_parameter>> {
    if (data_root.empty()) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "biome parameter data root is empty")};
    }
    const std::string path = report_path(data_root, dimension);
    if (!core::io::is_regular_file(path)) {
        return std::unexpected{
            make_error(error_code::not_found,
                       "biome parameter report not found at " + path)};
    }
    auto document = json::parse_file(path);
    if (!document) {
        return std::unexpected{document.error()};
    }
    if (!document->is_object()) {
        return std::unexpected{
            document_error(path, "the document is not an object")};
    }
    if (auto unknown = reject_unknown_fields(
            *document, "biome parameter report " + path, document_fields);
        !unknown) {
        return std::unexpected{unknown.error()};
    }
    if (auto format = supported_format(*document, path); !format) {
        return std::unexpected{format.error()};
    }

    const auto *version = required(*document, "game_version");
    if (version == nullptr || !version->as_string() ||
        version->as_string()->empty()) {
        return std::unexpected{
            document_error(path, "game_version is missing or empty")};
    }
    if (const auto *data_version = required(*document, "data_version");
        data_version != nullptr) {
        const auto number = data_version->as_int64();
        if (!number || *number < 0) {
            return std::unexpected{
                document_error(path, "data_version is not a non-negative "
                                     "integer")};
        }
    }
    const auto *dimension_field = required(*document, "dimension");
    if (dimension_field == nullptr || !dimension_field->as_string()) {
        return std::unexpected{
            document_error(path, "dimension is missing or not a string")};
    }
    if (*dimension_field->as_string() != dimension_name(dimension)) {
        return std::unexpected{document_error(
            path, "dimension is " + std::string{*dimension_field->as_string()} +
                      " but " + std::string{dimension_name(dimension)} +
                      " was requested")};
    }

    const auto *count_field = required(*document, "entry_count");
    if (count_field == nullptr) {
        return std::unexpected{document_error(path, "entry_count is missing")};
    }
    const auto count = count_field->as_int64();
    if (!count || *count <= 0) {
        return std::unexpected{
            document_error(path, "entry_count is not a positive integer")};
    }

    const auto *entry_field = required(*document, "entries");
    if (entry_field == nullptr || !entry_field->is_array()) {
        return std::unexpected{
            document_error(path, "entries is missing or not an array")};
    }
    const auto entries = entry_field->as_array();
    if (static_cast<std::int64_t>(entries->size()) != *count) {
        return std::unexpected{document_error(
            path, "entry_count does not match the number of entries")};
    }

    std::vector<biome_parameter> parameters;
    parameters.reserve(entries->size());
    for (std::size_t index = 0; index < entries->size(); ++index) {
        const auto &entry = (*entries)[index];
        if (!entry.is_object()) {
            return std::unexpected{document_error(
                path, "entry " + std::to_string(index) + " is not an object")};
        }
        if (auto unknown = reject_unknown_fields(
                entry, "biome parameter entry " + std::to_string(index),
                entry_fields);
            !unknown) {
            return std::unexpected{unknown.error()};
        }
        auto biome = decode_biome(entry, index);
        if (!biome) {
            return std::unexpected{document_error(path, biome.error().message)};
        }
        auto temperature = decode_interval(entry, "temperature", index);
        auto humidity = decode_interval(entry, "humidity", index);
        auto continentalness = decode_interval(entry, "continentalness", index);
        auto erosion = decode_interval(entry, "erosion", index);
        auto depth = decode_interval(entry, "depth", index);
        auto weirdness = decode_interval(entry, "weirdness", index);
        auto offset = decode_offset(entry, index);
        if (!temperature) {
            return std::unexpected{
                document_error(path, temperature.error().message)};
        }
        if (!humidity) {
            return std::unexpected{
                document_error(path, humidity.error().message)};
        }
        if (!continentalness) {
            return std::unexpected{
                document_error(path, continentalness.error().message)};
        }
        if (!erosion) {
            return std::unexpected{
                document_error(path, erosion.error().message)};
        }
        if (!depth) {
            return std::unexpected{document_error(path, depth.error().message)};
        }
        if (!weirdness) {
            return std::unexpected{
                document_error(path, weirdness.error().message)};
        }
        if (!offset) {
            return std::unexpected{
                document_error(path, offset.error().message)};
        }
        parameters.push_back(biome_parameter{
            .parameters =
                climate::parameter_point{
                    .temperature = *temperature,
                    .humidity = *humidity,
                    .continentalness = *continentalness,
                    .erosion = *erosion,
                    .depth = *depth,
                    .weirdness = *weirdness,
                    .offset = *offset,
                },
            .biome = std::move(*biome),
        });
    }
    return parameters;
}

} // namespace refrain::worldgen
