// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/world/saved_data.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::world {
namespace {

namespace nbt = data::nbt;

[[nodiscard]] auto failing(error_code code, std::string message)
    -> std::unexpected<error> {
    return std::unexpected{make_error(code, std::move(message))};
}

[[nodiscard]] auto valid_component(std::string_view text) -> bool {
    return !text.empty() && text.find('/') == std::string_view::npos &&
           text != "." && text != "..";
}

} // namespace

auto saved_data_path(std::string_view namespace_name, std::string_view name)
    -> result<std::string> {
    if (!valid_component(namespace_name) || !valid_component(name) ||
        !name.ends_with(".dat")) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       fmt::format("invalid saved data path '{}/{}'",
                                   namespace_name, name))};
    }
    const auto path{fmt::format("global/data/{}/{}", namespace_name, name)};
    if (!object_store::valid_path(path)) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "saved data path is unsafe")};
    }
    return path;
}

auto read_nbt_object(object_store &objects, std::string_view path,
                     std::size_t max_bytes) -> result<nbt::value> {
    auto bytes{objects.read(path, max_bytes)};
    if (!bytes) {
        return std::unexpected{bytes.error()};
    }
    return nbt::read(*bytes);
}

auto write_nbt_object(object_store &objects, std::string_view path,
                      const nbt::value &value, bool durable) -> status {
    auto bytes{nbt::write(value, "", nbt::compression::none)};
    if (!bytes) {
        return std::unexpected{bytes.error()};
    }
    return objects.write(path, *bytes, object_media::nbt, durable);
}

auto read_world_gen_settings(object_store &objects)
    -> result<world_gen_settings> {
    auto path{saved_data_path("minecraft", "world_gen_settings.dat")};
    if (!path) {
        return std::unexpected{path.error()};
    }
    auto document{read_nbt_object(objects, *path)};
    if (!document) {
        return std::unexpected{document.error()};
    }
    if (document->as_compound() == nullptr) {
        return failing(error_code::parse_error,
                       "world_gen_settings.dat root is not a compound");
    }
    world_gen_settings settings{};
    if (const auto *version{document->get("DataVersion")}; version != nullptr) {
        if (const auto value{version->as_int()}) {
            settings.data_version = *value;
        }
    }
    const auto *data{document->get("data")};
    if (data == nullptr || data->as_compound() == nullptr) {
        return failing(error_code::parse_error,
                       "world_gen_settings.dat has no data compound");
    }
    if (const auto *seed{data->get("seed")}; seed != nullptr) {
        if (const auto value{seed->as_long()}) {
            settings.seed = *value;
            settings.has_seed = true;
        } else if (const auto narrowed{seed->as_int()}) {
            settings.seed = *narrowed;
            settings.has_seed = true;
        }
    }
    if (const auto *generate{data->get("generate_structures")};
        generate != nullptr) {
        if (const auto value{generate->as_byte()}) {
            settings.generate_structures = *value != 0;
        }
    }
    return settings;
}

auto read_game_rules(object_store &objects)
    -> result<std::map<std::string, std::string>> {
    auto path{saved_data_path("minecraft", "game_rules.dat")};
    if (!path) {
        return std::unexpected{path.error()};
    }
    auto document{read_nbt_object(objects, *path)};
    if (!document) {
        return std::unexpected{document.error()};
    }
    const auto *data{document->get("data")};
    if (data == nullptr || data->as_compound() == nullptr) {
        return failing(error_code::parse_error,
                       "game_rules.dat has no data compound");
    }
    std::map<std::string, std::string> rules;
    for (const auto &[key, value] : *data->as_compound()) {
        if (const auto text{value.as_string()}) {
            rules.emplace(key, *text);
        }
    }
    return rules;
}

auto read_player_json(object_store &objects, std::string_view uuid,
                      std::string_view kind) -> result<std::string> {
    if (!object_store::valid_path(uuid) ||
        uuid.find('/') != std::string_view::npos) {
        return std::unexpected{
            make_error(error_code::invalid_argument, "invalid player uuid")};
    }
    if (kind != "advancements" && kind != "stats") {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "invalid player view kind")};
    }
    auto bytes{
        objects.read(fmt::format("players/{}/{}", uuid, kind), 16ULL << 20U)};
    if (!bytes) {
        return std::unexpected{bytes.error()};
    }
    std::string text(reinterpret_cast<const char *>(bytes->data()),
                     bytes->size());
    auto first{text.find_first_not_of(" \t\r\n")};
    if (first == std::string::npos || text[first] != '{') {
        return failing(
            error_code::parse_error,
            fmt::format("players/{}/{} is not a JSON object", uuid, kind));
    }
    return text;
}

auto list_player_views(object_store &objects)
    -> result<std::vector<std::string>> {
    auto paths{objects.list("players/")};
    if (!paths) {
        return std::unexpected{paths.error()};
    }
    std::set<std::string> players;
    for (const auto &path : *paths) {
        const auto relative{path.substr(std::string{"players/"}.size())};
        const auto slash{relative.find('/')};
        if (slash == std::string::npos) {
            continue;
        }
        const auto kind{relative.substr(slash + 1)};
        if (kind == "advancements" || kind == "stats") {
            players.insert(relative.substr(0, slash));
        }
    }
    return std::vector<std::string>{players.begin(), players.end()};
}

} // namespace refrain::world
