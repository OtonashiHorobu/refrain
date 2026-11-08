// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Test-side template priming: a structure oracle that only knows the blocks of
// the compared reference chunk would resolve every other template state to air
// because the template manager's registry does not contain the block name. The
// oracle tests prime the registry from the template palettes they are about to
// place, which gives the same state keys the manager resolves later.

#pragma once

#include <refrain/core/io.hpp>
#include <refrain/data/nbt.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/structure/template.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::worldgen::test_support {

inline auto palette_key(const data::nbt::compound &entry) -> std::string {
    auto name_iterator = entry.find("id");
    if (name_iterator == entry.end()) {
        name_iterator = entry.find("Name");
    }
    if (name_iterator == entry.end()) {
        return {};
    }
    const auto name = name_iterator->second.as_string();
    if (!name) {
        return {};
    }
    std::string key{*name};
    auto properties = entry.find("properties");
    if (properties == entry.end()) {
        properties = entry.find("Properties");
    }
    if (properties == entry.end() ||
        properties->second.as_compound() == nullptr) {
        return key;
    }
    key.push_back('[');
    bool first = true;
    for (const auto &[property, value] : *properties->second.as_compound()) {
        if (!first) {
            key.push_back(',');
        }
        first = false;
        key.append(property);
        key.push_back('=');
        key.append(value.as_string().value_or(""));
    }
    key.push_back(']');
    return key;
}

inline void prime_palette(block_state_registry &states,
                          const data::nbt::compound &root) {
    const auto prime_list = [&](const data::nbt::list_value &palette) {
        for (const auto &entry : palette.items) {
            const auto *compound = entry.as_compound();
            if (compound == nullptr) {
                continue;
            }
            const auto key = palette_key(*compound);
            if (!key.empty()) {
                (void)structure::resolve_block_state(states, key);
            }
        }
    };
    const auto palettes = root.find("palettes");
    if (palettes != root.end() && palettes->second.as_list() != nullptr) {
        for (const auto &entry : palettes->second.as_list()->items) {
            if (entry.as_list() != nullptr) {
                prime_list(*entry.as_list());
            }
        }
        return;
    }
    const auto palette = root.find("palette");
    if (palette != root.end() && palette->second.as_list() != nullptr) {
        prime_list(*palette->second.as_list());
    }
}

/// Interns every palette state of the templates under
/// `<templates_root>/<directory>`.
inline void prime_template_states(block_state_registry &states,
                                  std::string_view templates_root,
                                  std::string_view directory) {
    const std::string base =
        std::string{templates_root} + "/" + std::string{directory};
    const auto entries = core::io::list_directory(base);
    if (!entries) {
        return;
    }
    std::vector<std::string> sorted{entries->begin(), entries->end()};
    std::sort(sorted.begin(), sorted.end());
    for (const auto &entry : sorted) {
        const auto path = base + "/" + entry;
        if (core::io::is_directory(path)) {
            prime_template_states(states, templates_root,
                                  std::string{directory} + "/" + entry);
            continue;
        }
        if (entry.size() <= 4 || entry.substr(entry.size() - 4) != ".nbt") {
            continue;
        }
        const auto document =
            data::nbt::read_file(path, data::nbt::compression::gzip);
        if (!document) {
            continue;
        }
        const auto *root = document->as_compound();
        if (root != nullptr) {
            prime_palette(states, *root);
        }
    }
}

} // namespace refrain::worldgen::test_support
