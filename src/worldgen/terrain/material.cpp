// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/terrain/material.hpp>

#include <refrain/core/io.hpp>
#include <refrain/data/json.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

namespace refrain::worldgen::surface {

namespace {

using json = data::json::value;

auto fail(std::string_view message) -> error {
    return make_error(error_code::parse_error, message);
}

auto string_member(const json &node, std::string_view key)
    -> std::optional<std::string> {
    const json *found = node.find(key);
    if (found == nullptr) {
        return std::nullopt;
    }
    const auto value = found->as_string();
    return value ? std::optional<std::string>{std::string{*value}}
                 : std::nullopt;
}

auto double_member(const json &node, std::string_view key)
    -> std::optional<double> {
    const json *found = node.find(key);
    if (found == nullptr) {
        return std::nullopt;
    }
    const auto value = found->as_double();
    return value ? std::optional<double>{*value} : std::nullopt;
}

auto int_member(const json &node, std::string_view key) -> std::optional<jint> {
    const auto value = double_member(node, key);
    return value ? std::optional<jint>{static_cast<jint>(*value)}
                 : std::nullopt;
}

auto bool_member(const json &node, std::string_view key)
    -> std::optional<bool> {
    const json *found = node.find(key);
    if (found == nullptr) {
        return std::nullopt;
    }
    const auto value = found->as_bool();
    return value ? std::optional<bool>{*value} : std::nullopt;
}

auto type_of(const json &node) -> std::optional<std::string> {
    return string_member(node, "type");
}

auto decode_anchor(const json &node) -> result<vertical_anchor> {
    if (!node.is_object()) {
        return std::unexpected{fail("anchor is not an object")};
    }
    for (const auto &[key, kind] : std::initializer_list<
             std::pair<std::string_view, vertical_anchor::kind>>{
             {"absolute", vertical_anchor::kind::absolute},
             {"above_bottom", vertical_anchor::kind::above_bottom},
             {"below_top", vertical_anchor::kind::below_top},
             {"relative_to_sea_level",
              vertical_anchor::kind::relative_to_sea_level}}) {
        if (const auto value = int_member(node, key)) {
            return vertical_anchor{.type = kind, .value = *value};
        }
    }
    return std::unexpected{fail("unknown vertical anchor")};
}

auto decode_biome_set(const json &node) -> std::vector<std::string> {
    std::vector<std::string> result;
    if (const auto single = node.as_string()) {
        result.emplace_back(*single);
        return result;
    }
    if (const auto array = node.as_array()) {
        for (const auto &entry : *array) {
            if (const auto key = entry.as_string()) {
                result.emplace_back(*key);
            }
        }
    }
    return result;
}

auto decode_block_state(const json &node) -> std::optional<std::string> {
    if (const auto value = node.as_string()) {
        return std::string{*value};
    }
    if (node.is_object()) {
        return string_member(node, "Name");
    }
    return std::nullopt;
}

auto decode_condition_value(const json &node) -> result<condition>;

auto decode_condition_object(const json &node) -> result<condition> {
    const auto type = type_of(node);
    if (!type) {
        return std::unexpected{fail("material condition has no type")};
    }
    condition decoded;
    if (*type == "minecraft:biome") {
        const json *biomes = node.find("biome_is");
        if (biomes == nullptr) {
            return std::unexpected{fail("biome condition has no biome_is")};
        }
        decoded.type = condition::kind::biome;
        decoded.biome_is = decode_biome_set(*biomes);
    } else if (*type == "minecraft:noise_threshold") {
        const auto noise = string_member(node, "noise");
        const auto min = double_member(node, "min_threshold");
        const auto max = double_member(node, "max_threshold");
        if (!noise || !min || !max) {
            return std::unexpected{fail("noise_threshold is incomplete")};
        }
        decoded.type = condition::kind::noise_threshold;
        decoded.noise = *noise;
        decoded.min_threshold = *min;
        decoded.max_threshold = *max;
        decoded.is_3d = bool_member(node, "is_3d").value_or(false);
    } else if (*type == "minecraft:vertical_gradient") {
        const auto name = string_member(node, "random_name");
        const json *true_below = node.find("true_at_and_below");
        const json *false_above = node.find("false_at_and_above");
        if (!name || true_below == nullptr || false_above == nullptr) {
            return std::unexpected{fail("vertical_gradient is incomplete")};
        }
        auto below = decode_anchor(*true_below);
        auto above = decode_anchor(*false_above);
        if (!below || !above) {
            return std::unexpected{fail("vertical_gradient anchor is invalid")};
        }
        decoded.type = condition::kind::vertical_gradient;
        decoded.random_name = *name;
        decoded.true_at_and_below = *below;
        decoded.false_at_and_above = *above;
    } else if (*type == "minecraft:y_above") {
        const json *anchor = node.find("anchor");
        const auto multiplier = int_member(node, "surface_depth_multiplier");
        if (anchor == nullptr || !multiplier) {
            return std::unexpected{fail("y_above is incomplete")};
        }
        auto decoded_anchor = decode_anchor(*anchor);
        if (!decoded_anchor) {
            return std::unexpected{decoded_anchor.error()};
        }
        decoded.type = condition::kind::y_above;
        decoded.true_at_and_below = *decoded_anchor;
        decoded.surface_depth_multiplier = *multiplier;
        decoded.add_stone_depth =
            bool_member(node, "add_stone_depth").value_or(false);
    } else if (*type == "minecraft:water") {
        const auto offset = int_member(node, "offset");
        const auto multiplier = int_member(node, "surface_depth_multiplier");
        if (!offset || !multiplier) {
            return std::unexpected{fail("water condition is incomplete")};
        }
        decoded.type = condition::kind::water;
        decoded.offset = *offset;
        decoded.surface_depth_multiplier = *multiplier;
        decoded.add_stone_depth =
            bool_member(node, "add_stone_depth").value_or(false);
    } else if (*type == "minecraft:temperature") {
        decoded.type = condition::kind::temperature;
    } else if (*type == "minecraft:steep") {
        decoded.type = condition::kind::steep;
    } else if (*type == "minecraft:hole") {
        decoded.type = condition::kind::hole;
    } else if (*type == "minecraft:above_preliminary_surface") {
        decoded.type = condition::kind::above_preliminary_surface;
    } else if (*type == "minecraft:not") {
        const json *invert = node.find("invert");
        if (invert == nullptr) {
            return std::unexpected{fail("not condition has no invert")};
        }
        auto target = decode_condition_value(*invert);
        if (!target) {
            return std::unexpected{target.error()};
        }
        decoded.type = condition::kind::not_;
        decoded.target = std::make_unique<condition>(std::move(*target));
    } else if (*type == "minecraft:stone_depth") {
        const auto offset = int_member(node, "offset");
        if (!offset) {
            return std::unexpected{fail("stone_depth has no offset")};
        }
        const auto surface = string_member(node, "surface_type");
        decoded.type = condition::kind::stone_depth;
        decoded.offset = *offset;
        decoded.add_surface_depth =
            bool_member(node, "add_surface_depth").value_or(false);
        decoded.secondary_depth_range =
            int_member(node, "secondary_depth_range").value_or(0);
        decoded.surface = surface && *surface == "ceiling"
                              ? cave_surface::ceiling
                              : cave_surface::floor;
    } else {
        return std::unexpected{
            fail("unsupported material condition: " + *type)};
    }
    return decoded;
}

auto decode_condition_value(const json &node) -> result<condition> {
    if (const auto reference = node.as_string()) {
        return condition{.type = condition::kind::reference,
                         .reference = std::string{*reference}};
    }
    return decode_condition_object(node);
}

auto decode_rule_value(const json &node) -> result<rule>;

auto decode_rule_object(const json &node) -> result<rule> {
    const auto type = type_of(node);
    if (!type) {
        return std::unexpected{fail("material rule has no type")};
    }
    rule decoded;
    if (*type == "minecraft:block") {
        const json *state = node.find("result_state");
        if (state == nullptr) {
            return std::unexpected{fail("block rule has no result_state")};
        }
        const auto name = decode_block_state(*state);
        if (!name) {
            return std::unexpected{fail("block rule state is invalid")};
        }
        decoded.type = rule::kind::block;
        decoded.state = *name;
    } else if (*type == "minecraft:bandlands") {
        decoded.type = rule::kind::bandlands;
    } else if (*type == "minecraft:sequence") {
        const json *sequence = node.find("sequence");
        if (sequence == nullptr) {
            return std::unexpected{fail("sequence rule has no sequence")};
        }
        const auto entries = sequence->as_array();
        if (!entries || entries->empty()) {
            return std::unexpected{fail("sequence rule is empty")};
        }
        decoded.type = rule::kind::sequence;
        for (const auto &entry : *entries) {
            auto child = decode_rule_value(entry);
            if (!child) {
                return std::unexpected{child.error()};
            }
            decoded.sequence.push_back(std::move(*child));
        }
    } else if (*type == "minecraft:condition") {
        const json *condition_value = node.find("if_true");
        const json *then_run = node.find("then_run");
        if (condition_value == nullptr || then_run == nullptr) {
            return std::unexpected{fail("condition rule is incomplete")};
        }
        auto decoded_condition = decode_condition_value(*condition_value);
        if (!decoded_condition) {
            return std::unexpected{decoded_condition.error()};
        }
        auto decoded_rule = decode_rule_value(*then_run);
        if (!decoded_rule) {
            return std::unexpected{decoded_rule.error()};
        }
        decoded.type = rule::kind::condition;
        decoded.if_true =
            std::make_unique<condition>(std::move(*decoded_condition));
        decoded.then_run = std::make_unique<rule>(std::move(*decoded_rule));
    } else if (*type == "minecraft:ore_vein") {
        const auto ore = string_member(node, "ore_block");
        const auto raw = string_member(node, "raw_ore_block");
        const auto filler = string_member(node, "filler_block");
        const auto chance = double_member(node, "raw_ore_chance");
        const auto density = string_member(node, "density");
        const auto richness = string_member(node, "richness");
        const auto filler_gap = string_member(node, "filler_gap");
        if (!ore || !raw || !filler || !chance || !density || !richness ||
            !filler_gap) {
            return std::unexpected{fail("ore_vein rule is incomplete")};
        }
        decoded.type = rule::kind::ore_vein;
        decoded.ore_block = *ore;
        decoded.raw_ore_block = *raw;
        decoded.filler_block = *filler;
        decoded.raw_ore_chance = static_cast<float>(*chance);
        decoded.density = *density;
        decoded.richness = *richness;
        decoded.filler_gap = *filler_gap;
    } else {
        return std::unexpected{fail("unsupported material rule: " + *type)};
    }
    return decoded;
}

auto decode_rule_value(const json &node) -> result<rule> {
    if (const auto reference = node.as_string()) {
        return rule{.type = rule::kind::reference,
                    .reference = std::string{*reference}};
    }
    return decode_rule_object(node);
}

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

auto vertical_anchor::resolve(jint min_gen_y, jint gen_depth,
                              jint sea_level) const noexcept -> jint {
    switch (type) {
    case kind::absolute:
        return value;
    case kind::above_bottom:
        return min_gen_y + value;
    case kind::below_top:
        return gen_depth - 1 + min_gen_y - value;
    case kind::relative_to_sea_level:
        return sea_level + value;
    }
    return value;
}

auto load_material_documents(std::string_view data_root)
    -> result<material_documents> {
    material_documents documents;
    if (auto loaded =
            collect_documents(data_root, "material_rule", documents.rules);
        !loaded) {
        return std::unexpected{loaded.error()};
    }
    if (auto loaded = collect_documents(data_root, "material_condition",
                                        documents.conditions);
        !loaded) {
        return std::unexpected{loaded.error()};
    }
    return documents;
}

auto decode_rule(std::string_view json_text) -> result<rule> {
    auto parsed = json::parse(json_text);
    if (!parsed) {
        return std::unexpected{parsed.error()};
    }
    return decode_rule_value(*parsed);
}

auto decode_condition(std::string_view json_text) -> result<condition> {
    auto parsed = json::parse(json_text);
    if (!parsed) {
        return std::unexpected{parsed.error()};
    }
    return decode_condition_value(*parsed);
}

} // namespace refrain::worldgen::surface
