// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/dimension_generator.hpp>

#include <refrain/worldgen/biome/biome_manager.hpp>
#include <refrain/worldgen/biome/biome_view.hpp>
#include <refrain/worldgen/chunk/chunk_region.hpp>
#include <refrain/worldgen/terrain/aquifer.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace refrain::worldgen {

namespace {

auto fail(std::string_view message) -> error {
    return make_error(error_code::invalid_argument, message);
}

/// `<namespace>/<path>` to the document id `<namespace>:<path>`.
auto document_id(std::string_view path) -> std::string {
    std::string id{path};
    if (const auto slash = id.find('/'); slash != std::string::npos) {
        id[slash] = ':';
    }
    return id;
}

auto document_text(const surface::material_documents &documents,
                   std::string_view id) -> const std::string * {
    for (const auto &[key, text] : documents.rules) {
        if (key == id) {
            return &text;
        }
    }
    return nullptr;
}

auto solid_traits() -> state_traits {
    return state_traits{
        .air = false, .blocks_motion = true, .blocks_motion_no_leaves = true};
}

auto fluid_traits(bool water) -> state_traits {
    return state_traits{.air = false, .fluid = true, .water = water};
}

} // namespace

dimension_generator::dimension_generator(
    dimension_generator_config config, const dimension_profile &profile,
    density::noise_settings settings, dimension_biome_source biome_source,
    block_state_registry states, surface::material_documents documents,
    surface::rule material_rule, biome_climates climates, jint generation_min_y,
    jint generation_height)
    : config_{std::move(config)}, profile_{&profile},
      settings_{std::move(settings)}, biome_source_{std::move(biome_source)},
      states_{std::move(states)}, documents_{std::move(documents)},
      material_rule_{std::move(material_rule)}, climates_{std::move(climates)},
      noises_{settings_.legacy_random_source ? algorithm::legacy
                                             : algorithm::xoroshiro,
              config_.seed},
      root_factory_{new_positional_factory(settings_.legacy_random_source
                                               ? algorithm::legacy
                                               : algorithm::xoroshiro,
                                           config_.seed)},
      sampler_{*settings_.router.temperature,
               *settings_.router.vegetation,
               *settings_.router.continents,
               *settings_.router.erosion,
               *settings_.router.depth,
               *settings_.router.ridges,
               sampler_context_},
      generation_min_y_{generation_min_y},
      generation_height_{generation_height} {
    noises_status_ = noises_.load(config_.data_root);
    // The rule table must share the registry the chunk and the fill use.
    rule_states_ = surface::block_states{states_};
    if (config_.fixed_biome.empty()) {
        possible_biomes_.insert(biome_source_.biomes().begin(),
                                biome_source_.biomes().end());
    } else {
        possible_biomes_.insert(config_.fixed_biome);
    }
    air_state_ = states_.air();
    const auto block = states_.intern(settings_.default_block, solid_traits());
    default_block_state_ = block.value_or(no_block_state);
    if (settings_.default_fluid == "minecraft:air") {
        default_fluid_state_ = air_state_;
    } else {
        const bool water = settings_.default_fluid == "minecraft:water";
        const auto fluid =
            states_.intern(settings_.default_fluid, fluid_traits(water));
        default_fluid_state_ = fluid.value_or(no_block_state);
    }
    lava_state_ = states_.intern("minecraft:lava", fluid_traits(false))
                      .value_or(no_block_state);
}

auto dimension_generator::create(dimension_generator_config config)
    -> result<std::unique_ptr<dimension_generator>> {
    if (config.data_root.empty()) {
        return std::unexpected{fail("dimension generator has no data root")};
    }
    const auto &profile = dimension_profile_for(config.kind);
    auto settings =
        density::load_noise_settings(config.data_root, profile.noise_settings,
                                     algorithm::xoroshiro, config.seed);
    if (!settings) {
        return std::unexpected{settings.error()};
    }
    if (settings->min_y != profile.min_y ||
        settings->sea_level != profile.sea_level) {
        return std::unexpected{
            fail("noise settings do not match the dimension profile")};
    }
    const std::string rule_id = document_id(profile.material_rule);
    if (settings->material_rule != rule_id) {
        return std::unexpected{
            fail("noise settings select a different material rule")};
    }

    result<dimension_biome_source> biome_source =
        result<dimension_biome_source>{
            dimension_biome_source::fixed(config.kind, config.fixed_biome)};
    if (config.fixed_biome.empty()) {
        biome_source =
            dimension_biome_source::load(config.data_root, config.kind);
    }
    if (!biome_source) {
        return std::unexpected{biome_source.error()};
    }

    auto tags = load_block_tags(config.data_root);
    if (!tags) {
        return std::unexpected{tags.error()};
    }
    block_state_registry states;
    if (const auto applied = apply_block_tags(states, *tags); !applied) {
        return std::unexpected{applied.error()};
    }

    auto documents = surface::load_material_documents(config.data_root);
    if (!documents) {
        return std::unexpected{documents.error()};
    }
    const auto *rule_text = document_text(*documents, rule_id);
    if (rule_text == nullptr) {
        return std::unexpected{
            make_error(error_code::not_found,
                       "material rule " + rule_id + " is not in the data")};
    }
    auto rule = surface::decode_rule(*rule_text);
    if (!rule) {
        return std::unexpected{rule.error()};
    }

    biome_climates climates;
    if (const auto loaded = climates.load(config.data_root); !loaded) {
        return std::unexpected{loaded.error()};
    }

    // The reference clamps the noise settings to the level height; the nether
    // and the end generate 128 blocks inside a 256-block level.
    const jint generation_min_y = std::max(profile.min_y, settings->min_y);
    const jint generation_max_y = std::min(profile.min_y + profile.height,
                                           settings->min_y + settings->height);
    const jint generation_height = generation_max_y - generation_min_y;
    if (generation_height <= 0) {
        return std::unexpected{
            fail("the noise settings fall outside the level height")};
    }

    auto generator =
        std::unique_ptr<dimension_generator>{new dimension_generator{
            std::move(config), profile, std::move(*settings),
            std::move(*biome_source), std::move(states), std::move(*documents),
            std::move(*rule), std::move(climates), generation_min_y,
            generation_height}};
    if (!generator->noises_status_) {
        return std::unexpected{generator->noises_status_.error()};
    }
    return generator;
}

auto dimension_generator::make_aquifer(
    jint chunk_x, jint chunk_z, const density::sampler_context &context) const
    -> std::unique_ptr<terrain::aquifer> {
    const terrain::fluid_picker picker{.sea_level = profile_->sea_level,
                                       .default_fluid = default_fluid_state_,
                                       .lava = lava_state_,
                                       .air = air_state_};
    const terrain::block_ids blocks{.air = air_state_,
                                    .default_block = default_block_state_,
                                    .default_fluid = default_fluid_state_,
                                    .lava = lava_state_};
    if (!settings_.aquifers) {
        return std::make_unique<terrain::disabled_aquifer>(picker);
    }
    auto factory =
        root_factory_->from_hash_of("minecraft:aquifer")->fork_positional();
    const volume chunk_volume{16,           generation_height_, 16,
                              chunk_x * 16, generation_min_y_,  chunk_z * 16};
    return std::make_unique<terrain::noise_aquifer>(
        settings_.aquifer, *factory, chunk_volume, picker, blocks, context);
}

auto dimension_generator::generate(const chunk_pos &pos)
    -> result<std::unique_ptr<proto_chunk>> {
    auto chunk = std::make_unique<proto_chunk>(
        states_, pos.x, pos.z, profile_->min_y, profile_->height);
    chunk->set_status(chunk_status::biomes);
    fill_chunk_biomes(*chunk, biome_source_, sampler_);

    auto aquifer = make_aquifer(pos.x, pos.z, sampler_context_);
    const terrain::proto_fill_options fill_options{
        .air = air_state_,
        .default_block = default_block_state_,
        .default_fluid = default_fluid_state_,
        .lava = lava_state_,
    };
    terrain::fill_proto_chunk(*chunk, fill_options,
                              terrain::proto_fill_volume{
                                  .min_y = generation_min_y_,
                                  .height = generation_height_,
                              },
                              *settings_.router.final_density, sampler_context_,
                              *aquifer);

    // The surface rule resolves its anchors against the generation depth, so
    // the nether roof lines up with `below_top` of 128, not 256.
    jint max_block_y = profile_->min_y;
    for (jint y = profile_->min_y + profile_->height - 1; y >= profile_->min_y;
         --y) {
        bool occupied = false;
        for (jint z = 0; z < 16 && !occupied; ++z) {
            for (jint x = 0; x < 16; ++x) {
                if (chunk->state_at(x, y, z) != air_state_) {
                    occupied = true;
                    break;
                }
            }
        }
        if (occupied) {
            max_block_y = (y >> 4) * 16 + 15;
            break;
        }
    }
    const surface::generation_bounds bounds{
        .min_gen_y = profile_->min_y,
        .gen_depth = generation_height_,
        .sea_level = profile_->sea_level,
        .min_y = profile_->min_y,
        .max_block_y = max_block_y,
        .chunk_min_x = pos.x * 16,
        .chunk_min_z = pos.z * 16,
    };
    surface::surface_system system{
        rule_states_,         bounds,    noises_, *root_factory_,
        default_block_state_, air_state_};

    dimension_biome_resolver uncached{biome_source_, sampler_};
    chunk_biome_resolver biome_view{
        [&chunk, &pos](jint chunk_x, jint chunk_z) -> const proto_chunk * {
            return chunk_x == pos.x && chunk_z == pos.z ? chunk.get() : nullptr;
        },
        uncached};
    biome_manager manager{biome_view,
                          biome_manager::obfuscate_seed(config_.seed)};

    surface::material_providers providers;
    providers.biome = [this, &manager](jint x, jint y,
                                       jint z) -> std::string_view {
        return biome_source_.biome(manager.get_biome(x, y, z));
    };
    providers.preliminary_surface = [this](jint x, jint z) {
        return settings_.router.chunk_surface_level->sample_value(
            sampler_context_, x, 0, z);
    };
    providers.surface_depth = [&system](jint x, jint z) {
        return system.surface_depth(x, z);
    };
    providers.surface_secondary = [&system](jint x, jint z) {
        return system.surface_secondary(x, z);
    };
    providers.band = [&system](jint x, jint y, jint z) {
        return system.band(x, y, z);
    };
    providers.noise = [this](std::string_view id) { return noises_.get(id); };
    providers.density =
        [this](std::string_view id) -> result<density::sampler_ptr> {
        return density::decode_density("\"" + std::string{id} + "\"",
                                       settings_.documents);
    };
    providers.random_factory =
        [this](std::string_view name) -> positional_random_factory & {
        std::string key{name};
        if (const auto found = factories_.find(key);
            found != factories_.end()) {
            return *found->second;
        }
        auto source = root_factory_->from_hash_of(key);
        return *factories_.emplace(key, source->fork_positional())
                    .first->second;
    };
    providers.cold_enough_to_snow = [this](std::string_view biome, jint x,
                                           jint y, jint z) {
        const auto *climate = climates_.get(biome);
        if (climate == nullptr) {
            return false;
        }
        return temperatures_.temperature_at(*climate, x, y, z,
                                            profile_->sea_level) < 0.15F;
    };
    providers.possible_biomes = &possible_biomes_;

    surface::material_context rule_context{rule_states_, bounds, providers,
                                           sampler_context_};
    surface::rule_compiler compiler{documents_, rule_context};
    auto compiled = compiler.compile(material_rule_);
    if (!compiled) {
        return std::unexpected{compiled.error()};
    }
    if (const auto built =
            system.build_surface(*chunk, rule_context, *compiled, climates_,
                                 temperatures_, providers.biome);
        !built) {
        return std::unexpected{built.error()};
    }
    chunk->set_status(chunk_status::terrain);
    return chunk;
}

} // namespace refrain::worldgen
