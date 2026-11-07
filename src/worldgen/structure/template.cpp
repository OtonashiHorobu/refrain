// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/structure/template.hpp>

#include <refrain/worldgen/chunk/block_state_codec.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <utility>

namespace refrain::worldgen::structure {

namespace {

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
    // Unknown properties are ignored by the reference; fall back to the
    // block's plain key when the full state is not known.
    const auto name = state_block_name(*key);
    if (const auto found = states.find(name); found.has_value()) {
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

} // namespace refrain::worldgen::structure
