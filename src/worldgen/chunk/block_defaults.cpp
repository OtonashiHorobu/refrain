// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/chunk/block_defaults.hpp>

#include <refrain/core/io.hpp>
#include <refrain/data/json.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

namespace refrain::worldgen {

namespace {

using json = data::json::value;

auto report_path(std::string_view data_root) -> std::string {
    while (data_root.size() > 1 &&
           (data_root.back() == '/' || data_root.back() == '\\')) {
        data_root.remove_suffix(1);
    }
    std::string path = core::io::parent_directory(data_root);
    path += "/reports/block_defaults/defaults.json";
    return path;
}

auto document_error(std::string_view path, std::string_view message) -> error {
    std::string text{"block default report "};
    text += path;
    text += ": ";
    text += message;
    return make_error(error_code::parse_error, text);
}

} // namespace

auto load_block_defaults(std::string_view data_root)
    -> result<block_default_map> {
    if (data_root.empty()) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "block default data root is empty")};
    }
    const std::string path = report_path(data_root);
    if (!core::io::is_regular_file(path)) {
        return std::unexpected{
            make_error(error_code::not_found,
                       "block default report not found at " + path)};
    }
    auto document = json::value::parse_file(path);
    if (!document) {
        return std::unexpected{document.error()};
    }
    if (!document->is_object()) {
        return std::unexpected{
            document_error(path, "the document is not an object")};
    }
    const auto *version = document->find("game_version");
    if (version == nullptr || !version->as_string() ||
        version->as_string()->empty()) {
        return std::unexpected{
            document_error(path, "game_version is missing or empty")};
    }
    if (const auto *data_version = document->find("data_version");
        data_version != nullptr) {
        const auto number = data_version->as_int64();
        if (!number || *number < 0) {
            return std::unexpected{document_error(
                path, "data_version is not a non-negative integer")};
        }
    }
    const auto *blocks = document->find("blocks");
    if (blocks == nullptr || !blocks->is_object()) {
        return std::unexpected{
            document_error(path, "blocks is missing or not an object")};
    }
    const auto document_blocks = blocks->as_object();
    if (!document_blocks) {
        return std::unexpected{document_blocks.error()};
    }
    block_default_map defaults;
    for (const auto &[block, values] : document_blocks->get()) {
        if (!values.is_object()) {
            return std::unexpected{document_error(
                path, "default properties of " + block + " are not an object")};
        }
        const auto properties = values.as_object();
        if (!properties) {
            return std::unexpected{properties.error()};
        }
        block_default_properties decoded;
        for (const auto &[name, value] : properties->get()) {
            const auto text = value.as_string();
            if (!text) {
                return std::unexpected{
                    document_error(path, "default property " + name + " of " +
                                             block + " is not a string")};
            }
            decoded.push_back(block_default_property{
                .name = name, .value = std::string{*text}});
        }
        std::sort(decoded.begin(), decoded.end(),
                  [](const block_default_property &left,
                     const block_default_property &right) {
                      return left.name < right.name;
                  });
        defaults.emplace(block, std::move(decoded));
    }
    return defaults;
}

} // namespace refrain::worldgen
