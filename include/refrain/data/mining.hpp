// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Read-only block mining and tool tables.
//
// The mining data of a content version is extracted from the server jar into
// the tracked block mining report and compiled into the `MINE` and `TOOL`
// sections of the vanilla table container. This header turns those sections
// into immutable lookup tables: one record per block and one record per item
// that carries a tool component.
//
// A record the table does not carry - a modded block, or a container imported
// without the block mining report - falls back to the documented defaults: a
// block is unbreakable and an item has no tool component. The fallback is never
// guessed from a block or item name.

#pragma once

#include <refrain/data/vanilla.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::data {

/// Mining behaviour of one block.
struct block_mining {
    /// Destroy time of the block; `-1` means unbreakable.
    float hardness{-1.0F};
    /// Mining speed of shears on the block.
    float shears_speed{1.0F};
    /// Mining speed of a sword on the block.
    float sword_speed{1.0F};
    /// `block_mining_*` flag bits.
    std::uint8_t flags{0};
    /// Tool families that mine the block efficiently.
    std::uint8_t tool_mask{0};
    /// Lowest tool-material harvest level that yields drops.
    std::uint8_t needed_tier{0};
    /// Tool families that yield drops without a tier threshold.
    std::uint8_t direct_correct{0};

    [[nodiscard]] auto requires_correct_tool_for_drops() const noexcept
        -> bool {
        return (flags & block_mining_requires_correct_tool) != 0;
    }
    [[nodiscard]] auto is_air() const noexcept -> bool {
        return (flags & block_mining_air) != 0;
    }
    [[nodiscard]] auto is_fluid() const noexcept -> bool {
        return (flags & block_mining_fluid) != 0;
    }
    [[nodiscard]] auto unbreakable() const noexcept -> bool {
        return hardness < 0.0F;
    }
};

/// One item that carries a tool component.
struct tool_mining {
    /// Runtime item name.
    std::string name{};
    tool_type type{tool_type::none};
    tool_material material{tool_material::none};
    /// Harvest level of `material`, or `harvest_level_none` without tiered
    /// rules.
    std::uint8_t harvest_level{harvest_level_none};
    /// Mining speed of a tiered family on the blocks it mines efficiently.
    float tier_speed{1.0F};
    float default_mining_speed{1.0F};
    std::uint16_t damage_per_block{0};
    std::uint16_t max_stack_size{default_item_stack_size};
    std::uint32_t max_damage{0};
    bool can_destroy_blocks_in_creative{true};
};

/// Mining table of one content version: dense block id and block name to the
/// block's mining record.
///
/// `from_tables` copies the records out of an opened vanilla table container;
/// tests fill one directly. The table is immutable after construction.
class block_mining_table {
  public:
    block_mining_table() = default;

    /// Copy the block mining records of an opened vanilla table. A container
    /// without mining sections produces an empty table.
    [[nodiscard]] static auto from_tables(const vanilla_tables &tables)
        -> block_mining_table;

    /// Add or replace one record under a block name. A synthetic table that
    /// only looks blocks up by name uses this entry point; the record is not
    /// visible to `at`.
    void insert(std::string name, block_mining mining);
    /// Add or replace one record under a block name and dense block id. The
    /// runtime fills the table from the vanilla container through this entry
    /// point.
    void insert(std::uint16_t block, std::string name, block_mining mining);

    /// Record of one block name, or null when the table does not carry it.
    [[nodiscard]] auto find(std::string_view block) const noexcept
        -> const block_mining *;
    /// Record of one dense block id, or null when the table does not carry
    /// one. The id is the container's block order.
    [[nodiscard]] auto at(std::uint16_t block) const noexcept
        -> const block_mining *;

    /// The documented fallback: a block the table does not carry is
    /// unbreakable rather than mined with a guessed speed.
    [[nodiscard]] static auto missing() noexcept -> const block_mining &;

    [[nodiscard]] auto size() const noexcept -> std::size_t {
        return blocks_.size();
    }
    [[nodiscard]] auto empty() const noexcept -> bool {
        return blocks_.empty();
    }

  private:
    std::map<std::string, block_mining, std::less<>> blocks_{};
    std::vector<const block_mining *> by_id_{};
};

/// Tool table of one content version: runtime item name to the item's tool
/// record.
///
/// The table only carries items whose registry data has a tool component;
/// every other item mines at the bare-hand speed and yields no drop answer.
class item_tool_table {
  public:
    item_tool_table() = default;

    /// Copy the tool records of an opened vanilla table. A container without
    /// mining sections produces an empty table.
    [[nodiscard]] static auto from_tables(const vanilla_tables &tables)
        -> item_tool_table;

    void insert(std::string name, tool_mining tool);

    [[nodiscard]] auto find(std::string_view item) const noexcept
        -> const tool_mining *;

    /// The documented fallback: no tool component, bare-hand mining speed and
    /// no drop answer.
    [[nodiscard]] static auto missing() noexcept -> const tool_mining &;

    [[nodiscard]] auto size() const noexcept -> std::size_t {
        return tools_.size();
    }
    [[nodiscard]] auto empty() const noexcept -> bool { return tools_.empty(); }

  private:
    std::map<std::string, tool_mining, std::less<>> tools_{};
};

/// Mining speed of `tool` on `block`, reproducing the reference's
/// `getDestroySpeed` for the compressed record. A tiered family returns its
/// tier speed on the blocks it mines efficiently and the default speed
/// elsewhere; shears and swords return the block's per-family speed; an item
/// without mining rules returns its default speed.
[[nodiscard]] auto mining_speed(const tool_mining &tool,
                                const block_mining &block) -> float;

/// Whether `tool` yields drops on `block`, reproducing the reference's
/// `isCorrectToolForDrops`. A tiered family needs the block's efficient family
/// bit and a harvest level at or above the block's tier; shears and swords use
/// the block's direct-correct bits; an item without mining rules never does.
///
/// The block's own correct-tool requirement decides whether this matters:
/// `requires_correct_tool_for_drops() && !correct_tool_for_drops(tool, block)`
/// leaves the drops behind.
[[nodiscard]] auto correct_tool_for_drops(const tool_mining &tool,
                                          const block_mining &block) -> bool;

} // namespace refrain::data
