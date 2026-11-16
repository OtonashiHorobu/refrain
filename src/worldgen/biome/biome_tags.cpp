// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/biome/biome_tags.hpp>

#include <refrain/core/io.hpp>
#include <refrain/data/json.hpp>
#include <refrain/worldgen/content.hpp>

#include <algorithm>
#include <ranges>
#include <set>
#include <utility>
#include <vector>

namespace refrain::worldgen {

namespace {

auto collect_documents(const std::string &directory, const std::string &prefix,
                       std::unordered_map<std::string, data::json::value> &out)
    -> status {
    const auto entries = core::io::list_directory(directory);
    if (!entries) {
        return std::unexpected{entries.error()};
    }
    std::vector<std::string> sorted{entries->begin(), entries->end()};
    std::ranges::sort(sorted);
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

auto resolve_tag(
    std::string_view tag,
    const std::unordered_map<std::string, std::vector<std::string>> &entries,
    std::set<std::string, std::less<>> &visited, std::vector<std::string> &out)
    -> void {
    const auto found = entries.find(std::string{tag});
    if (found == entries.end() || !visited.insert(std::string{tag}).second) {
        return;
    }
    for (const auto &entry : found->second) {
        if (entry.size() > 1 && entry.front() == '#') {
            resolve_tag(entry.substr(1), entries, visited, out);
        } else {
            out.push_back(entry);
        }
    }
}

auto tag_matches(
    std::string_view tag, std::string_view biome,
    const std::unordered_map<std::string, std::vector<std::string>> &entries,
    std::set<std::string, std::less<>> &visited) -> bool {
    const auto found = entries.find(std::string{tag});
    if (found == entries.end() || !visited.insert(std::string{tag}).second) {
        return false;
    }
    for (const auto &entry : found->second) {
        if (entry.size() > 1 && entry.front() == '#') {
            if (tag_matches(entry.substr(1), biome, entries, visited)) {
                return true;
            }
        } else if (entry == biome) {
            return true;
        }
    }
    return false;
}

} // namespace

auto biome_tags::contains(std::string_view tag) const -> bool {
    return entries_.find(std::string{tag}) != entries_.end();
}

auto biome_tags::expand(std::string_view tag) const
    -> std::vector<std::string> {
    std::vector<std::string> out;
    std::set<std::string, std::less<>> visited;
    resolve_tag(tag, entries_, visited, out);
    return out;
}

auto biome_tags::matches(std::string_view tag, std::string_view biome) const
    -> bool {
    std::set<std::string, std::less<>> visited;
    return tag_matches(tag, biome, entries_, visited);
}

auto load_biome_tags(std::string_view data_root) -> result<biome_tags> {
    const auto materialized = materialize_worldgen_data(data_root);
    if (!materialized) {
        return std::unexpected{materialized.error()};
    }
    data_root = *materialized;
    const auto namespaces = core::io::list_directory(data_root);
    if (!namespaces) {
        return std::unexpected{namespaces.error()};
    }
    std::vector<std::string> sorted{namespaces->begin(), namespaces->end()};
    std::ranges::sort(sorted);
    std::unordered_map<std::string, std::vector<std::string>> entries;
    for (const auto &name : sorted) {
        const auto base = std::string{data_root} + "/" + name;
        if (!core::io::is_directory(base)) {
            continue;
        }
        const auto tags_root = base + "/tags/worldgen/biome";
        if (!core::io::is_directory(tags_root)) {
            continue;
        }
        std::unordered_map<std::string, data::json::value> documents;
        if (const auto collected = collect_documents(tags_root, "", documents);
            !collected) {
            return std::unexpected{collected.error()};
        }
        for (const auto &[id, document] : documents) {
            auto ids = collect_tag_ids(document);
            if (!ids) {
                return std::unexpected{
                    make_error(ids.error().code, ids.error().message + " (" +
                                                     name + ":" + id + ")")};
            }
            auto &values = entries[name + ":" + id];
            values.append_range(*ids | std::views::as_rvalue);
        }
    }
    return biome_tags{std::move(entries)};
}

} // namespace refrain::worldgen
