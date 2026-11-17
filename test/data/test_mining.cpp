// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Content-backed block mining tests: the tracked vanilla tables and the block
// mining report that generated them. They run in CI without a game jar because
// both files are tracked game-derived data; the report is the source of the
// table and the tests cross-check the two so a stale regeneration cannot ship.
//
// The fixtures are the shipped content, not synthetic data: the tests name the
// mining behaviour of a few reference blocks to prove the table decodes, the
// runtime lookups answer the compressed model, and the fallback for a block or
// tool the table does not carry.

#include <refrain/core/io.hpp>
#include <refrain/data/json.hpp>
#include <refrain/data/mining.hpp>
#include <refrain/data/vanilla.hpp>

#include <boost/ut.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using boost::ut::eq;
using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace data = refrain::data;

constexpr std::string_view content_version = "26.3";

[[nodiscard]] auto content_path(std::string_view relative) -> std::string {
    return std::string{REFRAIN_SOURCE_DIR} + "/content/vanilla/" +
           std::string{content_version} + "/" + std::string{relative};
}

/// The tracked table container, kept alive for a view.
struct tracked_tables {
    std::vector<std::byte> bytes{};
    std::optional<data::vanilla_tables> tables{};

    [[nodiscard]] auto available() const noexcept -> bool {
        return tables.has_value();
    }
};

[[nodiscard]] auto load_tracked_tables() -> tracked_tables {
    tracked_tables result{};
    auto bytes = refrain::core::io::read_file(content_path("tables.rvtb"));
    if (!bytes) {
        std::fprintf(stderr,
                     "data/block-mining: tracked tables missing; run the "
                     "content import first\n");
        return result;
    }
    result.bytes = std::move(*bytes);
    auto opened = data::vanilla_tables::open(result.bytes);
    if (!opened) {
        std::fprintf(stderr, "data/block-mining: %s\n",
                     opened.error().message.c_str());
        return result;
    }
    result.tables = std::move(*opened);
    return result;
}

/// Mining record of the named block.
[[nodiscard]] auto mining_of(const data::vanilla_tables &tables,
                             std::string_view block)
    -> std::optional<data::block_mining_view> {
    const auto found = tables.find_block(block);
    if (!found.has_value()) {
        return std::nullopt;
    }
    return tables.block_mining_at(*found);
}

/// One block of a synthetic container.
auto synthetic_block(std::string_view name, std::uint16_t id)
    -> data::block_spec {
    data::block_spec block;
    block.name = std::string{name};
    block.states.push_back(data::state_spec{
        .vanilla_id = id,
        .values = {},
        .is_default = true,
    });
    return block;
}

/// A container with two blocks and one tool, without mining data.
auto synthetic_spec() -> data::vanilla_spec {
    data::vanilla_spec spec;
    spec.data_version = 5023;
    spec.blocks = {synthetic_block("minecraft:air", 0),
                   synthetic_block("minecraft:stone", 1)};
    spec.items = {data::item_spec{.name = "minecraft:iron_pickaxe",
                                  .max_stack_size = 1,
                                  .max_damage = 250}};
    return spec;
}

/// The same container with a mining record per block and one tool.
auto synthetic_mining_spec() -> data::vanilla_spec {
    auto spec = synthetic_spec();
    data::block_mining_spec dirt{};
    dirt.hardness = 0.5F;
    dirt.flags = data::block_mining_requires_correct_tool;
    dirt.tool_mask = data::tool_type_bit(data::tool_type::shovel);
    data::block_mining_spec stone{};
    stone.hardness = 1.5F;
    stone.flags = data::block_mining_requires_correct_tool;
    stone.tool_mask = data::tool_type_bit(data::tool_type::pickaxe);
    stone.needed_tier = 1;
    spec.block_minings = {dirt, stone};
    spec.tools = {data::tool_mining_spec{
        .name = "minecraft:iron_pickaxe",
        .type = data::tool_type::pickaxe,
        .material = data::tool_material::iron,
        .harvest_level = 2,
        .tier_speed = 6.0F,
        .default_mining_speed = 1.0F,
        .damage_per_block = 1,
        .max_stack_size = 1,
        .max_damage = 250,
        .can_destroy_blocks_in_creative = true,
    }};
    return spec;
}

} // namespace

suite<"data/block-mining"> block_mining_tests = [] {
    "the tracked tables carry a mining record for every block"_test = [] {
        auto tracked = load_tracked_tables();
        expect(tracked.available());
        if (!tracked.available()) {
            return;
        }
        const auto &tables = *tracked.tables;
        expect(tables.has_mining());
        expect(tables.block_count() > 1000U);
        expect(tables.tool_count() > 0U);
        for (std::uint32_t block = 0; block < tables.block_count(); ++block) {
            const auto view =
                tables.block_mining_at(static_cast<std::uint16_t>(block));
            const auto hardness = view.hardness;
            expect(hardness == -1.0F || hardness >= 0.0F);
            expect((view.tool_mask & ~data::all_tool_type_bits) == 0U);
            expect((view.direct_correct & ~data::all_tool_type_bits) == 0U);
            expect(view.needed_tier <= data::max_harvest_level);
            expect(view.shears_speed >= 0.0F);
            expect(view.sword_speed >= 0.0F);
        }
    };

    "reference blocks carry their destroy time and tool requirement"_test = [] {
        auto tracked = load_tracked_tables();
        expect(tracked.available());
        if (!tracked.available()) {
            return;
        }
        const auto &tables = *tracked.tables;
        const auto stone = mining_of(tables, "minecraft:stone");
        expect(stone.has_value());
        if (stone.has_value()) {
            expect(stone->hardness == 1.5F);
            expect(stone->requires_correct_tool_for_drops());
            expect((stone->tool_mask &
                    data::tool_type_bit(data::tool_type::pickaxe)) != 0U);
            expect(stone->needed_tier == 0U);
        }
        const auto obsidian = mining_of(tables, "minecraft:obsidian");
        expect(obsidian.has_value());
        if (obsidian.has_value()) {
            expect(obsidian->hardness == 50.0F);
            expect(obsidian->needed_tier == 3U);
            expect(obsidian->requires_correct_tool_for_drops());
        }
        const auto bedrock = mining_of(tables, "minecraft:bedrock");
        expect(bedrock.has_value());
        if (bedrock.has_value()) {
            expect(bedrock->hardness == -1.0F);
            expect(bedrock->unbreakable());
            expect(!bedrock->requires_correct_tool_for_drops());
        }
        const auto air = mining_of(tables, "minecraft:air");
        expect(air.has_value());
        if (air.has_value()) {
            expect(air->is_air());
            expect(air->hardness == 0.0F);
        }
        const auto water = mining_of(tables, "minecraft:water");
        expect(water.has_value());
        if (water.has_value()) {
            expect(water->is_fluid());
        }
        const auto dirt = mining_of(tables, "minecraft:dirt");
        expect(dirt.has_value());
        if (dirt.has_value()) {
            expect((dirt->tool_mask &
                    data::tool_type_bit(data::tool_type::shovel)) != 0U);
            expect(!dirt->requires_correct_tool_for_drops());
        }
        const auto leaves = mining_of(tables, "minecraft:oak_leaves");
        expect(leaves.has_value());
        if (leaves.has_value()) {
            expect(leaves->shears_speed == 15.0F);
            expect(leaves->sword_speed == 1.5F);
        }
        const auto cobweb = mining_of(tables, "minecraft:cobweb");
        expect(cobweb.has_value());
        if (cobweb.has_value()) {
            expect(cobweb->requires_correct_tool_for_drops());
            expect((cobweb->direct_correct &
                    data::tool_type_bit(data::tool_type::shears)) != 0U);
            expect((cobweb->direct_correct &
                    data::tool_type_bit(data::tool_type::sword)) != 0U);
            expect(cobweb->shears_speed == 15.0F);
        }
        const auto iron_ore = mining_of(tables, "minecraft:iron_ore");
        expect(iron_ore.has_value());
        if (iron_ore.has_value()) {
            expect(iron_ore->needed_tier == 1U);
        }
        const auto diamond_ore = mining_of(tables, "minecraft:diamond_ore");
        expect(diamond_ore.has_value());
        if (diamond_ore.has_value()) {
            expect(diamond_ore->needed_tier == 2U);
        }
    };

    "tool items carry their family, material and limits"_test = [] {
        auto tracked = load_tracked_tables();
        expect(tracked.available());
        if (!tracked.available()) {
            return;
        }
        const auto &tables = *tracked.tables;
        const auto pickaxe = tables.find_tool("minecraft:iron_pickaxe");
        expect(pickaxe.has_value());
        if (pickaxe.has_value()) {
            const auto tool = tables.tool_at(*pickaxe);
            expect(tool.name == "minecraft:iron_pickaxe");
            expect(tool.type == data::tool_type::pickaxe);
            expect(tool.material == data::tool_material::iron);
            expect(tool.harvest_level == 2U);
            expect(tool.tier_speed == 6.0F);
            expect(tool.default_mining_speed == 1.0F);
            expect(tool.damage_per_block == 1U);
            expect(tool.max_stack_size == 1U);
            expect(tool.max_damage == 250U);
            expect(tool.can_destroy_blocks_in_creative);
        }
        const auto wooden = tables.find_tool("minecraft:wooden_pickaxe");
        expect(wooden.has_value());
        if (wooden.has_value()) {
            const auto tool = tables.tool_at(*wooden);
            expect(tool.material == data::tool_material::wood);
            expect(tool.harvest_level == 0U);
            expect(tool.tier_speed == 2.0F);
            expect(tool.max_damage == 59U);
        }
        const auto shears = tables.find_tool("minecraft:shears");
        expect(shears.has_value());
        if (shears.has_value()) {
            const auto tool = tables.tool_at(*shears);
            expect(tool.type == data::tool_type::shears);
            expect(tool.material == data::tool_material::none);
            expect(tool.harvest_level == data::harvest_level_none);
            expect(tool.max_damage == 238U);
        }
        const auto sword = tables.find_tool("minecraft:wooden_sword");
        expect(sword.has_value());
        if (sword.has_value()) {
            expect(tables.tool_at(*sword).type == data::tool_type::sword);
        }
        const auto mace = tables.find_tool("minecraft:mace");
        expect(mace.has_value());
        if (mace.has_value()) {
            expect(tables.tool_at(*mace).type == data::tool_type::none);
        }
        expect(!tables.find_tool("minecraft:not_a_tool").has_value());
        std::string_view previous{};
        for (std::uint32_t index = 0; index < tables.tool_count(); ++index) {
            const auto tool = tables.tool_at(index);
            expect(!tool.name.empty());
            expect(previous.empty() || tool.name > previous);
            previous = tool.name;
        }
    };

    "the runtime table reproduces the compressed model"_test = [] {
        auto tracked = load_tracked_tables();
        expect(tracked.available());
        if (!tracked.available()) {
            return;
        }
        auto blocks = data::block_mining_table::from_tables(*tracked.tables);
        auto tools = data::item_tool_table::from_tables(*tracked.tables);
        expect(!blocks.empty());
        expect(blocks.size() == tracked.tables->block_count());
        expect(tools.size() == tracked.tables->tool_count());
        const auto *stone = blocks.find("minecraft:stone");
        expect(stone != nullptr);
        const auto *iron = tools.find("minecraft:iron_pickaxe");
        expect(iron != nullptr);
        const auto *wooden = tools.find("minecraft:wooden_pickaxe");
        expect(wooden != nullptr);
        const auto *diamond = tools.find("minecraft:diamond_pickaxe");
        expect(diamond != nullptr);
        const auto *shears = tools.find("minecraft:shears");
        expect(shears != nullptr);
        if (stone != nullptr && iron != nullptr && wooden != nullptr &&
            diamond != nullptr && shears != nullptr) {
            expect(data::mining_speed(*iron, *stone) == 6.0F);
            expect(data::mining_speed(*wooden, *stone) == 2.0F);
            const auto *dirt = blocks.find("minecraft:dirt");
            expect(dirt != nullptr);
            if (dirt != nullptr) {
                expect(data::mining_speed(*iron, *dirt) == 1.0F);
            }
            const auto *cobweb = blocks.find("minecraft:cobweb");
            expect(cobweb != nullptr);
            if (cobweb != nullptr) {
                expect(data::mining_speed(*shears, *cobweb) == 15.0F);
                expect(data::correct_tool_for_drops(*shears, *cobweb));
            }
            const auto *iron_ore = blocks.find("minecraft:iron_ore");
            expect(iron_ore != nullptr);
            if (iron_ore != nullptr) {
                expect(data::correct_tool_for_drops(*iron, *iron_ore));
                expect(!data::correct_tool_for_drops(*wooden, *iron_ore));
                expect(data::correct_tool_for_drops(*wooden, *stone));
            }
            const auto *obsidian = blocks.find("minecraft:obsidian");
            expect(obsidian != nullptr);
            if (obsidian != nullptr) {
                expect(!data::correct_tool_for_drops(*iron, *obsidian));
                expect(data::correct_tool_for_drops(*diamond, *obsidian));
            }
        }
        // The dense id lookup agrees with the name lookup.
        const auto stone_id = tracked.tables->find_block("minecraft:stone");
        expect(stone_id.has_value());
        if (stone_id.has_value()) {
            const auto *by_id = blocks.at(*stone_id);
            expect(by_id != nullptr);
            if (by_id != nullptr && stone != nullptr) {
                expect(by_id->hardness == stone->hardness);
            }
        }
        expect(blocks.find("minecraft:not_a_block") == nullptr);
        expect(blocks.at(0xFFFFU) == nullptr);
        expect(tools.find("minecraft:not_a_tool") == nullptr);
        // The documented fallbacks: an unknown block is unbreakable, an
        // unknown item has no tool component.
        const auto &missing_block = data::block_mining_table::missing();
        expect(missing_block.hardness == -1.0F);
        expect(missing_block.unbreakable());
        expect(!missing_block.requires_correct_tool_for_drops());
        const auto &missing_tool = data::item_tool_table::missing();
        expect(missing_tool.type == data::tool_type::none);
        expect(missing_tool.default_mining_speed == 1.0F);
        expect(data::mining_speed(missing_tool, missing_block) == 1.0F);
        expect(!data::correct_tool_for_drops(missing_tool, missing_block));
    };

    "the tracked mining report agrees with the tables"_test = [] {
        auto tracked = load_tracked_tables();
        expect(tracked.available());
        if (!tracked.available()) {
            return;
        }
        const auto &tables = *tracked.tables;
        auto document = data::json::value::parse_file(
            content_path("reports/block_mining/mining.json"));
        expect(document.has_value());
        if (!document.has_value()) {
            std::fprintf(stderr, "data/block-mining: %s\n",
                         document.error().message.c_str());
            return;
        }
        const auto format = document->find("format");
        const auto version = document->find("data_version");
        const auto block_count = document->find("block_count");
        const auto tool_count = document->find("tool_count");
        const auto blocks = document->find("blocks");
        const auto tools = document->find("tools");
        expect(format != nullptr);
        expect(version != nullptr);
        expect(block_count != nullptr);
        expect(tool_count != nullptr);
        expect(blocks != nullptr);
        expect(tools != nullptr);
        if (format == nullptr || version == nullptr || block_count == nullptr ||
            tool_count == nullptr || blocks == nullptr || tools == nullptr) {
            return;
        }
        expect(format->as_string().value_or("") == "refrain.block-mining/1");
        expect(version->as_int64().value_or(0) == tables.data_version());
        expect(block_count->as_int64().value_or(-1) ==
               static_cast<std::int64_t>(tables.block_count()));
        expect(tool_count->as_int64().value_or(-1) ==
               static_cast<std::int64_t>(tables.tool_count()));
        const auto block_rows = blocks->as_array();
        const auto tool_rows = tools->as_array();
        expect(block_rows.has_value());
        expect(tool_rows.has_value());
        if (!block_rows.has_value() || !tool_rows.has_value()) {
            return;
        }
        expect(block_rows->size() == tables.block_count());
        expect(tool_rows->size() == tables.tool_count());
        if (block_rows->size() != tables.block_count() ||
            tool_rows->size() != tables.tool_count()) {
            return;
        }
        for (const auto &row : *block_rows) {
            const auto values = row.as_array();
            expect(values.has_value());
            if (!values.has_value() || values->size() != 8) {
                continue;
            }
            const auto name = (*values)[0].as_string().value_or("");
            const auto block = tables.find_block(name);
            expect(block.has_value());
            if (!block.has_value()) {
                continue;
            }
            const auto mining = tables.block_mining_at(*block);
            expect(mining.hardness ==
                   static_cast<float>((*values)[1].as_double().value_or(0.0)));
            expect(static_cast<int>(mining.flags) ==
                   static_cast<int>((*values)[2].as_int64().value_or(-1)));
            expect(static_cast<int>(mining.tool_mask) ==
                   static_cast<int>((*values)[3].as_int64().value_or(-1)));
            expect(static_cast<int>(mining.needed_tier) ==
                   static_cast<int>((*values)[4].as_int64().value_or(-1)));
            expect(mining.shears_speed ==
                   static_cast<float>((*values)[5].as_double().value_or(0.0)));
            expect(mining.sword_speed ==
                   static_cast<float>((*values)[6].as_double().value_or(0.0)));
            expect(static_cast<int>(mining.direct_correct) ==
                   static_cast<int>((*values)[7].as_int64().value_or(-1)));
        }
        for (const auto &row : *tool_rows) {
            const auto entry = row.find("id");
            expect(entry != nullptr);
            if (entry == nullptr) {
                continue;
            }
            const auto name = entry->as_string().value_or("");
            const auto index = tables.find_tool(name);
            expect(index.has_value());
            if (!index.has_value()) {
                continue;
            }
            const auto tool = tables.tool_at(*index);
            const auto type_name = row.find("tool_type");
            const auto max_damage = row.find("max_damage");
            expect(type_name != nullptr);
            expect(max_damage != nullptr);
            if (type_name != nullptr) {
                const auto text = type_name->as_string().value_or("");
                const auto expected =
                    [](data::tool_type type) -> std::string_view {
                    switch (type) {
                    case data::tool_type::pickaxe:
                        return "pickaxe";
                    case data::tool_type::axe:
                        return "axe";
                    case data::tool_type::shovel:
                        return "shovel";
                    case data::tool_type::hoe:
                        return "hoe";
                    case data::tool_type::shears:
                        return "shears";
                    case data::tool_type::sword:
                        return "sword";
                    case data::tool_type::none:
                    default:
                        return "none";
                    }
                };
                expect(text == expected(tool.type));
            }
            if (max_damage != nullptr) {
                expect(tool.max_damage ==
                       static_cast<std::uint32_t>(
                           max_damage->as_int64().value_or(0)));
            }
        }
    };

    "a container without mining sections answers the fallback"_test = [] {
        auto bytes = data::build_vanilla_tables(synthetic_spec());
        expect(bytes.has_value());
        if (!bytes.has_value()) {
            return;
        }
        auto tables = data::vanilla_tables::open(*bytes);
        expect(tables.has_value());
        if (!tables.has_value()) {
            return;
        }
        expect(!tables->has_mining());
        expect(tables->tool_count() == 0U);
        const auto stone = tables->find_block("minecraft:stone");
        expect(stone.has_value());
        if (stone.has_value()) {
            expect(tables->block_mining_at(*stone).hardness == -1.0F);
        }
        auto blocks = data::block_mining_table::from_tables(*tables);
        auto tools = data::item_tool_table::from_tables(*tables);
        expect(blocks.empty());
        expect(tools.empty());
        expect(blocks.find("minecraft:stone") == nullptr);
        expect(tools.find("minecraft:iron_pickaxe") == nullptr);
    };

    "a container round trip exposes the mining sections"_test = [] {
        auto bytes = data::build_vanilla_tables(synthetic_mining_spec());
        expect(bytes.has_value());
        if (!bytes.has_value()) {
            return;
        }
        auto tables = data::vanilla_tables::open(*bytes);
        expect(tables.has_value());
        if (!tables.has_value()) {
            return;
        }
        expect(tables->has_mining());
        expect(tables->tool_count() == 1U);
        const auto stone = tables->find_block("minecraft:stone");
        expect(stone.has_value());
        if (stone.has_value()) {
            const auto mining = tables->block_mining_at(*stone);
            expect(mining.hardness == 1.5F);
            expect(mining.requires_correct_tool_for_drops());
            expect(mining.needed_tier == 1U);
            expect((mining.tool_mask &
                    data::tool_type_bit(data::tool_type::pickaxe)) != 0U);
        }
        const auto tool = tables->find_tool("minecraft:iron_pickaxe");
        expect(tool.has_value());
        if (tool.has_value()) {
            const auto view = tables->tool_at(*tool);
            expect(view.type == data::tool_type::pickaxe);
            expect(view.material == data::tool_material::iron);
            expect(view.harvest_level == 2U);
            expect(view.tier_speed == 6.0F);
            expect(view.max_damage == 250U);
        }
        auto runtime = data::block_mining_table::from_tables(*tables);
        auto runtime_tools = data::item_tool_table::from_tables(*tables);
        const auto *by_name = runtime.find("minecraft:stone");
        expect(by_name != nullptr);
        const auto *by_id = runtime.at(*stone);
        expect(by_id != nullptr);
        const auto *pickaxe = runtime_tools.find("minecraft:iron_pickaxe");
        expect(pickaxe != nullptr);
        if (by_name != nullptr && pickaxe != nullptr) {
            expect(data::mining_speed(*pickaxe, *by_name) == 6.0F);
            expect(data::correct_tool_for_drops(*pickaxe, *by_name));
        }
    };

    "invalid mining specifications are rejected"_test = [] {
        auto wrong_count = synthetic_mining_spec();
        wrong_count.block_minings.pop_back();
        expect(!data::build_vanilla_tables(wrong_count).has_value());

        auto bad_hardness = synthetic_mining_spec();
        bad_hardness.block_minings[0].hardness = -2.0F;
        expect(!data::build_vanilla_tables(bad_hardness).has_value());

        auto bad_flags = synthetic_mining_spec();
        bad_flags.block_minings[0].flags = 0x80U;
        expect(!data::build_vanilla_tables(bad_flags).has_value());

        auto bad_mask = synthetic_mining_spec();
        bad_mask.block_minings[0].tool_mask = 0xFFU;
        expect(!data::build_vanilla_tables(bad_mask).has_value());

        auto bad_tier = synthetic_mining_spec();
        bad_tier.block_minings[0].needed_tier = 9U;
        expect(!data::build_vanilla_tables(bad_tier).has_value());

        auto bad_speed = synthetic_mining_spec();
        bad_speed.block_minings[0].shears_speed = -1.0F;
        expect(!data::build_vanilla_tables(bad_speed).has_value());

        auto duplicate_tool = synthetic_mining_spec();
        duplicate_tool.tools.push_back(duplicate_tool.tools.front());
        expect(!data::build_vanilla_tables(duplicate_tool).has_value());

        auto bad_tool_stack = synthetic_mining_spec();
        bad_tool_stack.tools.front().max_stack_size = 100;
        expect(!data::build_vanilla_tables(bad_tool_stack).has_value());

        auto missing_tools = synthetic_mining_spec();
        missing_tools.tools.clear();
        expect(!data::build_vanilla_tables(missing_tools).has_value());
    };
};
