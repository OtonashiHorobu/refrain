// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Shared world DataFixer kernel.
//
// The kernel is the single implementation of the reference schema changes the
// supported window covers: these tests pin the window and its fail-closed
// errors, the per-record-type applicability, the exact NBT the two item fixes
// rewrite, idempotency and determinism. They do not need a world on disk; the
// importer and the RWF migration tests cover the transaction around it.

#include <refrain/core/error.hpp>
#include <refrain/data/nbt.hpp>
#include <refrain/world/datafix.hpp>
#include <refrain/world/version.hpp>

#include <boost/ut.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "support/migration_world.hpp"

namespace {

using namespace boost::ut;

namespace nbt = refrain::data::nbt;
namespace world = refrain::world;

constexpr std::int32_t oldest{5013};
constexpr std::int32_t target{5023};

/// Inventory-holding document (the shape of a player record or a block entity)
/// with one legacy item stack.
[[nodiscard]] auto legacy_inventory_document() -> nbt::value {
    return refrain::test_support::legacy_player_record(oldest);
}

[[nodiscard]] auto encode(const nbt::value &document)
    -> std::vector<std::byte> {
    auto bytes{nbt::write(document, "", nbt::compression::none)};
    expect(static_cast<bool>(bytes));
    return bytes ? std::move(*bytes) : std::vector<std::byte>{};
}

[[nodiscard]] auto first_item(const nbt::value &document)
    -> const nbt::value * {
    const auto *items{document.get("Items")};
    if (items == nullptr) {
        items = document.get("Inventory");
    }
    const auto *list{items != nullptr ? items->as_list() : nullptr};
    if (list == nullptr || list->items.empty()) {
        return nullptr;
    }
    return &list->items.front();
}

[[nodiscard]] auto item_components(const nbt::value &document)
    -> const nbt::compound * {
    const auto *item{first_item(document)};
    const auto *components{item != nullptr ? item->get("components") : nullptr};
    return components != nullptr ? components->as_compound() : nullptr;
}

[[nodiscard]] auto component_translate(const nbt::compound &components)
    -> std::string_view {
    const auto name{components.find("minecraft:item_name")};
    if (name == components.end()) {
        return {};
    }
    const auto *compound{name->second.as_compound()};
    if (compound == nullptr) {
        return {};
    }
    const auto translate{compound->find("translate")};
    if (translate == compound->end()) {
        return {};
    }
    return translate->second.as_string().value_or("");
}

const suite datafix_kernel = [] {
    "datafix/window covers the reference schema steps"_test = [] {
        auto earliest{world::plan_datafix(oldest, target)};
        expect(static_cast<bool>(earliest));
        if (earliest) {
            expect(earliest->from_version == oldest);
            expect(earliest->to_version == target);
            expect(earliest->steps == 2U);
        }
        auto middle{world::plan_datafix(5014, target)};
        expect(static_cast<bool>(middle));
        if (middle) {
            expect(middle->steps == 1U);
        }
        auto recent{world::plan_datafix(5016, target)};
        expect(static_cast<bool>(recent));
        if (recent) {
            expect(recent->steps == 0U);
        }
        auto current{world::plan_datafix(target, target)};
        expect(static_cast<bool>(current));
        if (current) {
            expect(current->steps == 0U);
        }
        expect(world::datafix_step_count(oldest, target) == 2U);
        expect(world::datafix_step_count(5014, target) == 1U);
        expect(world::datafix_step_count(5016, target) == 0U);
        expect(world::oldest_migratable_data_version == oldest);
        expect(world::datafix_target_version == target);
    };

    "datafix/window refuses an out-of-window source naming the window"_test =
        [] {
            auto too_old{world::plan_datafix(5012, target)};
            expect(!too_old);
            if (!too_old) {
                expect(too_old.error().code ==
                       refrain::error_code::unsupported);
                const auto message{too_old.error().message};
                expect(message.find("5012") != std::string::npos);
                expect(message.find("5013") != std::string::npos);
                expect(message.find("5023") != std::string::npos);
                expect(message.find("window") != std::string::npos);
            }
            auto downgrade{world::plan_datafix(5024, 5023)};
            expect(!downgrade);
            if (!downgrade) {
                expect(downgrade.error().code ==
                       refrain::error_code::invalid_argument);
            }
            auto foreign_target{world::plan_datafix(oldest, 5022)};
            expect(!foreign_target);
            if (!foreign_target) {
                expect(foreign_target.error().code ==
                       refrain::error_code::unsupported);
                expect(foreign_target.error().message.find("5022") !=
                       std::string::npos);
            }
        };

    "datafix/record types and names are distinct"_test = [] {
        const std::vector<world::datafix_type> types{
            world::datafix_type::level,
            world::datafix_type::player,
            world::datafix_type::chunk,
            world::datafix_type::entity_chunk,
            world::datafix_type::poi_chunk,
            world::datafix_type::world_gen_settings,
            world::datafix_type::saved_data,
            world::datafix_type::structure,
            world::datafix_type::advancements,
            world::datafix_type::stats,
        };
        for (const auto type : types) {
            expect(!world::datafix_type_name(type).empty());
        }
        expect(world::datafix_type_name(world::datafix_type::player) ==
               "player");
        expect(world::datafix_type_name(world::datafix_type::chunk) == "chunk");
        expect(world::saved_data_type("world_gen_settings.dat") ==
               world::datafix_type::world_gen_settings);
        expect(world::saved_data_type(
                   "global/data/minecraft/world_gen_settings.dat") ==
               world::datafix_type::world_gen_settings);
        expect(world::saved_data_type("scoreboard.dat") ==
               world::datafix_type::saved_data);
    };

    "datafix/fixes the retired component and renames the buried map"_test = [] {
        auto document{legacy_inventory_document()};
        auto fixed{world::fix_document(document, world::datafix_type::player,
                                       oldest, target)};
        expect(static_cast<bool>(fixed));
        if (!fixed) {
            return;
        }
        expect(fixed->changed);
        expect(fixed->fixes_applied == 2U);
        const auto *components{item_components(document)};
        expect(components != nullptr);
        if (components == nullptr) {
            return;
        }
        expect(!components->contains("minecraft:block_transformer"));
        expect(component_translate(*components) ==
               "item.minecraft.buried_treasure_map");
    };

    "datafix/a 5014 record gets only the later fix"_test = [] {
        auto document{legacy_inventory_document()};
        auto fixed{world::fix_document(document, world::datafix_type::player,
                                       5014, target)};
        expect(static_cast<bool>(fixed));
        if (!fixed) {
            return;
        }
        expect(fixed->changed);
        expect(fixed->fixes_applied == 1U);
        const auto *components{item_components(document)};
        expect(components != nullptr);
        if (components == nullptr) {
            return;
        }
        // The component removal happened in schema 5014, before this source.
        expect(components->contains("minecraft:block_transformer"));
        expect(component_translate(*components) ==
               "item.minecraft.buried_treasure_map");
    };

    "datafix/a 5016 record is already fixed"_test = [] {
        auto document{legacy_inventory_document()};
        auto fixed{world::fix_document(document, world::datafix_type::player,
                                       5016, target)};
        expect(static_cast<bool>(fixed));
        if (fixed) {
            expect(!fixed->changed);
            expect(fixed->fixes_applied == 0U);
        }
        const auto *components{item_components(document)};
        expect(components != nullptr);
        if (components != nullptr) {
            expect(components->contains("minecraft:block_transformer"));
            expect(component_translate(*components) ==
                   "filled_map.buried_treasure");
        }
    };

    "datafix/applies only to record types that can carry item stacks"_test =
        [] {
            const auto fixed_types = {
                world::datafix_type::player,
                world::datafix_type::chunk,
                world::datafix_type::entity_chunk,
            };
            for (const auto type : fixed_types) {
                auto document{legacy_inventory_document()};
                auto fixed{world::fix_document(document, type, oldest, target)};
                expect(static_cast<bool>(fixed));
                if (fixed) {
                    expect(fixed->changed) << world::datafix_type_name(type);
                }
            }
            const auto untouched_types = {
                world::datafix_type::level,
                world::datafix_type::poi_chunk,
                world::datafix_type::world_gen_settings,
                world::datafix_type::saved_data,
                world::datafix_type::structure,
                world::datafix_type::advancements,
                world::datafix_type::stats,
            };
            for (const auto type : untouched_types) {
                auto document{legacy_inventory_document()};
                auto fixed{world::fix_document(document, type, oldest, target)};
                expect(static_cast<bool>(fixed));
                if (fixed) {
                    expect(!fixed->changed) << world::datafix_type_name(type);
                    expect(fixed->fixes_applied == 0U);
                }
            }
        };

    "datafix/second run over a fixed record is idempotent"_test = [] {
        auto document{legacy_inventory_document()};
        auto first{world::fix_document(document, world::datafix_type::player,
                                       oldest, target)};
        expect(static_cast<bool>(first));
        if (!first) {
            return;
        }
        const auto after_first{encode(document)};
        auto second{world::fix_document(document, world::datafix_type::player,
                                        oldest, target)};
        expect(static_cast<bool>(second));
        if (second) {
            expect(!second->changed);
            expect(second->fixes_applied == 0U);
        }
        expect(static_cast<bool>(encode(document) == after_first));
    };

    "datafix/the same input always produces the same output"_test = [] {
        auto left{legacy_inventory_document()};
        auto right{legacy_inventory_document()};
        auto left_fixed{world::fix_document(left, world::datafix_type::chunk,
                                            oldest, target)};
        auto right_fixed{world::fix_document(right, world::datafix_type::chunk,
                                             oldest, target)};
        expect(static_cast<bool>(left_fixed));
        expect(static_cast<bool>(right_fixed));
        expect(static_cast<bool>(encode(left) == encode(right)));
        // A second kernel instance run on the encoded form decodes to the same
        // canonical tree: the write is already key-sorted.
        auto decoded{nbt::read(encode(left))};
        expect(static_cast<bool>(decoded));
        if (decoded) {
            expect(static_cast<bool>(encode(*decoded) == encode(left)));
        }
    };

    "datafix/fails closed naming the type and version"_test = [] {
        auto document{legacy_inventory_document()};
        auto refused{world::fix_document(document, world::datafix_type::player,
                                         5012, target)};
        expect(!refused);
        if (!refused) {
            expect(refused.error().code == refrain::error_code::unsupported);
            const auto message{refused.error().message};
            expect(message.find("player") != std::string::npos);
            expect(message.find("5012") != std::string::npos);
            expect(message.find("5013") != std::string::npos);
            expect(message.find("5023") != std::string::npos);
        }
        nbt::value not_a_compound{nbt::make_int(1)};
        auto malformed{world::fix_document(
            not_a_compound, world::datafix_type::chunk, oldest, target)};
        expect(!malformed);
        if (!malformed) {
            expect(malformed.error().code == refrain::error_code::parse_error);
            expect(malformed.error().message.find("chunk") !=
                   std::string::npos);
        }
    };

    "datafix/fixes traversal reaches nested lists and compounds"_test = [] {
        auto document{refrain::test_support::legacy_chest_block_entity()};
        auto fixed{world::fix_document(document, world::datafix_type::chunk,
                                       oldest, target)};
        expect(static_cast<bool>(fixed));
        if (fixed) {
            expect(fixed->changed);
            expect(fixed->fixes_applied == 2U);
        }
        const auto *components{item_components(document)};
        expect(components != nullptr);
        if (components != nullptr) {
            expect(!components->contains("minecraft:block_transformer"));
            expect(component_translate(*components) ==
                   "item.minecraft.buried_treasure_map");
        }
    };

    "datafix/leaves a differently shaped item name alone"_test = [] {
        // A components map whose item name is not a plain translation component
        // must be left byte-identical; the reference fix only matches the plain
        // shape.
        nbt::compound name{
            {"translate", nbt::make_string("filled_map.buried_treasure")},
            {"fallback", nbt::make_string("Buried Treasure Map")},
        };
        nbt::compound components{
            {"minecraft:item_name", nbt::make_compound(std::move(name))},
        };
        nbt::compound item{
            {"id", nbt::make_string("minecraft:map")},
            {"components", nbt::make_compound(std::move(components))},
        };
        nbt::compound inventory{
            {"Inventory",
             nbt::make_list(
                 nbt::tag_type::compound,
                 std::vector<nbt::value>{nbt::make_compound(std::move(item))})},
            {"DataVersion", nbt::make_int(oldest)},
        };
        auto document{nbt::make_compound(std::move(inventory))};
        const auto before{encode(document)};
        auto fixed{world::fix_document(document, world::datafix_type::player,
                                       oldest, target)};
        expect(static_cast<bool>(fixed));
        if (fixed) {
            expect(!fixed->changed);
            expect(fixed->fixes_applied == 0U);
        }
        expect(static_cast<bool>(encode(document) == before));
    };

    "datafix/stamp writes the target version last"_test = [] {
        auto document{refrain::test_support::legacy_player_record(oldest)};
        expect(static_cast<bool>(world::stamp_data_version(document, target)));
        const auto *version{document.get("DataVersion")};
        expect(version != nullptr);
        if (version != nullptr) {
            expect(version->as_int() == target);
        }
        nbt::value not_a_compound{nbt::make_string("x")};
        expect(!world::stamp_data_version(not_a_compound, target));
    };
};

} // namespace
