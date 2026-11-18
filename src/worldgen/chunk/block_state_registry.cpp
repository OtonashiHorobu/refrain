// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/chunk/block_state_codec.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/content.hpp>

#include <refrain/core/io.hpp>
#include <refrain/data/json.hpp>
#include <refrain/data/vanilla.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace refrain::worldgen {

namespace {

constexpr std::array<std::string_view, 3> air_blocks{
    "minecraft:air",
    "minecraft:cave_air",
    "minecraft:void_air",
};

auto is_air_block(std::string_view name) -> bool {
    return std::ranges::contains(air_blocks, name);
}

auto parse_tag_values(const data::json::value &root)
    -> result<std::vector<std::string>> {
    const auto *values = root.find("values");
    if (values == nullptr || !values->is_array()) {
        return std::unexpected{make_error(error_code::parse_error,
                                          "block tag has no values array")};
    }
    const auto array = values->as_array();
    if (!array) {
        return std::unexpected{array.error()};
    }
    std::vector<std::string> entries;
    for (const auto &entry : *array) {
        if (const auto text = entry.as_string(); text.has_value()) {
            entries.emplace_back(*text);
            continue;
        }
        if (entry.is_object()) {
            const auto *id = entry.find("id");
            if (id != nullptr) {
                if (const auto text = id->as_string(); text.has_value()) {
                    entries.emplace_back(*text);
                    continue;
                }
            }
        }
        return std::unexpected{make_error(
            error_code::parse_error, "block tag value is not an id string")};
    }
    return entries;
}

} // namespace

void block_tag_set::add(std::string_view tag,
                        std::vector<std::string> entries) {
    entries_[std::string{tag}] = std::move(entries);
}

auto block_tag_set::has(std::string_view tag) const -> bool {
    return entries_.contains(tag);
}

auto block_tag_set::resolve(std::string_view tag) const
    -> result<std::vector<std::string>> {
    if (const auto cached = resolved_.find(tag); cached != resolved_.end()) {
        return cached->second;
    }
    if (!entries_.contains(tag)) {
        return std::unexpected{
            make_error(error_code::invalid_argument, "unknown block tag")};
    }
    std::vector<std::string> out;
    std::vector<std::string> visiting;
    if (const auto resolved = resolve_into(tag, out, visiting); !resolved) {
        return std::unexpected{resolved.error()};
    }
    std::ranges::sort(out);
    out.erase(std::unique(out.begin(), out.end()), out.end());
    resolved_[std::string{tag}] = out;
    return out;
}

auto block_tag_set::resolve_into(std::string_view tag,
                                 std::vector<std::string> &out,
                                 std::vector<std::string> &visiting) const
    -> status {
    if (std::ranges::contains(visiting, tag)) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "block tag reference cycle")};
    }
    const auto found = entries_.find(tag);
    if (found == entries_.end()) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "unknown nested block tag")};
    }
    visiting.emplace_back(tag);
    for (const auto &entry : found->second) {
        if (!entry.empty() && entry.front() == '#') {
            const auto nested = std::string_view{entry}.substr(1);
            if (const auto resolved = resolve_into(nested, out, visiting);
                !resolved) {
                return std::unexpected{resolved.error()};
            }
        } else {
            out.push_back(entry);
        }
    }
    visiting.pop_back();
    return {};
}

auto load_block_tags(std::string_view data_root) -> result<block_tag_set> {
    const auto materialized = materialize_worldgen_data(data_root);
    if (!materialized) {
        return std::unexpected{materialized.error()};
    }
    data_root = *materialized;
    block_tag_set tags;
    const auto namespaces = core::io::list_directory(data_root);
    if (!namespaces) {
        return std::unexpected{namespaces.error()};
    }
    for (const auto &name : *namespaces) {
        const auto directory =
            std::string{data_root} + "/" + name + "/tags/block";
        const auto files = core::io::list_directory(directory);
        if (!files) {
            if (files.error().code == error_code::not_found) {
                continue;
            }
            return std::unexpected{files.error()};
        }
        for (const auto &file : *files) {
            if (file.size() < 5 || file.substr(file.size() - 5) != ".json") {
                continue;
            }
            const auto path = directory + "/" + file;
            const auto document = data::json::value::parse_file(path);
            if (!document) {
                return std::unexpected{document.error()};
            }
            auto values = parse_tag_values(*document);
            if (!values) {
                return std::unexpected{values.error()};
            }
            tags.add(name + ":" + file.substr(0, file.size() - 5),
                     std::move(*values));
        }
    }
    return tags;
}

auto apply_block_tags(block_state_registry &states, const block_tag_set &tags)
    -> status {
    const auto resolve_names = [&](std::string_view tag) {
        std::set<std::string, std::less<>> names;
        if (!tags.has(tag)) {
            return names;
        }
        auto resolved = tags.resolve(tag);
        if (resolved) {
            names.insert(resolved->begin(), resolved->end());
        }
        return names;
    };
    const auto motion = resolve_names("minecraft:blocks_motion_in_heightmap");
    const auto motion_no_leaves =
        resolve_names("minecraft:blocks_motion_in_heightmap_no_leaves");
    for (const auto &[tag, entries] : tags.entries()) {
        (void)entries;
        auto names = tags.resolve(tag);
        if (!names) {
            return std::unexpected{names.error()};
        }
        std::vector<block_state_id> ids;
        ids.reserve(names->size());
        for (const auto &name : *names) {
            if (const auto found = states.find(name); found.has_value()) {
                ids.push_back(*found);
                continue;
            }
            const auto block = state_block_name(name);
            const bool air = is_air_block_name(block);
            const bool water = block == "minecraft:water";
            const bool fluid = water || block == "minecraft:lava" ||
                               block == "minecraft:bubble_column";
            const auto interned =
                states.intern(name, state_traits{
                                        .air = air,
                                        .fluid = fluid,
                                        .water = water,
                                        .blocks_motion = motion.contains(name),
                                        .blocks_motion_no_leaves =
                                            motion_no_leaves.contains(name),
                                    });
            if (!interned) {
                return std::unexpected{interned.error()};
            }
            ids.push_back(*interned);
        }
        states.add_tag(tag, ids);
    }
    return status_ok();
}

block_state_registry::block_state_registry() {
    (void)intern("minecraft:air", state_traits{.air = true}, 0, 0);
}

auto block_state_registry::infer_traits(std::string_view key) const
    -> state_traits {
    const auto block = state_block_name(key);
    const bool air = is_air_block_name(block);
    const bool water = block == "minecraft:water" ||
                       key.find("waterlogged=true") != std::string_view::npos;
    const bool fluid = water || block == "minecraft:lava" ||
                       block == "minecraft:bubble_column";
    const auto motion = tag_names_.find("minecraft:blocks_motion_in_heightmap");
    if (motion != tag_names_.end()) {
        const auto no_leaves =
            tag_names_.find("minecraft:blocks_motion_in_heightmap_no_leaves");
        return state_traits{
            .air = air,
            .fluid = fluid,
            .water = water,
            .blocks_motion = motion->second.contains(std::string{block}),
            .blocks_motion_no_leaves =
                no_leaves != tag_names_.end() &&
                no_leaves->second.contains(std::string{block}),
        };
    }
    const bool leaves = block.ends_with("_leaves");
    return state_traits{
        .air = air,
        .fluid = fluid,
        .water = water,
        .blocks_motion = !air && !fluid,
        .blocks_motion_no_leaves = !air && !fluid && !leaves,
    };
}

auto block_state_registry::intern_shadow(std::string_view key,
                                         const state_traits &traits,
                                         std::uint16_t block,
                                         std::uint16_t vanilla)
    -> result<block_state_id> {
    if (const auto existing = index_.find(key); existing != index_.end()) {
        return existing->second;
    }
    if (const auto existing = shadow_index_.find(key);
        existing != shadow_index_.end()) {
        return existing->second;
    }
    if (frozen_) {
        std::string message{
            "frozen block state table does not hold state key "};
        message += key;
        return std::unexpected{make_error(error_code::not_found, message)};
    }
    if (keys_.size() >= no_block_state) {
        return std::unexpected{
            make_error(error_code::out_of_memory, "block state table is full")};
    }
    keys_.emplace_back(key);
    const auto id = static_cast<block_state_id>(keys_.size() - 1);
    shadow_index_.emplace(std::string_view{keys_.back()}, id);
    traits_.push_back(traits);
    blocks_.push_back(block);
    vanilla_ids_.push_back(vanilla);
    return id;
}

auto block_state_registry::intern(std::string_view key,
                                  const state_traits &traits,
                                  std::uint16_t block, std::uint16_t vanilla)
    -> result<block_state_id> {
    if (const auto existing = index_.find(key); existing != index_.end()) {
        return existing->second;
    }
    if (const auto shadow = shadow_index_.find(key);
        shadow != shadow_index_.end()) {
        // A shared table already holds the key with the id it takes before
        // generation, so interning it returns that id without making it
        // visible to `find`.
        return shadow->second;
    }
    if (frozen_) {
        // The table holds the complete state universe of the content and was
        // frozen so every worker resolves a key to the same id. Appending it
        // now would make the id depend on which thread reached the key first,
        // so the key is rejected instead.
        std::string message{
            "frozen block state table does not hold state key "};
        message += key;
        return std::unexpected{make_error(error_code::not_found, message)};
    }
    if (keys_.size() >= no_block_state) {
        return std::unexpected{
            make_error(error_code::out_of_memory, "block state table is full")};
    }
    keys_.emplace_back(key);
    const auto id = static_cast<block_state_id>(keys_.size() - 1);
    index_.emplace(std::string_view{keys_.back()}, id);
    traits_.push_back(traits);
    blocks_.push_back(block);
    vanilla_ids_.push_back(vanilla);
    return id;
}

void block_state_registry::add_default_state(std::string_view block,
                                             block_state_id state) {
    defaults_.emplace(std::string{block}, state);
}

void block_state_registry::set_block_defaults(block_default_map defaults) {
    default_properties_ = std::move(defaults);
}

auto block_state_registry::default_properties(block_state_id id) const
    -> std::span<const block_default_property> {
    if (id >= keys_.size()) {
        return {};
    }
    return default_properties_for_block(state_block_name(keys_[id]));
}

auto block_state_registry::default_properties_for_block(
    std::string_view block) const -> std::span<const block_default_property> {
    const auto found = default_properties_.find(std::string{block});
    if (found == default_properties_.end()) {
        return {};
    }
    return found->second;
}

auto block_state_registry::find(std::string_view key) const
    -> std::optional<block_state_id> {
    if (const auto found = index_.find(key); found != index_.end()) {
        return found->second;
    }
    // A datapack names a plain block when it wants that block's default
    // state; the canonical key of that state spells its properties out.
    if (key.find('[') != std::string_view::npos) {
        return std::nullopt;
    }
    const auto found = defaults_.find(std::string{key});
    return found == defaults_.end()
               ? std::nullopt
               : std::optional<block_state_id>{found->second};
}

auto block_state_registry::block(block_state_id id) const -> std::uint16_t {
    return id < blocks_.size() ? blocks_[id] : 0;
}

auto block_state_registry::vanilla_id(block_state_id id) const
    -> std::uint16_t {
    return id < vanilla_ids_.size() ? vanilla_ids_[id] : no_block_state;
}

void block_state_registry::add_tag(std::string_view tag,
                                   std::span<const block_state_id> states) {
    auto &stored = tags_[std::string{tag}];
    stored.assign(states.begin(), states.end());
    std::ranges::sort(stored);
    stored.erase(std::unique(stored.begin(), stored.end()), stored.end());
    auto &ordered = ordered_tags_[std::string{tag}];
    ordered.clear();
    ordered.reserve(stored.size());
    for (const auto id : states) {
        if (!std::ranges::contains(ordered, id)) {
            ordered.push_back(id);
        }
    }
    // Block tags name blocks, not one property variant. Keep the block names so
    // a state variant interned after the tag was registered still matches.
    auto &names = tag_names_[std::string{tag}];
    for (const auto id : states) {
        if (id < keys_.size()) {
            names.emplace(state_block_name(keys_[id]));
        }
    }
}

auto block_state_registry::air() const -> block_state_id {
    const auto found = index_.find("minecraft:air");
    return found == index_.end() ? 0 : found->second;
}

auto block_state_registry::traits(block_state_id id) const -> state_traits {
    return id < traits_.size() ? traits_[id] : state_traits{};
}

auto block_state_registry::name(block_state_id id) const -> std::string_view {
    return id < keys_.size() ? std::string_view{keys_[id]} : std::string_view{};
}

auto block_state_registry::tag_states(std::string_view tag) const
    -> std::span<const block_state_id> {
    const auto found = ordered_tags_.find(tag);
    if (found == ordered_tags_.end()) {
        return {};
    }
    return found->second;
}

auto block_state_registry::in_tag(block_state_id id, std::string_view tag) const
    -> bool {
    const auto found = tags_.find(tag);
    if (found != tags_.end() && std::ranges::binary_search(found->second, id)) {
        return true;
    }
    const auto names = tag_names_.find(tag);
    return names != tag_names_.end() && id < keys_.size() &&
           names->second.contains(std::string{state_block_name(keys_[id])});
}

namespace {

auto build_registry_impl(const data::vanilla_tables &tables,
                         const block_tag_set &tag_set, bool shared)
    -> result<block_state_registry> {
    std::unordered_set<std::string> motion;
    std::unordered_set<std::string> motion_no_leaves;
    if (tag_set.has("minecraft:blocks_motion_in_heightmap")) {
        auto resolved = tag_set.resolve("minecraft:blocks_motion_in_heightmap");
        if (!resolved) {
            return std::unexpected{resolved.error()};
        }
        motion.insert(resolved->begin(), resolved->end());
    }
    if (tag_set.has("minecraft:blocks_motion_in_heightmap_no_leaves")) {
        auto resolved =
            tag_set.resolve("minecraft:blocks_motion_in_heightmap_no_leaves");
        if (!resolved) {
            return std::unexpected{resolved.error()};
        }
        motion_no_leaves.insert(resolved->begin(), resolved->end());
    }

    block_state_registry registry;
    block_default_map default_properties;
    std::unordered_map<std::string, std::vector<block_state_id>> by_block;
    for (std::uint32_t index = 0; index < tables.state_count(); ++index) {
        const auto state = tables.state_at(index);
        const auto block = tables.block_at(state.block);
        const std::string block_name{block.name};

        bool waterlogged = false;
        const auto properties = tables.block_properties(state.block);
        for (std::size_t i = 0; i < properties.size(); ++i) {
            if (tables.property_name(properties[i]) == "waterlogged" &&
                tables.property_value(properties[i], state.values[i]) ==
                    "true") {
                waterlogged = true;
            }
        }

        const bool water = block_name == "minecraft:water" || waterlogged;
        const bool fluid = water || block_name == "minecraft:lava" ||
                           block_name == "minecraft:bubble_column";
        const state_traits traits{
            .air = is_air_block(block_name),
            .fluid = fluid,
            .water = water,
            .blocks_motion = motion.contains(block_name),
            .blocks_motion_no_leaves = motion_no_leaves.contains(block_name),
        };

        auto id =
            registry.intern(state.key, traits, state.block, state.vanilla_id);
        if (!id) {
            return std::unexpected{id.error()};
        }
        if (shared && index == block.default_state) {
            // Generation folds a default-property spelling to the bare block
            // name. Interning the bare name up front means both spellings hit
            // the frozen table instead of appending a first-use id.
            const auto bare = registry.intern(block.name, traits, state.block,
                                              state.vanilla_id);
            if (!bare) {
                return std::unexpected{bare.error()};
            }
            registry.add_default_state(block.name, *bare);
        } else if (index == block.default_state) {
            registry.add_default_state(block.name, *id);
            block_default_properties defaults_for_block;
            const auto property_ids = tables.block_properties(state.block);
            for (std::size_t property = 0; property < property_ids.size();
                 ++property) {
                defaults_for_block.push_back(block_default_property{
                    .name = std::string{tables.property_name(
                        property_ids[property])},
                    .value = std::string{tables.property_value(
                        property_ids[property], state.values[property])},
                });
            }
            default_properties.emplace(block.name,
                                       std::move(defaults_for_block));
        }
        by_block[block_name].push_back(*id);
    }

    for (const auto &[tag, entries] : tag_set.entries()) {
        (void)entries;
        auto names = tag_set.resolve(tag);
        if (!names) {
            return std::unexpected{names.error()};
        }
        std::vector<block_state_id> ids;
        for (const auto &name : *names) {
            const auto found = by_block.find(name);
            if (found == by_block.end()) {
                continue;
            }
            ids.insert(ids.end(), found->second.begin(), found->second.end());
        }
        registry.add_tag(tag, ids);
    }
    registry.set_block_defaults(std::move(default_properties));
    return registry;
}

} // namespace

auto build_block_state_registry(const data::vanilla_tables &tables,
                                const block_tag_set &tag_set)
    -> result<block_state_registry> {
    return build_registry_impl(tables, tag_set, false);
}

auto build_shared_block_state_registry(const data::vanilla_tables &tables,
                                       const block_tag_set &tag_set)
    -> result<block_state_registry> {
    std::unordered_set<std::string> motion;
    std::unordered_set<std::string> motion_no_leaves;
    if (tag_set.has("minecraft:blocks_motion_in_heightmap")) {
        auto resolved = tag_set.resolve("minecraft:blocks_motion_in_heightmap");
        if (!resolved) {
            return std::unexpected{resolved.error()};
        }
        motion.insert(resolved->begin(), resolved->end());
    }
    if (tag_set.has("minecraft:blocks_motion_in_heightmap_no_leaves")) {
        auto resolved =
            tag_set.resolve("minecraft:blocks_motion_in_heightmap_no_leaves");
        if (!resolved) {
            return std::unexpected{resolved.error()};
        }
        motion_no_leaves.insert(resolved->begin(), resolved->end());
    }

    block_state_registry registry;
    block_default_map default_properties;
    std::unordered_map<std::string, std::vector<block_state_id>> by_block;
    for (std::uint32_t index = 0; index < tables.state_count(); ++index) {
        const auto state = tables.state_at(index);
        const auto block = tables.block_at(state.block);
        const std::string block_name{block.name};

        bool waterlogged = false;
        const auto properties = tables.block_properties(state.block);
        for (std::size_t i = 0; i < properties.size(); ++i) {
            if (tables.property_name(properties[i]) == "waterlogged" &&
                tables.property_value(properties[i], state.values[i]) ==
                    "true") {
                waterlogged = true;
            }
        }

        const bool water = block_name == "minecraft:water" || waterlogged;
        const bool fluid = water || block_name == "minecraft:lava" ||
                           block_name == "minecraft:bubble_column";
        const state_traits traits{
            .air = is_air_block(block_name),
            .fluid = fluid,
            .water = water,
            .blocks_motion = motion.contains(block_name),
            .blocks_motion_no_leaves = motion_no_leaves.contains(block_name),
        };

        // The canonical spelling is what a feature or structure state provider
        // derives to; the bare block name is what the terrain, the material
        // rule and a default-spelling lookup ask for. Both are interned before
        // the table freezes, in the table's key order, and both stay invisible
        // to `find`: a lookup keeps the per-generator registry's behaviour,
        // where a key the data pack did not intern is a miss.
        auto canonical = registry.intern_shadow(state.key, traits, state.block,
                                                state.vanilla_id);
        if (!canonical) {
            return std::unexpected{canonical.error()};
        }
        if (index == block.default_state) {
            if (const auto bare = registry.intern_shadow(
                    block.name, traits, state.block, state.vanilla_id);
                !bare) {
                return std::unexpected{bare.error()};
            }
            block_default_properties defaults_for_block;
            const auto property_ids = tables.block_properties(state.block);
            for (std::size_t property = 0; property < property_ids.size();
                 ++property) {
                defaults_for_block.push_back(block_default_property{
                    .name = std::string{tables.property_name(
                        property_ids[property])},
                    .value = std::string{tables.property_value(
                        property_ids[property], state.values[property])},
                });
            }
            default_properties.emplace(block.name,
                                       std::move(defaults_for_block));
        }
        by_block[block_name].push_back(*canonical);
    }

    for (const auto &[tag, entries] : tag_set.entries()) {
        (void)entries;
        auto names = tag_set.resolve(tag);
        if (!names) {
            return std::unexpected{names.error()};
        }
        std::vector<block_state_id> ids;
        for (const auto &name : *names) {
            const auto found = by_block.find(name);
            if (found == by_block.end()) {
                continue;
            }
            ids.insert(ids.end(), found->second.begin(), found->second.end());
        }
        registry.add_tag(tag, ids);
    }
    registry.set_block_defaults(std::move(default_properties));
    return registry;
}

} // namespace refrain::worldgen
