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
#include <string>
#include <utility>

namespace refrain::worldgen::structure {

namespace {

struct parsed_state {
    std::string_view name{};
    std::vector<std::pair<std::string, std::string>> properties{};
};

auto parse_state(std::string_view key) -> parsed_state;
auto find_state(
    const block_state_registry &states, std::string_view name,
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
    const auto *name_value = child(&entry, "Name");
    if (name_value == nullptr) {
        return std::unexpected{make_error(error_code::parse_error,
                                          "template palette has no Name")};
    }
    const auto name = name_value->as_string();
    if (!name) {
        return std::unexpected{make_error(error_code::parse_error,
                                          "template palette Name is invalid")};
    }
    std::string key{*name};
    const auto *properties = child(&entry, "Properties");
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

auto resolve_palette_state(const block_state_registry &states,
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
    // Unknown properties are ignored by the reference; fall back to the
    // block's plain key when the full state is not known.
    if (const auto found = states.find(parsed.name); found.has_value()) {
        return *found;
    }
    return states.air();
}

auto is_full_block(const block_state_registry &states, block_state_id state)
    -> bool {
    const auto traits = states.traits(state);
    return !traits.air && !traits.fluid && traits.blocks_motion;
}

auto load_palette(const block_state_registry &states,
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
    const block_state_registry &states, std::string_view name,
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

auto load_structure_template(const block_state_registry &states,
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

auto structure_template_manager::get_or_create(std::string_view id)
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
    if (templ.palettes.size() > 1) {
        legacy_random_source random{mth_get_seed(origin.x, origin.y, origin.z)};
        palette_index = static_cast<std::size_t>(
            random.next_int(static_cast<jint>(templ.palettes.size())));
    }
    const auto &palette = templ.palettes[palette_index];
    block_state_id barrier = states.air();
    if (const auto found = states.find("minecraft:barrier");
        found.has_value()) {
        barrier = *found;
    }
    std::size_t placed = 0;
    for (const auto &block : palette) {
        const auto relative = transform_position(
            block_pos{.x = block.x, .y = block.y, .z = block.z}, transform);
        const block_pos world{.x = origin.x + relative.x,
                              .y = origin.y + relative.y,
                              .z = origin.z + relative.z};
        if (settings.bounds.has_value() && !settings.bounds->contains(world)) {
            continue;
        }
        auto processed = block;
        bool dropped = false;
        for (const auto *processor : settings.processors) {
            const auto result = processor->process(states, processed, world);
            if (!result) {
                dropped = true;
                break;
            }
            processed = *result;
        }
        if (dropped) {
            continue;
        }
        const auto state = transform_state(states, processed.state, transform);
        if (processed.nbt != nullptr) {
            (void)region.set_block_state(world.x, world.y, world.z, barrier);
        }
        if (!region.set_block_state(world.x, world.y, world.z, state)) {
            continue;
        }
        ++placed;
        if (processed.nbt != nullptr) {
            auto entity = *processed.nbt;
            entity["x"] = data::nbt::value{world.x};
            entity["y"] = data::nbt::value{world.y};
            entity["z"] = data::nbt::value{world.z};
            (void)region.set_block_entity(world.x, world.y, world.z,
                                          std::move(entity));
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

auto block_ignore_processor::process(const block_state_registry &states,
                                     const template_block_info &info,
                                     const block_pos &world) const
    -> std::optional<template_block_info> {
    (void)world;
    const auto name = state_block_name(states.name(info.state));
    if (std::find(blocks_.begin(), blocks_.end(), name) != blocks_.end()) {
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
    std::vector<std::pair<std::string, std::string>> properties;
    properties.reserve(parsed.properties.size());
    for (const auto &[property, value] : parsed.properties) {
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
