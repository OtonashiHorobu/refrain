// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/structure/jigsaw.hpp>

#include <refrain/worldgen/chunk/proto_chunk.hpp>
#include <refrain/worldgen/structure/template.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <utility>

namespace refrain::worldgen::structure {

namespace {

auto string_field(const data::json::value &document, const char *key)
    -> std::optional<std::string> {
    const auto *value = document.find(key);
    if (value == nullptr) {
        return std::nullopt;
    }
    const auto text = value->as_string();
    if (!text) {
        return std::nullopt;
    }
    return std::string{*text};
}

auto int_field(const data::json::value &document, const char *key,
               jint fallback) -> jint {
    const auto *value = document.find(key);
    if (value == nullptr) {
        return fallback;
    }
    const auto number = value->as_int64();
    return number ? static_cast<jint>(*number) : fallback;
}

auto bool_field(const data::json::value &document, const char *key,
                bool fallback) -> bool {
    const auto *value = document.find(key);
    if (value == nullptr) {
        return fallback;
    }
    const auto flag = value->as_bool();
    return flag ? *flag : fallback;
}

auto decode_projection(const data::json::value &document) -> pool_projection {
    const auto name = string_field(document, "projection");
    if (name && *name == "terrain_matching") {
        return pool_projection::terrain_matching;
    }
    return pool_projection::rigid;
}

auto decode_liquid_override(const data::json::value &document)
    -> std::optional<liquid_settings> {
    const auto name = string_field(document, "override_liquid_settings");
    if (!name) {
        return std::nullopt;
    }
    if (*name == "ignore_waterlogging") {
        return liquid_settings::ignore_waterlogging;
    }
    return liquid_settings::apply_waterlogging;
}

auto decode_height_bound(const data::json::value &document)
    -> std::optional<height_bound> {
    struct anchor_field {
        const char *name;
        height_anchor anchor;
    };
    static constexpr anchor_field fields[]{
        {"absolute", height_anchor::absolute},
        {"above_bottom", height_anchor::above_bottom},
        {"below_top", height_anchor::below_top},
        {"relative_to_sea_level", height_anchor::relative_to_sea_level},
    };
    for (const auto &field : fields) {
        if (const auto *value = document.find(field.name); value != nullptr) {
            const auto number = value->as_int64();
            if (!number) {
                return std::nullopt;
            }
            return height_bound{.value = static_cast<jint>(*number),
                                .anchor = field.anchor};
        }
    }
    return std::nullopt;
}

auto decode_alias_binding(const data::json::value &document)
    -> result<pool_alias_binding>;

/// Decodes a weighted entry: `{ "data": ..., "weight": n }`.
auto decode_weighted(const data::json::value &entry, std::string &data,
                     jint &weight, bool &data_is_array) -> status {
    data_is_array = false;
    weight = 1;
    if (const auto *value = entry.find("weight"); value != nullptr) {
        const auto number = value->as_int64();
        if (!number) {
            return std::unexpected{make_error(
                error_code::parse_error, "weighted entry weight is invalid")};
        }
        weight = static_cast<jint>(*number);
    }
    const auto *value = entry.find("data");
    if (value == nullptr) {
        return std::unexpected{
            make_error(error_code::parse_error, "weighted entry has no data")};
    }
    if (value->is_array()) {
        data_is_array = true;
        return {};
    }
    const auto text = value->as_string();
    if (!text) {
        return std::unexpected{make_error(error_code::parse_error,
                                          "weighted entry data is invalid")};
    }
    data = *text;
    return {};
}

auto decode_alias_binding(const data::json::value &document)
    -> result<pool_alias_binding> {
    const auto type = string_field(document, "type");
    if (!type) {
        return std::unexpected{
            make_error(error_code::parse_error, "pool alias has no type")};
    }
    pool_alias_binding binding;
    if (*type == "minecraft:direct") {
        const auto alias = string_field(document, "alias");
        const auto target = string_field(document, "target");
        if (!alias || !target) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "direct alias is incomplete")};
        }
        binding.type = pool_alias_binding::kind::direct;
        binding.alias = *alias;
        binding.target = *target;
        return binding;
    }
    if (*type == "minecraft:random") {
        const auto alias = string_field(document, "alias");
        const auto *targets = document.find("targets");
        if (!alias || targets == nullptr || !targets->is_array()) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "random alias is incomplete")};
        }
        binding.type = pool_alias_binding::kind::random;
        binding.alias = *alias;
        for (const auto &entry : *targets->as_array()) {
            std::string data;
            jint weight = 1;
            bool array = false;
            if (const auto decoded =
                    decode_weighted(entry, data, weight, array);
                !decoded) {
                return std::unexpected{decoded.error()};
            }
            if (array) {
                return std::unexpected{make_error(
                    error_code::parse_error, "random alias target is a group")};
            }
            binding.targets.push_back(pool_alias_binding::weighted_target{
                .target = std::move(data), .weight = weight});
        }
        return binding;
    }
    if (*type == "minecraft:random_group") {
        const auto *groups = document.find("groups");
        if (groups == nullptr || !groups->is_array()) {
            return std::unexpected{make_error(
                error_code::parse_error, "random group alias is incomplete")};
        }
        binding.type = pool_alias_binding::kind::random_group;
        for (const auto &entry : *groups->as_array()) {
            std::string unused;
            jint weight = 1;
            bool array = false;
            if (const auto decoded =
                    decode_weighted(entry, unused, weight, array);
                !decoded) {
                return std::unexpected{decoded.error()};
            }
            if (!array) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "random group entry is not a list")};
            }
            pool_alias_binding::group group{.weight = weight};
            const auto *data = entry.find("data");
            for (const auto &nested : *data->as_array()) {
                auto binding_entry = decode_alias_binding(nested);
                if (!binding_entry) {
                    return std::unexpected{binding_entry.error()};
                }
                group.bindings.push_back(std::move(*binding_entry));
            }
            binding.groups.push_back(std::move(group));
        }
        return binding;
    }
    return std::unexpected{
        make_error(error_code::unsupported, "unsupported pool alias type")};
}

void expand_alias(const pool_alias_binding &binding, worldgen_random &random,
                  std::vector<std::pair<std::string, std::string>> &out) {
    const auto pick_weighted =
        [&random]<typename T>(const std::vector<T> &entries) -> const T * {
        jint total = 0;
        for (const auto &entry : entries) {
            total += entry.weight;
        }
        if (total <= 0) {
            return nullptr;
        }
        jint choice = random.next_int(total);
        for (const auto &entry : entries) {
            choice -= entry.weight;
            if (choice < 0) {
                return &entry;
            }
        }
        return &entries.back();
    };
    switch (binding.type) {
    case pool_alias_binding::kind::direct:
        out.emplace_back(binding.alias, binding.target);
        break;
    case pool_alias_binding::kind::random: {
        const auto *target = pick_weighted(binding.targets);
        if (target != nullptr) {
            out.emplace_back(binding.alias, target->target);
        }
        break;
    }
    case pool_alias_binding::kind::random_group: {
        const auto *group = pick_weighted(binding.groups);
        if (group != nullptr) {
            for (const auto &nested : group->bindings) {
                expand_alias(nested, random, out);
            }
        }
        break;
    }
    }
}

} // namespace

auto connectors_attach(const jigsaw_connector &source,
                       const jigsaw_connector &target) -> bool {
    constexpr std::array<jigsaw_front, 6> opposites{
        jigsaw_front::up,    jigsaw_front::down, jigsaw_front::south,
        jigsaw_front::north, jigsaw_front::east, jigsaw_front::west,
    };
    if (source.front != opposites[static_cast<std::size_t>(target.front)]) {
        return false;
    }
    if (!source.rollable && source.top != target.top) {
        return false;
    }
    return !target.has_name || source.target == target.name;
}

auto pool_alias_lookup::create(const std::vector<pool_alias_binding> &bindings,
                               const block_pos &pos, jlong seed)
    -> pool_alias_lookup {
    pool_alias_lookup lookup;
    if (bindings.empty()) {
        return lookup;
    }
    legacy_random_source base{seed};
    const jlong factory = base.next_long();
    worldgen_random random{std::make_unique<legacy_random_source>(
        mth_get_seed(pos.x, pos.y, pos.z) ^ factory)};
    for (const auto &binding : bindings) {
        expand_alias(binding, random, lookup.aliases_);
    }
    return lookup;
}

auto pool_alias_lookup::lookup(std::string_view pool) const -> std::string {
    for (auto it = aliases_.rbegin(); it != aliases_.rend(); ++it) {
        if (it->first == pool) {
            return it->second;
        }
    }
    return std::string{pool};
}

auto height_provider::sample(worldgen_random &random, jint min_y, jint height,
                             jint sea_level) const -> jint {
    const auto resolve = [&](const height_bound &bound) {
        switch (bound.anchor) {
        case height_anchor::absolute:
            return bound.value;
        case height_anchor::above_bottom:
            return min_y + bound.value;
        case height_anchor::below_top:
            return min_y + height - 1 - bound.value;
        case height_anchor::relative_to_sea_level:
            return sea_level + bound.value;
        }
        return bound.value;
    };
    if (type == kind::uniform) {
        const jint low = resolve(min);
        const jint high = resolve(max);
        if (low > high) {
            return low;
        }
        return random.next_int(high - low + 1) + low;
    }
    return resolve(min);
}

void jigsaw_template_pool::build_index() {
    templates.clear();
    for (std::size_t index = 0; index < raw_elements.size(); ++index) {
        const jint weight = std::max(1, raw_elements[index].weight);
        for (jint i = 0; i < weight; ++i) {
            templates.push_back(index);
        }
    }
    max_size.reset();
}

auto jigsaw_template_pool::get_random_template(worldgen_random &random) const
    -> const pool_element * {
    if (templates.empty()) {
        return nullptr;
    }
    const auto index = static_cast<std::size_t>(
        random.next_int(static_cast<jint>(templates.size())));
    return &raw_elements[templates[index]].element;
}

auto jigsaw_template_pool::get_shuffled_templates(worldgen_random &random) const
    -> std::vector<const pool_element *> {
    std::vector<std::size_t> order = templates;
    for (std::size_t i = order.size(); i > 1; --i) {
        const auto swap_to =
            static_cast<std::size_t>(random.next_int(static_cast<jint>(i)));
        std::swap(order[i - 1], order[swap_to]);
    }
    std::vector<const pool_element *> result;
    result.reserve(order.size());
    for (const auto index : order) {
        result.push_back(&raw_elements[index].element);
    }
    return result;
}

auto decode_pool_element(const data::json::value &document)
    -> result<pool_element> {
    const auto type = string_field(document, "element_type");
    if (!type) {
        return std::unexpected{
            make_error(error_code::parse_error, "pool element has no type")};
    }
    pool_element element;
    element.projection = decode_projection(document);
    element.liquid_override = decode_liquid_override(document);
    if (const auto *processors = document.find("processors");
        processors != nullptr) {
        if (const auto text = processors->as_string(); text.has_value()) {
            element.processor_list = *text;
        } else {
            element.inline_processors =
                std::make_shared<const data::json::value>(*processors);
        }
    }
    if (*type == "minecraft:empty_pool_element") {
        element.kind = pool_element_kind::empty;
        element.projection = pool_projection::terrain_matching;
        return element;
    }
    if (*type == "minecraft:single_pool_element" ||
        *type == "minecraft:legacy_single_pool_element") {
        const auto location = string_field(document, "location");
        if (!location) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "single pool element has no location")};
        }
        element.kind = *type == "minecraft:single_pool_element"
                           ? pool_element_kind::single
                           : pool_element_kind::legacy_single;
        element.template_id = *location;
        return element;
    }
    if (*type == "minecraft:feature_pool_element") {
        const auto feature = string_field(document, "feature");
        if (!feature) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "feature pool element has no feature")};
        }
        element.kind = pool_element_kind::feature;
        element.feature = *feature;
        return element;
    }
    if (*type == "minecraft:list_pool_element") {
        const auto *elements = document.find("elements");
        if (elements == nullptr || !elements->is_array()) {
            return std::unexpected{make_error(
                error_code::parse_error, "list pool element has no elements")};
        }
        element.kind = pool_element_kind::list;
        for (const auto &entry : *elements->as_array()) {
            auto child = decode_pool_element(entry);
            if (!child) {
                return std::unexpected{child.error()};
            }
            // A list applies its projection to every child.
            child->projection = element.projection;
            element.elements.push_back(std::move(*child));
        }
        if (element.elements.empty()) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "list pool element is empty")};
        }
        return element;
    }
    return std::unexpected{
        make_error(error_code::unsupported, "unsupported pool element type")};
}

auto decode_template_pool(std::string_view id,
                          const data::json::value &document)
    -> result<jigsaw_template_pool> {
    jigsaw_template_pool pool{.id = std::string{id}};
    const auto fallback = string_field(document, "fallback");
    if (!fallback) {
        return std::unexpected{make_error(error_code::parse_error,
                                          "template pool has no fallback")};
    }
    pool.fallback = *fallback;
    const auto *elements = document.find("elements");
    if (elements == nullptr || !elements->is_array()) {
        return std::unexpected{make_error(error_code::parse_error,
                                          "template pool has no elements")};
    }
    for (const auto &entry : *elements->as_array()) {
        const auto *element_value = entry.find("element");
        if (element_value == nullptr) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "pool entry has no element")};
        }
        auto element = decode_pool_element(*element_value);
        if (!element) {
            return std::unexpected{make_error(
                element.error().code,
                element.error().message + " in pool " + std::string{id})};
        }
        const jint weight = int_field(entry, "weight", 1);
        if (weight < 1) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "pool entry weight is invalid")};
        }
        pool.raw_elements.push_back(weighted_pool_element{
            .element = std::move(*element), .weight = weight});
    }
    pool.build_index();
    return pool;
}

auto decode_pool_alias_binding(const data::json::value &document)
    -> result<pool_alias_binding> {
    return decode_alias_binding(document);
}

auto decode_jigsaw_structure_config(const data::json::value &document)
    -> result<jigsaw_structure_config> {
    jigsaw_structure_config config;
    const auto start_pool = string_field(document, "start_pool");
    if (!start_pool) {
        return std::unexpected{
            make_error(error_code::parse_error, "jigsaw has no start pool")};
    }
    config.start_pool = *start_pool;
    if (auto name = string_field(document, "start_jigsaw_name"); name) {
        config.start_jigsaw_name = *name;
    }
    config.max_depth = int_field(document, "size", 6);
    if (config.max_depth < 0 || config.max_depth > 20) {
        return std::unexpected{
            make_error(error_code::parse_error, "jigsaw size is out of range")};
    }
    config.use_expansion_hack =
        bool_field(document, "use_expansion_hack", true);
    if (auto heightmap = string_field(document, "project_start_to_heightmap");
        heightmap) {
        config.project_start_to_heightmap = *heightmap;
    }
    const auto *height = document.find("start_height");
    if (height == nullptr) {
        return std::unexpected{
            make_error(error_code::parse_error, "jigsaw has no start height")};
    }
    if (const auto constant = decode_height_bound(*height); constant) {
        config.start_height = height_provider{
            .type = height_provider::kind::constant,
            .min = *constant,
            .max = *constant,
        };
    } else {
        const auto type = string_field(*height, "type");
        if (!type) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "start height has no type")};
        }
        if (*type == "minecraft:constant") {
            const auto *value = height->find("value");
            if (value == nullptr) {
                return std::unexpected{make_error(
                    error_code::parse_error, "constant height has no value")};
            }
            const auto bound = decode_height_bound(*value);
            if (!bound) {
                return std::unexpected{make_error(
                    error_code::parse_error, "constant height is invalid")};
            }
            config.start_height = height_provider{
                .type = height_provider::kind::constant,
                .min = *bound,
                .max = *bound,
            };
        } else if (*type == "minecraft:uniform") {
            const auto *min = height->find("min_inclusive");
            const auto *max = height->find("max_inclusive");
            if (min == nullptr || max == nullptr) {
                return std::unexpected{make_error(
                    error_code::parse_error, "uniform height is incomplete")};
            }
            const auto low = decode_height_bound(*min);
            const auto high = decode_height_bound(*max);
            if (!low || !high) {
                return std::unexpected{make_error(error_code::parse_error,
                                                  "uniform height is invalid")};
            }
            config.start_height = height_provider{
                .type = height_provider::kind::uniform,
                .min = *low,
                .max = *high,
            };
        } else {
            return std::unexpected{make_error(
                error_code::unsupported, "unsupported start height provider")};
        }
    }
    const auto *distance = document.find("max_distance_from_center");
    if (distance != nullptr) {
        if (const auto number = distance->as_int64(); number) {
            config.max_distance_horizontal = static_cast<jint>(*number);
            config.max_distance_vertical = static_cast<jint>(*number);
        } else {
            const auto horizontal = int_field(*distance, "horizontal", 80);
            const auto vertical =
                int_field(*distance, "vertical", config.max_distance_vertical);
            config.max_distance_horizontal = horizontal;
            config.max_distance_vertical = vertical;
        }
    }
    if (const auto *aliases = document.find("pool_aliases");
        aliases != nullptr && aliases->is_array()) {
        for (const auto &entry : *aliases->as_array()) {
            auto binding = decode_pool_alias_binding(entry);
            if (!binding) {
                return std::unexpected{binding.error()};
            }
            config.pool_aliases.push_back(std::move(*binding));
        }
    }
    if (const auto *padding = document.find("dimension_padding");
        padding != nullptr) {
        if (const auto number = padding->as_int64(); number) {
            config.padding_bottom = static_cast<jint>(*number);
            config.padding_top = static_cast<jint>(*number);
        } else {
            config.padding_bottom = int_field(*padding, "bottom", 0);
            config.padding_top = int_field(*padding, "top", 0);
        }
    }
    if (const auto liquid = string_field(document, "liquid_settings"); liquid) {
        config.liquid = *liquid == "ignore_waterlogging"
                            ? liquid_settings::ignore_waterlogging
                            : liquid_settings::apply_waterlogging;
    }
    return config;
}

} // namespace refrain::worldgen::structure
