// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/mining.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace refrain::data {

auto block_mining_table::from_tables(const vanilla_tables &tables)
    -> block_mining_table {
    block_mining_table table{};
    if (!tables.has_mining()) {
        return table;
    }
    table.by_id_.assign(tables.block_count(), nullptr);
    for (std::uint32_t block = 0; block < tables.block_count(); ++block) {
        const auto view =
            tables.block_mining_at(static_cast<std::uint16_t>(block));
        const auto name =
            tables.block_at(static_cast<std::uint16_t>(block)).name;
        block_mining mining{};
        mining.hardness = view.hardness;
        mining.shears_speed = view.shears_speed;
        mining.sword_speed = view.sword_speed;
        mining.flags = view.flags;
        mining.tool_mask = view.tool_mask;
        mining.needed_tier = view.needed_tier;
        mining.direct_correct = view.direct_correct;
        table.insert(static_cast<std::uint16_t>(block), std::string{name},
                     mining);
    }
    return table;
}

void block_mining_table::insert(std::string name, block_mining mining) {
    blocks_.insert_or_assign(std::move(name), mining);
}

void block_mining_table::insert(std::uint16_t block, std::string name,
                                block_mining mining) {
    if (block >= by_id_.size()) {
        by_id_.resize(static_cast<std::size_t>(block) + 1U, nullptr);
    }
    const auto [position, inserted] =
        blocks_.insert_or_assign(std::move(name), mining);
    by_id_[block] = &position->second;
}

auto block_mining_table::find(std::string_view block) const noexcept
    -> const block_mining * {
    const auto found = blocks_.find(block);
    return found == blocks_.end() ? nullptr : &found->second;
}

auto block_mining_table::at(std::uint16_t block) const noexcept
    -> const block_mining * {
    if (block >= by_id_.size()) {
        return nullptr;
    }
    return by_id_[block];
}

auto block_mining_table::missing() noexcept -> const block_mining & {
    static const block_mining missing{};
    return missing;
}

auto item_tool_table::from_tables(const vanilla_tables &tables)
    -> item_tool_table {
    item_tool_table table{};
    for (std::uint32_t index = 0; index < tables.tool_count(); ++index) {
        const auto view = tables.tool_at(index);
        tool_mining tool{};
        tool.name = std::string{view.name};
        tool.type = view.type;
        tool.material = view.material;
        tool.harvest_level = view.harvest_level;
        tool.tier_speed = view.tier_speed;
        tool.default_mining_speed = view.default_mining_speed;
        tool.damage_per_block = view.damage_per_block;
        tool.max_stack_size = view.max_stack_size;
        tool.max_damage = view.max_damage;
        tool.can_destroy_blocks_in_creative =
            view.can_destroy_blocks_in_creative;
        table.insert(std::string{view.name}, std::move(tool));
    }
    return table;
}

void item_tool_table::insert(std::string name, tool_mining tool) {
    tools_.insert_or_assign(std::move(name), std::move(tool));
}

auto item_tool_table::find(std::string_view item) const noexcept
    -> const tool_mining * {
    const auto found = tools_.find(item);
    return found == tools_.end() ? nullptr : &found->second;
}

auto item_tool_table::missing() noexcept -> const tool_mining & {
    static const tool_mining missing{};
    return missing;
}

auto mining_speed(const tool_mining &tool, const block_mining &block) -> float {
    switch (tool.type) {
    case tool_type::pickaxe:
    case tool_type::axe:
    case tool_type::shovel:
    case tool_type::hoe:
        return (block.tool_mask & tool_type_bit(tool.type)) != 0U
                   ? tool.tier_speed
                   : tool.default_mining_speed;
    case tool_type::shears:
        return block.shears_speed;
    case tool_type::sword:
        return block.sword_speed;
    case tool_type::none:
    default:
        return tool.default_mining_speed;
    }
}

auto correct_tool_for_drops(const tool_mining &tool, const block_mining &block)
    -> bool {
    switch (tool.type) {
    case tool_type::pickaxe:
    case tool_type::axe:
    case tool_type::shovel:
    case tool_type::hoe:
        return (block.tool_mask & tool_type_bit(tool.type)) != 0U &&
               tool.harvest_level >= block.needed_tier;
    case tool_type::shears:
    case tool_type::sword:
        return (block.direct_correct & tool_type_bit(tool.type)) != 0U;
    case tool_type::none:
    default:
        return false;
    }
}

} // namespace refrain::data
