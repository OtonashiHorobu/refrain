// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/sim/light.hpp>

#include <refrain/sim/world.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace refrain::sim {
namespace {

constexpr std::int32_t section_edge = 16;
constexpr std::int32_t section_columns = 256; // 16 × 16
constexpr std::int32_t min_section_y = -64;
constexpr std::int32_t max_section_y = 63;
constexpr std::int32_t section_span = max_section_y - min_section_y + 1;
constexpr std::int32_t section_index_offset = -min_section_y;
constexpr std::size_t light_bytes = 2048;
constexpr std::size_t layer_cells = 4096;
constexpr std::size_t layer_count = 2;

struct direction {
    std::int32_t dx;
    std::int32_t dy;
    std::int32_t dz;
};

constexpr std::array<direction, 6> directions{{
    {.dx = 1, .dy = 0, .dz = 0},
    {.dx = -1, .dy = 0, .dz = 0},
    {.dx = 0, .dy = 1, .dz = 0},
    {.dx = 0, .dy = -1, .dz = 0},
    {.dx = 0, .dy = 0, .dz = 1},
    {.dx = 0, .dy = 0, .dz = -1},
}};

[[nodiscard]] constexpr auto floor_div(std::int32_t value,
                                       std::int32_t divisor) noexcept
    -> std::int32_t {
    const auto quotient{value / divisor};
    return value % divisor != 0 && (value < 0) != (divisor < 0) ? quotient - 1
                                                                : quotient;
}

[[nodiscard]] constexpr auto section_coord(std::int32_t block) noexcept
    -> std::int32_t {
    return floor_div(block, section_edge);
}

[[nodiscard]] constexpr auto local_coord(std::int32_t block) noexcept
    -> std::int32_t {
    return block - section_coord(block) * section_edge;
}

[[nodiscard]] constexpr auto local_index(std::int32_t y, std::int32_t z,
                                         std::int32_t x) noexcept
    -> std::size_t {
    return static_cast<std::size_t>(((y & 15) << 8) | ((z & 15) << 4) |
                                    (x & 15));
}

[[nodiscard]] auto nibble_get(std::span<const std::byte> data,
                              std::size_t index) noexcept -> std::uint8_t {
    const auto packed = static_cast<std::uint8_t>(data[index >> 1U]);
    return static_cast<std::uint8_t>((packed >> ((index & 1U) * 4U)) & 0x0FU);
}

void nibble_set(std::vector<std::byte> &data, std::size_t index,
                std::uint8_t value) noexcept {
    const auto position = index >> 1U;
    const auto shift = (index & 1U) * 4U;
    const auto packed = static_cast<std::uint8_t>(data[position]);
    data[position] = static_cast<std::byte>((packed & ~(0x0FU << shift)) |
                                            ((value & 0x0FU) << shift));
}

[[nodiscard]] auto split_block_name(std::string_view key) noexcept
    -> std::string_view {
    const auto bracket{key.find('[')};
    return bracket == std::string_view::npos ? key : key.substr(0, bracket);
}

/// True when the canonical key carries `property=value`.
[[nodiscard]] auto has_property(std::string_view key, std::string_view property,
                                std::string_view value) noexcept -> bool {
    const auto bracket{key.find('[')};
    if (bracket == std::string_view::npos) {
        return false;
    }
    const auto properties{key.substr(bracket + 1)};
    std::size_t start{0};
    while (start <= properties.size()) {
        const auto end{properties.find(',', start)};
        const auto token{properties.substr(
            start, end == std::string_view::npos ? properties.size() - start
                                                 : end - start)};
        const auto equal{token.find('=')};
        if (equal != std::string_view::npos) {
            auto name{token.substr(0, equal)};
            while (!name.empty() && name.front() == ' ') {
                name.remove_prefix(1);
            }
            auto text{token.substr(equal + 1)};
            while (!text.empty() &&
                   (text.back() == ']' || text.back() == ' ')) {
                text.remove_suffix(1);
            }
            if (name == property && text == value) {
                return true;
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return false;
}

/// A block light source. An entry with `condition` only emits while the
/// canonical key carries `<condition>=true`.
struct emission_entry {
    std::string_view name;
    std::uint8_t level;
    std::string_view condition;
};

// TODO: the reference derives light emission and dampening from block code and
// the shipped content tables carry neither. This curated table stands in for
// the table import until the block-behaviour tables expose both fields; a block
// outside it is opaque and emits nothing, which can only make a scene darker
// than the reference.
constexpr std::array<emission_entry, 40> emission_table{{
    {"minecraft:amethyst_cluster", 5, ""},
    {"minecraft:beacon", 15, ""},
    {"minecraft:brewing_stand", 1, ""},
    {"minecraft:brown_mushroom", 1, ""},
    {"minecraft:campfire", 15, "lit"},
    {"minecraft:cave_vines", 14, ""},
    {"minecraft:cave_vines_plant", 14, ""},
    {"minecraft:conduit", 15, ""},
    {"minecraft:copper_torch", 14, ""},
    {"minecraft:copper_wall_torch", 14, ""},
    {"minecraft:crying_obsidian", 10, ""},
    {"minecraft:dragon_egg", 1, ""},
    {"minecraft:end_gateway", 15, ""},
    {"minecraft:end_rod", 14, ""},
    {"minecraft:enchanting_table", 7, ""},
    {"minecraft:ender_chest", 7, ""},
    {"minecraft:fire", 15, ""},
    {"minecraft:glow_lichen", 7, ""},
    {"minecraft:glowstone", 15, ""},
    {"minecraft:jack_o_lantern", 15, ""},
    {"minecraft:lantern", 15, ""},
    {"minecraft:large_amethyst_bud", 4, ""},
    {"minecraft:lava", 15, ""},
    {"minecraft:lava_cauldron", 15, ""},
    {"minecraft:magma_block", 3, ""},
    {"minecraft:medium_amethyst_bud", 2, ""},
    {"minecraft:ochre_froglight", 15, ""},
    {"minecraft:pearlescent_froglight", 15, ""},
    {"minecraft:redstone_lamp", 15, "lit"},
    {"minecraft:redstone_torch", 7, "lit"},
    {"minecraft:redstone_wall_torch", 7, "lit"},
    {"minecraft:respawn_anchor", 15, ""},
    {"minecraft:sea_lantern", 15, ""},
    {"minecraft:shroomlight", 15, ""},
    {"minecraft:soul_fire", 10, ""},
    {"minecraft:soul_lantern", 10, ""},
    {"minecraft:soul_torch", 10, ""},
    {"minecraft:soul_wall_torch", 10, ""},
    {"minecraft:torch", 14, ""},
    {"minecraft:wall_torch", 14, ""},
}};

/// Blocks whose shape does not fill their cell: light passes through the empty
/// part without a loss, so they take no part in the sky source edge.
constexpr std::string_view no_dampening_names[] = {
    "minecraft:air",
    "minecraft:cave_air",
    "minecraft:void_air",
    "minecraft:glass",
    "minecraft:glass_pane",
    "minecraft:iron_bars",
    "minecraft:chain",
    "minecraft:ladder",
    "minecraft:vine",
    "minecraft:glow_lichen",
    "minecraft:sculk_vein",
    "minecraft:torch",
    "minecraft:wall_torch",
    "minecraft:soul_torch",
    "minecraft:soul_wall_torch",
    "minecraft:copper_torch",
    "minecraft:copper_wall_torch",
    "minecraft:redstone_torch",
    "minecraft:redstone_wall_torch",
    "minecraft:fire",
    "minecraft:soul_fire",
    "minecraft:end_rod",
    "minecraft:lightning_rod",
    "minecraft:lantern",
    "minecraft:soul_lantern",
    "minecraft:campfire",
    "minecraft:soul_campfire",
    "minecraft:bamboo",
    "minecraft:pointed_dripstone",
    "minecraft:amethyst_cluster",
    "minecraft:short_grass",
    "minecraft:tall_grass",
    "minecraft:fern",
    "minecraft:large_fern",
    "minecraft:dead_bush",
    "minecraft:sugar_cane",
    "minecraft:seagrass",
    "minecraft:tall_seagrass",
    "minecraft:kelp",
    "minecraft:kelp_plant",
    "minecraft:nether_sprouts",
    "minecraft:crimson_roots",
    "minecraft:warped_roots",
    "minecraft:redstone_wire",
    "minecraft:tripwire",
    "minecraft:tripwire_hook",
    "minecraft:rail",
    "minecraft:powered_rail",
    "minecraft:detector_rail",
    "minecraft:activator_rail",
    "minecraft:wildflowers",
    "minecraft:leaf_litter",
    "minecraft:pale_hanging_moss",
    "minecraft:hanging_roots",
    "minecraft:moss_carpet",
    "minecraft:pink_petals",
    "minecraft:dandelion",
    "minecraft:poppy",
    "minecraft:blue_orchid",
    "minecraft:allium",
    "minecraft:azure_bluet",
    "minecraft:red_tulip",
    "minecraft:orange_tulip",
    "minecraft:white_tulip",
    "minecraft:pink_tulip",
    "minecraft:oxeye_daisy",
    "minecraft:cornflower",
    "minecraft:lily_of_the_valley",
    "minecraft:wither_rose",
    "minecraft:torchflower",
    "minecraft:brown_mushroom",
    "minecraft:red_mushroom",
    "minecraft:sweet_berry_bush",
    "minecraft:cocoa",
    "minecraft:cave_vines",
    "minecraft:cave_vines_plant",
    "minecraft:crimson_fungus",
    "minecraft:warped_fungus",
    "minecraft:sculk_sensor",
    "minecraft:calibrated_sculk_sensor",
};

/// Blocks whose cell is full but not solid for light: a path through them
/// loses one level. Fluids and translucent full blocks.
constexpr std::string_view partial_dampening_names[] = {
    "minecraft:water",       "minecraft:lava",          "minecraft:ice",
    "minecraft:frosted_ice", "minecraft:bubble_column", "minecraft:snow",
    "minecraft:snow_block",  "minecraft:powder_snow",
};

/// Suffixes of blocks whose shape covers only part of their cell. The
/// reference decides this from the collision shape; the name family is the
/// approximation available here.
constexpr std::string_view no_dampening_suffixes[] = {
    "_sapling",        "_tulip",  "_orchid",  "_bluet",  "_daisy",  "_bush",
    "_grass",          "_fern",   "_sprouts", "_roots",  "_flower", "_coral",
    "_vine",           "_lichen", "_hanging", "_carpet", "_candle", "_button",
    "_pressure_plate", "_sign",   "_banner",  "_slab",   "_stairs", "_trapdoor",
};

/// Suffixes the reference stores as a full block that is not solid for light.
constexpr std::string_view partial_dampening_suffixes[] = {
    "_leaves",
    "_fence",
    "_wall",
};

[[nodiscard]] auto light_property_for(std::string_view key)
    -> block_light_property {
    block_light_property property{};
    const auto name{split_block_name(key)};
    for (const auto &entry : emission_table) {
        if (entry.name != name) {
            continue;
        }
        if (entry.condition.empty() ||
            has_property(key, entry.condition, "true")) {
            property.emission = entry.level;
        }
        break;
    }
    if (name == "minecraft:light") {
        // The light block's level property is its own emission.
        for (auto level{15}; level >= 1; --level) {
            if (has_property(key, "level", std::to_string(level))) {
                property.emission = static_cast<std::uint8_t>(level);
                break;
            }
        }
    }
    for (const auto candidate : no_dampening_names) {
        if (candidate == name) {
            property.dampening = 0;
            return property;
        }
    }
    for (const auto candidate : partial_dampening_names) {
        if (candidate == name) {
            property.dampening = 1;
            return property;
        }
    }
    for (const auto suffix : no_dampening_suffixes) {
        if (name.ends_with(suffix)) {
            property.dampening = 0;
            return property;
        }
    }
    for (const auto suffix : partial_dampening_suffixes) {
        if (name.ends_with(suffix)) {
            property.dampening = 1;
            return property;
        }
    }
    return property;
}

/// One loaded column as the engine sees it.
struct column_state {
    chunk *record{nullptr};
    /// Highest block section Y plus one: the light layer range follows it.
    std::int32_t top_layer_y{0};
    std::uint16_t section_count{0};
    /// Light layers sorted by section Y. A column whose block sections have
    /// gaps keeps layers only where a block section or the padding layer
    /// exists; the gap sections are air and carry no stored light. The arrays
    /// are owned by the column's storage record and stay address-stable after
    /// `reset_layers()`.
    struct layer_ref {
        std::int8_t y{0};
        ::refrain::storage::light_section *layer{nullptr};
    };
    std::vector<layer_ref> layers{};
    std::array<std::int16_t, section_span> section_lookup{};
    /// Lowest sky light source cell per column: the Y of the first cell below
    /// the column's highest light damping block. `block_bottom()` means the
    /// whole column sees the sky.
    std::array<std::int32_t, section_columns> lowest_source{};

    [[nodiscard]] auto layer(std::int32_t section_y) noexcept
        -> ::refrain::storage::light_section * {
        const auto found{std::lower_bound(
            layers.begin(), layers.end(), section_y,
            [](const layer_ref &entry, std::int32_t value) {
                return static_cast<std::int32_t>(entry.y) < value;
            })};
        if (found == layers.end() ||
            static_cast<std::int32_t>(found->y) != section_y) {
            return nullptr;
        }
        return found->layer;
    }
};

[[nodiscard]] auto layer_array(::refrain::storage::light_section &layer,
                               light_layer which) -> std::vector<std::byte> & {
    return which == light_layer::sky ? layer.sky_light : layer.block_light;
}

[[nodiscard]] auto layer_index(light_layer which) noexcept -> std::size_t {
    return which == light_layer::sky ? 0 : 1;
}

struct node_pos {
    std::int32_t x{0};
    std::int16_t y{0};
    std::int32_t z{0};
};

struct increase_entry {
    node_pos node{};
    std::uint8_t level{0};
};

struct decrease_entry {
    node_pos node{};
    std::uint8_t level{0};
};

struct layer_queues {
    std::deque<decrease_entry> decrease{};
    std::deque<increase_entry> increase{};
};

} // namespace

struct light_engine::impl {
    explicit impl(world &owner) : owner_{&owner} {}

    world *owner_{nullptr};
    std::map<chunk_pos, column_state> columns_{};

    /// Most recently used column, so propagation within one column does not
    /// repeat the map lookup for every cell.
    column_state *cached_{nullptr};
    std::int32_t cached_x_{0};
    std::int32_t cached_z_{0};

    std::deque<node_pos> check_queue_{};
    std::array<layer_queues, layer_count> queues_{};
    std::set<light_section_change> changes_{};
    std::optional<chunk_pos> suppressed_origin_{};
    std::unordered_map<block_state_id, block_light_property> properties_{};

    [[nodiscard]] auto find_column(std::int32_t chunk_x, std::int32_t chunk_z)
        -> column_state * {
        if (cached_ != nullptr && cached_x_ == chunk_x &&
            cached_z_ == chunk_z) {
            return cached_;
        }
        const auto found{columns_.find(chunk_pos{.x = chunk_x, .z = chunk_z})};
        if (found == columns_.end()) {
            cached_ = nullptr;
            return nullptr;
        }
        cached_ = &found->second;
        cached_x_ = chunk_x;
        cached_z_ = chunk_z;
        return cached_;
    }

    void clear_cache() noexcept { cached_ = nullptr; }

    [[nodiscard]] auto property(block_state_id id) -> block_light_property {
        const auto found{properties_.find(id)};
        if (found != properties_.end()) {
            return found->second;
        }
        const auto key{owner_->registry().key(id)};
        const auto property{light_property_for(key)};
        properties_.emplace(id, property);
        return property;
    }

    [[nodiscard]] auto block_state_at(std::int32_t x, std::int32_t y,
                                      std::int32_t z) -> block_state_id {
        auto *column{find_column(section_coord(x), section_coord(z))};
        if (column == nullptr) {
            return air_block;
        }
        const auto &chunk{*column->record};
        if (y < block_bottom(*column) || y > block_top(*column)) {
            return air_block;
        }
        const auto lookup_y{section_coord(y) + section_index_offset};
        if (lookup_y < 0 || lookup_y >= section_span) {
            return air_block;
        }
        const auto section_index{
            column->section_lookup[static_cast<std::size_t>(lookup_y)]};
        if (section_index < 0) {
            return air_block;
        }
        const auto &section{
            chunk.sections[static_cast<std::size_t>(section_index)]};
        if (section.uniform) {
            return section.value;
        }
        return section.blocks[local_index(y, z, x)];
    }

    [[nodiscard]] auto opacity(std::int32_t x, std::int32_t y, std::int32_t z)
        -> std::uint8_t {
        const auto dampening{property(block_state_at(x, y, z)).dampening};
        return dampening == 0 ? 1 : dampening;
    }

    [[nodiscard]] auto emission(std::int32_t x, std::int32_t y, std::int32_t z)
        -> std::uint8_t {
        return property(block_state_at(x, y, z)).emission;
    }

    void note_change(const chunk_pos &pos, std::int32_t section_y) {
        if (suppressed_origin_ && *suppressed_origin_ == pos) {
            return;
        }
        changes_.insert(light_section_change{
            .pos = pos, .section_y = static_cast<std::int8_t>(section_y)});
    }

    [[nodiscard]] auto level_at(const node_pos &node, light_layer which)
        -> std::uint8_t {
        auto *column{find_column(section_coord(node.x), section_coord(node.z))};
        if (column == nullptr) {
            return 0;
        }
        auto *layer{column->layer(section_coord(node.y))};
        if (layer == nullptr) {
            return 0;
        }
        const auto &array{layer_array(*layer, which)};
        if (array.size() != light_bytes) {
            return 0;
        }
        return nibble_get(array,
                          local_index(local_coord(node.y), local_coord(node.z),
                                      local_coord(node.x)));
    }

    void set_level(const node_pos &node, light_layer which,
                   std::uint8_t value) {
        auto *column{find_column(section_coord(node.x), section_coord(node.z))};
        if (column == nullptr) {
            return;
        }
        auto *layer{column->layer(section_coord(node.y))};
        if (layer == nullptr) {
            return;
        }
        auto &array{layer_array(*layer, which)};
        if (array.size() != light_bytes) {
            return;
        }
        const auto index{local_index(local_coord(node.y), local_coord(node.z),
                                     local_coord(node.x))};
        if (nibble_get(array, index) == value) {
            return;
        }
        nibble_set(array, index, value);
        note_change(column->record->pos, section_coord(node.y));
    }

    void enqueue_increase(light_layer which, const node_pos &node,
                          std::uint8_t level) {
        if (level == 0) {
            return;
        }
        queues_[layer_index(which)].increase.push_back(
            increase_entry{.node = node, .level = level});
    }

    void enqueue_decrease(light_layer which, const node_pos &node,
                          std::uint8_t level) {
        if (level == 0) {
            return;
        }
        queues_[layer_index(which)].decrease.push_back(
            decrease_entry{.node = node, .level = level});
    }

    [[nodiscard]] static auto block_bottom(const column_state &column) noexcept
        -> std::int32_t {
        return column.record->y_pos * section_edge;
    }

    [[nodiscard]] static auto block_top(const column_state &column) noexcept
        -> std::int32_t {
        const auto &chunk{*column.record};
        const auto top_section{
            chunk.sections.empty()
                ? chunk.y_pos
                : static_cast<std::int32_t>(chunk.sections.back().y) + 1};
        return top_section * section_edge - 1;
    }

    void rebuild_section_lookup(column_state &column) {
        column.section_lookup.fill(-1);
        const auto &sections{column.record->sections};
        for (std::size_t index{0}; index < sections.size(); ++index) {
            const auto lookup{static_cast<std::int32_t>(sections[index].y) +
                              section_index_offset};
            if (lookup >= 0 && lookup < section_span) {
                column.section_lookup[static_cast<std::size_t>(lookup)] =
                    static_cast<std::int16_t>(index);
            }
        }
        column.section_count = static_cast<std::uint16_t>(sections.size());
    }

    /// Replace the column's light layers with zeroed arrays and rebuild the
    /// layer index. Layers exist for every block section plus the padding
    /// layers below the lowest and above the highest block section; a column
    /// whose sections have gaps keeps no layer in the gap, which is air and
    /// carries no stored light.
    [[nodiscard]] auto reset_layers(column_state &column) -> bool {
        auto &chunk{*column.record};
        if (chunk.sections.empty()) {
            chunk.storage.light_sections.clear();
            column.layers.clear();
            column.top_layer_y = chunk.y_pos;
            return false;
        }
        const auto top_section{
            static_cast<std::int32_t>(chunk.sections.back().y)};
        std::vector<std::int32_t> layer_ys{};
        layer_ys.reserve(chunk.sections.size() + 2);
        for (const auto &section : chunk.sections) {
            layer_ys.push_back(static_cast<std::int32_t>(section.y));
        }
        if (layer_ys.front() > chunk.y_pos - 1) {
            layer_ys.insert(layer_ys.begin(), chunk.y_pos - 1);
        }
        if (layer_ys.back() < top_section + 1) {
            layer_ys.push_back(top_section + 1);
        }
        chunk.storage.light_sections.clear();
        chunk.storage.light_sections.reserve(layer_ys.size());
        for (const auto y : layer_ys) {
            ::refrain::storage::light_section layer{};
            layer.y = static_cast<std::int8_t>(y);
            layer.sky_light.assign(light_bytes, std::byte{0});
            layer.block_light.assign(light_bytes, std::byte{0});
            chunk.storage.light_sections.push_back(std::move(layer));
        }
        column.top_layer_y = top_section + 1;
        column.layers.clear();
        column.layers.reserve(chunk.storage.light_sections.size());
        for (auto &layer : chunk.storage.light_sections) {
            column.layers.push_back(
                column_state::layer_ref{.y = layer.y, .layer = &layer});
        }
        return true;
    }

    /// Scan one column of blocks from the top down and return the Y of its
    /// lowest source cell, or `block_top + 1` when no cell of the column sees
    /// the sky.
    [[nodiscard]] auto compute_floor(const column_state &column,
                                     std::int32_t local_x, std::int32_t local_z)
        -> std::int32_t {
        const auto &chunk{*column.record};
        const auto &sections{chunk.sections};
        for (std::size_t index{sections.size()}; index-- > 0;) {
            const auto &section{sections[index]};
            const auto base_y{static_cast<std::int32_t>(section.y) *
                              section_edge};
            if (section.uniform) {
                if (property(section.value).dampening != 0) {
                    return base_y + section_edge;
                }
                continue;
            }
            for (auto local_y{section_edge - 1}; local_y >= 0; --local_y) {
                const auto id{section.blocks[static_cast<std::size_t>(
                    (local_y << 8) | (local_z << 4) | local_x)]};
                if (property(id).dampening != 0) {
                    return base_y + local_y + 1;
                }
            }
        }
        return block_bottom(column);
    }

    void refresh_floors(column_state &column) {
        for (auto z{0}; z < section_edge; ++z) {
            for (auto x{0}; x < section_edge; ++x) {
                column.lowest_source[static_cast<std::size_t>((z << 4) | x)] =
                    compute_floor(column, x, z);
            }
        }
    }

    [[nodiscard]] auto floor_at(const column_state &column,
                                std::int32_t local_x,
                                std::int32_t local_z) const -> std::int32_t {
        return column
            .lowest_source[static_cast<std::size_t>((local_z << 4) | local_x)];
    }

    [[nodiscard]] auto block_node(const column_state &column,
                                  std::int32_t local_x, std::int32_t y,
                                  std::int32_t local_z) const -> node_pos {
        return node_pos{.x = column.record->pos.x * section_edge + local_x,
                        .y = static_cast<std::int16_t>(y),
                        .z = column.record->pos.z * section_edge + local_z};
    }

    /// Fill the sky light source region of one column and enqueue the cells
    /// whose light reaches a non-source neighbour.
    void seed_sky_column(column_state &column, std::int32_t local_x,
                         std::int32_t local_z, std::int32_t floor_y) {
        const auto bottom{block_bottom(column)};
        const auto top{block_top(column)};
        const auto lowest{std::max(floor_y, bottom)};
        if (lowest <= top) {
            for (auto y{top}; y >= lowest; --y) {
                set_level(block_node(column, local_x, y, local_z),
                          light_layer::sky, max_light_level);
            }
            enqueue_increase(light_layer::sky,
                             block_node(column, local_x, lowest, local_z),
                             max_light_level);
        }
        constexpr std::array<std::pair<std::int32_t, std::int32_t>, 4> offsets{{
            {-1, 0},
            {1, 0},
            {0, -1},
            {0, 1},
        }};
        for (const auto &[dx, dz] : offsets) {
            const auto neighbour_x{local_x + dx};
            const auto neighbour_z{local_z + dz};
            if (neighbour_x < 0 || neighbour_x >= section_edge ||
                neighbour_z < 0 || neighbour_z >= section_edge) {
                continue;
            }
            const auto neighbour_floor{
                floor_at(column, neighbour_x, neighbour_z)};
            if (neighbour_floor >= floor_y) {
                continue;
            }
            for (auto y{std::max(floor_y - 1, bottom)};
                 y >= std::max(neighbour_floor, bottom); --y) {
                const auto node{block_node(column, local_x, y, local_z)};
                const auto candidate{static_cast<std::int32_t>(
                    max_light_level - opacity(node.x, node.y, node.z))};
                if (candidate > 0) {
                    enqueue_increase(light_layer::sky, node,
                                     static_cast<std::uint8_t>(candidate));
                }
            }
        }
    }

    /// Enqueue this column's sky sources that face a non-source cell of an
    /// already loaded neighbour. Without this a column loaded after its
    /// neighbour would never push its own sky light across the border.
    void seed_cross_border_sources(column_state &column) {
        const auto top{block_top(column)};
        const auto bottom{block_bottom(column)};
        const auto base_x{column.record->pos.x * section_edge};
        const auto base_z{column.record->pos.z * section_edge};
        const std::array<std::pair<std::int32_t, std::int32_t>, 4> sides{{
            {-1, 0},
            {1, 0},
            {0, -1},
            {0, 1},
        }};
        for (const auto &[side_x, side_z] : sides) {
            for (auto lateral{0}; lateral < section_edge; ++lateral) {
                const auto local_x{side_x < 0   ? 0
                                   : side_x > 0 ? section_edge - 1
                                                : lateral};
                const auto local_z{side_z < 0   ? 0
                                   : side_z > 0 ? section_edge - 1
                                                : lateral};
                const auto world_x{base_x + local_x};
                const auto world_z{base_z + local_z};
                auto *neighbour{find_column(section_coord(world_x + side_x),
                                            section_coord(world_z + side_z))};
                if (neighbour == nullptr) {
                    continue;
                }
                const auto neighbour_x{local_coord(world_x + side_x)};
                const auto neighbour_z{local_coord(world_z + side_z)};
                const auto our_floor{floor_at(column, local_x, local_z)};
                const auto neighbour_floor{
                    floor_at(*neighbour, neighbour_x, neighbour_z)};
                if (our_floor >= neighbour_floor) {
                    continue;
                }
                const auto first{std::max(our_floor, bottom)};
                const auto last{std::min(neighbour_floor - 1, top)};
                for (auto y{first}; y <= last; ++y) {
                    enqueue_increase(light_layer::sky,
                                     block_node(column, local_x, y, local_z),
                                     max_light_level);
                }
            }
        }
    }

    /// Enqueue the light the neighbouring columns carry into this column
    /// across a border.
    void seed_border_light(column_state &column, light_layer which) {
        const auto bottom{block_bottom(column)};
        const auto top{block_top(column)};
        const auto base_x{column.record->pos.x * section_edge};
        const auto base_z{column.record->pos.z * section_edge};
        for (auto y{top}; y >= bottom; --y) {
            for (auto lateral{0}; lateral < section_edge; ++lateral) {
                const std::array<node_pos, 4> inside{{
                    {.x = base_x,
                     .y = static_cast<std::int16_t>(y),
                     .z = base_z + lateral},
                    {.x = base_x + section_edge - 1,
                     .y = static_cast<std::int16_t>(y),
                     .z = base_z + lateral},
                    {.x = base_x + lateral,
                     .y = static_cast<std::int16_t>(y),
                     .z = base_z},
                    {.x = base_x + lateral,
                     .y = static_cast<std::int16_t>(y),
                     .z = base_z + section_edge - 1},
                }};
                const std::array<node_pos, 4> outside{{
                    {.x = inside[0].x - 1, .y = inside[0].y, .z = inside[0].z},
                    {.x = inside[1].x + 1, .y = inside[1].y, .z = inside[1].z},
                    {.x = inside[2].x, .y = inside[2].y, .z = inside[2].z - 1},
                    {.x = inside[3].x, .y = inside[3].y, .z = inside[3].z + 1},
                }};
                for (std::size_t side{0}; side < inside.size(); ++side) {
                    if (find_column(section_coord(outside[side].x),
                                    section_coord(outside[side].z)) ==
                        nullptr) {
                        continue;
                    }
                    const auto outside_level{level_at(outside[side], which)};
                    if (outside_level == 0) {
                        continue;
                    }
                    const auto &node{inside[side]};
                    const auto candidate{static_cast<std::int32_t>(
                        outside_level - opacity(node.x, node.y, node.z))};
                    if (candidate > 0) {
                        enqueue_increase(which, node,
                                         static_cast<std::uint8_t>(candidate));
                    }
                }
            }
        }
    }

    /// Place every emissive block of the column into the increase queue.
    void seed_block_sources(column_state &column) {
        const auto &chunk{*column.record};
        const auto base_x{chunk.pos.x * section_edge};
        const auto base_z{chunk.pos.z * section_edge};
        for (const auto &section : chunk.sections) {
            const auto base_y{static_cast<std::int32_t>(section.y) *
                              section_edge};
            if (section.uniform) {
                const auto emission{property(section.value).emission};
                if (emission == 0) {
                    continue;
                }
                for (auto local_y{0}; local_y < section_edge; ++local_y) {
                    for (auto local_z{0}; local_z < section_edge; ++local_z) {
                        for (auto local_x{0}; local_x < section_edge;
                             ++local_x) {
                            queues_[layer_index(light_layer::block)]
                                .increase.push_back(increase_entry{
                                    .node =
                                        node_pos{.x = base_x + local_x,
                                                 .y = static_cast<std::int16_t>(
                                                     base_y + local_y),
                                                 .z = base_z + local_z},
                                    .level = emission});
                        }
                    }
                }
                continue;
            }
            for (auto local_y{0}; local_y < section_edge; ++local_y) {
                for (auto local_z{0}; local_z < section_edge; ++local_z) {
                    for (auto local_x{0}; local_x < section_edge; ++local_x) {
                        const auto id{section.blocks[local_index(
                            local_y, local_z, local_x)]};
                        const auto emission{property(id).emission};
                        if (emission == 0) {
                            continue;
                        }
                        queues_[layer_index(light_layer::block)]
                            .increase.push_back(increase_entry{
                                .node = node_pos{.x = base_x + local_x,
                                                 .y = static_cast<std::int16_t>(
                                                     base_y + local_y),
                                                 .z = base_z + local_z},
                                .level = emission});
                    }
                }
            }
        }
    }

    void propagate_increase(light_layer which, const node_pos &node,
                            std::uint8_t level) {
        if (level > level_at(node, which)) {
            set_level(node, which, level);
        }
        for (const auto &step : directions) {
            const node_pos neighbour{
                .x = node.x + step.dx,
                .y = static_cast<std::int16_t>(node.y + step.dy),
                .z = node.z + step.dz};
            auto *neighbour_column{find_column(section_coord(neighbour.x),
                                               section_coord(neighbour.z))};
            if (neighbour_column == nullptr ||
                neighbour.y < block_bottom(*neighbour_column) ||
                neighbour.y > block_top(*neighbour_column) ||
                neighbour_column->layer(section_coord(neighbour.y)) ==
                    nullptr) {
                continue;
            }
            const auto cost{opacity(neighbour.x, neighbour.y, neighbour.z)};
            if (level <= cost) {
                continue;
            }
            const auto candidate{static_cast<std::uint8_t>(level - cost)};
            if (candidate > level_at(neighbour, which)) {
                set_level(neighbour, which, candidate);
                queues_[layer_index(which)].increase.push_back(
                    increase_entry{.node = neighbour, .level = candidate});
            }
        }
    }

    [[nodiscard]] auto intrinsic_level(const node_pos &node, light_layer which)
        -> std::uint8_t {
        if (which == light_layer::block) {
            return emission(node.x, node.y, node.z);
        }
        auto *column{find_column(section_coord(node.x), section_coord(node.z))};
        if (column == nullptr) {
            return 0;
        }
        return node.y >= floor_at(*column, local_coord(node.x),
                                  local_coord(node.z))
                   ? max_light_level
                   : 0;
    }

    void propagate_decrease(light_layer which, const node_pos &node,
                            std::uint8_t old_level) {
        for (const auto &step : directions) {
            const node_pos neighbour{
                .x = node.x + step.dx,
                .y = static_cast<std::int16_t>(node.y + step.dy),
                .z = node.z + step.dz};
            auto *neighbour_column{find_column(section_coord(neighbour.x),
                                               section_coord(neighbour.z))};
            if (neighbour_column == nullptr ||
                neighbour.y < block_bottom(*neighbour_column) ||
                neighbour.y > block_top(*neighbour_column) ||
                neighbour_column->layer(section_coord(neighbour.y)) ==
                    nullptr) {
                continue;
            }
            const auto neighbour_level{level_at(neighbour, which)};
            if (neighbour_level == 0) {
                continue;
            }
            const auto cost{opacity(neighbour.x, neighbour.y, neighbour.z)};
            if (old_level <= cost || neighbour_level <= old_level - cost) {
                set_level(neighbour, which, 0);
                enqueue_decrease(which, neighbour, neighbour_level);
                const auto intrinsic{intrinsic_level(neighbour, which)};
                if (intrinsic > 0) {
                    enqueue_increase(which, neighbour, intrinsic);
                }
            } else {
                // The neighbour keeps light from another source: let it
                // re-propagate into the cells this decrease cleared.
                enqueue_increase(which, neighbour, neighbour_level);
            }
        }
    }

    void apply_floor_change(column_state &column, const node_pos &node,
                            std::int32_t new_floor) {
        auto &floor{column.lowest_source[static_cast<std::size_t>(
            (local_coord(node.z) << 4) | local_coord(node.x))]};
        if (new_floor == floor) {
            return;
        }
        const auto bottom{block_bottom(column)};
        const auto top{block_top(column)};
        if (new_floor > floor) {
            const auto last{std::min(new_floor - 1, top)};
            for (auto y{std::max(floor, bottom)}; y <= last; ++y) {
                const node_pos cell{.x = node.x,
                                    .y = static_cast<std::int16_t>(y),
                                    .z = node.z};
                const auto level{level_at(cell, light_layer::sky)};
                if (level == 0) {
                    continue;
                }
                set_level(cell, light_layer::sky, 0);
                enqueue_decrease(light_layer::sky, cell, level);
            }
        } else {
            const auto last{std::min(floor - 1, top)};
            for (auto y{std::max(new_floor, bottom)}; y <= last; ++y) {
                const node_pos cell{.x = node.x,
                                    .y = static_cast<std::int16_t>(y),
                                    .z = node.z};
                set_level(cell, light_layer::sky, max_light_level);
                enqueue_increase(light_layer::sky, cell, max_light_level);
            }
        }
        floor = new_floor;
    }

    void check_node(const node_pos &node) {
        auto *column{find_column(section_coord(node.x), section_coord(node.z))};
        if (column == nullptr) {
            return;
        }
        if (column->section_count != column->record->sections.size()) {
            rebuild_section_lookup(*column);
        }
        const auto new_floor{
            compute_floor(*column, local_coord(node.x), local_coord(node.z))};
        apply_floor_change(*column, node, new_floor);
        for (const auto which : {light_layer::sky, light_layer::block}) {
            const auto old_level{level_at(node, which)};
            const auto intrinsic{intrinsic_level(node, which)};
            if (old_level != 0) {
                set_level(node, which, 0);
                enqueue_decrease(which, node, old_level);
            }
            if (intrinsic > 0) {
                enqueue_increase(which, node, intrinsic);
            }
            if (old_level == 0 && intrinsic == 0) {
                // The cell may admit light it blocked before.
                for (const auto &step : directions) {
                    const node_pos neighbour{
                        .x = node.x + step.dx,
                        .y = static_cast<std::int16_t>(node.y + step.dy),
                        .z = node.z + step.dz};
                    const auto level{level_at(neighbour, which)};
                    if (level > 0) {
                        enqueue_increase(which, neighbour, level);
                    }
                }
            }
        }
    }

    void initial_light(column_state &column) {
        refresh_floors(column);
        for (auto local_z{0}; local_z < section_edge; ++local_z) {
            for (auto local_x{0}; local_x < section_edge; ++local_x) {
                seed_sky_column(column, local_x, local_z,
                                floor_at(column, local_x, local_z));
            }
        }
        seed_cross_border_sources(column);
        seed_block_sources(column);
        seed_border_light(column, light_layer::sky);
        seed_border_light(column, light_layer::block);
    }

    [[nodiscard]] auto pending_nodes() const noexcept -> std::size_t {
        auto total{check_queue_.size()};
        for (const auto &queue : queues_) {
            total += queue.decrease.size() + queue.increase.size();
        }
        return total;
    }

    /// Process every queued node, decrease and increase pass in layer order.
    /// Returns the number of entries processed.
    auto drain(std::size_t budget) -> std::size_t {
        std::size_t processed{0};
        while (!check_queue_.empty() && processed < budget) {
            const auto node{check_queue_.front()};
            check_queue_.pop_front();
            check_node(node);
            ++processed;
        }
        for (std::size_t index{0}; index < layer_count; ++index) {
            const auto which{index == 0 ? light_layer::sky
                                        : light_layer::block};
            auto &queue{queues_[index].decrease};
            while (!queue.empty() && processed < budget) {
                const auto entry{queue.front()};
                queue.pop_front();
                propagate_decrease(which, entry.node, entry.level);
                ++processed;
            }
        }
        for (std::size_t index{0}; index < layer_count; ++index) {
            const auto which{index == 0 ? light_layer::sky
                                        : light_layer::block};
            auto &queue{queues_[index].increase};
            while (!queue.empty() && processed < budget) {
                const auto entry{queue.front()};
                queue.pop_front();
                propagate_increase(which, entry.node, entry.level);
                ++processed;
            }
        }
        return processed;
    }
};

light_engine::light_engine(world &owner)
    : impl_{std::make_unique<impl>(owner)} {}

light_engine::light_engine(light_engine &&other) noexcept = default;

auto light_engine::operator=(light_engine &&other) noexcept
    -> light_engine & = default;

light_engine::~light_engine() = default;

auto block_light_property_for(std::string_view key) -> block_light_property {
    return light_property_for(key);
}

void light_engine::attach(world &owner) {
    impl_->owner_ = &owner;
    impl_->columns_.clear();
    impl_->properties_.clear();
    impl_->clear_cache();
    for (const auto &pos : owner.chunk_positions()) {
        auto *column{owner.find_chunk(pos)};
        if (column == nullptr) {
            continue;
        }
        column_state state{};
        state.record = column;
        state.top_layer_y = column->sections.empty()
                                ? column->y_pos
                                : column->sections.back().y + 1;
        for (auto &layer : column->storage.light_sections) {
            state.layers.push_back(
                column_state::layer_ref{.y = layer.y, .layer = &layer});
        }
        impl_->rebuild_section_lookup(state);
        state.section_count =
            static_cast<std::uint16_t>(column->sections.size());
        impl_->refresh_floors(state);
        impl_->columns_.emplace(pos, std::move(state));
    }
}

void light_engine::column_loaded(chunk &column) {
    impl_->clear_cache();
    auto &state{impl_->columns_[column.pos]};
    state.record = &column;
    if (!impl_->reset_layers(state)) {
        impl_->rebuild_section_lookup(state);
        return;
    }
    impl_->rebuild_section_lookup(state);
    impl_->suppressed_origin_ = column.pos;
    impl_->initial_light(state);
    impl_->suppressed_origin_.reset();
    // The column's sections travel in its chunk packet; only light that
    // reached a neighbour column needs a separate update.
    static_cast<void>(impl_->drain(std::numeric_limits<std::size_t>::max()));
}

void light_engine::column_unloaded(const chunk_pos &pos) {
    impl_->columns_.erase(pos);
    impl_->clear_cache();
}

void light_engine::block_changed(const block_pos &pos) {
    // A block edit can add a section above the current top (a sparse imported
    // column). The light layer range follows the section range, so the column
    // is rebuilt instead of extending the index in place.
    auto *column{
        impl_->find_column(section_coord(pos.x), section_coord(pos.z))};
    if (column != nullptr && !column->record->sections.empty() &&
        static_cast<std::int32_t>(column->record->sections.back().y) + 1 !=
            column->top_layer_y) {
        const auto changed_pos{column->record->pos};
        const auto changed_top{
            static_cast<std::int32_t>(column->record->sections.back().y) + 1};
        column_loaded(*column->record);
        // The whole column was relit: clients holding it need every section.
        for (auto y{column->record->y_pos - 1}; y <= changed_top; ++y) {
            impl_->changes_.insert(light_section_change{
                .pos = changed_pos, .section_y = static_cast<std::int8_t>(y)});
        }
        return;
    }
    impl_->check_queue_.push_back(node_pos{.x = pos.x, .y = pos.y, .z = pos.z});
}

auto light_engine::run(std::size_t budget) -> std::size_t {
    return impl_->drain(budget);
}

auto light_engine::pending() const noexcept -> bool {
    return !impl_->check_queue_.empty() ||
           !impl_->queues_[0].decrease.empty() ||
           !impl_->queues_[1].decrease.empty() ||
           !impl_->queues_[0].increase.empty() ||
           !impl_->queues_[1].increase.empty();
}

auto light_engine::pending_nodes() const noexcept -> std::size_t {
    return impl_->pending_nodes();
}

auto light_engine::take_changes() -> std::vector<light_section_change> {
    std::vector<light_section_change> changes{impl_->changes_.begin(),
                                              impl_->changes_.end()};
    impl_->changes_.clear();
    return changes;
}

auto light_engine::sky_light_at(const block_pos &pos) const -> std::uint8_t {
    return impl_->level_at(node_pos{.x = pos.x, .y = pos.y, .z = pos.z},
                           light_layer::sky);
}

auto light_engine::block_light_at(const block_pos &pos) const -> std::uint8_t {
    return impl_->level_at(node_pos{.x = pos.x, .y = pos.y, .z = pos.z},
                           light_layer::block);
}

} // namespace refrain::sim
