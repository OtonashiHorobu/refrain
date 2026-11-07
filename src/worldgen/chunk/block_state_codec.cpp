// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/chunk/block_state_codec.hpp>

#include <refrain/data/json.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace refrain::worldgen {

namespace {

auto property_text(const data::json::value &value) -> result<std::string> {
    if (const auto text = value.as_string(); text.has_value()) {
        return std::string{*text};
    }
    if (const auto boolean = value.as_bool(); boolean.has_value()) {
        return *boolean ? "true" : "false";
    }
    if (const auto integer = value.as_int64(); integer.has_value()) {
        return std::to_string(*integer);
    }
    return std::unexpected{
        make_error(error_code::parse_error, "property value is not scalar")};
}

} // namespace

auto state_block_name(std::string_view key) -> std::string_view {
    const auto bracket = key.find('[');
    return bracket == std::string_view::npos ? key : key.substr(0, bracket);
}

auto is_air_block_name(std::string_view name) -> bool {
    return name == "minecraft:air" || name == "minecraft:cave_air" ||
           name == "minecraft:void_air";
}

auto decode_state_key(const data::json::value &value) -> result<std::string> {
    if (const auto text = value.as_string(); text.has_value()) {
        return std::string{*text};
    }
    if (!value.is_object()) {
        return std::unexpected{make_error(error_code::parse_error,
                                          "block state is not an object")};
    }
    const data::json::value *id = value.find("id");
    if (id == nullptr) {
        id = value.find("Name");
    }
    if (id == nullptr) {
        return std::unexpected{
            make_error(error_code::parse_error, "block state has no id")};
    }
    const auto name = id->as_string();
    if (!name) {
        return std::unexpected{make_error(error_code::parse_error,
                                          "block state id is not a string")};
    }
    std::string key{*name};
    const auto *properties = value.find("properties");
    if (properties == nullptr || !properties->is_object()) {
        return key;
    }
    const auto object = properties->as_object();
    if (!object) {
        return std::unexpected{object.error()};
    }
    std::vector<std::pair<std::string, std::string>> sorted;
    for (const auto &[property, raw] : object->get()) {
        auto text = property_text(raw);
        if (!text) {
            return std::unexpected{text.error()};
        }
        sorted.emplace_back(property, std::move(*text));
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const auto &left, const auto &right) {
                  return left.first < right.first;
              });
    key.push_back('[');
    for (std::size_t i = 0; i < sorted.size(); ++i) {
        if (i != 0) {
            key.push_back(',');
        }
        key.append(sorted[i].first);
        key.push_back('=');
        key.append(sorted[i].second);
    }
    key.push_back(']');
    return key;
}

} // namespace refrain::worldgen
