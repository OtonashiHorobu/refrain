// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/content.hpp>
#include <refrain/worldgen/density/density_router.hpp>

#include <refrain/core/io.hpp>
#include <refrain/data/json.hpp>
#include <refrain/worldgen/density/density_json.hpp>

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

} // namespace

auto load_noise_settings(std::string_view worldgen_root,
                         std::string_view settings_id,
                         algorithm random_algorithm, jlong seed)
    -> result<noise_settings> {
    const auto materialized = materialize_worldgen_data(worldgen_root);
    if (!materialized) {
        return std::unexpected{materialized.error()};
    }
    worldgen_root = *materialized;
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

    // The document selects the algorithm of every positional draw; the
    // caller's value is used only when the document omits the field.
    const json *legacy_node = parsed_settings->find("legacy_random_source");
    bool legacy_random_source = false;
    if (legacy_node != nullptr) {
        if (const auto value = legacy_node->as_bool(); value.has_value()) {
            legacy_random_source = *value;
        }
    }
    if (legacy_random_source) {
        random_algorithm = algorithm::legacy;
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
    const auto *noise_node = parsed_settings->find("noise");
    if (noise_node == nullptr || !noise_node->is_object()) {
        return std::unexpected{fail("noise settings have no noise geometry")};
    }
    const auto *min_y_node = noise_node->find("min_y");
    const auto *height_node = noise_node->find("height");
    if (min_y_node == nullptr || height_node == nullptr ||
        !min_y_node->is_number() || !height_node->is_number()) {
        return std::unexpected{fail("noise geometry is incomplete")};
    }
    settings.min_y = static_cast<jint>(*min_y_node->as_double());
    settings.height = static_cast<jint>(*height_node->as_double());
    if (settings.height <= 0) {
        return std::unexpected{fail("noise geometry has no height")};
    }
    settings.legacy_random_source = legacy_random_source;
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
        if (value == nullptr) {
            return std::unexpected{fail("noise router field is missing")};
        }
        // The router fields accept an inline density node (a constant number,
        // an object or a reference), the same nodes the documents use.
        auto decoded = decode_density(value->dump(), documents);
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
    const json *rule = parsed_settings->find("material_rule");
    if (rule == nullptr || !rule->is_string()) {
        return std::unexpected{fail("noise settings have no material rule")};
    }
    settings.material_rule = std::string{*rule->as_string()};
    if (const json *sea = parsed_settings->find("sea_level");
        sea != nullptr && sea->is_number()) {
        settings.sea_level = static_cast<jint>(*sea->as_double());
    }
    if (const json *aquifers = parsed_settings->find("aquifers");
        aquifers != nullptr && aquifers->is_object()) {
        settings.aquifers = true;
        struct field {
            const char *id;
            sampler_ptr aquifer_config::*member;
        };
        const std::array<field, 6> aquifer_fields{{
            {"barrier", &aquifer_config::barrier},
            {"fluid_level_floodedness",
             &aquifer_config::fluid_level_floodedness},
            {"fluid_level_spread", &aquifer_config::fluid_level_spread},
            {"lava", &aquifer_config::lava},
            {"exclusion", &aquifer_config::exclusion},
            {"surface_level", &aquifer_config::surface_level},
        }};
        for (const auto &entry : aquifer_fields) {
            const json *value = aquifers->find(entry.id);
            if (value == nullptr) {
                return std::unexpected{fail("aquifer config is incomplete")};
            }
            auto decoded = decode_density(value->dump(), documents);
            if (!decoded) {
                return std::unexpected{decoded.error()};
            }
            settings.aquifer.*entry.member = std::move(*decoded);
        }
    }
    settings.documents = documents;
    return settings;
}

} // namespace refrain::worldgen::density
