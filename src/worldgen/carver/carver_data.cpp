// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/carver/carver_data.hpp>

#include <refrain/core/io.hpp>
#include <refrain/data/json.hpp>

#include <string>
#include <utility>
#include <vector>

namespace refrain::worldgen::carver {

namespace {

auto json_files(std::string_view directory)
    -> result<std::vector<std::string>> {
    const auto entries = core::io::list_directory(directory);
    if (!entries) {
        return std::unexpected{entries.error()};
    }
    std::vector<std::string> files;
    for (const auto &entry : *entries) {
        if (entry.size() > 5 && entry.substr(entry.size() - 5) == ".json") {
            files.push_back(entry);
        }
    }
    return files;
}

auto biome_carver_ids(const data::json::value &document)
    -> result<std::vector<std::string>> {
    const auto *carvers = document.find("carvers");
    if (carvers == nullptr) {
        return std::vector<std::string>{};
    }
    if (const auto single = carvers->as_string(); single.has_value()) {
        return std::vector<std::string>{std::string{*single}};
    }
    if (!carvers->is_array()) {
        return std::unexpected{
            make_error(error_code::parse_error, "biome carvers is not a list")};
    }
    std::vector<std::string> ids;
    for (const auto &entry : *carvers->as_array()) {
        const auto id = entry.as_string();
        if (!id) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "biome carver is not an id")};
        }
        ids.emplace_back(*id);
    }
    return ids;
}

} // namespace

auto load_carvers(std::string_view data_root) -> result<loaded_carvers> {
    loaded_carvers loaded;
    const auto namespaces = core::io::list_directory(data_root);
    if (!namespaces) {
        return std::unexpected{namespaces.error()};
    }

    for (const auto &namespace_name : *namespaces) {
        const auto base = std::string{data_root} + "/" + namespace_name;
        if (const auto files = json_files(base + "/worldgen/carver"); files) {
            for (const auto &file : *files) {
                const auto document = data::json::value::parse_file(
                    base + "/worldgen/carver/" + file);
                if (!document) {
                    return std::unexpected{document.error()};
                }
                auto carver = decode_carver(*document);
                if (!carver) {
                    return std::unexpected{carver.error()};
                }
                loaded.carvers.emplace(namespace_name + ":" +
                                           file.substr(0, file.size() - 5),
                                       std::move(*carver));
            }
        } else if (files.error().code != error_code::not_found) {
            return std::unexpected{files.error()};
        }

        if (const auto files = json_files(base + "/worldgen/biome"); files) {
            for (const auto &file : *files) {
                const auto document = data::json::value::parse_file(
                    base + "/worldgen/biome/" + file);
                if (!document) {
                    return std::unexpected{document.error()};
                }
                auto ids = biome_carver_ids(*document);
                if (!ids) {
                    return std::unexpected{ids.error()};
                }
                std::vector<const world_carver *> carvers;
                for (const auto &id : *ids) {
                    const auto found = loaded.carvers.find(id);
                    if (found == loaded.carvers.end()) {
                        return std::unexpected{
                            make_error(error_code::not_found,
                                       "biome references a missing carver")};
                    }
                    carvers.push_back(found->second.get());
                }
                loaded.biomes.by_biome.emplace(
                    namespace_name + ":" + file.substr(0, file.size() - 5),
                    std::move(carvers));
            }
        } else if (files.error().code != error_code::not_found) {
            return std::unexpected{files.error()};
        }
    }
    return loaded;
}

} // namespace refrain::worldgen::carver
