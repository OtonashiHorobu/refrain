// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/structure/template.hpp>

#include <refrain/core/io.hpp>
#include <refrain/worldgen/chunk/block_state_codec.hpp>
#include <refrain/worldgen/core/rng.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <set>
#include <string>
#include <tuple>
#include <utility>

namespace refrain::worldgen::structure {

namespace {

struct parsed_state {
    std::string_view name{};
    std::vector<std::pair<std::string, std::string>> properties{};
};

auto parse_state(std::string_view key) -> parsed_state;
auto find_state(
    block_state_registry &states, std::string_view name,
    const std::vector<std::pair<std::string, std::string>> &properties)
    -> block_state_id;

auto child(const data::nbt::compound *entries, std::string_view key)
    -> const data::nbt::value * {
    if (entries == nullptr) {
        return nullptr;
    }
    const auto found = entries->find(key);
    return found == entries->end() ? nullptr : &found->second;
}

auto int_or(const data::nbt::compound *entries, std::string_view key,
            jint fallback) -> jint {
    const auto *value = child(entries, key);
    if (value == nullptr) {
        return fallback;
    }
    if (const auto number = value->as_int(); number.has_value()) {
        return *number;
    }
    if (const auto number = value->as_short(); number.has_value()) {
        return *number;
    }
    if (const auto number = value->as_byte(); number.has_value()) {
        return *number;
    }
    return fallback;
}

auto int_list(const data::nbt::list_value *list, std::size_t index) -> jint {
    if (list == nullptr || index >= list->items.size()) {
        return 0;
    }
    const auto &item = list->items[index];
    if (const auto number = item.as_int(); number.has_value()) {
        return *number;
    }
    if (const auto number = item.as_short(); number.has_value()) {
        return *number;
    }
    if (const auto number = item.as_byte(); number.has_value()) {
        return *number;
    }
    return 0;
}

auto double_list(const data::nbt::list_value *list, std::size_t index)
    -> double {
    if (list == nullptr || index >= list->items.size()) {
        return 0.0;
    }
    const auto &item = list->items[index];
    if (const auto number = item.as_double(); number.has_value()) {
        return *number;
    }
    if (const auto number = item.as_float(); number.has_value()) {
        return static_cast<double>(*number);
    }
    return 0.0;
}

auto child_list(const data::nbt::compound *entries, std::string_view key)
    -> const data::nbt::list_value * {
    const auto *value = child(entries, key);
    return value == nullptr ? nullptr : value->as_list();
}

auto block_state_key(const data::nbt::compound &entry) -> result<std::string> {
    const auto *name_value = child(&entry, "id");
    if (name_value == nullptr) {
        name_value = child(&entry, "Name");
    }
    if (name_value == nullptr) {
        return std::unexpected{
            make_error(error_code::parse_error, "template palette has no id")};
    }
    const auto name = name_value->as_string();
    if (!name) {
        return std::unexpected{make_error(error_code::parse_error,
                                          "template palette Name is invalid")};
    }
    std::string key{*name};
    const auto *properties = child(&entry, "properties");
    if (properties == nullptr) {
        properties = child(&entry, "Properties");
    }
    if (properties == nullptr) {
        return key;
    }
    const auto *values = properties->as_compound();
    if (values == nullptr || values->empty()) {
        return key;
    }
    key.push_back('[');
    bool first = true;
    for (const auto &[property, value] : *values) {
        const auto text = value.as_string();
        if (!text) {
            continue;
        }
        if (!first) {
            key.push_back(',');
        }
        first = false;
        key.append(property);
        key.push_back('=');
        key.append(*text);
    }
    key.push_back(']');
    return key;
}

auto resolve_palette_state(block_state_registry &states,
                           const data::nbt::compound &entry) -> block_state_id {
    const auto key = block_state_key(entry);
    if (!key) {
        return states.air();
    }
    if (const auto found = states.find(*key); found.has_value()) {
        return *found;
    }
    const auto parsed = parse_state(*key);
    const auto by_properties =
        find_state(states, parsed.name, parsed.properties);
    if (by_properties != no_block_state) {
        return by_properties;
    }
    bool known_block = false;
    for (std::size_t id = 0; id < states.size(); ++id) {
        if (parse_state(states.name(static_cast<block_state_id>(id))).name ==
            parsed.name) {
            known_block = true;
            break;
        }
    }
    if (!known_block) {
        return states.air();
    }
    // The state table has the block but not this property combination; keep
    // the template's properties instead of collapsing to the default state.
    const bool fluid =
        parsed.name == "minecraft:water" || parsed.name == "minecraft:lava";
    const bool leaves = parsed.name.ends_with("_leaves");
    const auto inserted =
        states.intern(*key, {.air = false,
                             .fluid = fluid,
                             .water = parsed.name == "minecraft:water",
                             .blocks_motion = !fluid,
                             .blocks_motion_no_leaves = !fluid && !leaves});
    return inserted.has_value() ? *inserted : states.air();
}

auto is_full_block(block_state_registry &states, block_state_id state) -> bool {
    const auto traits = states.traits(state);
    return !traits.air && !traits.fluid && traits.blocks_motion;
}

auto load_palette(block_state_registry &states,
                  const data::nbt::list_value &palette_list,
                  const data::nbt::list_value *blocks)
    -> std::vector<template_block_info> {
    std::vector<block_state_id> palette;
    palette.reserve(palette_list.items.size());
    for (const auto &entry : palette_list.items) {
        const auto *value = entry.as_compound();
        palette.push_back(value == nullptr
                              ? states.air()
                              : resolve_palette_state(states, *value));
    }

    std::vector<template_block_info> full;
    std::vector<template_block_info> other;
    std::vector<template_block_info> block_entities;
    if (blocks != nullptr) {
        for (const auto &entry : blocks->items) {
            const auto *value = entry.as_compound();
            if (value == nullptr) {
                continue;
            }
            const auto *pos = child_list(value, "pos");
            template_block_info info{
                .x = int_list(pos, 0),
                .y = int_list(pos, 1),
                .z = int_list(pos, 2),
                .state = states.air(),
            };
            const auto index = int_or(value, "state", 0);
            if (index >= 0 &&
                static_cast<std::size_t>(index) < palette.size()) {
                info.state = palette[static_cast<std::size_t>(index)];
            }
            if (const auto *tag = child(value, "nbt");
                tag != nullptr && tag->as_compound() != nullptr) {
                info.nbt = std::make_shared<const data::nbt::compound>(
                    *tag->as_compound());
            }
            if (info.nbt != nullptr) {
                block_entities.push_back(std::move(info));
            } else if (is_full_block(states, info.state)) {
                full.push_back(std::move(info));
            } else {
                other.push_back(std::move(info));
            }
        }
    }
    const auto by_y_x_z = [](const template_block_info &left,
                             const template_block_info &right) {
        if (left.y != right.y) {
            return left.y < right.y;
        }
        if (left.x != right.x) {
            return left.x < right.x;
        }
        return left.z < right.z;
    };
    std::stable_sort(full.begin(), full.end(), by_y_x_z);
    std::stable_sort(other.begin(), other.end(), by_y_x_z);
    std::stable_sort(block_entities.begin(), block_entities.end(), by_y_x_z);
    full.insert(full.end(), std::make_move_iterator(other.begin()),
                std::make_move_iterator(other.end()));
    full.insert(full.end(), std::make_move_iterator(block_entities.begin()),
                std::make_move_iterator(block_entities.end()));
    return full;
}

auto parse_state(std::string_view key) -> parsed_state {
    parsed_state out;
    const auto open = key.find('[');
    if (open == std::string_view::npos) {
        out.name = key;
        return out;
    }
    out.name = key.substr(0, open);
    const auto close = key.rfind(']');
    if (close == std::string_view::npos || close < open) {
        return out;
    }
    std::string_view body = key.substr(open + 1, close - open - 1);
    while (!body.empty()) {
        const auto comma = body.find(',');
        const auto entry = body.substr(0, comma);
        const auto equals = entry.find('=');
        if (equals != std::string_view::npos) {
            out.properties.emplace_back(std::string{entry.substr(0, equals)},
                                        std::string{entry.substr(equals + 1)});
        }
        if (comma == std::string_view::npos) {
            break;
        }
        body = body.substr(comma + 1);
    }
    return out;
}

auto find_state(
    block_state_registry &states, std::string_view name,
    const std::vector<std::pair<std::string, std::string>> &properties)
    -> block_state_id {
    for (std::size_t id = 0; id < states.size(); ++id) {
        const auto key = states.name(static_cast<block_state_id>(id));
        const auto parsed = parse_state(key);
        if (parsed.name != name) {
            continue;
        }
        bool matches = true;
        for (const auto &[property, value] : properties) {
            const auto found = std::find_if(
                parsed.properties.begin(), parsed.properties.end(),
                [&](const auto &entry) { return entry.first == property; });
            if (found == parsed.properties.end() || found->second != value) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return static_cast<block_state_id>(id);
        }
    }
    return no_block_state;
}

auto direction_opposite(std::string_view direction) -> std::string_view {
    if (direction == "north") {
        return "south";
    }
    if (direction == "south") {
        return "north";
    }
    if (direction == "east") {
        return "west";
    }
    if (direction == "west") {
        return "east";
    }
    return direction;
}

auto mirror_direction(std::string_view direction, template_mirror mirror)
    -> std::string_view {
    if (mirror == template_mirror::left_right &&
        (direction == "north" || direction == "south")) {
        return direction_opposite(direction);
    }
    if (mirror == template_mirror::front_back &&
        (direction == "east" || direction == "west")) {
        return direction_opposite(direction);
    }
    return direction;
}

auto rotate_direction(std::string_view direction, template_rotation rotation)
    -> std::string_view {
    if (rotation == template_rotation::none || direction == "up" ||
        direction == "down") {
        return direction;
    }
    auto clockwise = [](std::string_view value) -> std::string_view {
        if (value == "north") {
            return "east";
        }
        if (value == "east") {
            return "south";
        }
        if (value == "south") {
            return "west";
        }
        return "north";
    };
    if (rotation == template_rotation::clockwise_90) {
        return clockwise(direction);
    }
    if (rotation == template_rotation::clockwise_180) {
        return direction_opposite(direction);
    }
    return clockwise(clockwise(clockwise(direction)));
}

auto transform_rotation_value(std::string_view value, template_mirror mirror,
                              template_rotation rotation) -> std::string {
    jint parsed = 0;
    const auto *begin = value.data();
    const auto *end = begin + value.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{}) {
        return std::string{value};
    }
    jint steps = ((parsed % 16) + 16) % 16;
    if (mirror == template_mirror::left_right) {
        const jint centered = steps > 8 ? steps - 16 : steps;
        steps = ((8 - centered) % 16 + 16) % 16;
    } else if (mirror == template_mirror::front_back) {
        const jint centered = steps > 8 ? steps - 16 : steps;
        steps = ((-centered) % 16 + 16) % 16;
    }
    switch (rotation) {
    case template_rotation::clockwise_90:
        steps = (steps + 4) % 16;
        break;
    case template_rotation::clockwise_180:
        steps = (steps + 8) % 16;
        break;
    case template_rotation::counterclockwise_90:
        steps = (steps + 12) % 16;
        break;
    case template_rotation::none:
        break;
    }
    return std::to_string(steps);
}

auto build_state_key(
    std::string_view name,
    const std::vector<std::pair<std::string, std::string>> &properties)
    -> std::string {
    std::string key{name};
    if (properties.empty()) {
        return key;
    }
    key.push_back('[');
    bool first = true;
    for (const auto &[property, value] : properties) {
        if (!first) {
            key.push_back(',');
        }
        first = false;
        key.append(property);
        key.push_back('=');
        key.append(value);
    }
    key.push_back(']');
    return key;
}

} // namespace

auto load_structure_template(block_state_registry &states,
                             const data::nbt::value &root)
    -> result<structure_template> {
    const auto *entries = root.as_compound();
    if (entries == nullptr) {
        return std::unexpected{make_error(error_code::parse_error,
                                          "template root is not a compound")};
    }
    structure_template out;
    const auto *size = child_list(entries, "size");
    out.size_x = int_list(size, 0);
    out.size_y = int_list(size, 1);
    out.size_z = int_list(size, 2);

    const auto *blocks = child_list(entries, "blocks");
    if (const auto *palettes = child_list(entries, "palettes");
        palettes != nullptr) {
        for (const auto &entry : palettes->items) {
            const auto *palette = entry.as_list();
            if (palette == nullptr) {
                out.palettes.push_back({});
                continue;
            }
            out.palettes.push_back(load_palette(states, *palette, blocks));
        }
    } else {
        const auto *palette = child_list(entries, "palette");
        out.palettes.push_back(load_palette(
            states, palette == nullptr ? data::nbt::list_value{} : *palette,
            blocks));
    }

    if (const auto *list = child_list(entries, "entities"); list != nullptr) {
        for (const auto &entry : list->items) {
            const auto *value = entry.as_compound();
            if (value == nullptr) {
                continue;
            }
            const auto *tag = child(value, "nbt");
            if (tag == nullptr || tag->as_compound() == nullptr) {
                continue;
            }
            const auto *pos = child_list(value, "pos");
            const auto *block_pos = child_list(value, "blockPos");
            out.entities.push_back(template_entity_info{
                .x = double_list(pos, 0),
                .y = double_list(pos, 1),
                .z = double_list(pos, 2),
                .block_x = int_list(block_pos, 0),
                .block_y = int_list(block_pos, 1),
                .block_z = int_list(block_pos, 2),
                .nbt = *tag->as_compound(),
            });
        }
    }
    return out;
}

auto structure_template_manager::get_or_create(std::string_view id) const
    -> const structure_template & {
    const std::string key{id};
    if (const auto found = cache_.find(key); found != cache_.end()) {
        return found->second;
    }
    structure_template loaded;
    const auto separator = key.find(':');
    if (separator != std::string::npos && separator + 1 < key.size()) {
        const std::string path = root_ + "/" + key.substr(0, separator) +
                                 "/structure/" + key.substr(separator + 1) +
                                 ".nbt";
        if (const auto document =
                data::nbt::read_file(path, data::nbt::compression::gzip);
            document.has_value()) {
            if (auto parsed = load_structure_template(*states_, *document);
                parsed.has_value()) {
                loaded = std::move(*parsed);
            }
        }
    }
    return cache_.emplace(key, std::move(loaded)).first->second;
}

auto rotated_template_size(const structure_template &templ,
                           template_rotation rotation) -> block_pos {
    switch (rotation) {
    case template_rotation::clockwise_90:
    case template_rotation::counterclockwise_90:
        return block_pos{
            .x = templ.size_z, .y = templ.size_y, .z = templ.size_x};
    default:
        return block_pos{
            .x = templ.size_x, .y = templ.size_y, .z = templ.size_z};
    }
}

auto zero_position_with_transform(const structure_template &templ,
                                  const block_pos &zero, template_mirror mirror,
                                  template_rotation rotation) -> block_pos {
    const jint size_x = templ.size_x - 1;
    const jint size_z = templ.size_z - 1;
    const jint mirror_delta_x =
        mirror == template_mirror::front_back ? size_x : 0;
    const jint mirror_delta_z =
        mirror == template_mirror::left_right ? size_z : 0;
    switch (rotation) {
    case template_rotation::counterclockwise_90:
        return block_pos{.x = zero.x + mirror_delta_z,
                         .y = zero.y,
                         .z = zero.z + size_x - mirror_delta_x};
    case template_rotation::clockwise_90:
        return block_pos{.x = zero.x + size_z - mirror_delta_z,
                         .y = zero.y,
                         .z = zero.z + mirror_delta_x};
    case template_rotation::clockwise_180:
        return block_pos{.x = zero.x + size_x - mirror_delta_x,
                         .y = zero.y,
                         .z = zero.z + size_z - mirror_delta_z};
    case template_rotation::none:
        break;
    }
    return block_pos{.x = zero.x + mirror_delta_x,
                     .y = zero.y,
                     .z = zero.z + mirror_delta_z};
}

void capped_rule_processor::finalize_processing(
    block_state_registry &states, jint level_seed, const block_pos &position,
    std::vector<template_block_info> &blocks) const {
    if (limit_ <= 0 || blocks.empty()) {
        return;
    }
    legacy_random_source base{level_seed};
    const jlong factory = base.next_long();
    legacy_random_source random{
        mth_get_seed(position.x, position.y, position.z) ^ factory};
    std::vector<std::size_t> indices(blocks.size());
    for (std::size_t i = 0; i < indices.size(); ++i) {
        indices[i] = i;
    }
    for (std::size_t i = indices.size(); i > 1; --i) {
        const auto swap_to =
            static_cast<std::size_t>(random.next_int(static_cast<jint>(i)));
        std::swap(indices[i - 1], indices[swap_to]);
    }
    jint replaced = 0;
    for (const auto index : indices) {
        if (replaced >= limit_) {
            break;
        }
        auto &block = blocks[index];
        if (state_block_name(states.name(block.state)) != input_block_) {
            continue;
        }
        auto output = states.find(output_state_);
        if (!output) {
            const auto interned =
                states.intern(output_state_, states.traits(block.state));
            if (!interned) {
                continue;
            }
            output = *interned;
        }
        block.state = *output;
        if (!loot_table_.empty()) {
            auto entity = std::make_shared<data::nbt::compound>();
            (*entity)["LootTable"] = data::nbt::value{std::string{loot_table_}};
            (*entity)["LootTableSeed"] =
                data::nbt::value{static_cast<jlong>(random.next_long())};
            block.nbt = std::move(entity);
        }
        ++replaced;
    }
}

auto decode_processor_list(block_state_registry &states,
                           const data::json::value &document)
    -> result<std::shared_ptr<structure_processor_list>> {
    const auto string_of =
        [](const data::json::value *value) -> std::optional<std::string> {
        if (value == nullptr) {
            return std::nullopt;
        }
        const auto text = value->as_string();
        if (!text) {
            return std::nullopt;
        }
        return std::string{*text};
    };
    const auto double_of =
        [](const data::json::value *value) -> std::optional<double> {
        if (value == nullptr) {
            return std::nullopt;
        }
        const auto number = value->as_double();
        if (!number) {
            return std::nullopt;
        }
        return *number;
    };
    const auto array_of = [](const data::json::value *value)
        -> std::optional<std::span<const data::json::value>> {
        if (value == nullptr) {
            return std::nullopt;
        }
        const auto array = value->as_array();
        if (!array) {
            return std::nullopt;
        }
        return *array;
    };
    (void)states;
    const auto *processors = document.find("processors");
    const auto entries = array_of(processors);
    if (!entries) {
        return std::unexpected{make_error(error_code::parse_error,
                                          "processor list has no processors")};
    }
    auto list = std::make_shared<structure_processor_list>();
    for (const auto &entry : *entries) {
        const auto type = string_of(entry.find("processor_type"));
        if (!type) {
            return std::unexpected{
                make_error(error_code::parse_error, "processor has no type")};
        }
        if (*type == "minecraft:block_rot") {
            const auto integrity = double_of(entry.find("integrity"));
            if (!integrity) {
                return std::unexpected{make_error(
                    error_code::parse_error, "block_rot has no integrity")};
            }
            std::vector<std::string> rottable;
            const auto *rottable_value = entry.find("rottable_blocks");
            const bool has_rottable = rottable_value != nullptr;
            if (const auto text = string_of(rottable_value); text.has_value()) {
                rottable.push_back(*text);
            } else if (const auto array = array_of(rottable_value);
                       array.has_value()) {
                for (const auto &item : *array) {
                    if (const auto item_text = item.as_string();
                        item_text.has_value()) {
                        rottable.emplace_back(*item_text);
                    }
                }
            }
            list->owned.push_back(std::make_unique<block_rot_processor>(
                static_cast<float>(*integrity), std::move(rottable),
                has_rottable));
            list->processors.push_back(list->owned.back().get());
        } else if (*type == "minecraft:protected_blocks") {
            const auto value = string_of(entry.find("value"));
            if (!value) {
                return std::unexpected{make_error(
                    error_code::parse_error, "protected_blocks has no value")};
            }
            std::string tag{*value};
            if (tag.starts_with('#')) {
                tag.erase(0, 1);
            }
            list->owned.push_back(
                std::make_unique<protected_blocks_processor>(std::move(tag)));
            list->processors.push_back(list->owned.back().get());
        } else if (*type == "minecraft:block_ignore") {
            std::vector<std::string> blocks;
            if (const auto array = array_of(entry.find("blocks"));
                array.has_value()) {
                for (const auto &item : *array) {
                    if (const auto item_text = item.as_string();
                        item_text.has_value()) {
                        blocks.emplace_back(*item_text);
                    }
                }
            }
            list->owned.push_back(
                std::make_unique<block_ignore_processor>(std::move(blocks)));
            list->processors.push_back(list->owned.back().get());
        } else if (*type != "minecraft:nop") {
            return std::unexpected{
                make_error(error_code::parse_error, "unsupported processor")};
        }
    }
    return list;
}

namespace {

auto neighbour_pos(const block_pos &pos, std::string_view direction)
    -> block_pos {
    if (direction == "north") {
        return {.x = pos.x, .y = pos.y, .z = pos.z - 1};
    }
    if (direction == "south") {
        return {.x = pos.x, .y = pos.y, .z = pos.z + 1};
    }
    if (direction == "east") {
        return {.x = pos.x + 1, .y = pos.y, .z = pos.z};
    }
    if (direction == "west") {
        return {.x = pos.x - 1, .y = pos.y, .z = pos.z};
    }
    if (direction == "up") {
        return {.x = pos.x, .y = pos.y + 1, .z = pos.z};
    }
    return {.x = pos.x, .y = pos.y - 1, .z = pos.z};
}

auto opposite_direction(std::string_view direction) -> std::string_view {
    if (direction == "north") {
        return "south";
    }
    if (direction == "south") {
        return "north";
    }
    if (direction == "east") {
        return "west";
    }
    if (direction == "west") {
        return "east";
    }
    return direction;
}

auto is_shape_sensitive(block_state_registry &states, block_state_id state)
    -> bool {
    const auto block = state_block_name(states.name(state));
    return block.ends_with("_fence") || block.ends_with("_stairs") ||
           block.ends_with("_wall");
}

auto update_template_block(block_state_registry &states, chunk_region &region,
                           const block_pos &pos) -> bool {
    const auto current = region.state_at(pos.x, pos.y, pos.z);
    auto parsed = parse_state(states.name(current));
    const auto find_property = [&](std::string_view name) -> std::string {
        for (const auto &[property, value] : parsed.properties) {
            if (property == name) {
                return value;
            }
        }
        return {};
    };
    const auto is_fence = [](std::string_view name) {
        return name.ends_with("_fence");
    };
    const auto replace = [&](std::string_view name,
                             std::string_view value) -> bool {
        bool changed = false;
        for (auto &[property, current_value] : parsed.properties) {
            if (property == name && current_value != value) {
                current_value = std::string{value};
                changed = true;
            }
        }
        return changed;
    };
    bool changed = false;
    if (is_fence(parsed.name)) {
        for (const auto *direction : {"north", "east", "south", "west"}) {
            const auto target = neighbour_pos(pos, direction);
            const auto state = region.state_at(target.x, target.y, target.z);
            const auto name = std::string{states.name(state)};
            const auto traits = states.traits(state);
            const bool solid =
                traits.blocks_motion && !name.ends_with("_fence") &&
                !name.ends_with("_stairs") && !name.ends_with("_slab") &&
                !name.ends_with("_door") && !name.ends_with("_trapdoor") &&
                !name.ends_with("_fence_gate") && !name.ends_with("_wall") &&
                name != "minecraft:water";
            if (replace(direction,
                        (is_fence(name) || solid) ? "true" : "false")) {
                changed = true;
            }
        }
    } else if (parsed.name.ends_with("_stairs")) {
        const auto facing = find_property("facing");
        const auto half = find_property("half");
        const auto stairs_neighbour = [&](std::string_view direction)
            -> std::pair<std::string, std::string> {
            const auto target = neighbour_pos(pos, direction);
            const auto parsed_neighbour = parse_state(
                states.name(region.state_at(target.x, target.y, target.z)));
            if (!parsed_neighbour.name.ends_with("_stairs")) {
                return {};
            }
            std::string neighbour_facing;
            std::string neighbour_half;
            for (const auto &[property, value] : parsed_neighbour.properties) {
                if (property == "facing") {
                    neighbour_facing = value;
                } else if (property == "half") {
                    neighbour_half = value;
                }
            }
            return {neighbour_facing, neighbour_half};
        };
        const auto axis = [](std::string_view value) {
            return value == "north" || value == "south" ? 'z' : 'x';
        };
        std::string shape = "straight";
        const auto [behind_facing, behind_half] = stairs_neighbour(facing);
        if (!behind_facing.empty() && behind_half == half &&
            axis(behind_facing) != axis(facing)) {
            shape = behind_facing == (facing == "north"   ? "west"
                                      : facing == "east"  ? "north"
                                      : facing == "south" ? "east"
                                                          : "south")
                        ? "outer_left"
                        : "outer_right";
        } else {
            const auto [front_facing, front_half] =
                stairs_neighbour(opposite_direction(facing));
            if (!front_facing.empty() && front_half == half &&
                axis(front_facing) != axis(facing)) {
                shape = front_facing == (facing == "north"   ? "west"
                                         : facing == "east"  ? "north"
                                         : facing == "south" ? "east"
                                                             : "south")
                            ? "inner_left"
                            : "inner_right";
            }
        }
        if (replace("shape", shape)) {
            changed = true;
        }
    } else if (parsed.name.ends_with("_wall")) {
        static constexpr std::array<
            std::pair<const char *, std::pair<jint, jint>>, 4>
            directions{{
                {"north", {0, -1}},
                {"east", {1, 0}},
                {"south", {0, 1}},
                {"west", {-1, 0}},
            }};
        for (const auto &[name, offset] : directions) {
            const auto target = block_pos{.x = pos.x + offset.first,
                                          .y = pos.y,
                                          .z = pos.z + offset.second};
            const auto neighbour =
                region.state_at(target.x, target.y, target.z);
            const bool wall =
                state_block_name(states.name(neighbour)).ends_with("_wall");
            std::string value = "none";
            if (wall) {
                const auto above =
                    region.state_at(target.x, target.y + 1, target.z);
                value = state_block_name(states.name(above)).ends_with("_wall")
                            ? "tall"
                            : "low";
            }
            if (replace(name, value)) {
                changed = true;
            }
        }
    }
    if (!changed) {
        return false;
    }
    const auto key = build_state_key(parsed.name, parsed.properties);
    const auto updated = find_state(states, parsed.name, parsed.properties);
    if (updated != no_block_state) {
        (void)region.set_block_state(pos.x, pos.y, pos.z, updated);
        return true;
    }
    const auto interned = states.intern(key, states.traits(current));
    if (interned) {
        (void)region.set_block_state(pos.x, pos.y, pos.z, *interned);
        return true;
    }
    return false;
}

} // namespace

namespace {

auto replace_block_name(std::string key, std::string_view replacement)
    -> std::string {
    const auto open = key.find('[');
    const std::string props =
        open == std::string::npos ? std::string{} : key.substr(open);
    return std::string{replacement} + props;
}

} // namespace

auto rule_processor::process(block_state_registry &states,
                             const chunk_region &region,
                             const template_block_info &info,
                             const block_pos &world,
                             worldgen_random *random) const
    -> std::optional<template_block_info> {
    (void)region;
    worldgen_random local{std::make_unique<legacy_random_source>(
        mth_get_seed(world.x, world.y, world.z))};
    auto &rng = random == nullptr ? local : *random;
    const auto name = state_block_name(states.name(info.state));
    for (const auto &rule : rules_) {
        bool matches = false;
        if (!rule.tag.empty()) {
            matches = states.in_tag(info.state, rule.tag);
        } else {
            for (const auto &block : rule.blocks) {
                matches = matches || block == name;
                if (matches) {
                    break;
                }
            }
        }
        if (!matches) {
            continue;
        }
        if (rule.probability >= 0.0F && rng.next_float() >= rule.probability) {
            continue;
        }
        auto result = info;
        result.state = rule.output;
        return result;
    }
    return info;
}

auto protected_block_processor::process(block_state_registry &states,
                                        const chunk_region &region,
                                        const template_block_info &info,
                                        const block_pos &world,
                                        worldgen_random *random) const
    -> std::optional<template_block_info> {
    (void)random;
    const auto state = region.state_at(world.x, world.y, world.z);
    if (states.in_tag(state, tag_)) {
        return std::nullopt;
    }
    return info;
}

auto lava_submerged_processor::process(block_state_registry &states,
                                       const chunk_region &region,
                                       const template_block_info &info,
                                       const block_pos &world,
                                       worldgen_random *random) const
    -> std::optional<template_block_info> {
    (void)random;
    const auto current = region.state_at(world.x, world.y, world.z);
    if (states.name(current) == "minecraft:lava" &&
        !states.traits(info.state).blocks_motion) {
        auto result = info;
        result.state = lava_;
        return result;
    }
    return info;
}

auto block_age_processor::process(block_state_registry &states,
                                  const chunk_region &region,
                                  const template_block_info &info,
                                  const block_pos &world,
                                  worldgen_random *random) const
    -> std::optional<template_block_info> {
    (void)region;
    // Positional processors randomise from the transformed world position, not
    // from the template-local position carried by the block info.
    worldgen_random local{std::make_unique<legacy_random_source>(
        mth_get_seed(world.x, world.y, world.z))};
    auto &rng = random == nullptr ? local : *random;
    const std::string name{states.name(info.state)};
    const std::string block{state_block_name(name)};
    const auto replace = [&](std::string_view replacement,
                             std::string_view properties) {
        const std::string key =
            std::string{replacement} + "[" + std::string{properties} + "]";
        if (const auto found = states.find(key); found) {
            return *found;
        }
        const auto interned = states.intern(key, states.traits(info.state));
        return interned ? *interned : info.state;
    };
    const auto properties = [&]() -> std::string {
        const auto open = name.find('[');
        if (open == std::string::npos) {
            return {};
        }
        auto text = name.substr(open + 1, name.size() - open - 2);
        return text;
    };
    const auto stair = [&](bool mossy) {
        const char *facing = nullptr;
        switch (rng.next_int(4)) {
        case 0:
            facing = "north";
            break;
        case 1:
            facing = "east";
            break;
        case 2:
            facing = "south";
            break;
        default:
            facing = "west";
            break;
        }
        const char *half = rng.next_int(2) == 0 ? "top" : "bottom";
        const std::string props =
            std::string{"facing="} + facing + ",half=" + half;
        const char *id = mossy ? "minecraft:mossy_stone_brick_stairs"
                               : "minecraft:stone_brick_stairs";
        const std::string key = std::string{id} + "[" + props + "]";
        if (const auto found = states.find(key); found) {
            return *found;
        }
        const auto interned = states.intern(key, states.traits(info.state));
        return interned ? *interned : info.state;
    };
    auto result = info;
    if (block == "minecraft:stone_bricks" || block == "minecraft:stone" ||
        block == "minecraft:chiseled_stone_bricks") {
        if (rng.next_float() >= 0.5F) {
            return info;
        }
        // Both stair variants are built before the mossiness and array
        // choices, so each consumes its facing and half draws.
        const auto non_mossy_stairs = stair(false);
        const auto mossy_stairs = stair(true);
        const bool mossy = rng.next_float() < mossiness_;
        const auto choice = rng.next_int(2);
        if (choice == 0) {
            const auto found =
                states.find(mossy ? "minecraft:mossy_stone_bricks"
                                  : "minecraft:cracked_stone_bricks");
            if (found) {
                result.state = *found;
            }
        } else {
            result.state = mossy ? mossy_stairs : non_mossy_stairs;
        }
        return result;
    }
    if (block.ends_with("_stairs")) {
        if (rng.next_float() >= 0.5F) {
            return info;
        }
        const bool mossy = rng.next_float() < mossiness_;
        if (rng.next_int(2) == 0) {
            if (mossy) {
                result.state =
                    replace("minecraft:mossy_stone_brick_stairs", properties());
            } else {
                const auto found = states.find("minecraft:stone_slab");
                if (found) {
                    result.state = *found;
                }
            }
        } else {
            const auto found =
                states.find(mossy ? "minecraft:mossy_stone_brick_slab"
                                  : "minecraft:stone_brick_slab");
            if (found) {
                result.state = *found;
            }
        }
        return result;
    }
    if (block.ends_with("_slab")) {
        if (rng.next_float() < mossiness_) {
            result.state =
                replace("minecraft:mossy_stone_brick_slab", properties());
            return result;
        }
        return info;
    }
    if (block.ends_with("_wall")) {
        if (rng.next_float() < mossiness_) {
            result.state =
                replace("minecraft:mossy_stone_brick_wall", properties());
            return result;
        }
        return info;
    }
    if (block == "minecraft:obsidian") {
        if (rng.next_float() < 0.15F) {
            const auto found = states.find("minecraft:crying_obsidian");
            if (found) {
                result.state = *found;
                return result;
            }
        }
        return info;
    }
    return info;
}

auto blackstone_replace_processor::process(block_state_registry &states,
                                           const chunk_region &region,
                                           const template_block_info &info,
                                           const block_pos &world,
                                           worldgen_random *random) const
    -> std::optional<template_block_info> {
    (void)region;
    (void)random;
    (void)world;
    static const std::array<std::pair<std::string_view, std::string_view>, 23>
        replacements{{
            {"minecraft:cobblestone", "minecraft:blackstone"},
            {"minecraft:mossy_cobblestone", "minecraft:blackstone"},
            {"minecraft:stone", "minecraft:polished_blackstone"},
            {"minecraft:stone_bricks", "minecraft:polished_blackstone_bricks"},
            {"minecraft:mossy_stone_bricks",
             "minecraft:polished_blackstone_bricks"},
            {"minecraft:cobblestone_stairs", "minecraft:blackstone_stairs"},
            {"minecraft:mossy_cobblestone_stairs",
             "minecraft:blackstone_stairs"},
            {"minecraft:stone_stairs", "minecraft:polished_blackstone_stairs"},
            {"minecraft:stone_brick_stairs",
             "minecraft:polished_blackstone_brick_stairs"},
            {"minecraft:mossy_stone_brick_stairs",
             "minecraft:polished_blackstone_brick_stairs"},
            {"minecraft:cobblestone_slab", "minecraft:blackstone_slab"},
            {"minecraft:mossy_cobblestone_slab", "minecraft:blackstone_slab"},
            {"minecraft:smooth_stone_slab",
             "minecraft:polished_blackstone_slab"},
            {"minecraft:stone_slab", "minecraft:polished_blackstone_slab"},
            {"minecraft:stone_brick_slab",
             "minecraft:polished_blackstone_brick_slab"},
            {"minecraft:mossy_stone_brick_slab",
             "minecraft:polished_blackstone_brick_slab"},
            {"minecraft:stone_brick_wall",
             "minecraft:polished_blackstone_brick_wall"},
            {"minecraft:mossy_stone_brick_wall",
             "minecraft:polished_blackstone_brick_wall"},
            {"minecraft:cobblestone_wall", "minecraft:blackstone_wall"},
            {"minecraft:mossy_cobblestone_wall", "minecraft:blackstone_wall"},
            {"minecraft:chiseled_stone_bricks",
             "minecraft:chiseled_polished_blackstone"},
            {"minecraft:cracked_stone_bricks",
             "minecraft:cracked_polished_blackstone_bricks"},
            {"minecraft:iron_bars", "minecraft:iron_chain"},
        }};
    const auto name = state_block_name(states.name(info.state));
    for (const auto &[from, to] : replacements) {
        if (name == from) {
            const auto key =
                replace_block_name(std::string{states.name(info.state)}, to);
            if (const auto found = states.find(key); found) {
                auto result = info;
                result.state = *found;
                return result;
            }
        }
    }
    return info;
}

auto place_template(chunk_region &region, block_state_registry &states,
                    const structure_template &templ, const block_pos &origin,
                    const template_transform &transform) -> std::size_t {
    return place_template(region, states, templ, origin,
                          template_place_settings{.transform = transform});
}

auto place_template(chunk_region &region, block_state_registry &states,
                    const structure_template &templ, const block_pos &origin,
                    const template_place_settings &settings) -> std::size_t {
    const auto &transform = settings.transform;
    if (templ.palettes.empty()) {
        return 0;
    }
    std::size_t palette_index = 0;
    if (settings.random != nullptr) {
        palette_index = static_cast<std::size_t>(settings.random->next_int(
            static_cast<jint>(templ.palettes.size())));
    } else if (templ.palettes.size() > 1) {
        {
            legacy_random_source random{
                mth_get_seed(origin.x, origin.y, origin.z)};
            palette_index = static_cast<std::size_t>(
                random.next_int(static_cast<jint>(templ.palettes.size())));
        }
    }
    const auto &palette = templ.palettes[palette_index];
    block_state_id barrier = states.air();
    if (const auto found = states.find("minecraft:barrier");
        found.has_value()) {
        barrier = *found;
    }
    bool whole_piece = false;
    bool any_shape = false;
    for (const auto *processor : settings.processors) {
        if (processor->evaluates_entire_piece_state()) {
            whole_piece = true;
            break;
        }
    }
    std::vector<template_block_info> processed_blocks;
    std::vector<block_pos> processed_positions;
    processed_blocks.reserve(palette.size());
    processed_positions.reserve(palette.size());
    for (const auto &block : palette) {
        const auto relative = transform_position(
            block_pos{.x = block.x, .y = block.y, .z = block.z}, transform);
        const block_pos world{.x = origin.x + relative.x,
                              .y = origin.y + relative.y,
                              .z = origin.z + relative.z};
        if (!whole_piece && settings.bounds.has_value() &&
            !settings.bounds->contains(world)) {
            continue;
        }
        auto processed = block;
        bool dropped = false;
        for (const auto *processor : settings.processors) {
            const auto result = processor->process(states, region, processed,
                                                   world, settings.random);
            if (!result) {
                dropped = true;
                break;
            }
            processed = *result;
        }
        if (dropped) {
            continue;
        }
        if (is_shape_sensitive(states, processed.state)) {
            any_shape = true;
        }
        processed_blocks.push_back(std::move(processed));
        processed_positions.push_back(world);
    }
    if (whole_piece) {
        for (const auto *processor : settings.processors) {
            if (processor->evaluates_entire_piece_state()) {
                processor->finalize_processing(states, settings.level_seed,
                                               origin, processed_blocks);
            }
        }
    }
    std::size_t placed = 0;
    std::vector<block_pos> placed_positions;
    for (std::size_t index = 0; index < processed_blocks.size(); ++index) {
        const auto &processed = processed_blocks[index];
        const auto world = processed_positions[index];
        if (settings.bounds.has_value() && !settings.bounds->contains(world)) {
            continue;
        }
        const auto state = transform_state(states, processed.state, transform);
        std::string previous_fluid;
        if (settings.liquids == liquid_settings::apply_waterlogging) {
            previous_fluid = std::string{
                states.name(region.state_at(world.x, world.y, world.z))};
            const bool source =
                previous_fluid == "minecraft:water" ||
                previous_fluid.starts_with("minecraft:water[level=0") ||
                previous_fluid.starts_with("minecraft:water[level=0,");
            if (!source) {
                previous_fluid.clear();
            }
        }
        if (processed.nbt != nullptr) {
            (void)region.set_block_state(world.x, world.y, world.z, barrier);
        }
        if (!region.set_block_state(world.x, world.y, world.z, state)) {
            continue;
        }
        placed_positions.push_back(world);
        ++placed;
        if (!previous_fluid.empty()) {
            auto current = std::string{states.name(state)};
            if (current.find("waterlogged=false") != std::string::npos) {
                current = with_state_property(current, "waterlogged", "true");
                std::optional<block_state_id> waterlogged =
                    states.find(current);
                if (!waterlogged) {
                    if (const auto inserted =
                            states.intern(current, states.traits(state));
                        inserted) {
                        waterlogged = *inserted;
                    }
                }
                if (waterlogged) {
                    (void)region.set_block_state(world.x, world.y, world.z,
                                                 *waterlogged);
                }
            }
        }
        if (processed.nbt != nullptr) {
            auto entity = *processed.nbt;
            if (settings.loot_random != nullptr &&
                entity.find("LootTable") != entity.end()) {
                entity["LootTableSeed"] = data::nbt::value{
                    static_cast<jlong>(settings.loot_random->next_long())};
            }
            entity["x"] = data::nbt::value{world.x};
            entity["y"] = data::nbt::value{world.y};
            entity["z"] = data::nbt::value{world.z};
            (void)region.set_block_entity(world.x, world.y, world.z,
                                          std::move(entity));
        }
    }
    if (!any_shape) {
        return placed;
    }
    std::set<std::tuple<jint, jint, jint>> placed_set;
    for (const auto &pos : placed_positions) {
        placed_set.emplace(pos.x, pos.y, pos.z);
    }
    for (const auto &pos : placed_positions) {
        for (const auto *direction :
             {"north", "east", "south", "west", "up", "down"}) {
            const auto target = neighbour_pos(pos, direction);
            if (placed_set.contains(std::tuple<jint, jint, jint>{
                    target.x, target.y, target.z})) {
                continue;
            }
            if (is_shape_sensitive(states,
                                   region.state_at(pos.x, pos.y, pos.z))) {
                (void)update_template_block(states, region, pos);
            }
            if (is_shape_sensitive(
                    states, region.state_at(target.x, target.y, target.z))) {
                (void)update_template_block(states, region, target);
            }
        }
    }
    for (const auto &pos : placed_positions) {
        if (is_shape_sensitive(states, region.state_at(pos.x, pos.y, pos.z))) {
            (void)update_template_block(states, region, pos);
        }
    }
    return placed;
}

auto template_bounding_box(const structure_template &templ,
                           const template_transform &transform,
                           const block_pos &origin) -> template_bounds {
    const auto low =
        transform_position(block_pos{.x = 0, .y = 0, .z = 0}, transform);
    const auto high = transform_position(block_pos{.x = templ.size_x - 1,
                                                   .y = templ.size_y - 1,
                                                   .z = templ.size_z - 1},
                                         transform);
    template_bounds bounds{
        .min_x = origin.x + std::min(low.x, high.x),
        .min_y = origin.y + std::min(low.y, high.y),
        .min_z = origin.z + std::min(low.z, high.z),
        .max_x = origin.x + std::max(low.x, high.x),
        .max_y = origin.y + std::max(low.y, high.y),
        .max_z = origin.z + std::max(low.z, high.z),
    };
    return bounds;
}

auto block_ignore_processor::process(block_state_registry &states,
                                     const chunk_region &region,
                                     const template_block_info &info,
                                     const block_pos &world,
                                     worldgen_random *random) const
    -> std::optional<template_block_info> {
    (void)region;
    (void)world;
    (void)random;
    const auto name = state_block_name(states.name(info.state));
    if (std::find(blocks_.begin(), blocks_.end(), name) != blocks_.end()) {
        return std::nullopt;
    }
    return info;
}

auto block_rot_processor::process(block_state_registry &states,
                                  const chunk_region &region,
                                  const template_block_info &info,
                                  const block_pos &world,
                                  worldgen_random *random) const
    -> std::optional<template_block_info> {
    (void)region;
    if (has_rottable_) {
        const auto name = state_block_name(states.name(info.state));
        const auto matches = [&](std::string_view entry) {
            if (entry.starts_with('#')) {
                return states.in_tag(info.state, entry.substr(1));
            }
            return entry == name;
        };
        if (std::none_of(rottable_.begin(), rottable_.end(), matches)) {
            return info;
        }
    }
    float draw = 0.0F;
    if (random != nullptr) {
        draw = random->next_float();
    } else {
        legacy_random_source local{mth_get_seed(world.x, world.y, world.z)};
        draw = local.next_float();
    }
    if (!(draw <= integrity_)) {
        return std::nullopt;
    }
    return info;
}

auto protected_blocks_processor::process(block_state_registry &states,
                                         const chunk_region &region,
                                         const template_block_info &info,
                                         const block_pos &world,
                                         worldgen_random *random) const
    -> std::optional<template_block_info> {
    (void)random;
    if (states.in_tag(region.state_at(world.x, world.y, world.z), tag_)) {
        return std::nullopt;
    }
    return info;
}

auto transform_position(const block_pos &pos,
                        const template_transform &transform) -> block_pos {
    jint x = pos.x;
    jint z = pos.z;
    switch (transform.mirror) {
    case template_mirror::left_right:
        z = -z;
        break;
    case template_mirror::front_back:
        x = -x;
        break;
    case template_mirror::none:
        break;
    }
    const jint pivot_x = transform.pivot.x;
    const jint pivot_z = transform.pivot.z;
    switch (transform.rotation) {
    case template_rotation::counterclockwise_90:
        return {
            .x = pivot_x - pivot_z + z, .y = pos.y, .z = pivot_x + pivot_z - x};
    case template_rotation::clockwise_90:
        return {
            .x = pivot_x + pivot_z - z, .y = pos.y, .z = pivot_z - pivot_x + x};
    case template_rotation::clockwise_180:
        return {
            .x = pivot_x + pivot_x - x, .y = pos.y, .z = pivot_z + pivot_z - z};
    case template_rotation::none:
        break;
    }
    return {.x = x, .y = pos.y, .z = z};
}

auto transform_state(block_state_registry &states, block_state_id state,
                     const template_transform &transform) -> block_state_id {
    const auto parsed = parse_state(states.name(state));
    const auto is_face = [](std::string_view name) {
        return name == "north" || name == "south" || name == "east" ||
               name == "west" || name == "up" || name == "down";
    };
    // Stair shapes mirror with the block's own rule before the generic
    // facing transform: LEFT_RIGHT swaps both left/right pairs for Z-axis
    // facings, FRONT_BACK swaps only the outer pair for X-axis facings.
    std::string stair_shape{};
    std::string original_facing{};
    if (parsed.name.ends_with("_stairs")) {
        for (const auto &[property, value] : parsed.properties) {
            if (property == "shape") {
                stair_shape = value;
            } else if (property == "facing") {
                original_facing = value;
            }
        }
        const bool z_axis =
            original_facing == "north" || original_facing == "south";
        const bool x_axis =
            original_facing == "east" || original_facing == "west";
        const auto swap = [&](std::string_view left, std::string_view right) {
            if (stair_shape == left) {
                stair_shape = right;
            } else if (stair_shape == right) {
                stair_shape = left;
            }
        };
        if (transform.mirror == template_mirror::left_right && z_axis) {
            swap("outer_left", "outer_right");
            swap("inner_left", "inner_right");
        } else if (transform.mirror == template_mirror::front_back && x_axis) {
            swap("outer_left", "outer_right");
        }
    }
    std::vector<std::pair<std::string, std::string>> properties;
    properties.reserve(parsed.properties.size());
    for (const auto &[property, value] : parsed.properties) {
        if (property == "shape" && !stair_shape.empty()) {
            properties.emplace_back(property, stair_shape);
            continue;
        }
        if (property == "facing") {
            properties.emplace_back(
                property, std::string{rotate_direction(
                              mirror_direction(value, transform.mirror),
                              transform.rotation)});
        } else if (property == "axis") {
            if (transform.rotation == template_rotation::clockwise_90 ||
                transform.rotation == template_rotation::counterclockwise_90) {
                properties.emplace_back(property, value == "x"   ? "z"
                                                  : value == "z" ? "x"
                                                                 : value);
            } else {
                properties.emplace_back(property, value);
            }
        } else if (property == "rotation") {
            properties.emplace_back(
                property, transform_rotation_value(value, transform.mirror,
                                                   transform.rotation));
        } else if (is_face(property)) {
            const auto target =
                rotate_direction(mirror_direction(property, transform.mirror),
                                 transform.rotation);
            properties.emplace_back(std::string{target}, value);
        } else {
            properties.emplace_back(property, value);
        }
    }
    const auto found = find_state(states, parsed.name, properties);
    if (found != no_block_state) {
        return found;
    }
    const auto key = build_state_key(parsed.name, properties);
    const auto interned = states.intern(key, states.traits(state));
    return interned.has_value() ? *interned : state;
}

} // namespace refrain::worldgen::structure
