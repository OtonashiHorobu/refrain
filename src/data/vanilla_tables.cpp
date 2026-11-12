// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/vanilla.hpp>

#include <refrain/data/checksum.hpp>

#include <algorithm>
#include <array>
#include <bit>
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

void put_u16(std::vector<std::byte> &out, std::uint16_t value) {
    out.push_back(static_cast<std::byte>(value & 0xFFU));
    out.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
}

void put_u32(std::vector<std::byte> &out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
    }
}

void put_u64(std::vector<std::byte> &out, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
    }
}

void put_i32(std::vector<std::byte> &out, std::int32_t value) {
    put_u32(out, static_cast<std::uint32_t>(value));
}

void put_bytes(std::vector<std::byte> &out, std::span<const std::byte> bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

auto read_u16(std::span<const std::byte> bytes, std::size_t offset)
    -> std::uint16_t {
    return static_cast<std::uint16_t>(bytes[offset]) |
           (static_cast<std::uint16_t>(bytes[offset + 1]) << 8U);
}

auto read_u32(std::span<const std::byte> bytes, std::size_t offset)
    -> std::uint32_t {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(bytes[offset + i]) << (8 * i);
    }
    return value;
}

auto read_u64(std::span<const std::byte> bytes, std::size_t offset)
    -> std::uint64_t {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(bytes[offset + i]) << (8 * i);
    }
    return value;
}

auto read_i32(std::span<const std::byte> bytes, std::size_t offset)
    -> std::int32_t {
    return static_cast<std::int32_t>(read_u32(bytes, offset));
}

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
    std::sort(blocks.begin(), blocks.end(),
              [](const block_spec *left, const block_spec *right) {
                  return left->name < right->name;
              });
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
        std::sort(order.begin(), order.end(),
                  [&](std::size_t left, std::size_t right) {
                      return block.property_names[left] <
                             block.property_names[right];
                  });

        std::vector<std::string> names;
        std::vector<std::vector<std::string>> values;
        names.reserve(property_count);
        values.reserve(property_count);
        for (const auto index : order) {
            names.push_back(block.property_names[index]);
            values.push_back(block.property_values[index]);
            std::sort(values.back().begin(), values.back().end());
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
                const auto position = std::lower_bound(
                    property_values.begin(), property_values.end(), value);
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
        std::sort(block_states.begin(), block_states.end(),
                  [&](const built_state &left, const built_state &right) {
                      return left.key < right.key;
                  });
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
    std::sort(
        presets.begin(), presets.end(),
        [](const biome_preset_spec *left, const biome_preset_spec *right) {
            return left->name < right->name;
        });
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
    std::sort(registries.begin(), registries.end(),
              [](const registry_spec *left, const registry_spec *right) {
                  return left->name < right->name;
              });
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
        std::sort(entries.begin(), entries.end(),
                  [](const registry_entry_spec *left,
                     const registry_entry_spec *right) {
                      return left->name < right->name;
                  });
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
    std::sort(items.begin(), items.end(),
              [](const item_spec *left, const item_spec *right) {
                  return left->name < right->name;
              });
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
    std::copy(digest.begin(), digest.end(),
              out.begin() + static_cast<std::ptrdiff_t>(digest_position));
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
    const auto version = read_u16(bytes, 4);
    if (version != vanilla_table_version) {
        return std::unexpected{fail("unsupported container version")};
    }
    if (read_u16(bytes, 6) != vanilla_table_kind) {
        return std::unexpected{fail("unsupported container kind")};
    }
    if (read_u32(bytes, 32) != 1) {
        return std::unexpected{fail("unsupported digest algorithm")};
    }
    if (read_u64(bytes, 24) != bytes.size() - header_size) {
        return std::unexpected{fail("payload size does not match the file")};
    }

    vanilla_tables table;
    table.bytes_ = bytes;
    table.info_.version = version;
    table.info_.kind = vanilla_table_kind;
    table.info_.data_version = read_i32(bytes, 12);

    const auto section_count = read_u32(bytes, 16);
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
        record.offset = read_u32(bytes, base + 4);
        record.size = read_u32(bytes, base + 8);
        if (record.offset % 8U != 0 || record.offset < table_end ||
            record.size > bytes.size() ||
            record.offset > bytes.size() - record.size) {
            return std::unexpected{fail("section outside the file")};
        }
        const auto payload = std::span<const std::byte>{bytes}.subspan(
            record.offset, record.size);
        if (crc32c(payload) != read_u32(bytes, base + 12)) {
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

    const auto str_count = read_u32(bytes, table.strings_.offset);
    if (table.strings_.size < 4U + (str_count + 1ULL) * 4U) {
        return std::unexpected{fail("string pool truncated")};
    }
    table.info_.strings = str_count;
    const auto strings_data =
        table.strings_.offset + 4U + (str_count + 1ULL) * 4U;
    std::uint32_t strings_size = 0;
    for (std::uint32_t i = 0; i <= str_count; ++i) {
        const auto value =
            read_u32(bytes, table.strings_.offset + 4U + i * 4ULL);
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
    table.info_.blocks = read_u32(bytes, table.blocks_.offset);
    if (table.blocks_.size < 4U + table.info_.blocks * block_record_size) {
        return std::unexpected{fail("block section truncated")};
    }
    table.info_.properties = read_u32(bytes, table.properties_.offset);
    if (table.properties_.size <
        4U + table.info_.properties * property_record_size) {
        return std::unexpected{fail("property section truncated")};
    }
    const auto value_count = read_u32(bytes, table.property_values_.offset);
    if (table.property_values_.size < 4U + value_count * 4ULL) {
        return std::unexpected{fail("value section truncated")};
    }
    table.info_.states = read_u32(bytes, table.states_.offset);
    if (table.states_.size < 4U + table.info_.states * state_record_size) {
        return std::unexpected{fail("state section truncated")};
    }
    table.info_.presets = read_u32(bytes, table.biomes_.offset);
    if (table.biomes_.size < 4U + table.info_.presets * preset_record_size) {
        return std::unexpected{fail("biome section truncated")};
    }
    table.info_.registries = read_u32(bytes, table.registries_.offset);
    if (table.registries_.size <
        4U + table.info_.registries * registry_record_size) {
        return std::unexpected{fail("registry section truncated")};
    }
    table.info_.items = read_u32(bytes, table.items_.offset);
    if (table.items_.size < 4U + table.info_.items * item_record_size) {
        return std::unexpected{fail("item section truncated")};
    }

    const auto block_base = table.blocks_.offset + 4U;
    const auto property_base = table.properties_.offset + 4U;
    const auto state_base = table.states_.offset + 4U;

    // Strings referenced by blocks and properties must be in range.
    for (std::uint32_t i = 0; i < table.info_.blocks; ++i) {
        const auto base = block_base + i * block_record_size;
        const auto first = read_u32(bytes, base + 4);
        const auto count = read_u32(bytes, base + 8);
        const auto default_state = read_u32(bytes, base + 12);
        const auto first_property = read_u32(bytes, base + 16);
        const auto property_count = read_u16(bytes, base + 20);
        if (read_u32(bytes, base) >= table.info_.strings) {
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
        const auto first_value = read_u32(bytes, base + 4);
        const auto value_count_property = read_u16(bytes, base + 8);
        if (read_u32(bytes, base) >= table.info_.strings) {
            return std::unexpected{fail("property name out of range")};
        }
        if (value_count_property == 0 ||
            static_cast<std::uint64_t>(first_value) + value_count_property >
                value_count) {
            return std::unexpected{fail("property value range invalid")};
        }
    }
    for (std::uint32_t i = 0; i < value_count; ++i) {
        if (read_u32(bytes, table.property_values_.offset + 4U + i * 4ULL) >=
            table.info_.strings) {
            return std::unexpected{fail("value string out of range")};
        }
    }
    for (std::uint32_t i = 0; i < table.info_.states; ++i) {
        const auto base = state_base + i * state_record_size;
        const auto block = read_u16(bytes, base);
        const auto property_count = static_cast<std::size_t>(bytes[base + 4]);
        const auto key = read_u32(bytes, base + 16);
        if (block >= table.info_.blocks || property_count > 8 ||
            key >= table.info_.strings) {
            return std::unexpected{fail("state record invalid")};
        }
        const auto block_record = block_base + block * block_record_size;
        const auto first_property = read_u32(bytes, block_record + 16);
        const auto block_property_count = read_u16(bytes, block_record + 20);
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
                read_u16(bytes, property_record + 8);
            if (std::to_integer<std::size_t>(bytes[base + 5 + p]) >=
                value_count_property) {
                return std::unexpected{fail("state value index out of range")};
            }
        }
    }
    for (std::uint32_t i = 0; i < table.info_.presets; ++i) {
        const auto base = table.biomes_.offset + 4U + i * preset_record_size;
        const auto first_entry = read_u32(bytes, base + 4);
        const auto entry_count = read_u32(bytes, base + 8);
        const auto required =
            4ULL +
            static_cast<std::uint64_t>(table.info_.presets) *
                preset_record_size +
            (static_cast<std::uint64_t>(first_entry) + entry_count) *
                biome_entry_size;
        if (read_u32(bytes, base) >= table.info_.strings ||
            required > table.biomes_.size) {
            return std::unexpected{fail("biome preset record invalid")};
        }
    }
    for (std::uint32_t i = 0; i < table.info_.registries; ++i) {
        const auto base =
            table.registries_.offset + 4U + i * registry_record_size;
        const auto first_entry = read_u32(bytes, base + 4);
        const auto entry_count = read_u32(bytes, base + 8);
        const auto required =
            4ULL +
            static_cast<std::uint64_t>(table.info_.registries) *
                registry_record_size +
            (static_cast<std::uint64_t>(first_entry) + entry_count) *
                registry_entry_size;
        if (read_u32(bytes, base) >= table.info_.strings ||
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
    std::sort(table.vanilla_index_.begin(), table.vanilla_index_.end());

    return table;
}

auto vanilla_tables::string_at(std::uint32_t index) const -> std::string_view {
    if (index >= info_.strings) {
        return {};
    }
    const auto offsets = strings_.offset + 4U;
    const auto data = offsets + (info_.strings + 1ULL) * 4U;
    const auto begin = read_u32(bytes_, offsets + index * 4ULL);
    const auto end = read_u32(bytes_, offsets + (index + 1ULL) * 4ULL);
    const auto pointer =
        reinterpret_cast<const char *>(bytes_.data() + data + begin);
    return std::string_view{pointer, end - begin};
}

auto vanilla_tables::block_at(std::uint16_t id) const -> block_view {
    const auto base = blocks_.offset + 4U + id * block_record_size;
    return block_view{
        .name = string_at(read_u32(bytes_, base)),
        .first_state = read_u32(bytes_, base + 4),
        .state_count = read_u32(bytes_, base + 8),
        .default_state = read_u32(bytes_, base + 12),
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
    const auto block = read_u16(bytes_, base);
    const auto count = static_cast<std::size_t>(bytes_[base + 4]);
    return state_view{
        .block = block,
        .vanilla_id = read_u16(bytes_, base + 2),
        .key = string_at(read_u32(bytes_, base + 16)),
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
    const auto found = std::lower_bound(
        vanilla_index_.begin(), vanilla_index_.end(), vanilla_id,
        [](const std::pair<std::uint16_t, std::uint32_t> &entry,
           std::uint16_t value) { return entry.first < value; });
    if (found == vanilla_index_.end() || found->first != vanilla_id) {
        return std::nullopt;
    }
    return found->second;
}

auto vanilla_tables::property_name(std::uint16_t id) const -> std::string_view {
    return string_at(
        read_u32(bytes_, properties_.offset + 4U + id * property_record_size));
}

auto vanilla_tables::property_value_count(std::uint16_t id) const
    -> std::uint8_t {
    return static_cast<std::uint8_t>(read_u16(
        bytes_, properties_.offset + 4U + id * property_record_size + 8));
}

auto vanilla_tables::property_value(std::uint16_t id, std::uint8_t index) const
    -> std::string_view {
    const auto base = properties_.offset + 4U + id * property_record_size;
    const auto first_value = read_u32(bytes_, base + 4);
    const auto value = read_u32(bytes_, property_values_.offset + 4U +
                                            (first_value + index) * 4ULL);
    return string_at(value);
}

auto vanilla_tables::block_properties(std::uint16_t block) const
    -> std::span<const std::uint8_t> {
    const auto base = blocks_.offset + 4U + block * block_record_size;
    const auto first = read_u32(bytes_, base + 16);
    const auto count = read_u16(bytes_, base + 20);
    return std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t *>(
            bytes_.data() + block_properties_.offset + first),
        count};
}

auto vanilla_tables::preset_at(std::uint32_t index) const -> std::string_view {
    return string_at(
        read_u32(bytes_, biomes_.offset + 4U + index * preset_record_size));
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
    return read_u32(bytes_,
                    biomes_.offset + 4U + preset * preset_record_size + 8);
}

auto vanilla_tables::biome_parameter(std::uint32_t preset,
                                     std::uint32_t index) const
    -> biome_parameter_view {
    const auto preset_base = biomes_.offset + 4U + preset * preset_record_size;
    const auto first_entry = read_u32(bytes_, preset_base + 4);
    const auto entry_base = biomes_.offset + 4U +
                            info_.presets * preset_record_size +
                            (first_entry + index) * biome_entry_size;
    biome_parameter_view view;
    for (std::size_t i = 0; i < 6; ++i) {
        view.min[i] = read_i32(bytes_, entry_base + i * 4ULL);
        view.max[i] = read_i32(bytes_, entry_base + 24U + i * 4ULL);
    }
    view.offset = read_i32(bytes_, entry_base + 48U);
    view.biome = string_at(read_u32(bytes_, entry_base + 52U));
    return view;
}

auto vanilla_tables::registry_at(std::uint32_t index) const
    -> std::string_view {
    return string_at(read_u32(bytes_, registries_.offset + 4U +
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
    return read_u32(bytes_, registries_.offset + 4U +
                                registry * registry_record_size + 8);
}

auto vanilla_tables::registry_entry(std::uint32_t registry,
                                    std::uint32_t index) const
    -> registry_entry_view {
    const auto registry_base =
        registries_.offset + 4U + registry * registry_record_size;
    const auto first_entry = read_u32(bytes_, registry_base + 4);
    const auto entry_base = registries_.offset + 4U +
                            info_.registries * registry_record_size +
                            (first_entry + index) * registry_entry_size;
    return registry_entry_view{
        .name = string_at(read_u32(bytes_, entry_base)),
        .protocol_id = read_i32(bytes_, entry_base + 4),
    };
}

auto vanilla_tables::item_at(std::uint32_t index) const -> item_view {
    const auto base = items_.offset + 4U + index * item_record_size;
    return item_view{
        .name = string_at(read_u32(bytes_, base)),
        .max_stack_size = read_u16(bytes_, base + 4),
        .max_damage = read_u32(bytes_, base + 8),
    };
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
