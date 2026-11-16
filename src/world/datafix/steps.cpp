// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The first fix table of the shared world DataFixer.
//
// Both steps were read from the reference game's DataFixers registration and
// its fix classes; the comments record the reference names so a later table
// extraction can diff against them. They are deliberately written as generic
// NBT walks: a record retrieved at an older data version can nest item stacks
// in inventories, block-entity lists, entity lists or component maps, and the
// reference fixes match on the item stack shape, not on where it appears.
//
// The walk is lenient the way the reference fixes are lenient about absent
// optional fields: a compound without the touched shape is left alone. It never
// guesses a value, drops an unknown field or reorders anything (compounds are
// key-sorted maps, so the encoding is canonical already).

#include "registry.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::world::datafix_detail {
namespace {

namespace nbt = data::nbt;

/// Visit every compound of an NBT tree: the root, nested compounds and the
/// compounds inside lists. `visit` returns true when it changed its node.
///
/// The child keys are copied before recursing because a visitor may erase a key
/// from the compound it was handed; the list index stays valid because a
/// visitor cannot insert or remove list items.
template <typename Visit>
[[nodiscard]] auto walk_compounds(nbt::value &node, Visit &visit) -> bool {
    bool changed{false};
    if (auto *compound{node.as_compound()}; compound != nullptr) {
        changed = visit(*compound);
        std::vector<std::string> keys;
        keys.reserve(compound->size());
        for (const auto &[key, child] : *compound) {
            static_cast<void>(child);
            keys.push_back(key);
        }
        for (const auto &key : keys) {
            if (auto *child{node.get(key)}; child != nullptr) {
                changed = walk_compounds(*child, visit) || changed;
            }
        }
        return changed;
    }
    if (auto *list{node.as_list()}; list != nullptr) {
        for (auto &item : list->items) {
            changed = walk_compounds(item, visit) || changed;
        }
    }
    return changed;
}

/// Reference `RemoveBlockTransformerComponentFix` (schema 5014): drop the
/// retired `minecraft:block_transformer` component from every item stack
/// component map. The reference fix rewrites the whole component map to its
/// remainder without that key; removing the single key is equivalent because
/// the component map is an open compound.
[[nodiscard]] auto remove_block_transformer(nbt::compound &node) -> bool {
    const auto components{node.find("components")};
    if (components == node.end()) {
        return false;
    }
    auto *map{components->second.as_compound()};
    if (map == nullptr) {
        return false;
    }
    return map->erase("minecraft:block_transformer") > 0;
}

/// Reference `UnfilledBuriedTreasureMapFix` (schema 5016): rename the legacy
/// buried-treasure map item name. The reference fix only touches
/// `minecraft:map` stacks whose item name is a plain translation component
/// carrying the old key; a map with any other name shape is left alone.
[[nodiscard]] auto rename_buried_treasure_map(nbt::compound &node) -> bool {
    const auto id{node.find("id")};
    if (id == node.end()) {
        return false;
    }
    const auto item_id{id->second.as_string()};
    if (!item_id || *item_id != "minecraft:map") {
        return false;
    }
    const auto components{node.find("components")};
    if (components == node.end()) {
        return false;
    }
    auto *map{components->second.as_compound()};
    if (map == nullptr) {
        return false;
    }
    const auto name{map->find("minecraft:item_name")};
    if (name == map->end()) {
        return false;
    }
    auto *component{name->second.as_compound()};
    // A plain translation component has exactly one field, `translate`.
    if (component == nullptr || component->size() != 1) {
        return false;
    }
    const auto translate{component->find("translate")};
    if (translate == component->end()) {
        return false;
    }
    const auto key{translate->second.as_string()};
    if (!key || *key != "filled_map.buried_treasure") {
        return false;
    }
    translate->second = nbt::make_string("item.minecraft.buried_treasure_map");
    return true;
}

[[nodiscard]] auto remove_block_transformer_document(nbt::value &document)
    -> bool {
    auto visit{&remove_block_transformer};
    return walk_compounds(document, visit);
}

[[nodiscard]] auto rename_buried_treasure_map_document(nbt::value &document)
    -> bool {
    auto visit{&rename_buried_treasure_map};
    return walk_compounds(document, visit);
}

// Sorted by schema version; `fix_document` relies on that order.
//
// TODO: generate the fix table from the reference datafixer's own before/after
// output (a development-time extraction against the user's jar) before the
// source window is widened. These two entries are transcribed from the
// reference fix classes and cover only the item-component changes of the
// current window.
constexpr fix_step steps[]{
    {
        .schema_version = 5014,
        .types = item_record_types,
        .name = "RemoveBlockTransformerComponentFix",
        .apply = &remove_block_transformer_document,
    },
    {
        .schema_version = 5016,
        .types = item_record_types,
        .name = "UnfilledBuriedTreasureMapFix",
        .apply = &rename_buried_treasure_map_document,
    },
};

} // namespace

auto fix_steps() noexcept -> std::span<const fix_step> { return steps; }

} // namespace refrain::world::datafix_detail
