// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/density.hpp>
#include <refrain/worldgen/overworld_biome_builder.hpp>
#include <refrain/worldgen/surface.hpp>

#include <boost/ut.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace refrain;
using namespace refrain::worldgen;
using namespace refrain::worldgen::surface;

class fixture {
  public:
    fixture() {
        possible_biomes.insert("minecraft:plains");
        band_state = states.intern("minecraft:orange_terracotta");
        auto source = new_random_source(algorithm::xoroshiro, 42);
        noise = std::make_unique<noise_stack>(
            normal_noise{normal_noise_parameters{}}.create(*source));
        factory = new_positional_factory(algorithm::xoroshiro, 99);

        providers.biome = [this](jint, jint, jint) -> std::string_view {
            return biome;
        };
        providers.preliminary_surface = [this](jint, jint) {
            return preliminary_surface;
        };
        providers.surface_depth = [this](jint, jint) { return surface_depth; };
        providers.surface_secondary = [this](jint, jint) {
            return surface_secondary;
        };
        providers.band = [this](jint, jint, jint) { return band_state; };
        providers.noise = [this](std::string_view) -> const noise_stack * {
            return noise.get();
        };
        providers.density =
            [](std::string_view id) -> result<density::sampler_ptr> {
            if (id == "test:density") {
                return density::constant(1.0F);
            }
            if (id == "test:richness") {
                return density::constant(0.5F);
            }
            if (id == "test:gap") {
                return density::constant(-1.0F);
            }
            if (id == "test:negative") {
                return density::constant(-1.0F);
            }
            return density::constant(0.0F);
        };
        providers.random_factory =
            [this](std::string_view) -> positional_random_factory & {
            return *factory;
        };
        providers.cold_enough_to_snow = [](std::string_view key, jint, jint,
                                           jint) {
            return key == "minecraft:plains";
        };
        providers.possible_biomes = &possible_biomes;
        context = std::make_unique<material_context>(states, bounds, providers,
                                                     density_context);
    }

    auto make_compiler() -> rule_compiler {
        return rule_compiler{documents, *context};
    }

    block_states states{};
    std::set<std::string> possible_biomes{};
    material_documents documents{};
    std::string biome{"minecraft:plains"};
    jint surface_depth{3};
    double surface_secondary{0.0};
    float preliminary_surface{60.0F};
    state_id band_state{no_state};
    std::unique_ptr<noise_stack> noise{};
    std::unique_ptr<positional_random_factory> factory{};
    density::sampler_context density_context{};
    generation_bounds bounds{};
    material_providers providers{};
    std::unique_ptr<material_context> context{};
};

auto compile_json(fixture &env, std::string_view json) -> rule_evaluator {
    const auto decoded = decode_rule(json);
    if (!decoded) {
        return {};
    }
    auto compiler = env.make_compiler();
    const auto compiled = compiler.compile(*decoded);
    return compiled ? *compiled : rule_evaluator{};
}

} // namespace

using namespace boost::ut;

suite<"worldgen/surface-rules"> surface_rule_tests = [] {
    "block, condition and sequence rules evaluate against the context"_test =
        [] {
            fixture env;
            env.context->update_xz(0, 0, 0, 0);
            env.context->update_y(1, 1, std::numeric_limits<jint>::min(), 10);
            const state_id dirt = env.states.intern("minecraft:dirt");
            const auto evaluator =
                compile_json(env, R"({"type":"minecraft:condition",
                    "if_true":{"type":"minecraft:y_above",
                               "anchor":{"absolute":0},
                               "surface_depth_multiplier":0,
                               "add_stone_depth":false},
                    "then_run":{"type":"minecraft:block",
                                "result_state":"minecraft:dirt"}})");
            expect(evaluator != nullptr);
            expect(evaluator(0, 10, 0) == std::optional<state_id>{dirt});

            env.context->update_y(1, 1, std::numeric_limits<jint>::min(), -1);
            expect(!evaluator(0, -1, 0).has_value());

            const auto sequence =
                compile_json(env, R"({"type":"minecraft:sequence",
                    "sequence":[{"type":"minecraft:block",
                                 "result_state":"minecraft:sand"},
                                {"type":"minecraft:block",
                                 "result_state":"minecraft:gravel"}]})");
            expect(sequence != nullptr);
            expect(
                sequence(0, 0, 0) ==
                std::optional<state_id>{env.states.intern("minecraft:sand")});
        };

    "condition kinds read the context state machines"_test = [] {
        fixture env;
        env.context->update_xz(0, 0, -4, 0);
        env.context->update_y(1, 1, std::numeric_limits<jint>::min(), 10);

        auto compile_condition_json =
            [&env](std::string_view json) -> condition_evaluator {
            const auto decoded = decode_condition(json);
            if (!decoded) {
                return {};
            }
            auto compiler = env.make_compiler();
            const auto compiled = compiler.compile_condition(*decoded);
            return compiled ? *compiled : condition_evaluator{};
        };

        const auto steep =
            compile_condition_json(R"({"type":"minecraft:steep"})");
        expect(steep != nullptr);
        expect(steep());

        const auto hole =
            compile_condition_json(R"({"type":"minecraft:hole"})");
        expect(hole != nullptr);
        expect(!hole());
        env.surface_depth = 0;
        env.context->update_xz(0, 0, 0, 0);
        expect(hole());

        const auto water = compile_condition_json(
            R"({"type":"minecraft:water","offset":0,
                "surface_depth_multiplier":0,"add_stone_depth":false})");
        expect(water != nullptr);
        expect(water());
        env.context->update_y(1, 1, 20, 10);
        expect(!water());
        env.context->update_y(1, 1, 20, 20);
        expect(water());

        const auto stone = compile_condition_json(
            R"({"type":"minecraft:stone_depth","offset":0,
                "add_surface_depth":false,"secondary_depth_range":0,
                "surface_type":"floor"})");
        expect(stone != nullptr);
        env.context->update_y(1, 1, std::numeric_limits<jint>::min(), 10);
        expect(stone());
        env.context->update_y(5, 1, std::numeric_limits<jint>::min(), 10);
        expect(!stone());

        const auto preliminary = compile_condition_json(
            R"({"type":"minecraft:above_preliminary_surface"})");
        expect(preliminary != nullptr);
        env.surface_depth = 3;
        env.context->update_xz(0, 0, 0, 0);
        env.context->update_y(1, 1, std::numeric_limits<jint>::min(), 55);
        expect(preliminary());
        env.context->update_y(1, 1, std::numeric_limits<jint>::min(), 54);
        expect(!preliminary());

        const auto gradient = compile_condition_json(
            R"({"type":"minecraft:vertical_gradient",
                "random_name":"minecraft:test",
                "true_at_and_below":{"absolute":-64},
                "false_at_and_above":{"absolute":320}})");
        expect(gradient != nullptr);
        env.context->update_y(1, 1, std::numeric_limits<jint>::min(), -64);
        expect(gradient());
        env.context->update_y(1, 1, std::numeric_limits<jint>::min(), 320);
        expect(!gradient());

        const auto noise = compile_condition_json(
            R"({"type":"minecraft:noise_threshold","noise":"test:noise",
                "min_threshold":-100.0,"max_threshold":100.0})");
        expect(noise != nullptr);
        expect(noise());
        const auto never = compile_condition_json(
            R"({"type":"minecraft:noise_threshold","noise":"test:noise",
                "min_threshold":100.0,"max_threshold":200.0})");
        expect(never != nullptr);
        expect(!never());

        const auto temperature =
            compile_condition_json(R"({"type":"minecraft:temperature"})");
        expect(temperature != nullptr);
        env.biome = "minecraft:plains";
        env.context->update_y(1, 1, std::numeric_limits<jint>::min(), 10);
        expect(temperature());
        env.biome = "minecraft:desert";
        env.context->update_y(1, 1, std::numeric_limits<jint>::min(), 11);
        expect(!temperature());

        const auto not_steep = compile_condition_json(
            R"({"type":"minecraft:not","invert":{"type":"minecraft:steep"}})");
        expect(not_steep != nullptr);
        env.context->update_xz(0, 0, -4, 0);
        expect(!not_steep());
    };

    "biome conditions use the possible-biome shortcut and the getter"_test =
        [] {
            fixture env;
            env.context->update_xz(0, 0, 0, 0);
            env.context->update_y(1, 1, std::numeric_limits<jint>::min(), 10);
            auto compile_condition_json =
                [&env](std::string_view json) -> condition_evaluator {
                const auto decoded = decode_condition(json);
                if (!decoded) {
                    return {};
                }
                auto compiler = env.make_compiler();
                const auto compiled = compiler.compile_condition(*decoded);
                return compiled ? *compiled : condition_evaluator{};
            };
            // A second possible biome keeps the condition lazy: neither
            // "can never" nor "always" applies, so the getter is consulted.
            env.possible_biomes.insert("minecraft:desert");
            const auto plains = compile_condition_json(
                R"({"type":"minecraft:biome","biome_is":"minecraft:plains"})");
            expect(plains != nullptr);
            expect(plains());

            env.biome = "minecraft:desert";
            env.context->update_y(1, 1, std::numeric_limits<jint>::min(), 11);
            expect(!plains());

            // possible = {plains}: desert can never match, plains always does.
            env.possible_biomes.erase("minecraft:desert");
            const auto desert = compile_condition_json(
                R"({"type":"minecraft:biome","biome_is":"minecraft:desert"})");
            expect(desert != nullptr);
            expect(!desert());
            const auto always_plains = compile_condition_json(
                R"({"type":"minecraft:biome","biome_is":"minecraft:plains"})");
            expect(always_plains != nullptr);
            expect(always_plains());
        };

    "ore vein rules read densities and blocks"_test = [] {
        fixture env;
        env.context->update_xz(0, 0, 0, 0);
        env.context->update_y(1, 1, std::numeric_limits<jint>::min(), 0);
        const auto vein = compile_json(env, R"({"type":"minecraft:ore_vein",
                "ore_block":"minecraft:copper_ore",
                "raw_ore_block":"minecraft:raw_copper_block",
                "filler_block":"minecraft:granite",
                "raw_ore_chance":0.02,
                "density":"test:density",
                "richness":"test:richness",
                "filler_gap":"test:gap"})");
        expect(vein != nullptr);
        const auto ore = env.states.find("minecraft:copper_ore");
        const auto raw = env.states.find("minecraft:raw_copper_block");
        const auto filler = env.states.find("minecraft:granite");
        expect(ore.has_value() && raw.has_value() && filler.has_value());
        std::size_t found = 0;
        for (jint x = 0; x < 16; ++x) {
            for (jint y = 0; y < 16; ++y) {
                for (jint z = 0; z < 16; ++z) {
                    const auto state = vein(x, y, z);
                    expect(state.has_value());
                    if (state == std::optional<state_id>{*ore} ||
                        state == std::optional<state_id>{*raw} ||
                        state == std::optional<state_id>{*filler}) {
                        ++found;
                    }
                }
            }
        }
        expect(found == 4096U);

        const auto empty = compile_json(env, R"({"type":"minecraft:ore_vein",
                "ore_block":"minecraft:copper_ore",
                "raw_ore_block":"minecraft:raw_copper_block",
                "filler_block":"minecraft:granite",
                "raw_ore_chance":0.02,
                "density":"test:negative",
                "richness":"test:richness",
                "filler_gap":"test:gap"})");
        expect(empty != nullptr);
        expect(!empty(0, 0, 0).has_value());
    };

    "references resolve and cycles are rejected"_test = [] {
        fixture env;
        env.documents.rules.emplace_back(
            "test:stone",
            R"({"type":"minecraft:block","result_state":"minecraft:stone"})");
        env.documents.rules.emplace_back(
            "test:a", R"({"type":"minecraft:sequence","sequence":["test:b"]})");
        env.documents.rules.emplace_back("test:b", R"("test:a")");
        env.documents.conditions.emplace_back(
            "test:floor",
            R"({"type":"minecraft:stone_depth","offset":0,
                "add_surface_depth":false,"secondary_depth_range":0,
                "surface_type":"floor"})");
        env.context->update_xz(0, 0, 0, 0);
        env.context->update_y(1, 1, std::numeric_limits<jint>::min(), 0);

        auto compiler = env.make_compiler();
        const auto reference = decode_rule(R"("test:stone")");
        expect(reference.has_value());
        const auto compiled = compiler.compile(*reference);
        expect(compiled.has_value());
        expect((*compiled)(0, 0, 0) ==
               std::optional<state_id>{env.states.intern("minecraft:stone")});

        const auto cyclic = decode_rule(R"("test:a")");
        expect(cyclic.has_value());
        const auto rejected = compiler.compile(*cyclic);
        expect(!rejected.has_value());

        const auto missing = decode_rule(R"("test:missing")");
        expect(missing.has_value());
        expect(!compiler.compile(*missing).has_value());

        const auto condition_reference = decode_condition(R"("test:floor")");
        expect(condition_reference.has_value());
        const auto condition = compiler.compile_condition(*condition_reference);
        expect(condition.has_value());
    };

    "the real overworld rule tree compiles"_test = [] {
        const char *root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
        if (root == nullptr) {
            return;
        }
        const auto documents = load_material_documents(root);
        expect(documents.has_value()) << documents.error().message;
        if (!documents) {
            return;
        }
        fixture env;
        env.documents = *documents;
        env.possible_biomes.clear();
        for (const auto &parameter : overworld_parameters()) {
            env.possible_biomes.insert(parameter.biome);
        }
        const auto *overworld = [&]() -> const std::string * {
            for (const auto &[id, text] : env.documents.rules) {
                if (id == "minecraft:overworld") {
                    return &text;
                }
            }
            return nullptr;
        }();
        expect(overworld != nullptr);
        if (overworld == nullptr) {
            return;
        }
        const auto decoded = decode_rule(*overworld);
        expect(decoded.has_value());
        if (!decoded) {
            return;
        }
        auto compiler = env.make_compiler();
        const auto compiled = compiler.compile(*decoded);
        if (!compiled) {
            std::fprintf(stderr, "overworld compile: %s\n",
                         compiled.error().message.c_str());
        }
        expect(compiled.has_value());
        if (!compiled) {
            return;
        }
        for (const jint y : {300, 100, 60, 0, -50}) {
            env.context->update_xz(0, 0, 0, 0);
            env.context->update_y(1, 1, std::numeric_limits<jint>::min(), y);
            (void)(*compiled)(0, y, 0);
        }
        expect(true);
    };
};
