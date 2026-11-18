// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/dimension_generator.hpp>

#include <refrain/core/io.hpp>
#include <refrain/data/content_view.hpp>
#include <refrain/data/datapack.hpp>
#include <refrain/data/registry.hpp>
#include <refrain/data/vanilla.hpp>
#include <refrain/platform/process.hpp>
#include <refrain/worldgen/biome/biome_manager.hpp>
#include <refrain/worldgen/biome/biome_view.hpp>
#include <refrain/worldgen/carver/carver_data.hpp>
#include <refrain/worldgen/carver/carver_stage.hpp>
#include <refrain/worldgen/chunk/block_defaults.hpp>
#include <refrain/worldgen/chunk/chunk_region.hpp>
#include <refrain/worldgen/stage_profile.hpp>
#include <refrain/worldgen/terrain/aquifer.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fmt/format.h>

namespace refrain::worldgen {

namespace {

auto fail(std::string_view message) -> error {
    return make_error(error_code::invalid_argument, message);
}

/// The block-state table container beside the data directory, the layout of an
/// installed content root.
auto table_path(std::string_view data_root) -> std::string {
    while (data_root.size() > 1 &&
           (data_root.back() == '/' || data_root.back() == '\\')) {
        data_root.remove_suffix(1);
    }
    std::string path = core::io::parent_directory(data_root);
    path += "/tables.rvtb";
    return path;
}

/// True when a merged view path is a document chunk generation reads. A view
/// that carries none of these leaves the installed datapack untouched.
auto is_generation_input_path(std::string_view path) -> bool {
    constexpr std::string_view data_prefix{"data/"};
    if (!path.starts_with(data_prefix)) {
        return false;
    }
    auto rest{path.substr(data_prefix.size())};
    const auto namespace_end{rest.find('/')};
    if (namespace_end == std::string_view::npos) {
        return false;
    }
    rest.remove_prefix(namespace_end + 1U);
    return rest.starts_with("worldgen/") || rest.starts_with("tags/") ||
           rest.starts_with("structure/");
}

/// True when a merged view path is a block tag document.
auto is_block_tag_path(std::string_view path) -> bool {
    constexpr std::string_view data_prefix{"data/"};
    if (!path.starts_with(data_prefix)) {
        return false;
    }
    auto rest{path.substr(data_prefix.size())};
    const auto namespace_end{rest.find('/')};
    if (namespace_end == std::string_view::npos) {
        return false;
    }
    rest.remove_prefix(namespace_end + 1U);
    return rest.starts_with("tags/block/");
}

/// Recursively copy one directory tree. Entries are visited in sorted order and
/// symlinks are skipped, so the materialized tree is a deterministic, closed
/// copy of the installed content.
auto copy_tree(std::string_view source, std::string_view destination)
    -> status {
    auto names{core::io::list_directory(source)};
    if (!names) {
        return std::unexpected{names.error()};
    }
    for (const auto &name : *names) {
        std::string from{source};
        from.push_back('/');
        from += name;
        std::string to{destination};
        to.push_back('/');
        to += name;
        if (core::io::is_symlink(from)) {
            continue;
        }
        if (core::io::is_directory(from)) {
            if (auto created{core::io::create_directories(to)}; !created) {
                return created;
            }
            if (auto copied{copy_tree(from, to)}; !copied) {
                return copied;
            }
            continue;
        }
        if (!core::io::is_regular_file(from)) {
            continue;
        }
        auto bytes{core::io::read_file(from, {.allow_symlink = false})};
        if (!bytes) {
            return std::unexpected{bytes.error()};
        }
        if (auto written{core::io::write_file(to, *bytes)}; !written) {
            return written;
        }
    }
    return status_ok();
}

void remove_tree(std::string_view path) noexcept {
    std::error_code error;
    std::filesystem::remove_all(std::filesystem::path{std::string{path}},
                                error);
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

struct dimension_shared_data {
    dimension_shared_data(const dimension_profile &dimension_profile,
                          density::noise_settings dimension_settings,
                          block_tag_set block_tags,
                          surface::material_documents material_documents,
                          surface::rule rule, biome_climates dimension_climates,
                          carver::loaded_carvers dimension_carvers, jlong seed)
        : profile{dimension_profile}, settings{std::move(dimension_settings)},
          tags{std::move(block_tags)}, documents{std::move(material_documents)},
          material_rule{std::move(rule)},
          climates{std::move(dimension_climates)},
          carvers{std::move(dimension_carvers)},
          noises{settings.legacy_random_source ? algorithm::legacy
                                               : algorithm::xoroshiro,
                 seed},
          generation_min_y{std::max(profile.min_y, settings.min_y)},
          generation_height{std::min(profile.min_y + profile.height,
                                     settings.min_y + settings.height) -
                            generation_min_y} {}

    const dimension_profile &profile;
    density::noise_settings settings;
    block_tag_set tags;
    surface::material_documents documents;
    surface::rule material_rule;
    biome_climates climates;
    carver::loaded_carvers carvers;
    biome_temperatures temperatures{};
    surface::noise_registry noises;
    std::set<std::string> possible_biomes{};
    block_default_map block_defaults{};
    jint generation_min_y{0};
    jint generation_height{0};
    /// Registry mapping of the content generation the data was loaded for.
    /// Null for a boot generator. Used to seed a per-generator state table in
    /// content-key order.
    std::shared_ptr<const data::registry_generation> registries{};
};

dimension_generator::dimension_generator(
    dimension_generator_config config,
    std::shared_ptr<const dimension_shared_data> shared,
    dimension_biome_source biome_source,
    std::shared_ptr<block_state_registry> states)
    : config_{std::move(config)}, shared_{std::move(shared)},
      profile_{&shared_->profile}, biome_source_{std::move(biome_source)},
      states_{std::move(states)},
      root_factory_{new_positional_factory(
          shared_->settings.legacy_random_source ? algorithm::legacy
                                                 : algorithm::xoroshiro,
          config_.seed)},
      sampler_{*shared_->settings.router.temperature,
               *shared_->settings.router.vegetation,
               *shared_->settings.router.continents,
               *shared_->settings.router.erosion,
               *shared_->settings.router.depth,
               *shared_->settings.router.ridges,
               sampler_context_} {
    // The rule table must share the registry the chunk and the fill use.
    rule_states_ = surface::block_states{*states_};
    air_state_ = states_->air();
    const auto resolve = [this](std::string_view name,
                                state_traits traits) -> block_state_id {
        // A shared table already holds the content's complete state list, so
        // the lookup must not append a first-use duplicate of a name the
        // table resolves.
        if (const auto found = states_->find(name); found.has_value()) {
            return *found;
        }
        const auto interned = states_->intern(name, traits);
        return interned.value_or(no_block_state);
    };
    default_block_state_ =
        resolve(shared_->settings.default_block, solid_traits());
    if (shared_->settings.default_fluid == "minecraft:air") {
        default_fluid_state_ = air_state_;
    } else {
        const bool water = shared_->settings.default_fluid == "minecraft:water";
        default_fluid_state_ =
            resolve(shared_->settings.default_fluid, fluid_traits(water));
    }
    lava_state_ = resolve("minecraft:lava", fluid_traits(false));
}

auto dimension_generator::load_shared(dimension_generator_config config)
    -> result<std::shared_ptr<const dimension_shared_data>> {
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

    result<block_tag_set> tags{config.block_tags != nullptr
                                   ? result<block_tag_set>{*config.block_tags}
                                   : load_block_tags(config.data_root)};
    if (!tags) {
        return std::unexpected{tags.error()};
    }
    // A snapshot that also ships the block default report lets a bare state
    // key answer state-dependent questions. Older snapshots omit it, and the
    // generator keeps the spelling-only behaviour. The report is loaded once
    // per shared instance so batch workers do not parse it again.
    auto block_defaults = load_block_defaults(config.data_root);
    if (!block_defaults) {
        if (block_defaults.error().code != error_code::not_found) {
            return std::unexpected{block_defaults.error()};
        }
        block_defaults = block_default_map{};
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

    carver::loaded_carvers carvers;
    if (profile.has_carvers) {
        auto loaded = carver::load_carvers(config.data_root);
        if (!loaded) {
            return std::unexpected{loaded.error()};
        }
        carvers = std::move(*loaded);
    }

    auto shared = std::make_shared<dimension_shared_data>(
        profile, std::move(*settings), std::move(*tags), std::move(*documents),
        std::move(*rule), std::move(climates), std::move(carvers), config.seed);
    if (shared->generation_height <= 0) {
        return std::unexpected{
            fail("the noise settings fall outside the level height")};
    }
    if (config.fixed_biome.empty()) {
        shared->possible_biomes.insert((*biome_source).biomes().begin(),
                                       (*biome_source).biomes().end());
    } else {
        shared->possible_biomes.insert(config.fixed_biome);
    }
    if (const auto loaded = shared->noises.load(config.data_root); !loaded) {
        return std::unexpected{loaded.error()};
    }
    shared->block_defaults = std::move(*block_defaults);
    shared->registries = config.registries;
    return std::shared_ptr<const dimension_shared_data>{std::move(shared)};
}

namespace {

auto load_biome_source(const dimension_generator_config &config)
    -> result<dimension_biome_source> {
    if (!config.fixed_biome.empty()) {
        return dimension_biome_source::fixed(config.kind, config.fixed_biome);
    }
    return dimension_biome_source::load(config.data_root, config.kind);
}

} // namespace

auto dimension_generator::create(dimension_generator_config config)
    -> result<std::unique_ptr<dimension_generator>> {
    auto shared = load_shared(config);
    if (!shared) {
        return std::unexpected{shared.error()};
    }
    return create(std::move(config), std::move(*shared));
}

auto dimension_generator::create(
    dimension_generator_config config,
    std::shared_ptr<const dimension_shared_data> shared)
    -> result<std::unique_ptr<dimension_generator>> {
    if (config.data_root.empty()) {
        return std::unexpected{fail("dimension generator has no data root")};
    }
    auto biome_source = load_biome_source(config);
    if (!biome_source) {
        return std::unexpected{biome_source.error()};
    }
    auto states = std::make_shared<block_state_registry>();
    if (const auto applied = apply_block_tags(*states, shared->tags);
        !applied) {
        return std::unexpected{applied.error()};
    }
    // A snapshot that also ships the block default report lets a bare state
    // key answer state-dependent questions. Older snapshots omit it, and the
    // generator keeps the spelling-only behaviour.
    if (!shared->block_defaults.empty()) {
        states->set_block_defaults(shared->block_defaults);
    }
    // Seed the generation-scoped block entries in content-key order, so the
    // ids a registry generation defines do not depend on which block the
    // generator happens to resolve first.
    if (shared->registries != nullptr) {
        for (const auto &entry :
             shared->registries->entries(data::registry_kind::block)) {
            const auto name{entry.location.to_string()};
            if (states->find(name).has_value()) {
                continue;
            }
            static_cast<void>(states->intern(name, states->infer_traits(name)));
        }
    }
    return std::unique_ptr<dimension_generator>{
        new dimension_generator{std::move(config), std::move(shared),
                                std::move(*biome_source), std::move(states)}};
}

auto dimension_generator::create(
    dimension_generator_config config,
    std::shared_ptr<const dimension_shared_data> shared,
    std::shared_ptr<block_state_registry> states)
    -> result<std::unique_ptr<dimension_generator>> {
    if (config.data_root.empty()) {
        return std::unexpected{fail("dimension generator has no data root")};
    }
    if (states == nullptr) {
        return std::unexpected{fail("dimension generator has no state table")};
    }
    auto biome_source = load_biome_source(config);
    if (!biome_source) {
        return std::unexpected{biome_source.error()};
    }
    return std::unique_ptr<dimension_generator>{
        new dimension_generator{std::move(config), std::move(shared),
                                std::move(*biome_source), std::move(states)}};
}

auto dimension_generator::possible_biomes(
    const dimension_generator_config &config,
    const std::shared_ptr<const dimension_shared_data> &shared)
    -> result<std::vector<std::string>> {
    (void)shared;
    if (!config.fixed_biome.empty()) {
        return std::vector<std::string>{config.fixed_biome};
    }
    auto source = load_biome_source(config);
    if (!source) {
        return std::unexpected{source.error()};
    }
    return std::vector<std::string>{source->biomes().begin(),
                                    source->biomes().end()};
}

auto dimension_generator::build_shared_states(
    dimension_generator_config config,
    const std::shared_ptr<const dimension_shared_data> &shared)
    -> result<std::shared_ptr<block_state_registry>> {
    if (shared == nullptr) {
        return std::unexpected{
            fail("the shared block state table needs dimension data")};
    }
    const std::string tables_path = config.tables_path.empty()
                                        ? table_path(config.data_root)
                                        : config.tables_path;
    if (!core::io::is_regular_file(tables_path)) {
        return std::unexpected{make_error(
            error_code::not_found,
            "the shared block state table needs the content block table at " +
                tables_path)};
    }
    auto bytes = core::io::read_file(tables_path);
    if (!bytes) {
        return std::unexpected{bytes.error()};
    }
    auto tables = data::vanilla_tables::open(*bytes);
    if (!tables) {
        return std::unexpected{tables.error()};
    }
    auto registry = build_shared_block_state_registry(*tables, shared->tags);
    if (!registry) {
        return std::unexpected{registry.error()};
    }
    return std::shared_ptr<block_state_registry>{
        std::make_shared<block_state_registry>(std::move(*registry))};
}

auto dimension_generator::settings() const noexcept
    -> const density::noise_settings & {
    return shared_->settings;
}

auto dimension_generator::carvers() const noexcept
    -> const carver::carver_biomes * {
    return profile_->has_carvers ? &shared_->carvers.biomes : nullptr;
}

auto dimension_generator::material_rule() const noexcept -> std::string_view {
    return shared_->settings.material_rule;
}

auto dimension_generator::generation_min_y() const noexcept -> jint {
    return shared_->generation_min_y;
}

auto dimension_generator::generation_height() const noexcept -> jint {
    return shared_->generation_height;
}

auto dimension_generator::make_aquifer(
    jint chunk_x, jint chunk_z, const density::sampler_context &context) const
    -> aquifer_holder {
    const terrain::fluid_picker picker{.sea_level = profile_->sea_level,
                                       .default_fluid = default_fluid_state_,
                                       .lava = lava_state_,
                                       .air = air_state_};
    const terrain::block_ids blocks{.air = air_state_,
                                    .default_block = default_block_state_,
                                    .default_fluid = default_fluid_state_,
                                    .lava = lava_state_};
    aquifer_holder holder;
    if (!shared_->settings.aquifers) {
        holder.aquifer = std::make_unique<terrain::disabled_aquifer>(picker);
        return holder;
    }
    holder.factory =
        root_factory_->from_hash_of("minecraft:aquifer")->fork_positional();
    const volume chunk_volume{
        16,           shared_->generation_height, 16,
        chunk_x * 16, shared_->generation_min_y,  chunk_z * 16};
    holder.aquifer = std::make_unique<terrain::noise_aquifer>(
        shared_->settings.aquifer, *holder.factory, chunk_volume, picker,
        blocks, context);
    return holder;
}

auto dimension_generator::make_chunk(const chunk_pos &pos) const
    -> std::unique_ptr<proto_chunk> {
    return std::make_unique<proto_chunk>(*states_, pos.x, pos.z,
                                         profile_->min_y, profile_->height);
}

auto dimension_generator::generate_height_column(const chunk_pos &pos)
    -> result<std::unique_ptr<proto_chunk>> {
    auto chunk = make_chunk(pos);
    chunk->set_status(chunk_status::biomes);
    const density::sampler_context context;
    auto aquifer = make_aquifer(pos.x, pos.z, context);
    const terrain::proto_fill_options fill_options{
        .air = air_state_,
        .default_block = default_block_state_,
        .default_fluid = default_fluid_state_,
        .lava = lava_state_,
    };
    terrain::fill_proto_chunk(*chunk, fill_options,
                              terrain::proto_fill_volume{
                                  .min_y = shared_->generation_min_y,
                                  .height = shared_->generation_height,
                              },
                              *shared_->settings.router.final_density, context,
                              *aquifer.aquifer);
    return chunk;
}

void dimension_generator::reset_biome_search() { biome_source_.reset_search(); }

auto dimension_generator::biome_at_quart(jint quart_x, jint quart_y,
                                         jint quart_z) -> std::uint16_t {
    return biome_source_.noise_biome(sampler_, quart_x, quart_y, quart_z);
}

auto dimension_generator::biome_name(std::uint16_t id) const
    -> std::string_view {
    return biome_source_.biome(id);
}

auto dimension_generator::biome_temperature(std::string_view biome, jint x,
                                            jint y, jint z) const
    -> std::optional<float> {
    const auto *climate = shared_->climates.get(biome);
    if (climate == nullptr) {
        return std::nullopt;
    }
    return shared_->temperatures.temperature_at(*climate, x, y, z,
                                                profile_->sea_level);
}

auto dimension_generator::generate(const chunk_pos &pos)
    -> result<std::unique_ptr<proto_chunk>> {
    return generate_terrain(pos, terrain_stages{.carvers = carvers()});
}

auto dimension_generator::generate_terrain(const chunk_pos &pos,
                                           const terrain_stages &stages)
    -> result<std::unique_ptr<proto_chunk>> {
    std::unique_ptr<proto_chunk> chunk;
    {
        const stage_timer timer{stages.profile, "terrain.chunk"};
        chunk = make_chunk(pos);
    }
    {
        const stage_timer timer{stages.profile, "terrain.biome"};
        chunk->set_status(chunk_status::biomes);
        fill_chunk_biomes(*chunk, biome_source_, sampler_);
    }

    const auto &fill_context = stages.fill_context != nullptr
                                   ? *stages.fill_context
                                   : sampler_context_;
    aquifer_holder aquifer;
    {
        const stage_timer timer{stages.profile, "terrain.fill"};
        aquifer = make_aquifer(pos.x, pos.z, fill_context);
        const terrain::proto_fill_options fill_options{
            .air = air_state_,
            .default_block = default_block_state_,
            .default_fluid = default_fluid_state_,
            .lava = lava_state_,
        };
        terrain::fill_proto_chunk(*chunk, fill_options,
                                  terrain::proto_fill_volume{
                                      .min_y = shared_->generation_min_y,
                                      .height = shared_->generation_height,
                                  },
                                  *shared_->settings.router.final_density,
                                  fill_context, *aquifer.aquifer);
    }

    if (!stages.surface && stages.carvers == nullptr) {
        chunk->set_status(chunk_status::terrain);
        return chunk;
    }

    // The scan for the highest occupied section and the rest of the surface
    // inputs are timed apart because only the scan is proportional to the
    // chunk height. The surface pass and the carvers follow under their own
    // timers.
    jint max_block_y = profile_->min_y;
    {
        const stage_timer timer{stages.profile, "terrain.surface_scan"};
        // The surface rule resolves its anchors against the generation depth,
        // so the nether roof lines up with `below_top` of 128, not 256.
        for (jint y = profile_->min_y + profile_->height - 1;
             y >= profile_->min_y; --y) {
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
    }
    stage_timer prepare{stages.profile, "terrain.surface_prepare"};

    const surface::generation_bounds bounds{
        .min_gen_y = profile_->min_y,
        .gen_depth = shared_->generation_height,
        .sea_level = profile_->sea_level,
        .min_y = profile_->min_y,
        .max_block_y = max_block_y,
        .chunk_min_x = pos.x * 16,
        .chunk_min_z = pos.z * 16,
    };
    surface::surface_system system{rule_states_,         bounds,
                                   shared_->noises,      *root_factory_,
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
        return shared_->settings.router.chunk_surface_level->sample_value(
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
    providers.noise = [this](std::string_view id) {
        return shared_->noises.get(id);
    };
    providers.density =
        [this](std::string_view id) -> result<density::sampler_ptr> {
        return density::decode_density("\"" + std::string{id} + "\"",
                                       shared_->settings.documents);
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
        const auto *climate = shared_->climates.get(biome);
        if (climate == nullptr) {
            return false;
        }
        return shared_->temperatures.temperature_at(
                   *climate, x, y, z, profile_->sea_level) < 0.15F;
    };
    providers.possible_biomes = &shared_->possible_biomes;

    if (rule_context_.has_value()) {
        rule_context_->rebind(rule_states_, bounds, providers,
                              sampler_context_);
    } else {
        rule_context_.emplace(rule_states_, bounds, providers,
                              sampler_context_);
    }
    if (!compiled_rule_.has_value()) {
        surface::rule_compiler compiler{shared_->documents, *rule_context_};
        auto compiled = compiler.compile(shared_->material_rule);
        if (!compiled) {
            return std::unexpected{compiled.error()};
        }
        compiled_rule_ = std::move(*compiled);
    }
    auto &rule_context = *rule_context_;
    const auto &rule = *compiled_rule_;
    prepare.stop();
    if (stages.surface) {
        const stage_timer timer{stages.profile, "terrain.surface"};
        if (const auto built = system.build_surface(
                *chunk, rule_context, rule, shared_->climates,
                shared_->temperatures, providers.biome);
            !built) {
            return std::unexpected{built.error()};
        }
    }
    if (stages.carvers != nullptr) {
        const stage_timer timer{stages.profile, "terrain.carver"};
        carver::carver_generator generator{
            [this](jint quart_x, jint quart_z) -> std::string_view {
                return biome_name(biome_at_quart(quart_x, 0, quart_z));
            },
            *stages.carvers, config_.seed,
            provider::generation_context{.min_y = shared_->generation_min_y,
                                         .height = shared_->generation_height,
                                         .sea_level = profile_->sea_level}};
        const auto mask = generator.generate(pos);
        carver::carve_apply_context apply{
            .aquifer = aquifer.aquifer.get(),
            .states = states_.get(),
            .top_material = {},
            .filter = {},
        };
        if (stages.surface) {
            apply.top_material =
                [&system, &rule_context, &rule,
                 &chunk](jint block_x, jint block_y, jint block_z,
                         bool under_fluid) -> std::optional<surface::state_id> {
                return system.top_material(
                    rule_context, rule, block_x, block_y, block_z, under_fluid,
                    [&chunk](jint x, jint z) {
                        return chunk->first_available(
                                   heightmap_kind::world_surface_wg, x, z) -
                               1;
                    });
            };
        }
        carver::apply_carving_mask(*chunk, mask, apply);
    }
    chunk->set_status(chunk_status::terrain);
    return chunk;
}

struct generation_materialization {
    std::string root{};

    ~generation_materialization() { remove_tree(root); }

    generation_materialization() = default;
    generation_materialization(const generation_materialization &) = delete;
    auto operator=(const generation_materialization &)
        -> generation_materialization & = delete;
    generation_materialization(generation_materialization &&) = delete;
    auto operator=(generation_materialization &&)
        -> generation_materialization & = delete;
};

namespace {

/// The tag values of one merged block tag record, spelled the way
/// `load_block_tags` spells them.
auto block_tag_entries(const data::tag_record &record)
    -> std::vector<std::string> {
    std::vector<std::string> entries;
    entries.reserve(record.values.size());
    for (const auto &value : record.values) {
        if (value.tag_reference) {
            entries.push_back("#" + value.id.to_string());
        } else {
            entries.push_back(value.id.to_string());
        }
    }
    return entries;
}

/// Append the tags of `source` to `destination`, keeping the destination's
/// values first (they come from the lower-priority installed datapack).
void append_block_tags(const block_tag_set &source,
                       block_tag_set &destination) {
    for (const auto &[tag, entries] : source.entries()) {
        std::vector<std::string> merged{entries};
        if (const auto found{destination.entries().find(tag)};
            found != destination.entries().end()) {
            merged = found->second;
            merged.append_range(entries);
        }
        destination.add(tag, std::move(merged));
    }
}

/// A process-unique temporary directory name; the materialized tree is owned
/// by one generation and removed by its `generation_materialization` token.
[[nodiscard]] auto materialization_root() -> std::string {
    static std::atomic<std::uint64_t> sequence{0};
    return fmt::format("{}/refrain-content-generation-{}-{}",
                       core::io::temp_directory(), platform::process_id(),
                       sequence.fetch_add(1));
}

} // namespace

generation_data::generation_data(
    std::shared_ptr<const data::content_view> view,
    std::shared_ptr<const data::registry_generation> registries,
    std::uint64_t view_generation, std::uint64_t registry_generation)
    : view_{std::move(view)}, registries_{std::move(registries)},
      view_generation_{view_generation},
      registry_generation_{registry_generation} {}

generation_data::~generation_data() = default;

auto generation_data::build(
    std::shared_ptr<const data::content_view> view,
    std::shared_ptr<const data::registry_generation> registries,
    std::uint64_t view_generation, std::uint64_t registry_generation,
    std::string_view base_data_root, std::string_view base_templates_root)
    -> result<std::shared_ptr<const generation_data>> {
    if (view == nullptr) {
        return std::unexpected{
            fail("a content generation needs a published content view")};
    }
    if (registries == nullptr) {
        registries = std::make_shared<const data::registry_generation>(
            view->registries());
    }
    auto generation{std::shared_ptr<generation_data>{
        new generation_data{std::move(view), std::move(registries),
                            view_generation, registry_generation}}};
    bool has_generation_input{false};
    bool has_block_tags{false};
    for (const auto &file : generation->view_->packs().list()) {
        has_generation_input =
            has_generation_input || is_generation_input_path(file.path);
        has_block_tags = has_block_tags || is_block_tag_path(file.path);
        if (has_generation_input && has_block_tags) {
            break;
        }
    }
    if (has_generation_input) {
        // The view changes a document chunk generation reads: compose the
        // merged datapack once for every generator bound to this generation.
        auto holder{std::make_shared<generation_materialization>()};
        holder->root = materialization_root();
        const auto data_target{holder->root + "/data"};
        if (auto created{core::io::create_directories(data_target)}; !created) {
            return std::unexpected{created.error()};
        }
        if (!base_data_root.empty() && core::io::is_directory(base_data_root)) {
            if (auto copied{copy_tree(base_data_root, data_target)}; !copied) {
                return std::unexpected{copied.error()};
            }
        }
        if (!base_templates_root.empty() &&
            core::io::is_directory(base_templates_root)) {
            if (auto copied{copy_tree(base_templates_root, data_target)};
                !copied) {
                return std::unexpected{copied.error()};
            }
        }
        // The worldgen loaders read the reports and the block table from the
        // installed root that contains the data directory, not from the data
        // directory itself. Materialize those siblings so the merged root keeps
        // the layout the loaders expect.
        if (!base_data_root.empty()) {
            const auto installed_root{
                core::io::parent_directory(base_data_root)};
            const auto reports_source{installed_root + "/reports"};
            if (core::io::is_directory(reports_source)) {
                if (auto copied{
                        copy_tree(reports_source, holder->root + "/reports")};
                    !copied) {
                    return std::unexpected{copied.error()};
                }
            }
            const auto table_source{installed_root + "/tables.rvtb"};
            if (core::io::is_regular_file(table_source)) {
                auto bytes{core::io::read_file(table_source)};
                if (!bytes) {
                    return std::unexpected{bytes.error()};
                }
                if (auto written{core::io::write_file(
                        holder->root + "/tables.rvtb", *bytes)};
                    !written) {
                    return std::unexpected{written.error()};
                }
            }
        }
        for (const auto &file : generation->view_->packs().list()) {
            if (!file.path.starts_with("data/")) {
                continue;
            }
            auto bytes{generation->view_->packs().read(file.path)};
            if (!bytes) {
                return std::unexpected{bytes.error()};
            }
            const auto destination{holder->root + "/" + file.path};
            if (auto created{core::io::create_directories(
                    core::io::parent_directory(destination))};
                !created) {
                return std::unexpected{created.error()};
            }
            if (auto written{core::io::write_file(destination, *bytes)};
                !written) {
                return std::unexpected{written.error()};
            }
        }
        generation->materialized_ = std::move(holder);
        generation->data_root_ = data_target;
        generation->templates_root_ = data_target;
    } else {
        generation->data_root_ = std::string{base_data_root};
        generation->templates_root_ = std::string{base_templates_root};
    }
    block_tag_set tags{};
    if (!base_data_root.empty() && core::io::is_directory(base_data_root)) {
        auto loaded{load_block_tags(base_data_root)};
        if (!loaded) {
            return std::unexpected{loaded.error()};
        }
        tags = std::move(*loaded);
    }
    if (has_block_tags) {
        if (const auto *view_tags{generation->view_->tags()};
            view_tags != nullptr) {
            block_tag_set overlay{};
            for (const auto *record : view_tags->tags_of("block")) {
                overlay.add(record->id.to_string(), block_tag_entries(*record));
            }
            append_block_tags(overlay, tags);
        }
    }
    generation->block_tags_ = std::move(tags);
    generation->overlays_generation_content_ =
        has_generation_input || has_block_tags;
    return std::shared_ptr<const generation_data>{std::move(generation)};
}

} // namespace refrain::worldgen
