// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/data/vanilla.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace refrain::sim {

/// Stack size an item has when the item registry does not name one, and the
/// absolute ceiling the protocol allows for any item.
inline constexpr std::uint32_t default_stack_size =
    data::default_item_stack_size;
inline constexpr std::uint32_t absolute_stack_size = 99;

/// Per-item facts the runtime needs from the item registry: how many items one
/// stack holds and how much damage it absorbs. `max_damage` is zero for an
/// item that cannot be damaged.
struct item_definition {
    std::uint16_t max_stack_size{
        static_cast<std::uint16_t>(default_stack_size)};
    std::uint32_t max_damage{0};
};

/// Immutable view of the item registry data, keyed by the runtime item name.
///
/// The table is built once at startup from the vanilla tables and then read
/// from the simulation thread. A lookup for an item the table does not know
/// falls back to `default_stack_size` and an indestructible item, so a missing
/// or partial registry degrades to the previous global limit instead of
/// rejecting stacks.
class item_table {
  public:
    item_table() = default;

    /// Copy the per-item records out of an opened vanilla table container.
    [[nodiscard]] static auto from_tables(const data::vanilla_tables &tables)
        -> item_table;

    /// Add or replace one record. The runtime fills the table from the
    /// vanilla container; a synthetic table uses the same entry point.
    void insert(std::string name, item_definition definition);

    [[nodiscard]] auto find(std::string_view item) const noexcept
        -> const item_definition *;
    [[nodiscard]] auto stack_limit(std::string_view item) const noexcept
        -> std::uint32_t;
    [[nodiscard]] auto max_damage(std::string_view item) const noexcept
        -> std::uint32_t;
    [[nodiscard]] auto size() const noexcept -> std::size_t {
        return items_.size();
    }
    [[nodiscard]] auto empty() const noexcept -> bool { return items_.empty(); }

  private:
    std::map<std::string, item_definition, std::less<>> items_{};
};

/// The component kinds this server models. The numeric value is the stable
/// component id: the stack component map is ordered by it, so hashing, saving
/// and encoding iterate the same order on every run. Wire ids are resolved
/// from the registry at the protocol boundary, not here.
enum class item_component_kind : std::uint8_t {
    damage = 0,
    custom_name = 1,
    enchantments = 2,
};

/// One enchantment and its level. Entries are kept sorted by enchantment name
/// so the component has one representation.
struct item_enchantment {
    std::string enchantment{};
    std::uint32_t level{0};

    friend auto operator==(const item_enchantment &, const item_enchantment &)
        -> bool = default;
};

/// Durability already lost by the stack.
struct component_damage {
    std::uint32_t amount{0};

    friend auto operator==(const component_damage &, const component_damage &)
        -> bool = default;
};

/// Replacement display name.
struct component_custom_name {
    std::string text{};

    friend auto operator==(const component_custom_name &,
                           const component_custom_name &) -> bool = default;
};

/// Enchantments carried by the stack itself.
struct component_enchantments {
    std::vector<item_enchantment> entries{};

    friend auto operator==(const component_enchantments &,
                           const component_enchantments &) -> bool = default;
};

using item_component = std::variant<component_damage, component_custom_name,
                                    component_enchantments>;

/// One inventory stack: an item key, a count and the data components that
/// differ from the item's own defaults.
///
/// The component map is ordered by component id and only holds components the
/// stack overrides. Durability lives in the `damage` component, so two stacks
/// merge only when their components match exactly.
struct item_stack {
    std::string item{};
    std::uint32_t count{0};
    std::map<item_component_kind, item_component> components{};

    friend auto operator==(const item_stack &, const item_stack &)
        -> bool = default;
};

/// Empty when the count is zero or the item key is empty.
[[nodiscard]] auto stack_empty(const item_stack &stack) noexcept -> bool;

/// True when both stacks name the same item and carry equal components, which
/// is what the merge rules require.
[[nodiscard]] auto same_item(const item_stack &left,
                             const item_stack &right) noexcept -> bool;

/// Damage recorded in the stack, zero when it has no damage component.
[[nodiscard]] auto stack_damage(const item_stack &stack) noexcept
    -> std::uint32_t;

/// Enchantments of the stack, or nullptr when it carries none.
[[nodiscard]] auto stack_enchantments(const item_stack &stack) noexcept
    -> const std::vector<item_enchantment> *;

/// Custom name of the stack, or nullptr when it carries none.
[[nodiscard]] auto stack_custom_name(const item_stack &stack) noexcept
    -> const std::string *;

/// Durability the item absorbs; zero when the registry does not know the item
/// or the item cannot be damaged.
[[nodiscard]] auto stack_max_damage(const item_stack &stack,
                                    const item_table &table) noexcept
    -> std::uint32_t;

/// Set the damage component; zero removes it.
void set_stack_damage(item_stack &stack, std::uint32_t amount) noexcept;

/// Replace the enchantments; the entries are sorted by name and an empty list
/// removes the component. A level outside the protocol range 1..255 is
/// rejected and leaves the stack unchanged.
[[nodiscard]] auto set_stack_enchantments(item_stack &stack,
                                          std::vector<item_enchantment> entries)
    -> status;

/// Replace the custom name; an empty name removes the component.
void set_stack_custom_name(item_stack &stack, std::string text);

/// What one use did to a stack.
enum class damage_result : std::uint8_t {
    unchanged,
    damaged,
    broken,
};

/// Apply `amount` durability points to `stack`.
///
/// A stack of an item without max damage, an empty stack and `amount == 0`
/// are unchanged. A stack whose damage reaches its item's max damage is
/// destroyed: the count drops to zero and the components are cleared.
[[nodiscard]] auto damage_stack(item_stack &stack, const item_table &table,
                                std::uint32_t amount = 1) -> damage_result;

} // namespace refrain::sim
