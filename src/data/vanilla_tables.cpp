// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/vanilla.hpp>

#include <refrain/core/byte_order.hpp>
#include <refrain/data/checksum.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace refrain::data {

namespace {

constexpr std::size_t header_size = 72;
constexpr std::size_t section_record_size = 16;
constexpr std::size_t block_record_size = 24;
constexpr std::size_t property_record_size = 16;
constexpr std::size_t state_record_size = 20;
constexpr std::size_t preset_record_size = 16;
constexpr std::size_t biome_entry_size = 56;
constexpr std::size_t registry_record_size = 16;
constexpr std::size_t registry_entry_size = 8;
constexpr std::size_t item_record_size = 16;
constexpr std::size_t max_sections = 16;

constexpr std::array<std::byte, 4> magic{std::byte{'R'}, std::byte{'V'},
                                         std::byte{'T'}, std::byte{'B'}};

auto fail(std::string_view message) -> error {
    return make_error(error_code::invalid_argument, message);
}

/// Whether a stored tool family mines at one tier speed; the tiered families
/// are the first four values of `tool_type`.
auto tiered_tool(std::uint8_t type) -> bool {
    return type <= static_cast<std::uint8_t>(tool_type::hoe);
}

/// Append `value` to `out` as little-endian bytes.
template <core::byte_order::fixed_width_integer T>
void append_le(std::vector<std::byte> &out, T value) {
    const auto offset{out.size()};
    out.resize(offset + sizeof(T));
    core::byte_order::store_le<T>(out, offset, value);
}

void put_u16(std::vector<std::byte> &out, std::uint16_t value) {
    append_le(out, value);
}

void put_u32(std::vector<std::byte> &out, std::uint32_t value) {
    append_le(out, value);
}

void put_u64(std::vector<std::byte> &out, std::uint64_t value) {
    append_le(out, value);
}

void put_i32(std::vector<std::byte> &out, std::int32_t value) {
    append_le(out, value);
}

void put_bytes(std::vector<std::byte> &out, std::span<const std::byte> bytes) {
    out.append_range(bytes);
}

void put_f64(std::vector<std::byte> &out, double value) {
    append_le(out, std::bit_cast<std::uint64_t>(value));
}

void put_f32(std::vector<std::byte> &out, float value) {
    append_le(out, std::bit_cast<std::uint32_t>(value));
}

using core::byte_order::load_le;
using core::byte_order::load_le16;
using core::byte_order::load_le32;
using core::byte_order::load_le64;

auto align8(std::size_t value) -> std::size_t {
    return (value + 7U) & ~std::size_t{7U};
}

auto hash64(std::string_view text) -> std::uint64_t {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const char ch : text) {
        hash ^= static_cast<std::uint8_t>(ch);
        hash *= 1099511628211ULL;
    }
    hash ^= hash >> 33U;
    hash *= 0xFF51AFD7ED558CCDULL;
    hash ^= hash >> 33U;
    return hash;
}

auto capacity_for(std::size_t count) -> std::size_t {
    std::size_t capacity = 8;
    while (capacity < count * 2U) {
        capacity <<= 1U;
    }
    return capacity;
}

/// Exact string interning for the build step.
class interner {
  public:
    auto add(std::string_view text) -> std::uint32_t {
        const std::string key{text};
        if (const auto found = ids_.find(key); found != ids_.end()) {
            return found->second;
        }
        const auto id = static_cast<std::uint32_t>(strings_.size());
        strings_.push_back(key);
        ids_.emplace(key, id);
        return id;
    }

    [[nodiscard]] auto strings() const -> const std::vector<std::string> & {
        return strings_;
    }

  private:
    std::vector<std::string> strings_{};
    std::unordered_map<std::string, std::uint32_t> ids_{};
};

struct built_block {
    std::uint32_t name{0};
    std::uint32_t first_state{0};
    std::uint32_t state_count{0};
    std::uint32_t default_state{0};
    std::uint32_t first_property{0};
    std::uint16_t property_count{0};
};

struct built_property {
    std::uint32_t name{0};
    std::uint32_t first_value{0};
    std::uint16_t value_count{0};
};

struct built_state {
    std::uint16_t block{0};
    std::uint16_t vanilla_id{0};
    std::uint8_t property_count{0};
    std::array<std::uint8_t, 8> values{};
    std::uint32_t key{0};
    bool is_default{false};
};

struct built_biome_entry {
    std::array<std::int32_t, 6> min{};
    std::array<std::int32_t, 6> max{};
    std::int32_t offset{0};
    std::uint32_t biome{0};
};

struct built_preset {
    std::uint32_t name{0};
    std::uint32_t first_entry{0};
    std::uint32_t entry_count{0};
};

struct built_registry {
    std::uint32_t name{0};
    std::uint32_t first_entry{0};
    std::uint32_t entry_count{0};
};

struct built_registry_entry {
    std::uint32_t name{0};
    std::int32_t protocol_id{0};
};

struct built_item {
    std::uint32_t name{0};
    std::uint16_t max_stack_size{0};
    std::uint32_t max_damage{0};
};

struct built_mining {
    float hardness{0.0F};
    float shears_speed{1.0F};
    float sword_speed{1.0F};
    std::uint8_t flags{0};
    std::uint8_t tool_mask{0};
    std::uint8_t needed_tier{0};
    std::uint8_t direct_correct{0};
};

struct built_tool {
    std::uint32_t name{0};
    std::uint8_t type{0};
    std::uint8_t material{0};
    std::uint8_t harvest_level{harvest_level_none};
    std::uint8_t flags{0};
    float tier_speed{1.0F};
    float default_mining_speed{1.0F};
    std::uint16_t damage_per_block{0};
    std::uint16_t max_stack_size{0};
    std::uint32_t max_damage{0};
};

/// Flag bit of one stored tool record: the item can break blocks in creative.
inline constexpr std::uint8_t tool_flag_can_destroy_in_creative = 1U;

/// Number of bytes of one stored block mining record.
constexpr std::size_t mining_record_size = 16;

/// Number of bytes of one stored tool record.
constexpr std::size_t tool_record_size = 24;

struct built_light {
    std::uint8_t emission{0};
    std::uint8_t dampening{15};
    std::uint8_t flags{0};
    std::uint32_t shape{no_light_shape};
};

/// Flag bits of one stored light record.
inline constexpr std::uint8_t light_flag_can_occlude = 1U;
inline constexpr std::uint8_t light_flag_use_shape = 2U;

/// Sentinel that stores "no occlusion shape" in the fixed-width record.
inline constexpr std::uint16_t no_light_shape_u16 = 0xFFFFU;

/// Number of bytes of one stored light record.
constexpr std::size_t light_record_size = 8;

/// Number of bytes of one stored light shape record.
constexpr std::size_t light_shape_record_size = 16;

/// Number of bytes of one stored light shape box.
constexpr std::size_t light_box_size = 48;

/// Canonical state key: `name` or `name[k=v,...]` with sorted property names.
auto canonical_key(std::string_view block, std::span<const std::string> names,
                   std::span<const std::string> values) -> std::string {
    if (names.empty()) {
        return std::string{block};
    }
    std::string key{block};
    key.push_back('[');
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i != 0) {
            key.push_back(',');
        }
        key.append(names[i]);
        key.push_back('=');
        key.append(values[i]);
    }
    key.push_back(']');
    return key;
}

} // namespace

auto build_vanilla_tables(const vanilla_spec &spec)
    -> result<std::vector<std::byte>> {
    interner pool;

    // Blocks sorted by name; duplicates rejected.
    std::vector<const block_spec *> blocks;
    blocks.reserve(spec.blocks.size());
    for (const auto &block : spec.blocks) {
        if (block.name.empty()) {
            return std::unexpected{fail("block with an empty name")};
        }
        blocks.push_back(&block);
    }
    std::ranges::sort(blocks, {}, &block_spec::name);
    for (std::size_t i = 1; i < blocks.size(); ++i) {
        if (blocks[i - 1]->name == blocks[i]->name) {
            return std::unexpected{
                fail("duplicate block name: " + blocks[i]->name)};
        }
    }

    std::vector<built_block> out_blocks;
    std::vector<built_property> out_properties;
    std::vector<std::uint32_t> property_value_offsets;
    std::vector<std::uint8_t> block_property_ids;
    std::vector<built_state> out_states;
    std::unordered_map<std::string, std::uint32_t> property_ids;

    out_blocks.reserve(blocks.size());
    out_states.reserve(spec.blocks.size() * 8U);

    for (std::uint32_t block_id = 0;
         block_id < static_cast<std::uint32_t>(blocks.size()); ++block_id) {
        const auto &block = *blocks[block_id];
        const auto property_count = block.property_names.size();
        if (property_count != block.property_values.size()) {
            return std::unexpected{
                fail("property name/value count mismatch in " + block.name)};
        }

        // Property order is the sorted order of the names; the state values are
        // reordered to match.
        std::vector<std::size_t> order(property_count);
        for (std::size_t i = 0; i < property_count; ++i) {
            order[i] = i;
        }
        std::ranges::sort(order, {},
                          [&block](std::size_t index) -> const std::string & {
                              return block.property_names[index];
                          });

        std::vector<std::string> names;
        std::vector<std::vector<std::string>> values;
        names.reserve(property_count);
        values.reserve(property_count);
        for (const auto index : order) {
            names.push_back(block.property_names[index]);
            values.push_back(block.property_values[index]);
            std::ranges::sort(values.back());
            if (values.back().empty()) {
                return std::unexpected{
                    fail("property without values in " + block.name)};
            }
            if (values.back().size() > 255U) {
                return std::unexpected{fail(
                    "property with more than 255 values in " + block.name)};
            }
        }
        for (std::size_t i = 1; i < names.size(); ++i) {
            if (names[i - 1] == names[i]) {
                return std::unexpected{
                    fail("duplicate property name in " + block.name)};
            }
        }

        const auto first_property =
            static_cast<std::uint32_t>(block_property_ids.size());
        for (std::size_t i = 0; i < names.size(); ++i) {
            std::string key{names[i]};
            key.push_back('\0');
            for (const auto &value : values[i]) {
                key.append(value);
                key.push_back('\0');
            }
            auto found = property_ids.find(key);
            if (found == property_ids.end()) {
                const auto id =
                    static_cast<std::uint32_t>(out_properties.size());
                built_property property{};
                property.name = pool.add(names[i]);
                property.first_value =
                    static_cast<std::uint32_t>(property_value_offsets.size());
                property.value_count =
                    static_cast<std::uint16_t>(values[i].size());
                for (const auto &value : values[i]) {
                    property_value_offsets.push_back(pool.add(value));
                }
                if (id > 255U) {
                    return std::unexpected{
                        fail("more than 255 distinct properties")};
                }
                out_properties.push_back(property);
                property_ids.emplace(std::move(key), id);
                block_property_ids.push_back(static_cast<std::uint8_t>(id));
            } else {
                block_property_ids.push_back(
                    static_cast<std::uint8_t>(found->second));
            }
        }
        const auto first_state = static_cast<std::uint32_t>(out_states.size());
        std::vector<built_state> block_states;
        block_states.reserve(block.states.size());
        for (const auto &state : block.states) {
            if (state.values.size() != property_count) {
                return std::unexpected{
                    fail("state value count mismatch in " + block.name)};
            }
            built_state built{};
            built.block = static_cast<std::uint16_t>(block_id);
            built.vanilla_id = state.vanilla_id;
            built.property_count = static_cast<std::uint8_t>(property_count);
            if (property_count > max_state_properties) {
                return std::unexpected{fail(
                    "state with more than five properties in " + block.name)};
            }
            std::vector<std::string> ordered_values;
            ordered_values.reserve(property_count);
            for (std::size_t i = 0; i < property_count; ++i) {
                const auto &property_values = values[i];
                const auto &value = state.values[order[i]];
                const auto position =
                    std::ranges::lower_bound(property_values, value);
                if (position == property_values.end() || *position != value) {
                    return std::unexpected{fail("state value '" + value +
                                                "' is not declared by " +
                                                block.name)};
                }
                built.values[i] = static_cast<std::uint8_t>(
                    position - property_values.begin());
                ordered_values.push_back(value);
            }
            built.key =
                pool.add(canonical_key(block.name, names, ordered_values));
            built.is_default = state.is_default;
            block_states.push_back(built);
        }
        std::ranges::sort(block_states, {}, &built_state::key);
        for (std::size_t i = 1; i < block_states.size(); ++i) {
            if (block_states[i - 1].key == block_states[i].key) {
                return std::unexpected{
                    fail("duplicate state key in " + block.name)};
            }
        }

        std::uint32_t default_state = 0;
        std::size_t defaults = 0;
        for (std::size_t i = 0; i < block_states.size(); ++i) {
            if (block_states[i].is_default) {
                default_state = static_cast<std::uint32_t>(i);
                ++defaults;
            }
        }
        if (defaults != 1) {
            return std::unexpected{
                fail("block needs exactly one default state: " + block.name)};
        }

        built_block built_block{};
        built_block.name = pool.add(block.name);
        built_block.first_state = first_state;
        built_block.state_count =
            static_cast<std::uint32_t>(block_states.size());
        built_block.default_state = first_state + default_state;
        built_block.first_property = first_property;
        built_block.property_count = static_cast<std::uint16_t>(names.size());
        out_blocks.push_back(built_block);
        for (auto &state : block_states) {
            state.block = static_cast<std::uint16_t>(block_id);
            out_states.push_back(state);
        }
    }

    // Biome presets: sorted by name; entries keep the input order.
    std::vector<const biome_preset_spec *> presets;
    presets.reserve(spec.biome_presets.size());
    for (const auto &preset : spec.biome_presets) {
        presets.push_back(&preset);
    }
    std::ranges::sort(presets, {}, &biome_preset_spec::name);
    for (std::size_t i = 1; i < presets.size(); ++i) {
        if (presets[i - 1]->name == presets[i]->name) {
            return std::unexpected{
                fail("duplicate biome preset: " + presets[i]->name)};
        }
    }
    std::vector<built_preset> out_presets;
    std::vector<built_biome_entry> out_biome_entries;
    for (const auto *preset : presets) {
        built_preset built{};
        built.name = pool.add(preset->name);
        built.first_entry =
            static_cast<std::uint32_t>(out_biome_entries.size());
        built.entry_count = static_cast<std::uint32_t>(preset->entries.size());
        for (const auto &entry : preset->entries) {
            built_biome_entry built_entry{};
            built_entry.min = entry.min;
            built_entry.max = entry.max;
            built_entry.offset = entry.offset;
            built_entry.biome = pool.add(entry.biome);
            out_biome_entries.push_back(built_entry);
        }
        out_presets.push_back(built);
    }

    // Registries and entries: both sorted by name.
    std::vector<const registry_spec *> registries;
    registries.reserve(spec.registries.size());
    for (const auto &registry : spec.registries) {
        registries.push_back(&registry);
    }
    std::ranges::sort(registries, {}, &registry_spec::name);
    for (std::size_t i = 1; i < registries.size(); ++i) {
        if (registries[i - 1]->name == registries[i]->name) {
            return std::unexpected{
                fail("duplicate registry: " + registries[i]->name)};
        }
    }
    std::vector<built_registry> out_registries;
    std::vector<built_registry_entry> out_registry_entries;
    for (const auto *registry : registries) {
        built_registry built{};
        built.name = pool.add(registry->name);
        built.first_entry =
            static_cast<std::uint32_t>(out_registry_entries.size());
        built.entry_count =
            static_cast<std::uint32_t>(registry->entries.size());
        std::vector<const registry_entry_spec *> entries;
        entries.reserve(registry->entries.size());
        for (const auto &entry : registry->entries) {
            entries.push_back(&entry);
        }
        std::ranges::sort(entries, {}, &registry_entry_spec::name);
        for (std::size_t i = 1; i < entries.size(); ++i) {
            if (entries[i - 1]->name == entries[i]->name) {
                return std::unexpected{
                    fail("duplicate registry entry: " + entries[i]->name)};
            }
        }
        for (const auto *entry : entries) {
            out_registry_entries.push_back(built_registry_entry{
                .name = pool.add(entry->name),
                .protocol_id = entry->protocol_id,
            });
        }
        out_registries.push_back(built);
    }

    std::vector<const item_spec *> items;
    items.reserve(spec.items.size());
    for (const auto &item : spec.items) {
        if (item.name.empty()) {
            return std::unexpected{fail("item with an empty name")};
        }
        if (item.max_stack_size < 1U || item.max_stack_size > 99U) {
            return std::unexpected{
                fail("item max stack size out of range: " + item.name)};
        }
        items.push_back(&item);
    }
    std::ranges::sort(items, {}, &item_spec::name);
    for (std::size_t i = 1; i < items.size(); ++i) {
        if (items[i - 1]->name == items[i]->name) {
            return std::unexpected{fail("duplicate item: " + items[i]->name)};
        }
    }
    std::vector<built_item> out_items;
    out_items.reserve(items.size());
    for (const auto *item : items) {
        out_items.push_back(built_item{
            .name = pool.add(item->name),
            .max_stack_size = item->max_stack_size,
            .max_damage = item->max_damage,
        });
    }

    // Mining records: the input aligns one record per block; the container
    // orders blocks by name, so the records are reordered by name once here.
    std::vector<built_mining> out_minings;
    std::vector<built_tool> out_tools;
    if (!spec.block_minings.empty() || !spec.tools.empty()) {
        if (spec.block_minings.empty() || spec.tools.empty()) {
            return std::unexpected{fail("incomplete mining sections")};
        }
        if (spec.block_minings.size() != spec.blocks.size()) {
            return std::unexpected{
                fail("mining records do not cover every block")};
        }
        std::unordered_map<std::string_view, const block_mining_spec *>
            mining_by_name;
        mining_by_name.reserve(spec.blocks.size());
        for (std::size_t i = 0; i < spec.blocks.size(); ++i) {
            if (!mining_by_name
                     .emplace(spec.blocks[i].name, &spec.block_minings[i])
                     .second) {
                return std::unexpected{
                    fail("duplicate block name: " + spec.blocks[i].name)};
            }
        }
        out_minings.reserve(blocks.size());
        for (const auto *block : blocks) {
            const auto found = mining_by_name.find(block->name);
            if (found == mining_by_name.end()) {
                return std::unexpected{
                    fail("missing mining record for " + block->name)};
            }
            const auto &mining = *found->second;
            if (!std::isfinite(mining.hardness) ||
                !(mining.hardness == -1.0F || mining.hardness >= 0.0F)) {
                return std::unexpected{
                    fail("invalid block hardness: " + block->name)};
            }
            if (!std::isfinite(mining.shears_speed) ||
                mining.shears_speed < 0.0F ||
                !std::isfinite(mining.sword_speed) ||
                mining.sword_speed < 0.0F) {
                return std::unexpected{
                    fail("invalid block mining speed: " + block->name)};
            }
            if ((mining.flags & ~(block_mining_requires_correct_tool |
                                  block_mining_air | block_mining_fluid)) !=
                    0U ||
                (mining.tool_mask & ~all_tool_type_bits) != 0U ||
                (mining.direct_correct & ~all_tool_type_bits) != 0U ||
                mining.needed_tier > max_harvest_level) {
                return std::unexpected{
                    fail("invalid block mining flags: " + block->name)};
            }
            out_minings.push_back(built_mining{
                .hardness = mining.hardness,
                .shears_speed = mining.shears_speed,
                .sword_speed = mining.sword_speed,
                .flags = mining.flags,
                .tool_mask = mining.tool_mask,
                .needed_tier = mining.needed_tier,
                .direct_correct = mining.direct_correct,
            });
        }
    }

    std::vector<const tool_mining_spec *> sorted_tools;
    sorted_tools.reserve(spec.tools.size());
    for (const auto &tool : spec.tools) {
        if (tool.name.empty()) {
            return std::unexpected{fail("tool with an empty name")};
        }
        const auto type = static_cast<std::uint8_t>(tool.type);
        if (type != static_cast<std::uint8_t>(tool_type::none) &&
            type > static_cast<std::uint8_t>(tool_type::sword)) {
            return std::unexpected{fail("unknown tool family: " + tool.name)};
        }
        if (static_cast<std::uint8_t>(tool.material) >
            static_cast<std::uint8_t>(tool_material::gold)) {
            return std::unexpected{fail("unknown tool material: " + tool.name)};
        }
        if (tiered_tool(type)) {
            if (tool.material == tool_material::none ||
                tool.harvest_level > max_harvest_level) {
                return std::unexpected{
                    fail("a tiered tool has no material: " + tool.name)};
            }
        } else if (tool.material != tool_material::none ||
                   tool.harvest_level != harvest_level_none) {
            return std::unexpected{
                fail("a non-tiered tool names a material: " + tool.name)};
        }
        if (!std::isfinite(tool.tier_speed) || tool.tier_speed < 0.0F ||
            !std::isfinite(tool.default_mining_speed) ||
            tool.default_mining_speed < 0.0F) {
            return std::unexpected{
                fail("invalid tool mining speed: " + tool.name)};
        }
        if (tool.max_stack_size < 1U || tool.max_stack_size > 99U) {
            return std::unexpected{
                fail("tool stack size out of range: " + tool.name)};
        }
        sorted_tools.push_back(&tool);
    }
    std::ranges::sort(sorted_tools, {}, &tool_mining_spec::name);
    for (std::size_t i = 1; i < sorted_tools.size(); ++i) {
        if (sorted_tools[i - 1]->name == sorted_tools[i]->name) {
            return std::unexpected{
                fail("duplicate tool: " + sorted_tools[i]->name)};
        }
    }
    out_tools.reserve(sorted_tools.size());
    for (const auto *tool : sorted_tools) {
        const auto type = static_cast<std::uint8_t>(tool->type);
        out_tools.push_back(built_tool{
            .name = pool.add(tool->name),
            .type = type,
            .material = static_cast<std::uint8_t>(tool->material),
            .harvest_level =
                tiered_tool(type) ? tool->harvest_level : harvest_level_none,
            .flags = static_cast<std::uint8_t>(
                tool->can_destroy_blocks_in_creative
                    ? tool_flag_can_destroy_in_creative
                    : 0U),
            .tier_speed = tool->tier_speed,
            .default_mining_speed = tool->default_mining_speed,
            .damage_per_block = tool->damage_per_block,
            .max_stack_size = tool->max_stack_size,
            .max_damage = tool->max_damage,
        });
    }

    if (out_states.size() > 0xFFFFU) {
        return std::unexpected{fail("too many block states")};
    }
    if (pool.strings().size() > 0xFFFFFFFFULL) {
        return std::unexpected{fail("string pool overflow")};
    }

    // --- serialize ---------------------------------------------------------

    std::size_t pool_bytes = 0;
    for (const auto &text : pool.strings()) {
        pool_bytes += text.size();
    }
    if (pool_bytes > max_string_pool_bytes) {
        return std::unexpected{fail("string pool over 64 MiB")};
    }

    std::vector<std::byte> strings;
    put_u32(strings, static_cast<std::uint32_t>(pool.strings().size()));
    std::uint32_t offset = 0;
    for (const auto &text : pool.strings()) {
        put_u32(strings, offset);
        offset += static_cast<std::uint32_t>(text.size());
    }
    put_u32(strings, offset);
    for (const auto &text : pool.strings()) {
        put_bytes(strings, std::as_bytes(std::span<const char>{text.data(),
                                                               text.size()}));
    }

    std::vector<std::byte> blocks_section;
    put_u32(blocks_section, static_cast<std::uint32_t>(out_blocks.size()));
    for (const auto &block : out_blocks) {
        put_u32(blocks_section, block.name);
        put_u32(blocks_section, block.first_state);
        put_u32(blocks_section, block.state_count);
        put_u32(blocks_section, block.default_state);
        put_u32(blocks_section, block.first_property);
        put_u16(blocks_section, block.property_count);
        put_u16(blocks_section, 0);
    }

    std::vector<std::byte> properties_section;
    put_u32(properties_section,
            static_cast<std::uint32_t>(out_properties.size()));
    for (const auto &property : out_properties) {
        put_u32(properties_section, property.name);
        put_u32(properties_section, property.first_value);
        put_u16(properties_section, property.value_count);
        put_u16(properties_section, 0);
        put_u32(properties_section, 0);
    }

    std::vector<std::byte> values_section;
    put_u32(values_section,
            static_cast<std::uint32_t>(property_value_offsets.size()));
    for (const auto value : property_value_offsets) {
        put_u32(values_section, value);
    }

    std::vector<std::byte> states_section;
    put_u32(states_section, static_cast<std::uint32_t>(out_states.size()));
    for (const auto &state : out_states) {
        put_u16(states_section, state.block);
        put_u16(states_section, state.vanilla_id);
        states_section.push_back(static_cast<std::byte>(state.property_count));
        for (std::size_t i = 0; i < 8; ++i) {
            states_section.push_back(static_cast<std::byte>(
                i < state.property_count ? state.values[i] : 0));
        }
        for (int i = 0; i < 3; ++i) {
            states_section.push_back(std::byte{0});
        }
        put_u32(states_section, state.key);
    }

    std::vector<std::byte> biomes_section;
    put_u32(biomes_section, static_cast<std::uint32_t>(out_presets.size()));
    for (const auto &preset : out_presets) {
        put_u32(biomes_section, preset.name);
        put_u32(biomes_section, preset.first_entry);
        put_u32(biomes_section, preset.entry_count);
        put_u32(biomes_section, 0);
    }
    for (const auto &entry : out_biome_entries) {
        for (const auto value : entry.min) {
            put_i32(biomes_section, value);
        }
        for (const auto value : entry.max) {
            put_i32(biomes_section, value);
        }
        put_i32(biomes_section, entry.offset);
        put_u32(biomes_section, entry.biome);
    }

    std::vector<std::byte> registries_section;
    put_u32(registries_section,
            static_cast<std::uint32_t>(out_registries.size()));
    for (const auto &registry : out_registries) {
        put_u32(registries_section, registry.name);
        put_u32(registries_section, registry.first_entry);
        put_u32(registries_section, registry.entry_count);
        put_u32(registries_section, 0);
    }
    for (const auto &entry : out_registry_entries) {
        put_u32(registries_section, entry.name);
        put_i32(registries_section, entry.protocol_id);
    }

    std::vector<std::byte> items_section;
    put_u32(items_section, static_cast<std::uint32_t>(out_items.size()));
    for (const auto &item : out_items) {
        put_u32(items_section, item.name);
        put_u16(items_section, item.max_stack_size);
        put_u16(items_section, 0);
        put_u32(items_section, item.max_damage);
        put_u32(items_section, 0);
    }

    std::vector<std::byte> mining_section;
    std::vector<std::byte> tools_section;
    if (!out_minings.empty()) {
        put_u32(mining_section, static_cast<std::uint32_t>(out_minings.size()));
        for (const auto &mining : out_minings) {
            put_f32(mining_section, mining.hardness);
            put_f32(mining_section, mining.shears_speed);
            put_f32(mining_section, mining.sword_speed);
            mining_section.push_back(static_cast<std::byte>(mining.flags));
            mining_section.push_back(static_cast<std::byte>(mining.tool_mask));
            mining_section.push_back(
                static_cast<std::byte>(mining.needed_tier));
            mining_section.push_back(
                static_cast<std::byte>(mining.direct_correct));
        }
        put_u32(tools_section, static_cast<std::uint32_t>(out_tools.size()));
        for (const auto &tool : out_tools) {
            put_u32(tools_section, tool.name);
            tools_section.push_back(static_cast<std::byte>(tool.type));
            tools_section.push_back(static_cast<std::byte>(tool.material));
            tools_section.push_back(static_cast<std::byte>(tool.harvest_level));
            tools_section.push_back(static_cast<std::byte>(tool.flags));
            put_f32(tools_section, tool.tier_speed);
            put_f32(tools_section, tool.default_mining_speed);
            put_u16(tools_section, tool.damage_per_block);
            put_u16(tools_section, tool.max_stack_size);
            put_u32(tools_section, tool.max_damage);
        }
    }

    std::vector<std::byte> light_states_section;
    std::vector<std::byte> light_shapes_section;
    if (!spec.block_lights.empty()) {
        // One light record per dense state, in the same order as the state
        // section. The report addresses records by the game's state id, while
        // the container's state order is canonical (block name, then state
        // key), so the records are reordered once here.
        const auto shape_count = spec.light_shapes.size();
        if (shape_count > 0xFFFFU) {
            return std::unexpected{
                fail("more than 65535 light occlusion shapes")};
        }
        std::uint64_t box_count = 0;
        for (const auto &shape : spec.light_shapes) {
            for (const auto &face : shape.faces) {
                if (face.size() > max_light_boxes_per_face) {
                    return std::unexpected{fail(
                        "light shape face with more than " +
                        std::to_string(max_light_boxes_per_face) + " boxes")};
                }
                for (const auto &box : face) {
                    for (const auto value : box) {
                        if (!std::isfinite(value)) {
                            return std::unexpected{
                                fail("non-finite light shape coordinate")};
                        }
                    }
                    if (box[0] > box[3] || box[1] > box[4] || box[2] > box[5]) {
                        return std::unexpected{
                            fail("light shape box with min above max")};
                    }
                    ++box_count;
                }
            }
        }
        if (box_count > 0xFFFFFFFFULL) {
            return std::unexpected{fail("too many light shape boxes")};
        }
        std::unordered_map<std::uint32_t, const block_light_spec *> by_id;
        by_id.reserve(spec.block_lights.size());
        for (const auto &light : spec.block_lights) {
            if (light.emission > max_light_level ||
                light.dampening > max_light_level) {
                return std::unexpected{
                    fail("light level out of range for a block state")};
            }
            if (light.shape != no_light_shape && light.shape >= shape_count) {
                return std::unexpected{
                    fail("light shape index out of range for a block state")};
            }
            if (!by_id.emplace(light.vanilla_id, &light).second) {
                return std::unexpected{
                    fail("duplicate light record for a block state")};
            }
        }
        if (by_id.size() != out_states.size()) {
            return std::unexpected{
                fail("block light records do not cover every block state")};
        }
        std::vector<built_light> out_lights;
        out_lights.reserve(out_states.size());
        for (const auto &state : out_states) {
            const auto found = by_id.find(state.vanilla_id);
            if (found == by_id.end()) {
                return std::unexpected{
                    fail("block light records do not cover every block state")};
            }
            const auto &light = *found->second;
            built_light built{};
            built.emission = light.emission;
            built.dampening = light.dampening;
            built.flags = static_cast<std::uint8_t>(
                (light.can_occlude ? light_flag_can_occlude : 0U) |
                (light.use_shape_for_light_occlusion ? light_flag_use_shape
                                                     : 0U));
            built.shape = light.shape;
            out_lights.push_back(built);
        }

        put_u32(light_states_section,
                static_cast<std::uint32_t>(out_lights.size()));
        for (const auto &light : out_lights) {
            light_states_section.push_back(
                static_cast<std::byte>(light.emission));
            light_states_section.push_back(
                static_cast<std::byte>(light.dampening));
            light_states_section.push_back(static_cast<std::byte>(light.flags));
            light_states_section.push_back(std::byte{0});
            put_u16(light_states_section,
                    light.shape == no_light_shape
                        ? no_light_shape_u16
                        : static_cast<std::uint16_t>(light.shape));
            put_u16(light_states_section, 0);
        }

        put_u32(light_shapes_section, static_cast<std::uint32_t>(shape_count));
        put_u32(light_shapes_section, static_cast<std::uint32_t>(box_count));
        std::uint32_t first_box = 0;
        std::vector<std::byte> shape_boxes;
        for (const auto &shape : spec.light_shapes) {
            put_u32(light_shapes_section, first_box);
            std::size_t shape_boxes_count = 0;
            for (std::size_t face = 0; face < light_shape_faces; ++face) {
                const auto count = shape.faces[face].size();
                light_shapes_section.push_back(static_cast<std::byte>(count));
                shape_boxes_count += count;
            }
            for (std::size_t pad = 0; pad < 2; ++pad) {
                light_shapes_section.push_back(std::byte{0});
            }
            for (std::size_t pad = 0; pad < 4; ++pad) {
                light_shapes_section.push_back(std::byte{0});
            }
            first_box += static_cast<std::uint32_t>(shape_boxes_count);
            for (const auto &face : shape.faces) {
                for (const auto &box : face) {
                    for (const auto value : box) {
                        put_f64(shape_boxes, value);
                    }
                }
            }
        }
        light_shapes_section.append_range(shape_boxes);
    }

    std::vector<std::byte> block_properties_section;
    block_properties_section.reserve(block_property_ids.size());
    for (const auto id : block_property_ids) {
        block_properties_section.push_back(static_cast<std::byte>(id));
    }

    struct section_out {
        std::array<char, 4> tag;
        std::vector<std::byte> bytes;
    };
    std::vector<section_out> sections{
        {.tag = {'B', 'I', 'O', ' '}, .bytes = std::move(biomes_section)},
        {.tag = {'B', 'L', 'K', 'S'}, .bytes = std::move(blocks_section)},
        {.tag = {'B', 'P', 'I', 'X'},
         .bytes = std::move(block_properties_section)},
        {.tag = {'P', 'R', 'P', 'S'}, .bytes = std::move(properties_section)},
        {.tag = {'P', 'V', 'A', 'L'}, .bytes = std::move(values_section)},
        {.tag = {'I', 'T', 'M', 'S'}, .bytes = std::move(items_section)},
        {.tag = {'R', 'E', 'G', 'S'}, .bytes = std::move(registries_section)},
        {.tag = {'S', 'T', 'A', 'T'}, .bytes = std::move(states_section)},
        {.tag = {'S', 'T', 'R', 'S'}, .bytes = std::move(strings)},
    };
    if (!light_states_section.empty()) {
        sections.push_back({.tag = {'L', 'G', 'H', 'T'},
                            .bytes = std::move(light_states_section)});
        sections.push_back({.tag = {'L', 'S', 'H', 'P'},
                            .bytes = std::move(light_shapes_section)});
    }
    if (!mining_section.empty()) {
        sections.push_back(
            {.tag = {'M', 'I', 'N', 'E'}, .bytes = std::move(mining_section)});
        sections.push_back(
            {.tag = {'T', 'O', 'O', 'L'}, .bytes = std::move(tools_section)});
    }

    std::size_t cursor = header_size + sections.size() * section_record_size;
    std::vector<std::uint32_t> offsets;
    offsets.reserve(sections.size());
    for (const auto &section : sections) {
        cursor = align8(cursor);
        offsets.push_back(static_cast<std::uint32_t>(cursor));
        cursor += section.bytes.size();
    }
    cursor = align8(cursor);
    if (cursor > 0xFFFFFFFFULL) {
        return std::unexpected{fail("container over 4 GiB")};
    }

    std::vector<std::byte> out;
    out.reserve(cursor);
    put_bytes(out, magic);
    put_u16(out, vanilla_table_version);
    put_u16(out, vanilla_table_kind);
    put_u32(out, 0);
    put_i32(out, spec.data_version);
    put_u32(out, static_cast<std::uint32_t>(sections.size()));
    put_u32(out, 0);
    put_u64(out, static_cast<std::uint64_t>(cursor - header_size));
    put_u32(out, 1);
    put_u32(out, 0);
    const std::size_t digest_position = out.size();
    out.resize(out.size() + 32);

    for (std::size_t i = 0; i < sections.size(); ++i) {
        for (const auto ch : sections[i].tag) {
            out.push_back(static_cast<std::byte>(ch));
        }
        put_u32(out, offsets[i]);
        put_u32(out, static_cast<std::uint32_t>(sections[i].bytes.size()));
        put_u32(out, crc32c(sections[i].bytes));
    }

    for (std::size_t i = 0; i < sections.size(); ++i) {
        while (out.size() < offsets[i]) {
            out.push_back(std::byte{0});
        }
        put_bytes(out, sections[i].bytes);
    }
    while (out.size() < cursor) {
        out.push_back(std::byte{0});
    }

    const auto digest =
        blake3(std::span<const std::byte>{out}.subspan(header_size));
    std::ranges::copy(digest, out.begin() +
                                  static_cast<std::ptrdiff_t>(digest_position));
    return out;
}

// --- reader -----------------------------------------------------------------

auto vanilla_tables::open(std::span<const std::byte> bytes, bool verify)
    -> result<vanilla_tables> {
    if (bytes.size() < header_size) {
        return std::unexpected{fail("container shorter than its header")};
    }
    for (std::size_t i = 0; i < magic.size(); ++i) {
        if (bytes[i] != magic[i]) {
            return std::unexpected{fail("bad container magic")};
        }
    }
    const auto version = load_le16(bytes, 4);
    if (version != vanilla_table_version) {
        return std::unexpected{fail("unsupported container version")};
    }
    if (load_le16(bytes, 6) != vanilla_table_kind) {
        return std::unexpected{fail("unsupported container kind")};
    }
    if (load_le32(bytes, 32) != 1) {
        return std::unexpected{fail("unsupported digest algorithm")};
    }
    if (load_le64(bytes, 24) != bytes.size() - header_size) {
        return std::unexpected{fail("payload size does not match the file")};
    }

    vanilla_tables table;
    table.bytes_ = bytes;
    table.info_.version = version;
    table.info_.kind = vanilla_table_kind;
    table.info_.data_version = load_le<std::int32_t>(bytes, 12);

    const auto section_count = load_le32(bytes, 16);
    if (section_count > max_sections) {
        return std::unexpected{fail("too many sections")};
    }
    const auto table_end =
        header_size +
        static_cast<std::size_t>(section_count) * section_record_size;
    if (table_end > bytes.size()) {
        return std::unexpected{fail("section table outside the file")};
    }
    if (verify) {
        const auto stored = std::span<const std::byte>{bytes}.subspan(40, 32);
        const auto computed =
            blake3(std::span<const std::byte>{bytes}.subspan(header_size));
        for (std::size_t i = 0; i < 32; ++i) {
            if (stored[i] != computed[i]) {
                return std::unexpected{fail("payload digest mismatch")};
            }
        }
    }

    bool have_strings = false;
    bool have_blocks = false;
    bool have_block_properties = false;
    bool have_properties = false;
    bool have_values = false;
    bool have_states = false;
    bool have_biomes = false;
    bool have_registries = false;
    bool have_items = false;
    bool have_light_states = false;
    bool have_light_shapes = false;
    bool have_mining_states = false;
    bool have_tool_records = false;

    for (std::uint32_t i = 0; i < section_count; ++i) {
        const auto base =
            header_size + static_cast<std::size_t>(i) * section_record_size;
        const std::array<char, 4> tag{
            static_cast<char>(bytes[base]),
            static_cast<char>(bytes[base + 1]),
            static_cast<char>(bytes[base + 2]),
            static_cast<char>(bytes[base + 3]),
        };
        section record{};
        record.offset = load_le32(bytes, base + 4);
        record.size = load_le32(bytes, base + 8);
        if (record.offset % 8U != 0 || record.offset < table_end ||
            record.size > bytes.size() ||
            record.offset > bytes.size() - record.size) {
            return std::unexpected{fail("section outside the file")};
        }
        const auto payload = std::span<const std::byte>{bytes}.subspan(
            record.offset, record.size);
        if (crc32c(payload) != load_le32(bytes, base + 12)) {
            return std::unexpected{fail("section checksum mismatch")};
        }
        const std::string_view name{tag.data(), tag.size()};
        if (name == "STRS") {
            table.strings_ = record;
            have_strings = true;
        } else if (name == "BLKS") {
            table.blocks_ = record;
            have_blocks = true;
        } else if (name == "BPIX") {
            table.block_properties_ = record;
            have_block_properties = true;
        } else if (name == "PRPS") {
            table.properties_ = record;
            have_properties = true;
        } else if (name == "PVAL") {
            table.property_values_ = record;
            have_values = true;
        } else if (name == "STAT") {
            table.states_ = record;
            have_states = true;
        } else if (name == "BIO ") {
            table.biomes_ = record;
            have_biomes = true;
        } else if (name == "REGS") {
            table.registries_ = record;
            have_registries = true;
        } else if (name == "ITMS") {
            table.items_ = record;
            have_items = true;
        } else if (name == "LGHT") {
            table.light_states_ = record;
            have_light_states = true;
        } else if (name == "LSHP") {
            table.light_shapes_section_ = record;
            have_light_shapes = true;
        } else if (name == "MINE") {
            table.mining_ = record;
            have_mining_states = true;
        } else if (name == "TOOL") {
            table.tools_section_ = record;
            have_tool_records = true;
        }
    }
    if (!have_strings || !have_blocks || !have_block_properties ||
        !have_properties || !have_values || !have_states || !have_biomes ||
        !have_registries || !have_items) {
        return std::unexpected{fail("missing section")};
    }
    table.section_table_ =
        section{.offset = static_cast<std::uint32_t>(header_size),
                .size = static_cast<std::uint32_t>(section_count *
                                                   section_record_size)};

    // --- structural validation --------------------------------------------

    const auto str_count = load_le32(bytes, table.strings_.offset);
    if (table.strings_.size < 4U + (str_count + 1ULL) * 4U) {
        return std::unexpected{fail("string pool truncated")};
    }
    table.info_.strings = str_count;
    const auto strings_data =
        table.strings_.offset + 4U + (str_count + 1ULL) * 4U;
    std::uint32_t strings_size = 0;
    for (std::uint32_t i = 0; i <= str_count; ++i) {
        const auto value =
            load_le32(bytes, table.strings_.offset + 4U + i * 4ULL);
        if (i == 0 && value != 0) {
            return std::unexpected{fail("string pool does not start at zero")};
        }
        if (value < strings_size) {
            return std::unexpected{fail("string pool offsets not monotonic")};
        }
        strings_size = value;
    }
    if (strings_data + strings_size >
        table.strings_.offset + table.strings_.size) {
        return std::unexpected{fail("string pool overrun")};
    }
    table.info_.blocks = load_le32(bytes, table.blocks_.offset);
    if (table.blocks_.size < 4U + table.info_.blocks * block_record_size) {
        return std::unexpected{fail("block section truncated")};
    }
    table.info_.properties = load_le32(bytes, table.properties_.offset);
    if (table.properties_.size <
        4U + table.info_.properties * property_record_size) {
        return std::unexpected{fail("property section truncated")};
    }
    const auto value_count = load_le32(bytes, table.property_values_.offset);
    if (table.property_values_.size < 4U + value_count * 4ULL) {
        return std::unexpected{fail("value section truncated")};
    }
    table.info_.states = load_le32(bytes, table.states_.offset);
    if (table.states_.size < 4U + table.info_.states * state_record_size) {
        return std::unexpected{fail("state section truncated")};
    }
    table.info_.presets = load_le32(bytes, table.biomes_.offset);
    if (table.biomes_.size < 4U + table.info_.presets * preset_record_size) {
        return std::unexpected{fail("biome section truncated")};
    }
    table.info_.registries = load_le32(bytes, table.registries_.offset);
    if (table.registries_.size <
        4U + table.info_.registries * registry_record_size) {
        return std::unexpected{fail("registry section truncated")};
    }
    table.info_.items = load_le32(bytes, table.items_.offset);
    if (table.items_.size < 4U + table.info_.items * item_record_size) {
        return std::unexpected{fail("item section truncated")};
    }
    if (have_light_states != have_light_shapes) {
        return std::unexpected{fail("incomplete light sections")};
    }
    if (have_light_states) {
        const auto count = load_le32(bytes, table.light_states_.offset);
        if (count != table.info_.states) {
            return std::unexpected{
                fail("light record count does not match the state count")};
        }
        if (table.light_states_.size <
            4U + static_cast<std::uint64_t>(count) * light_record_size) {
            return std::unexpected{fail("light state section truncated")};
        }
        const auto shape_count =
            load_le32(bytes, table.light_shapes_section_.offset);
        const auto box_count =
            load_le32(bytes, table.light_shapes_section_.offset + 4U);
        if (shape_count > 0xFFFFU) {
            return std::unexpected{fail("too many light shapes")};
        }
        const auto required =
            8U +
            static_cast<std::uint64_t>(shape_count) * light_shape_record_size +
            static_cast<std::uint64_t>(box_count) * light_box_size;
        if (table.light_shapes_section_.size < required) {
            return std::unexpected{fail("light shape section truncated")};
        }
        table.info_.light_shapes = shape_count;
        table.light_shapes_ = shape_count;
        table.light_boxes_ = box_count;
        table.have_light_ = true;
        const auto light_base = table.light_states_.offset + 4U;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto base = light_base + i * light_record_size;
            const auto emission = std::to_integer<std::uint8_t>(bytes[base]);
            const auto dampening =
                std::to_integer<std::uint8_t>(bytes[base + 1U]);
            const auto flags = std::to_integer<std::uint8_t>(bytes[base + 2U]);
            const auto shape = load_le16(bytes, base + 4U);
            if (emission > max_light_level || dampening > max_light_level ||
                (flags & ~(light_flag_can_occlude | light_flag_use_shape)) !=
                    0U) {
                return std::unexpected{fail("light state record invalid")};
            }
            if (shape != no_light_shape_u16 && shape >= shape_count) {
                return std::unexpected{fail("light state shape out of range")};
            }
        }
        const auto shape_base = table.light_shapes_section_.offset + 8U;
        const auto box_base =
            shape_base +
            static_cast<std::size_t>(shape_count) * light_shape_record_size;
        for (std::uint32_t i = 0; i < shape_count; ++i) {
            const auto base = shape_base + i * light_shape_record_size;
            std::uint64_t boxes = 0;
            for (std::size_t face = 0; face < light_shape_faces; ++face) {
                const auto face_boxes =
                    std::to_integer<std::uint8_t>(bytes[base + 4U + face]);
                if (face_boxes > max_light_boxes_per_face) {
                    return std::unexpected{
                        fail("light shape face has too many boxes")};
                }
                boxes += face_boxes;
            }
            const auto first_box = load_le32(bytes, base);
            if (static_cast<std::uint64_t>(first_box) + boxes > box_count) {
                return std::unexpected{fail("light shape boxes out of range")};
            }
        }
        for (std::uint32_t i = 0; i < box_count; ++i) {
            const auto base =
                box_base + static_cast<std::size_t>(i) * light_box_size;
            for (std::size_t coordinate = 0; coordinate < 6; ++coordinate) {
                const auto value = std::bit_cast<double>(load_le64(
                    bytes, base + coordinate * sizeof(std::uint64_t)));
                if (!std::isfinite(value)) {
                    return std::unexpected{
                        fail("non-finite light shape coordinate")};
                }
            }
        }
    }

    if (have_mining_states != have_tool_records) {
        return std::unexpected{fail("incomplete mining sections")};
    }
    if (have_mining_states) {
        const auto count = load_le32(bytes, table.mining_.offset);
        if (count != table.info_.blocks) {
            return std::unexpected{
                fail("mining record count does not match the block count")};
        }
        if (table.mining_.size <
            4U + static_cast<std::uint64_t>(count) * mining_record_size) {
            return std::unexpected{fail("mining section truncated")};
        }
        const auto mining_base = table.mining_.offset + 4U;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto base = mining_base + i * mining_record_size;
            const auto hardness = std::bit_cast<float>(load_le32(bytes, base));
            const auto shears_speed =
                std::bit_cast<float>(load_le32(bytes, base + 4U));
            const auto sword_speed =
                std::bit_cast<float>(load_le32(bytes, base + 8U));
            const auto flags = std::to_integer<std::uint8_t>(bytes[base + 12U]);
            const auto mask = std::to_integer<std::uint8_t>(bytes[base + 13U]);
            const auto needed =
                std::to_integer<std::uint8_t>(bytes[base + 14U]);
            const auto direct =
                std::to_integer<std::uint8_t>(bytes[base + 15U]);
            if (!std::isfinite(hardness) ||
                !(hardness == -1.0F || hardness >= 0.0F) ||
                !std::isfinite(shears_speed) || shears_speed < 0.0F ||
                !std::isfinite(sword_speed) || sword_speed < 0.0F) {
                return std::unexpected{fail("mining record invalid")};
            }
            if ((flags & ~(block_mining_requires_correct_tool |
                           block_mining_air | block_mining_fluid)) != 0U ||
                (mask & ~all_tool_type_bits) != 0U ||
                (direct & ~all_tool_type_bits) != 0U ||
                needed > max_harvest_level) {
                return std::unexpected{fail("mining flags invalid")};
            }
        }
        const auto tool_count = load_le32(bytes, table.tools_section_.offset);
        if (table.tools_section_.size <
            4U + static_cast<std::uint64_t>(tool_count) * tool_record_size) {
            return std::unexpected{fail("tool section truncated")};
        }
        const auto tool_base = table.tools_section_.offset + 4U;
        std::string_view previous{};
        for (std::uint32_t i = 0; i < tool_count; ++i) {
            const auto base = tool_base + i * tool_record_size;
            const auto name_index = load_le32(bytes, base);
            if (name_index >= table.info_.strings) {
                return std::unexpected{fail("tool name out of range")};
            }
            const auto name = table.string_at(name_index);
            if (name.empty() || (!previous.empty() && name <= previous)) {
                return std::unexpected{
                    fail("tool records are not sorted by name")};
            }
            previous = name;
            const auto type = std::to_integer<std::uint8_t>(bytes[base + 4U]);
            const auto material =
                std::to_integer<std::uint8_t>(bytes[base + 5U]);
            const auto harvest =
                std::to_integer<std::uint8_t>(bytes[base + 6U]);
            const auto flags = std::to_integer<std::uint8_t>(bytes[base + 7U]);
            const auto tier_speed =
                std::bit_cast<float>(load_le32(bytes, base + 8U));
            const auto default_speed =
                std::bit_cast<float>(load_le32(bytes, base + 12U));
            const auto stack = load_le16(bytes, base + 18U);
            if ((type != static_cast<std::uint8_t>(tool_type::none) &&
                 type > static_cast<std::uint8_t>(tool_type::sword)) ||
                material > static_cast<std::uint8_t>(tool_material::gold) ||
                (flags & ~tool_flag_can_destroy_in_creative) != 0U) {
                return std::unexpected{fail("tool record invalid")};
            }
            if (!std::isfinite(tier_speed) || tier_speed < 0.0F ||
                !std::isfinite(default_speed) || default_speed < 0.0F) {
                return std::unexpected{fail("tool speed invalid")};
            }
            if (stack < 1U || stack > 99U) {
                return std::unexpected{fail("tool stack size out of range")};
            }
            const bool tiered =
                tiered_tool(type) &&
                material != static_cast<std::uint8_t>(tool_material::none);
            if (tiered) {
                if (harvest > max_harvest_level) {
                    return std::unexpected{
                        fail("tool harvest level out of range")};
                }
            } else if (harvest != harvest_level_none ||
                       material !=
                           static_cast<std::uint8_t>(tool_material::none)) {
                return std::unexpected{fail("tool material invalid")};
            }
        }
        table.info_.tools = tool_count;
        table.have_mining_ = true;
    }

    const auto block_base = table.blocks_.offset + 4U;
    const auto property_base = table.properties_.offset + 4U;
    const auto state_base = table.states_.offset + 4U;

    // Strings referenced by blocks and properties must be in range.
    for (std::uint32_t i = 0; i < table.info_.blocks; ++i) {
        const auto base = block_base + i * block_record_size;
        const auto first = load_le32(bytes, base + 4);
        const auto count = load_le32(bytes, base + 8);
        const auto default_state = load_le32(bytes, base + 12);
        const auto first_property = load_le32(bytes, base + 16);
        const auto property_count = load_le16(bytes, base + 20);
        if (load_le32(bytes, base) >= table.info_.strings) {
            return std::unexpected{fail("block name out of range")};
        }
        if (count == 0 ||
            static_cast<std::uint64_t>(first) + count > table.info_.states ||
            default_state < first ||
            static_cast<std::uint64_t>(default_state) >=
                static_cast<std::uint64_t>(first) + count) {
            return std::unexpected{fail("block state range invalid")};
        }
        if (static_cast<std::uint64_t>(first_property) + property_count >
            table.block_properties_.size) {
            return std::unexpected{fail("block property range invalid")};
        }
    }
    for (std::uint32_t i = 0; i < table.info_.properties; ++i) {
        const auto base = property_base + i * property_record_size;
        const auto first_value = load_le32(bytes, base + 4);
        const auto value_count_property = load_le16(bytes, base + 8);
        if (load_le32(bytes, base) >= table.info_.strings) {
            return std::unexpected{fail("property name out of range")};
        }
        if (value_count_property == 0 ||
            static_cast<std::uint64_t>(first_value) + value_count_property >
                value_count) {
            return std::unexpected{fail("property value range invalid")};
        }
    }
    for (std::uint32_t i = 0; i < value_count; ++i) {
        if (load_le32(bytes, table.property_values_.offset + 4U + i * 4ULL) >=
            table.info_.strings) {
            return std::unexpected{fail("value string out of range")};
        }
    }
    for (std::uint32_t i = 0; i < table.info_.states; ++i) {
        const auto base = state_base + i * state_record_size;
        const auto block = load_le16(bytes, base);
        const auto property_count = static_cast<std::size_t>(bytes[base + 4]);
        const auto key = load_le32(bytes, base + 16);
        if (block >= table.info_.blocks || property_count > 8 ||
            key >= table.info_.strings) {
            return std::unexpected{fail("state record invalid")};
        }
        const auto block_record = block_base + block * block_record_size;
        const auto first_property = load_le32(bytes, block_record + 16);
        const auto block_property_count = load_le16(bytes, block_record + 20);
        if (property_count != block_property_count) {
            return std::unexpected{fail("state property count mismatch")};
        }
        for (std::size_t p = 0; p < property_count; ++p) {
            const auto property_id = static_cast<std::size_t>(
                bytes[table.block_properties_.offset + first_property + p]);
            if (property_id >= table.info_.properties) {
                return std::unexpected{fail("state property id out of range")};
            }
            const auto property_record =
                property_base + property_id * property_record_size;
            const auto value_count_property =
                load_le16(bytes, property_record + 8);
            if (std::to_integer<std::size_t>(bytes[base + 5 + p]) >=
                value_count_property) {
                return std::unexpected{fail("state value index out of range")};
            }
        }
    }
    for (std::uint32_t i = 0; i < table.info_.presets; ++i) {
        const auto base = table.biomes_.offset + 4U + i * preset_record_size;
        const auto first_entry = load_le32(bytes, base + 4);
        const auto entry_count = load_le32(bytes, base + 8);
        const auto required =
            4ULL +
            static_cast<std::uint64_t>(table.info_.presets) *
                preset_record_size +
            (static_cast<std::uint64_t>(first_entry) + entry_count) *
                biome_entry_size;
        if (load_le32(bytes, base) >= table.info_.strings ||
            required > table.biomes_.size) {
            return std::unexpected{fail("biome preset record invalid")};
        }
    }
    for (std::uint32_t i = 0; i < table.info_.registries; ++i) {
        const auto base =
            table.registries_.offset + 4U + i * registry_record_size;
        const auto first_entry = load_le32(bytes, base + 4);
        const auto entry_count = load_le32(bytes, base + 8);
        const auto required =
            4ULL +
            static_cast<std::uint64_t>(table.info_.registries) *
                registry_record_size +
            (static_cast<std::uint64_t>(first_entry) + entry_count) *
                registry_entry_size;
        if (load_le32(bytes, base) >= table.info_.strings ||
            required > table.registries_.size) {
            return std::unexpected{fail("registry record invalid")};
        }
    }

    // --- indexes ------------------------------------------------------------

    table.block_index_.assign(capacity_for(table.info_.blocks), 0);
    const auto block_mask = table.block_index_.size() - 1;
    for (std::uint32_t i = 0; i < table.info_.blocks; ++i) {
        const auto name = table.block_at(static_cast<std::uint16_t>(i)).name;
        const auto hash = hash64(name);
        auto slot = static_cast<std::size_t>(hash) & block_mask;
        while (table.block_index_[slot] != 0) {
            slot = (slot + 1) & block_mask;
        }
        table.block_index_[slot] = i + 1;
    }

    table.state_index_.assign(capacity_for(table.info_.states), 0);
    const auto state_mask = table.state_index_.size() - 1;
    table.vanilla_index_.reserve(table.info_.states);
    for (std::uint32_t i = 0; i < table.info_.states; ++i) {
        const auto state = table.state_at(i);
        const auto hash = hash64(state.key);
        auto slot = static_cast<std::size_t>(hash) & state_mask;
        while (table.state_index_[slot] != 0) {
            slot = (slot + 1) & state_mask;
        }
        table.state_index_[slot] = i + 1;
        table.vanilla_index_.emplace_back(state.vanilla_id, i);
    }
    std::ranges::sort(table.vanilla_index_);

    return table;
}

auto vanilla_tables::light_at(std::uint32_t state) const -> block_light_view {
    block_light_view view{};
    if (!have_light_ || state >= info_.states) {
        return view;
    }
    const auto base = light_states_.offset + 4U +
                      static_cast<std::size_t>(state) * light_record_size;
    view.emission = std::to_integer<std::uint8_t>(bytes_[base]);
    view.dampening = std::to_integer<std::uint8_t>(bytes_[base + 1U]);
    const auto flags = std::to_integer<std::uint8_t>(bytes_[base + 2U]);
    view.can_occlude = (flags & light_flag_can_occlude) != 0U;
    view.use_shape_for_light_occlusion = (flags & light_flag_use_shape) != 0U;
    const auto shape = load_le16(bytes_, base + 4U);
    view.shape = shape == no_light_shape_u16
                     ? no_light_shape
                     : static_cast<std::uint32_t>(shape);
    return view;
}

auto vanilla_tables::light_shape_at(std::uint32_t shape) const
    -> light_shape_view {
    light_shape_view view{};
    if (!have_light_ || shape >= light_shapes_) {
        return view;
    }
    const auto base = light_shapes_section_.offset + 8U +
                      static_cast<std::size_t>(shape) * light_shape_record_size;
    for (std::size_t face = 0; face < light_shape_faces; ++face) {
        view.face_boxes[face] =
            std::to_integer<std::uint8_t>(bytes_[base + 4U + face]);
    }
    return view;
}

auto vanilla_tables::light_box(std::uint32_t shape, std::uint8_t face,
                               std::uint16_t index) const -> light_box_view {
    light_box_view view{};
    if (!have_light_ || shape >= light_shapes_ || face >= light_shape_faces) {
        return view;
    }
    const auto shape_base =
        light_shapes_section_.offset + 8U +
        static_cast<std::size_t>(shape) * light_shape_record_size;
    const auto first_box = load_le32(bytes_, shape_base);
    std::uint64_t offset = first_box;
    for (std::size_t before = 0; before < face; ++before) {
        offset +=
            std::to_integer<std::uint8_t>(bytes_[shape_base + 4U + before]);
    }
    offset += index;
    if (offset >= light_boxes_) {
        return view;
    }
    const auto box_base =
        light_shapes_section_.offset + 8U +
        static_cast<std::size_t>(light_shapes_) * light_shape_record_size +
        static_cast<std::size_t>(offset) * light_box_size;
    for (std::size_t coordinate = 0; coordinate < 6; ++coordinate) {
        view.bounds[coordinate] = std::bit_cast<double>(
            load_le64(bytes_, box_base + coordinate * sizeof(std::uint64_t)));
    }
    return view;
}

auto vanilla_tables::string_at(std::uint32_t index) const -> std::string_view {
    if (index >= info_.strings) {
        return {};
    }
    const auto offsets = strings_.offset + 4U;
    const auto data = offsets + (info_.strings + 1ULL) * 4U;
    const auto begin = load_le32(bytes_, offsets + index * 4ULL);
    const auto end = load_le32(bytes_, offsets + (index + 1ULL) * 4ULL);
    const auto pointer =
        reinterpret_cast<const char *>(bytes_.data() + data + begin);
    return std::string_view{pointer, end - begin};
}

auto vanilla_tables::block_at(std::uint16_t id) const -> block_view {
    const auto base = blocks_.offset + 4U + id * block_record_size;
    return block_view{
        .name = string_at(load_le32(bytes_, base)),
        .first_state = load_le32(bytes_, base + 4),
        .state_count = load_le32(bytes_, base + 8),
        .default_state = load_le32(bytes_, base + 12),
    };
}

auto vanilla_tables::find_block(std::string_view name) const
    -> std::optional<std::uint16_t> {
    if (block_index_.empty()) {
        return std::nullopt;
    }
    const auto mask = block_index_.size() - 1;
    auto slot = static_cast<std::size_t>(hash64(name)) & mask;
    while (block_index_[slot] != 0) {
        const auto id = static_cast<std::uint16_t>(block_index_[slot] - 1);
        if (block_at(id).name == name) {
            return id;
        }
        slot = (slot + 1) & mask;
    }
    return std::nullopt;
}

auto vanilla_tables::state_at(std::uint32_t id) const -> state_view {
    const auto base = states_.offset + 4U + id * state_record_size;
    const auto block = load_le16(bytes_, base);
    const auto count = static_cast<std::size_t>(bytes_[base + 4]);
    return state_view{
        .block = block,
        .vanilla_id = load_le16(bytes_, base + 2),
        .key = string_at(load_le32(bytes_, base + 16)),
        .values =
            std::span<const std::uint8_t>{
                reinterpret_cast<const std::uint8_t *>(bytes_.data() + base +
                                                       5),
                count},
    };
}

auto vanilla_tables::find_state(std::string_view key) const
    -> std::optional<std::uint32_t> {
    if (state_index_.empty()) {
        return std::nullopt;
    }
    const auto mask = state_index_.size() - 1;
    auto slot = static_cast<std::size_t>(hash64(key)) & mask;
    while (state_index_[slot] != 0) {
        const auto id = state_index_[slot] - 1;
        if (state_at(id).key == key) {
            return id;
        }
        slot = (slot + 1) & mask;
    }
    return std::nullopt;
}

auto vanilla_tables::state_by_vanilla_id(std::uint16_t vanilla_id) const
    -> std::optional<std::uint32_t> {
    const auto found = std::ranges::lower_bound(
        vanilla_index_, vanilla_id, {},
        &std::pair<std::uint16_t, std::uint32_t>::first);
    if (found == vanilla_index_.end() || found->first != vanilla_id) {
        return std::nullopt;
    }
    return found->second;
}

auto vanilla_tables::property_name(std::uint16_t id) const -> std::string_view {
    return string_at(
        load_le32(bytes_, properties_.offset + 4U + id * property_record_size));
}

auto vanilla_tables::property_value_count(std::uint16_t id) const
    -> std::uint8_t {
    return static_cast<std::uint8_t>(load_le16(
        bytes_, properties_.offset + 4U + id * property_record_size + 8));
}

auto vanilla_tables::property_value(std::uint16_t id, std::uint8_t index) const
    -> std::string_view {
    const auto base = properties_.offset + 4U + id * property_record_size;
    const auto first_value = load_le32(bytes_, base + 4);
    const auto value = load_le32(bytes_, property_values_.offset + 4U +
                                             (first_value + index) * 4ULL);
    return string_at(value);
}

auto vanilla_tables::block_properties(std::uint16_t block) const
    -> std::span<const std::uint8_t> {
    const auto base = blocks_.offset + 4U + block * block_record_size;
    const auto first = load_le32(bytes_, base + 16);
    const auto count = load_le16(bytes_, base + 20);
    return std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t *>(
            bytes_.data() + block_properties_.offset + first),
        count};
}

auto vanilla_tables::preset_at(std::uint32_t index) const -> std::string_view {
    return string_at(
        load_le32(bytes_, biomes_.offset + 4U + index * preset_record_size));
}

auto vanilla_tables::find_preset(std::string_view name) const
    -> std::optional<std::uint32_t> {
    for (std::uint32_t i = 0; i < info_.presets; ++i) {
        if (preset_at(i) == name) {
            return i;
        }
    }
    return std::nullopt;
}

auto vanilla_tables::biome_parameter_count(std::uint32_t preset) const
    -> std::uint32_t {
    return load_le32(bytes_,
                     biomes_.offset + 4U + preset * preset_record_size + 8);
}

auto vanilla_tables::biome_parameter(std::uint32_t preset,
                                     std::uint32_t index) const
    -> biome_parameter_view {
    const auto preset_base = biomes_.offset + 4U + preset * preset_record_size;
    const auto first_entry = load_le32(bytes_, preset_base + 4);
    const auto entry_base = biomes_.offset + 4U +
                            info_.presets * preset_record_size +
                            (first_entry + index) * biome_entry_size;
    biome_parameter_view view;
    for (std::size_t i = 0; i < 6; ++i) {
        view.min[i] = load_le<std::int32_t>(bytes_, entry_base + i * 4ULL);
        view.max[i] =
            load_le<std::int32_t>(bytes_, entry_base + 24U + i * 4ULL);
    }
    view.offset = load_le<std::int32_t>(bytes_, entry_base + 48U);
    view.biome = string_at(load_le32(bytes_, entry_base + 52U));
    return view;
}

auto vanilla_tables::registry_at(std::uint32_t index) const
    -> std::string_view {
    return string_at(load_le32(bytes_, registries_.offset + 4U +
                                           index * registry_record_size));
}

auto vanilla_tables::find_registry(std::string_view name) const
    -> std::optional<std::uint32_t> {
    for (std::uint32_t i = 0; i < info_.registries; ++i) {
        if (registry_at(i) == name) {
            return i;
        }
    }
    return std::nullopt;
}

auto vanilla_tables::registry_entry_count(std::uint32_t registry) const
    -> std::uint32_t {
    return load_le32(bytes_, registries_.offset + 4U +
                                 registry * registry_record_size + 8);
}

auto vanilla_tables::registry_entry(std::uint32_t registry,
                                    std::uint32_t index) const
    -> registry_entry_view {
    const auto registry_base =
        registries_.offset + 4U + registry * registry_record_size;
    const auto first_entry = load_le32(bytes_, registry_base + 4);
    const auto entry_base = registries_.offset + 4U +
                            info_.registries * registry_record_size +
                            (first_entry + index) * registry_entry_size;
    return registry_entry_view{
        .name = string_at(load_le32(bytes_, entry_base)),
        .protocol_id = load_le<std::int32_t>(bytes_, entry_base + 4),
    };
}

auto vanilla_tables::item_at(std::uint32_t index) const -> item_view {
    const auto base = items_.offset + 4U + index * item_record_size;
    return item_view{
        .name = string_at(load_le32(bytes_, base)),
        .max_stack_size = load_le16(bytes_, base + 4),
        .max_damage = load_le32(bytes_, base + 8),
    };
}

auto vanilla_tables::block_mining_at(std::uint16_t block) const
    -> block_mining_view {
    if (!have_mining_ || block >= info_.blocks) {
        return block_mining_view{};
    }
    const auto base = mining_.offset + 4U + block * mining_record_size;
    block_mining_view view;
    view.hardness = std::bit_cast<float>(load_le32(bytes_, base));
    view.shears_speed = std::bit_cast<float>(load_le32(bytes_, base + 4U));
    view.sword_speed = std::bit_cast<float>(load_le32(bytes_, base + 8U));
    view.flags = std::to_integer<std::uint8_t>(bytes_[base + 12U]);
    view.tool_mask = std::to_integer<std::uint8_t>(bytes_[base + 13U]);
    view.needed_tier = std::to_integer<std::uint8_t>(bytes_[base + 14U]);
    view.direct_correct = std::to_integer<std::uint8_t>(bytes_[base + 15U]);
    return view;
}

auto vanilla_tables::tool_at(std::uint32_t index) const -> tool_mining_view {
    if (!have_mining_ || index >= info_.tools) {
        return tool_mining_view{};
    }
    const auto base = tools_section_.offset + 4U + index * tool_record_size;
    tool_mining_view view;
    view.name = string_at(load_le32(bytes_, base));
    view.type = static_cast<tool_type>(
        std::to_integer<std::uint8_t>(bytes_[base + 4U]));
    view.material = static_cast<tool_material>(
        std::to_integer<std::uint8_t>(bytes_[base + 5U]));
    view.harvest_level = std::to_integer<std::uint8_t>(bytes_[base + 6U]);
    const auto flags = std::to_integer<std::uint8_t>(bytes_[base + 7U]);
    view.tier_speed = std::bit_cast<float>(load_le32(bytes_, base + 8U));
    view.default_mining_speed =
        std::bit_cast<float>(load_le32(bytes_, base + 12U));
    view.damage_per_block = load_le16(bytes_, base + 16U);
    view.max_stack_size = load_le16(bytes_, base + 18U);
    view.max_damage = load_le32(bytes_, base + 20U);
    view.can_destroy_blocks_in_creative =
        (flags & tool_flag_can_destroy_in_creative) != 0U;
    return view;
}

auto vanilla_tables::find_tool(std::string_view name) const
    -> std::optional<std::uint32_t> {
    std::uint32_t low = 0;
    std::uint32_t high = info_.tools;
    while (low < high) {
        const auto middle = low + (high - low) / 2;
        if (tool_at(middle).name < name) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    if (low < info_.tools && tool_at(low).name == name) {
        return low;
    }
    return std::nullopt;
}

auto vanilla_tables::find_item(std::string_view name) const
    -> std::optional<std::uint32_t> {
    std::uint32_t low = 0;
    std::uint32_t high = info_.items;
    while (low < high) {
        const auto middle = low + (high - low) / 2;
        if (item_at(middle).name < name) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    if (low < info_.items && item_at(low).name == name) {
        return low;
    }
    return std::nullopt;
}

} // namespace refrain::data
