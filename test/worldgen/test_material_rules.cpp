// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/terrain/material.hpp>

#include <boost/ut.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {

using namespace refrain;
using namespace refrain::worldgen::surface;

auto find_document(const material_documents &documents, std::string_view id,
                   bool condition) -> std::string_view {
    const auto &entries = condition ? documents.conditions : documents.rules;
    for (const auto &[key, text] : entries) {
        if (key == id) {
            return text;
        }
    }
    return {};
}

} // namespace

using namespace boost::ut;

suite<"worldgen/material-rules"> material_rule_tests = [] {
    "vertical anchors resolve against the generation bounds"_test = [] {
        expect(vertical_anchor{.type = vertical_anchor::kind::absolute,
                               .value = 12}
                   .resolve(-64, 384, 63) == 12);
        expect(vertical_anchor{.type = vertical_anchor::kind::above_bottom,
                               .value = 5}
                   .resolve(-64, 384, 63) == -59);
        expect(vertical_anchor{.type = vertical_anchor::kind::below_top,
                               .value = 3}
                   .resolve(-64, 384, 63) == 316);
        expect(vertical_anchor{.type =
                                   vertical_anchor::kind::relative_to_sea_level,
                               .value = 2}
                   .resolve(-64, 384, 63) == 65);
    };

    "block, sequence and condition rules decode"_test = [] {
        const auto block = decode_rule(R"({"type":"minecraft:block",
            "result_state":"minecraft:grass_block"})");
        expect(block.has_value());
        expect(block->type == rule::kind::block);
        expect(block->state == "minecraft:grass_block");

        const auto sequence = decode_rule(R"({"type":"minecraft:sequence",
            "sequence":["minecraft:bedrock_floor",
                        {"type":"minecraft:bandlands"}]})");
        expect(sequence.has_value());
        expect(sequence->type == rule::kind::sequence);
        expect(sequence->sequence.size() == 2U);
        expect(sequence->sequence[0].type == rule::kind::reference);
        expect(sequence->sequence[0].reference == "minecraft:bedrock_floor");
        expect(sequence->sequence[1].type == rule::kind::bandlands);

        const auto conditional = decode_rule(R"({"type":"minecraft:condition",
            "if_true":"minecraft:on_floor",
            "then_run":{"type":"minecraft:block",
                        "result_state":"minecraft:dirt"}})");
        expect(conditional.has_value());
        expect(conditional->type == rule::kind::condition);
        expect(conditional->if_true != nullptr);
        expect(conditional->if_true->type == condition::kind::reference);
        expect(conditional->if_true->reference == "minecraft:on_floor");
        expect(conditional->then_run != nullptr);
        expect(conditional->then_run->state == "minecraft:dirt");
    };

    "ore vein rules decode all references"_test = [] {
        const auto vein = decode_rule(R"({"type":"minecraft:ore_vein",
            "ore_block":"minecraft:copper_ore",
            "raw_ore_block":"minecraft:raw_copper_block",
            "filler_block":"minecraft:granite",
            "raw_ore_chance":0.02,
            "density":"minecraft:overworld/ore_vein/copper_density",
            "richness":"minecraft:overworld/ore_vein/richness",
            "filler_gap":"minecraft:overworld/ore_vein/gap"})");
        expect(vein.has_value());
        expect(vein->type == rule::kind::ore_vein);
        expect(vein->ore_block == "minecraft:copper_ore");
        expect(vein->raw_ore_chance == 0.02F);
        expect(vein->density == "minecraft:overworld/ore_vein/copper_density");
    };

    "every condition type decodes"_test = [] {
        const auto biome = decode_condition(
            R"({"type":"minecraft:biome","biome_is":["minecraft:desert",
                "minecraft:badlands"]})");
        expect(biome.has_value());
        expect(biome->type == condition::kind::biome);
        expect(biome->biome_is.size() == 2U);

        const auto noise =
            decode_condition(R"({"type":"minecraft:noise_threshold",
            "noise":"minecraft:surface","min_threshold":-0.9,
            "max_threshold":0.1,"is_3d":true})");
        expect(noise.has_value());
        expect(noise->type == condition::kind::noise_threshold);
        expect(noise->is_3d);
        expect(noise->min_threshold == -0.9);

        const auto gradient = decode_condition(
            R"({"type":"minecraft:vertical_gradient",
                "random_name":"minecraft:minecraft:deepslate",
                "true_at_and_below":{"absolute":-8},
                "false_at_and_above":{"above_bottom":8}})");
        expect(gradient.has_value());
        expect(gradient->type == condition::kind::vertical_gradient);
        expect(gradient->true_at_and_below.value == -8);
        expect(gradient->true_at_and_below.type ==
               vertical_anchor::kind::absolute);
        expect(gradient->false_at_and_above.type ==
               vertical_anchor::kind::above_bottom);

        const auto y_above = decode_condition(R"({"type":"minecraft:y_above",
            "anchor":{"absolute":97},"surface_depth_multiplier":2,
            "add_stone_depth":false})");
        expect(y_above.has_value());
        expect(y_above->type == condition::kind::y_above);
        expect(y_above->surface_depth_multiplier == 2);

        const auto water = decode_condition(R"({"type":"minecraft:water",
            "offset":-6,"surface_depth_multiplier":-1,
            "add_stone_depth":true})");
        expect(water.has_value());
        expect(water->type == condition::kind::water);
        expect(water->add_stone_depth);

        const auto stone = decode_condition(R"({"type":"minecraft:stone_depth",
            "offset":0,"add_surface_depth":true,"secondary_depth_range":6,
            "surface_type":"ceiling"})");
        expect(stone.has_value());
        expect(stone->type == condition::kind::stone_depth);
        expect(stone->surface == cave_surface::ceiling);
        expect(stone->secondary_depth_range == 6);

        for (const auto &[text, kind] : std::initializer_list<
                 std::pair<std::string_view, condition::kind>>{
                 {R"({"type":"minecraft:temperature"})",
                  condition::kind::temperature},
                 {R"({"type":"minecraft:steep"})", condition::kind::steep},
                 {R"({"type":"minecraft:hole"})", condition::kind::hole},
                 {R"({"type":"minecraft:above_preliminary_surface"})",
                  condition::kind::above_preliminary_surface}}) {
            const auto decoded = decode_condition(text);
            expect(decoded.has_value());
            expect(decoded->type == kind);
        }

        const auto inverted = decode_condition(
            R"({"type":"minecraft:not","invert":"minecraft:on_floor"})");
        expect(inverted.has_value());
        expect(inverted->type == condition::kind::not_);
        expect(inverted->target != nullptr);
        expect(inverted->target->reference == "minecraft:on_floor");
    };

    "the reference datapack documents decode"_test = [] {
        const char *root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
        if (root == nullptr) {
            return;
        }
        const auto documents = load_material_documents(root);
        expect(documents.has_value())
            << (documents ? "" : documents.error().message);
        if (!documents) {
            return;
        }
        expect(documents->rules.size() == 42U);
        expect(documents->conditions.size() == 8U);
        for (const auto &[id, text] : documents->rules) {
            const auto decoded = decode_rule(text);
            if (!decoded) {
                std::fprintf(stderr, "%s: %s\n", id.c_str(),
                             decoded.error().message.c_str());
            }
            expect(decoded.has_value());
        }
        for (const auto &[id, text] : documents->conditions) {
            const auto decoded = decode_condition(text);
            if (!decoded) {
                std::fprintf(stderr, "%s: %s\n", id.c_str(),
                             decoded.error().message.c_str());
            }
            expect(decoded.has_value());
        }

        const auto overworld = decode_rule(
            find_document(*documents, "minecraft:overworld", false));
        expect(overworld.has_value());
        if (!overworld) {
            return;
        }
        expect(overworld->type == rule::kind::sequence);
        expect(overworld->sequence.size() == 5U);
        expect(overworld->sequence[0].reference == "minecraft:bedrock_floor");
        expect(overworld->sequence[1].reference ==
               "minecraft:overworld/copper_ore_vein");
        expect(overworld->sequence[2].reference ==
               "minecraft:overworld/iron_ore_vein");
        expect(overworld->sequence[3].type == rule::kind::condition);
        expect(overworld->sequence[3].if_true->type ==
               condition::kind::above_preliminary_surface);
        expect(overworld->sequence[3].then_run->reference ==
               "minecraft:overworld/surface");
        expect(overworld->sequence[4].reference ==
               "minecraft:overworld/underground");

        const auto on_floor = decode_condition(
            find_document(*documents, "minecraft:on_floor", true));
        expect(on_floor.has_value());
        expect(on_floor->type == condition::kind::stone_depth);
        expect(on_floor->surface == cave_surface::floor);
    };
};
