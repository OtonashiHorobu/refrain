// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/content.hpp>
#include <refrain/worldgen/structure/structure_data.hpp>

#include <refrain/core/io.hpp>

#include <algorithm>
#include <set>
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

auto collect_tag_ids(const data::json::value &document)
    -> result<std::vector<std::string>> {
    const data::json::value *values = &document;
    if (document.is_object()) {
        values = document.find("values");
    }
    if (values == nullptr || !values->is_array()) {
        return std::unexpected{
            make_error(error_code::parse_error, "biome tag has no values")};
    }
    std::vector<std::string> ids;
    for (const auto &entry : *values->as_array()) {
        if (const auto text = entry.as_string(); text.has_value()) {
            ids.emplace_back(*text);
            continue;
        }
        const auto *id = entry.find("id");
        if (id == nullptr) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "biome tag entry has no id")};
        }
        const auto text = id->as_string();
        if (!text) {
            return std::unexpected{text.error()};
        }
        ids.emplace_back(*text);
    }
    return ids;
}

void resolve_tag(
    const std::string &tag,
    const std::unordered_map<std::string, std::vector<std::string>> &tags,
    std::set<std::string> &visited, std::vector<std::string> &out) {
    const auto found = tags.find(tag);
    if (found == tags.end() || !visited.insert(tag).second) {
        return;
    }
    for (const auto &entry : found->second) {
        if (!entry.empty() && entry.front() == '#') {
            resolve_tag(entry.substr(1), tags, visited, out);
        } else {
            out.push_back(entry);
        }
    }
}

} // namespace

auto structure_data_set::allows_biome(const structure_document &structure,
                                      std::string_view biome) const -> bool {
    return std::find(structure.biome_ids.begin(), structure.biome_ids.end(),
                     biome) != structure.biome_ids.end();
}

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
        if (const auto name = biomes->as_string(); name.has_value()) {
            if (name->empty() || name->front() != '#') {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "structure biome string is not a tag")};
            }
            decoded.biome_tag = name->substr(1);
        } else if (const auto ids = collect_tag_ids(*biomes); ids) {
            decoded.biome_ids = *ids;
        } else {
            return std::unexpected{ids.error()};
        }
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
    const auto materialized = materialize_worldgen_data(data_root);
    if (!materialized) {
        return std::unexpected{materialized.error()};
    }
    data_root = *materialized;
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
        const auto tags_root = base + "/tags/worldgen/biome";
        if (core::io::is_directory(tags_root)) {
            std::unordered_map<std::string, data::json::value> documents;
            if (const auto collected =
                    collect_documents(tags_root, "", documents);
                !collected) {
                return std::unexpected{collected.error()};
            }
            for (const auto &[id, document] : documents) {
                auto ids = collect_tag_ids(document);
                if (!ids) {
                    return std::unexpected{
                        make_error(ids.error().code,
                                   ids.error().message + " (" + name + ":" +
                                       id + ") " + document.dump())};
                }
                auto &values = result.biome_tags[name + ":" + id];
                values.insert(values.end(),
                              std::make_move_iterator(ids->begin()),
                              std::make_move_iterator(ids->end()));
            }
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
            if (!decoded->biome_tag.empty()) {
                std::set<std::string> visited;
                resolve_tag(decoded->biome_tag, result.biome_tags, visited,
                            decoded->biome_ids);
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
