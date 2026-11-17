// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Content-backed block light tests: the tracked vanilla tables and the block
// light report that generated them. They run in CI without a game jar because
// both files are tracked game-derived data; the report is the source of the
// table and the tests cross-check the two so a stale regeneration cannot ship.
//
// The fixtures are the shipped content, not synthetic data: the tests name the
// light behaviour of a few reference blocks to prove the table decodes, and
// the fallback for a state the table does not carry.

#include <refrain/core/io.hpp>
#include <refrain/data/json.hpp>
#include <refrain/data/vanilla.hpp>
#include <refrain/sim/light.hpp>

#include <boost/ut.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
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
namespace sim = refrain::sim;

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
                     "data/block-light: tracked tables missing; run the "
                     "content import first\n");
        return result;
    }
    result.bytes = std::move(*bytes);
    auto opened = data::vanilla_tables::open(result.bytes);
    if (!opened) {
        std::fprintf(stderr, "data/block-light: %s\n",
                     opened.error().message.c_str());
        return result;
    }
    result.tables = std::move(*opened);
    return result;
}

/// Light record of the named block's default state.
[[nodiscard]] auto default_light(const data::vanilla_tables &tables,
                                 std::string_view block)
    -> std::optional<data::block_light_view> {
    const auto found{tables.find_block(block)};
    if (!found.has_value()) {
        return std::nullopt;
    }
    return tables.light_at(tables.block_at(*found).default_state);
}

/// Light record of one canonical state key.
[[nodiscard]] auto state_light(const data::vanilla_tables &tables,
                               std::string_view key)
    -> std::optional<data::block_light_view> {
    const auto found{tables.find_state(key)};
    if (!found.has_value()) {
        return std::nullopt;
    }
    return tables.light_at(*found);
}

} // namespace

suite<"data/block-light"> block_light_tests = [] {
    "the tracked tables carry a light record for every state"_test = [] {
        auto tracked = load_tracked_tables();
        expect(tracked.available());
        if (!tracked.available()) {
            return;
        }
        const auto &tables = *tracked.tables;
        expect(tables.has_light());
        expect(tables.light_shape_count() > 0U);
        expect(tables.state_count() > 1000U);
        for (std::uint32_t state{0}; state < tables.state_count(); ++state) {
            const auto light{tables.light_at(state)};
            expect(light.emission <= data::max_light_level);
            expect(light.dampening <= data::max_light_level);
            if (light.shape != data::no_light_shape) {
                expect(light.can_occlude);
                expect(light.use_shape_for_light_occlusion);
                expect(light.shape < tables.light_shape_count());
            }
        }
    };

    "emissive blocks carry the reference light levels"_test = [] {
        auto tracked = load_tracked_tables();
        expect(tracked.available());
        if (!tracked.available()) {
            return;
        }
        const auto &tables = *tracked.tables;
        struct expectation {
            std::string_view block;
            std::uint8_t emission;
        };
        constexpr std::array<expectation, 15> defaults{{
            {"minecraft:torch", 14},
            {"minecraft:wall_torch", 14},
            {"minecraft:soul_torch", 10},
            {"minecraft:copper_torch", 14},
            {"minecraft:glowstone", 15},
            {"minecraft:lantern", 15},
            {"minecraft:soul_lantern", 10},
            {"minecraft:sea_lantern", 15},
            {"minecraft:shroomlight", 15},
            {"minecraft:jack_o_lantern", 15},
            {"minecraft:beacon", 15},
            {"minecraft:end_rod", 14},
            {"minecraft:crying_obsidian", 10},
            {"minecraft:magma_block", 3},
            {"minecraft:amethyst_cluster", 5},
        }};
        for (const auto &[block, emission] : defaults) {
            const auto light{default_light(tables, block)};
            expect(light.has_value()) << std::string{block};
            if (light.has_value()) {
                expect(eq(static_cast<int>(light->emission),
                          static_cast<int>(emission)))
                    << std::string{block};
            }
        }
        // Property-dependent emitters: the lit and levelled variants differ
        // from the block's default state.
        const auto unlit_lamp{
            state_light(tables, "minecraft:redstone_lamp[lit=false]")};
        const auto lit_lamp{
            state_light(tables, "minecraft:redstone_lamp[lit=true]")};
        expect(unlit_lamp.has_value());
        expect(lit_lamp.has_value());
        if (unlit_lamp.has_value() && lit_lamp.has_value()) {
            expect(eq(static_cast<int>(unlit_lamp->emission), 0));
            expect(eq(static_cast<int>(lit_lamp->emission), 15));
        }
        const auto light_seven{
            state_light(tables, "minecraft:light[level=7,waterlogged=false]")};
        const auto light_fifteen{
            state_light(tables, "minecraft:light[level=15,waterlogged=false]")};
        expect(light_seven.has_value());
        expect(light_fifteen.has_value());
        if (light_seven.has_value() && light_fifteen.has_value()) {
            expect(eq(static_cast<int>(light_seven->emission), 7));
            expect(eq(static_cast<int>(light_fifteen->emission), 15));
        }
        const auto unlit_campfire{state_light(
            tables,
            "minecraft:campfire[facing=north,lit=false,signal_fire=false,"
            "waterlogged=false]")};
        expect(unlit_campfire.has_value());
        if (unlit_campfire.has_value()) {
            expect(eq(static_cast<int>(unlit_campfire->emission), 0));
        }
    };

    "sky damping and shape occlusion match the reference families"_test = [] {
        auto tracked = load_tracked_tables();
        expect(tracked.available());
        if (!tracked.available()) {
            return;
        }
        const auto &tables = *tracked.tables;
        const auto dampening_of = [&tables](std::string_view block) {
            const auto light{default_light(tables, block)};
            expect(light.has_value()) << std::string{block};
            return light.has_value() ? light->dampening : -1;
        };
        // Air and glass pass sky light; leaves and water lose one level per
        // block; stone is opaque; tinted glass blocks sky light by itself.
        expect(eq(static_cast<int>(dampening_of("minecraft:air")), 0));
        expect(eq(static_cast<int>(dampening_of("minecraft:glass")), 0));
        expect(eq(static_cast<int>(dampening_of("minecraft:oak_leaves")), 1));
        expect(eq(static_cast<int>(dampening_of("minecraft:water")), 1));
        expect(eq(static_cast<int>(dampening_of("minecraft:stone")), 15));
        expect(
            eq(static_cast<int>(dampening_of("minecraft:tinted_glass")), 15));

        // A slab is not solid enough to be opaque but it uses its shape for
        // light occlusion: the bottom slab has a full bottom face shape and an
        // empty top face, the top slab the reverse. A double slab is a full
        // block and stops light without a shape.
        const auto bottom_slab{state_light(
            tables,
            "minecraft:smooth_stone_slab[type=bottom,waterlogged=false]")};
        const auto top_slab{state_light(
            tables, "minecraft:smooth_stone_slab[type=top,waterlogged=false]")};
        const auto double_slab{state_light(
            tables,
            "minecraft:smooth_stone_slab[type=double,waterlogged=false]")};
        expect(bottom_slab.has_value());
        expect(top_slab.has_value());
        expect(double_slab.has_value());
        if (bottom_slab.has_value()) {
            expect(eq(static_cast<int>(bottom_slab->dampening), 0));
            expect(bottom_slab->shape != data::no_light_shape);
            if (bottom_slab->shape != data::no_light_shape) {
                const auto shape{tables.light_shape_at(bottom_slab->shape)};
                expect(shape.face_boxes[0] > 0U);
                expect(shape.face_boxes[1] == 0U);
                const auto box{tables.light_box(bottom_slab->shape, 0, 0)};
                expect(box.bounds[0] == 0.0);
                expect(box.bounds[3] == 1.0);
            }
        }
        if (top_slab.has_value()) {
            expect(eq(static_cast<int>(top_slab->dampening), 0));
            expect(top_slab->shape != data::no_light_shape);
            if (top_slab->shape != data::no_light_shape) {
                const auto shape{tables.light_shape_at(top_slab->shape)};
                expect(shape.face_boxes[0] == 0U);
                expect(shape.face_boxes[1] > 0U);
            }
        }
        if (double_slab.has_value()) {
            expect(eq(static_cast<int>(double_slab->dampening), 15));
            expect(double_slab->shape == data::no_light_shape);
        }
    };

    "a state the table does not carry stays on the fallback"_test = [] {
        auto tracked = load_tracked_tables();
        expect(tracked.available());
        if (!tracked.available()) {
            return;
        }
        auto light_table{sim::block_light_table::from_tables(*tracked.tables)};
        expect(!light_table.empty());
        expect(light_table.shape_count() ==
               tracked.tables->light_shape_count());
        // One entry per canonical state key, plus the bare block-name spelling
        // of every default state whose canonical key carries properties (a
        // stored palette and a datapack write that state as the bare name).
        std::size_t expected_size{tracked.tables->state_count()};
        const auto block_count{tracked.tables->block_count()};
        for (std::uint32_t block{0}; block < block_count; ++block) {
            const auto view{
                tracked.tables->block_at(static_cast<std::uint16_t>(block))};
            if (view.state_count > 0 &&
                tracked.tables->state_at(view.default_state).key != view.name) {
                ++expected_size;
            }
        }
        expect(light_table.size() == expected_size);
        expect(light_table.find("minecraft:not_a_block") == nullptr);
        const auto *torch{light_table.find("minecraft:torch")};
        expect(torch != nullptr);
        if (torch != nullptr) {
            expect(eq(static_cast<int>(torch->emission), 14));
            expect(eq(static_cast<int>(torch->dampening), 0));
        }
        const auto *stone{light_table.find("minecraft:stone")};
        expect(stone != nullptr);
        if (stone != nullptr) {
            expect(eq(static_cast<int>(stone->dampening), 15));
        }
        // The documented fallback: emission 0, dampening 15, no shape.
        const sim::block_light_property fallback{};
        expect(eq(static_cast<int>(fallback.emission), 0));
        expect(eq(static_cast<int>(fallback.dampening), 15));
        expect(fallback.shape == nullptr);
    };

    "the tracked light report agrees with the tables"_test = [] {
        auto tracked = load_tracked_tables();
        expect(tracked.available());
        if (!tracked.available()) {
            return;
        }
        const auto &tables = *tracked.tables;
        auto document = data::json::value::parse_file(
            content_path("reports/block_light/light.json"));
        expect(document.has_value());
        if (!document.has_value()) {
            std::fprintf(stderr, "data/block-light: %s\n",
                         document.error().message.c_str());
            return;
        }
        const auto format{document->find("format")};
        const auto version{document->find("data_version")};
        const auto states{document->find("states")};
        const auto shapes{document->find("shapes")};
        expect(format != nullptr);
        expect(version != nullptr);
        expect(states != nullptr);
        expect(shapes != nullptr);
        if (format == nullptr || version == nullptr || states == nullptr ||
            shapes == nullptr) {
            return;
        }
        expect(format->as_string().value_or("") == "refrain.block-light/1");
        expect(version->as_int64().value_or(0) == tables.data_version());
        const auto state_count{document->find("state_count")};
        const auto shape_count{document->find("shape_count")};
        expect(state_count != nullptr);
        expect(shape_count != nullptr);
        if (state_count == nullptr || shape_count == nullptr) {
            return;
        }
        expect(state_count->as_int64().value_or(-1) ==
               static_cast<std::int64_t>(tables.state_count()));
        expect(shape_count->as_int64().value_or(-1) ==
               static_cast<std::int64_t>(tables.light_shape_count()));
        const auto state_rows{states->as_array()};
        const auto shape_rows{shapes->as_array()};
        expect(state_rows.has_value());
        expect(shape_rows.has_value());
        if (!state_rows.has_value() || !shape_rows.has_value()) {
            return;
        }
        expect(state_rows->size() == tables.state_count());
        expect(shape_rows->size() == tables.light_shape_count());
        if (state_rows->size() != tables.state_count() ||
            shape_rows->size() != tables.light_shape_count()) {
            return;
        }
        // The report lists states by the game's dense state id, the container
        // orders them canonically by block name and state key. The report row
        // a container state belongs to is addressed by that state's game id.
        std::size_t mismatches{0};
        for (std::uint32_t state{0}; state < tables.state_count(); ++state) {
            const auto row_index{
                static_cast<std::size_t>(tables.state_at(state).vanilla_id)};
            if (row_index >= state_rows->size()) {
                ++mismatches;
                continue;
            }
            const auto row{(*state_rows)[row_index].as_array()};
            expect(row.has_value());
            if (!row.has_value() || row->size() != 5) {
                return;
            }
            const auto light{tables.light_at(state)};
            const auto id{(*row)[0].as_int64().value_or(-1)};
            const auto emission{(*row)[1].as_int64().value_or(-1)};
            const auto dampening{(*row)[2].as_int64().value_or(-1)};
            const auto flags{(*row)[3].as_int64().value_or(-1)};
            const auto shape{(*row)[4].as_int64().value_or(-2)};
            if (id != static_cast<std::int64_t>(row_index) ||
                emission != static_cast<std::int64_t>(light.emission) ||
                dampening != static_cast<std::int64_t>(light.dampening) ||
                ((flags & 1) != 0) != light.can_occlude ||
                ((flags & 2) != 0) != light.use_shape_for_light_occlusion ||
                (shape < 0
                     ? data::no_light_shape
                     : static_cast<std::uint32_t>(shape)) != light.shape) {
                ++mismatches;
            }
        }
        expect(mismatches == 0) << "the report and the table disagree";
        for (std::uint32_t shape{0}; shape < tables.light_shape_count();
             ++shape) {
            const auto row{(*shape_rows)[shape].as_array()};
            expect(row.has_value());
            if (!row.has_value() || row->size() != 6) {
                return;
            }
            const auto faces{tables.light_shape_at(shape)};
            for (std::size_t face{0}; face < 6; ++face) {
                const auto boxes{(*row)[face].as_array()};
                expect(boxes.has_value());
                if (!boxes.has_value()) {
                    return;
                }
                expect(boxes->size() == faces.face_boxes[face]);
                for (std::size_t index{0}; index < boxes->size(); ++index) {
                    const auto bounds{(*boxes)[index].as_array()};
                    expect(bounds.has_value());
                    if (!bounds.has_value() || bounds->size() != 6) {
                        return;
                    }
                    const auto expected{
                        tables.light_box(shape, static_cast<std::uint8_t>(face),
                                         static_cast<std::uint16_t>(index))};
                    for (std::size_t axis{0}; axis < 6; ++axis) {
                        expect((*bounds)[axis].as_double().value_or(-1.0) ==
                               expected.bounds[axis]);
                    }
                }
            }
        }
    };
};
