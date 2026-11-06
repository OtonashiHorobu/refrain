// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/density_router.hpp>

#include <refrain/core/io.hpp>
#include <refrain/data/json.hpp>
#include <refrain/worldgen/density_json.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace refrain::worldgen::density {

namespace {

using json = data::json::value;

auto fail(std::string_view message) -> error {
    return make_error(error_code::invalid_argument, message);
}

/// Collects `<data_root>/<namespace>/worldgen/<directory>/**/*.json`.
auto collect_documents(std::string_view data_root, std::string_view directory,
                       std::vector<std::pair<std::string, std::string>> &out)
    -> status {
    auto namespaces = core::io::list_directory(data_root);
    if (!namespaces) {
        return std::unexpected{namespaces.error()};
    }
    for (const auto &name : *namespaces) {
        const std::string base = std::string{data_root} + "/" + name +
                                 "/worldgen/" + std::string{directory};
        if (!core::io::is_directory(base)) {
            continue;
        }
        std::vector<std::pair<std::string, std::string>> pending;
        auto walk = [&](auto &&self, std::string_view path,
                        std::string_view prefix) -> status {
            auto entries = core::io::list_directory(path);
            if (!entries) {
                return std::unexpected{entries.error()};
            }
            for (const auto &entry : *entries) {
                const std::string child = std::string{path} + "/" + entry;
                const std::string id_path =
                    prefix.empty() ? entry : std::string{prefix} + "/" + entry;
                if (core::io::is_directory(child)) {
                    if (auto walked = self(self, child, id_path); !walked) {
                        return walked;
                    }
                } else if (entry.ends_with(".json")) {
                    auto text = core::io::read_text(child);
                    if (!text) {
                        return std::unexpected{text.error()};
                    }
                    pending.emplace_back(
                        name + ":" + id_path.substr(0, id_path.size() - 5),
                        std::move(*text));
                }
            }
            return status_ok();
        };
        if (auto walked = walk(walk, base, ""); !walked) {
            return walked;
        }
        std::sort(pending.begin(), pending.end());
        for (auto &entry : pending) {
            out.push_back(std::move(entry));
        }
    }
    return status_ok();
}

auto decode_by_id(std::string_view id, const density_documents &documents)
    -> result<sampler_ptr> {
    const std::string quoted = "\"" + std::string{id} + "\"";
    return decode_density(quoted, documents);
}

} // namespace

auto load_noise_settings(std::string_view worldgen_root,
                         std::string_view settings_id,
                         algorithm random_algorithm, jlong seed)
    -> result<noise_settings> {
    const std::string settings_id_text{settings_id};
    const auto slash = settings_id_text.find('/');
    if (slash == std::string::npos) {
        return std::unexpected{fail("settings id must be <namespace>/<path>")};
    }
    const std::string settings_path =
        std::string{worldgen_root} + "/" + settings_id_text.substr(0, slash) +
        "/worldgen/noise_settings/" + settings_id_text.substr(slash + 1) +
        ".json";
    auto settings_text = core::io::read_text(settings_path);
    if (!settings_text) {
        return std::unexpected{settings_text.error()};
    }
    auto parsed_settings = json::parse(*settings_text);
    if (!parsed_settings) {
        return std::unexpected{parsed_settings.error()};
    }

    density_documents documents;
    documents.seed = seed;
    documents.random_algorithm = random_algorithm;
    if (auto collected = collect_documents(worldgen_root, "density_function",
                                           documents.functions);
        !collected) {
        return std::unexpected{collected.error()};
    }
    if (auto collected =
            collect_documents(worldgen_root, "noise", documents.noises);
        !collected) {
        return std::unexpected{collected.error()};
    }

    noise_settings settings;
    const auto router_node = parsed_settings->find("noise_router");
    if (router_node == nullptr || !router_node->is_object()) {
        return std::unexpected{fail("noise settings have no router")};
    }
    struct router_field {
        const char *id;
        sampler_ptr noise_router::*member;
    };
    const std::array<router_field, 8> fields{{
        {"temperature", &noise_router::temperature},
        {"vegetation", &noise_router::vegetation},
        {"continents", &noise_router::continents},
        {"erosion", &noise_router::erosion},
        {"depth", &noise_router::depth},
        {"ridges", &noise_router::ridges},
        {"final_density", &noise_router::final_density},
        {"chunk_surface_level", &noise_router::chunk_surface_level},
    }};
    for (const auto &field : fields) {
        const json *value = router_node->find(field.id);
        if (value == nullptr || !value->is_string()) {
            return std::unexpected{fail("noise router field is missing")};
        }
        auto decoded = decode_by_id(*value->as_string(), documents);
        if (!decoded) {
            return std::unexpected{decoded.error()};
        }
        settings.router.*field.member = std::move(*decoded);
    }
    if (const json *block = parsed_settings->find("default_block");
        block != nullptr && block->is_string()) {
        settings.default_block = std::string{*block->as_string()};
    }
    if (const json *fluid = parsed_settings->find("default_fluid");
        fluid != nullptr && fluid->is_string()) {
        settings.default_fluid = std::string{*fluid->as_string()};
    }
    if (const json *sea = parsed_settings->find("sea_level");
        sea != nullptr && sea->is_number()) {
        settings.sea_level = static_cast<jint>(*sea->as_double());
    }
    settings.aquifers = parsed_settings->find("aquifers") != nullptr;
    return settings;
}

} // namespace refrain::worldgen::density
