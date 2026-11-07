// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/structure/structure_data.hpp>

#include <refrain/core/io.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace refrain::worldgen::structure {

namespace {

auto adaptation_from(std::string_view name) -> terrain_adaptation {
    if (name == "beard_thin") {
        return terrain_adaptation::beard_thin;
    }
    if (name == "beard_box") {
        return terrain_adaptation::beard_box;
    }
    if (name == "bury") {
        return terrain_adaptation::bury;
    }
    if (name == "encapsulate") {
        return terrain_adaptation::encapsulate;
    }
    return terrain_adaptation::none;
}

auto collect_documents(const std::string &directory, const std::string &prefix,
                       std::unordered_map<std::string, data::json::value> &out)
    -> status {
    const auto entries = core::io::list_directory(directory);
    if (!entries) {
        return std::unexpected{entries.error()};
    }
    std::vector<std::string> sorted{entries->begin(), entries->end()};
    std::sort(sorted.begin(), sorted.end());
    for (const auto &entry : sorted) {
        const auto path = directory + "/" + entry;
        const auto id = prefix.empty() ? entry : prefix + "/" + entry;
        if (core::io::is_directory(path)) {
            if (const auto collected = collect_documents(path, id, out);
                !collected) {
                return collected;
            }
            continue;
        }
        if (entry.size() <= 5 || entry.substr(entry.size() - 5) != ".json") {
            continue;
        }
        auto document = data::json::value::parse_file(path);
        if (!document) {
            return std::unexpected{document.error()};
        }
        out.emplace(id.substr(0, id.size() - 5), std::move(*document));
    }
    return {};
}

} // namespace

auto decode_structure_document(std::string_view id,
                               const data::json::value &document)
    -> result<structure_document> {
    const auto *type = document.find("type");
    const auto *step = document.find("step");
    if (type == nullptr || step == nullptr) {
        return std::unexpected{make_error(error_code::parse_error,
                                          "structure document is incomplete")};
    }
    const auto type_name = type->as_string();
    const auto step_name = step->as_string();
    if (!type_name || !step_name) {
        return std::unexpected{make_error(
            error_code::parse_error, "structure type/step is not a string")};
    }
    structure_document decoded{
        .id = std::string{id},
        .type = std::string{*type_name},
        .step = std::string{*step_name},
        .config = document,
    };
    if (const auto *biomes = document.find("biomes"); biomes != nullptr) {
        decoded.biomes = *biomes;
    }
    if (const auto *adaptation = document.find("terrain_adaptation");
        adaptation != nullptr) {
        const auto name = adaptation->as_string();
        if (!name) {
            return std::unexpected{name.error()};
        }
        decoded.adaptation = adaptation_from(*name);
    }
    return decoded;
}

auto load_structure_data(std::string_view data_root)
    -> result<structure_data_set> {
    structure_data_set result;
    const auto namespaces = core::io::list_directory(data_root);
    if (!namespaces) {
        return std::unexpected{namespaces.error()};
    }
    std::vector<std::string> sorted{namespaces->begin(), namespaces->end()};
    std::sort(sorted.begin(), sorted.end());
    for (const auto &name : sorted) {
        const auto base = std::string{data_root} + "/" + name;
        if (!core::io::is_directory(base)) {
            continue;
        }
        const auto worldgen = base + "/worldgen";
        std::unordered_map<std::string, data::json::value> structures;
        if (core::io::is_directory(worldgen + "/structure")) {
            if (const auto collected =
                    collect_documents(worldgen + "/structure", "", structures);
                !collected) {
                return std::unexpected{collected.error()};
            }
        }
        for (auto &[id, document] : structures) {
            const auto full_id = name + ":" + id;
            auto decoded = decode_structure_document(full_id, document);
            if (!decoded) {
                return std::unexpected{decoded.error()};
            }
            result.structures.emplace(full_id, std::move(*decoded));
        }
        std::unordered_map<std::string, data::json::value> sets;
        if (core::io::is_directory(worldgen + "/structure_set")) {
            if (const auto collected =
                    collect_documents(worldgen + "/structure_set", "", sets);
                !collected) {
                return std::unexpected{collected.error()};
            }
        }
        for (auto &[id, document] : sets) {
            const auto full_id = name + ":" + id;
            auto decoded = decode_structure_set(full_id, document);
            if (!decoded) {
                return std::unexpected{decoded.error()};
            }
            result.sets.emplace(full_id, std::move(*decoded));
        }
    }
    return result;
}

} // namespace refrain::worldgen::structure
