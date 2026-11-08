// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/terrain/surface.hpp>

#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/chunk/proto_chunk.hpp>

#include <refrain/core/io.hpp>
#include <refrain/data/json.hpp>
#include <refrain/worldgen/core/mth.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace refrain::worldgen::surface {

namespace {

auto fail(std::string_view message) -> error {
    return make_error(error_code::parse_error, message);
}

auto default_properties(std::string_view name) -> state_properties {
    if (name == "minecraft:air" || name == "minecraft:cave_air" ||
        name == "minecraft:void_air") {
        return state_properties{.air = true};
    }
    if (name == "minecraft:water") {
        return state_properties{.fluid = true, .water = true};
    }
    if (name == "minecraft:lava") {
        return state_properties{.fluid = true};
    }
    return state_properties{};
}

/// Java `LazyXZCondition`: recompute only when the column changed.
auto lazy_xz(material_context &context, std::function<bool()> compute)
    -> condition_evaluator {
    struct cached {
        material_context *context{nullptr};
        std::function<bool()> compute{};
        jlong last_update{0};
        bool result{false};
    };
    auto state = std::make_shared<cached>();
    state->context = &context;
    state->compute = std::move(compute);
    state->last_update = context.last_update_xz() - 1;
    return [state]() -> bool {
        if (state->context->last_update_xz() == state->last_update) {
            return state->result;
        }
        state->last_update = state->context->last_update_xz();
        state->result = state->compute();
        return state->result;
    };
}

/// Java `LazyYCondition`: recompute only when the block position changed.
auto lazy_y(material_context &context, std::function<bool()> compute)
    -> condition_evaluator {
    struct cached {
        material_context *context{nullptr};
        std::function<bool()> compute{};
        jlong last_update{0};
        bool result{false};
    };
    auto state = std::make_shared<cached>();
    state->context = &context;
    state->compute = std::move(compute);
    state->last_update = context.last_update_y() - 1;
    return [state]() -> bool {
        if (state->context->last_update_y() == state->last_update) {
            return state->result;
        }
        state->last_update = state->context->last_update_y();
        state->result = state->compute();
        return state->result;
    };
}

auto contains_biome(const std::vector<std::string> &keys, std::string_view key)
    -> bool {
    return std::any_of(
        keys.begin(), keys.end(),
        [key](const std::string &entry) { return entry == key; });
}

} // namespace

auto material_context::density_cache::value_at(jint x, jint y, jint z) const
    -> float {
    const jint max_x = min_x + static_cast<jint>(size_x) - 1;
    const jint max_y = min_y + static_cast<jint>(size_y) - 1;
    const jint max_z = min_z + static_cast<jint>(size_z) - 1;
    if (x < min_x || x > max_x || y < min_y || y > max_y || z < min_z ||
        z > max_z) {
        return sampler->sample_value(*sampler_context, x, y, z);
    }
    const auto index = static_cast<std::size_t>(y - min_y) +
                       (static_cast<std::size_t>(x - min_x) +
                        static_cast<std::size_t>(z - min_z) * size_x) *
                           size_y;
    return values[index];
}

auto block_states::intern(std::string_view name) -> state_id {
    if (registry_ != nullptr) {
        if (const auto found = registry_->find(name); found.has_value()) {
            return *found;
        }
        const auto block = name.substr(0, name.find('['));
        const bool non_solid =
            block == "minecraft:end_portal" ||
            block == "minecraft:end_gateway" || block == "minecraft:fire" ||
            block == "minecraft:torch" || block == "minecraft:wall_torch" ||
            block == "minecraft:soul_torch" ||
            block == "minecraft:soul_wall_torch" ||
            block == "minecraft:glow_lichen" || block == "minecraft:sculk_vein";
        const auto id = registry_->intern(
            name, state_traits{.air = false,
                               .blocks_motion = !non_solid,
                               .blocks_motion_no_leaves =
                                   !non_solid && !block.ends_with("_leaves")});
        return id.has_value() ? *id : no_state;
    }
    return intern(name, default_properties(name));
}

auto block_states::intern(std::string_view name, state_properties properties)
    -> state_id {
    if (registry_ != nullptr) {
        if (const auto found = registry_->find(name); found.has_value()) {
            return *found;
        }
        const auto id = registry_->intern(
            name,
            state_traits{.air = properties.air,
                         .fluid = properties.fluid,
                         .water = properties.water,
                         .blocks_motion = !properties.air && !properties.fluid,
                         .blocks_motion_no_leaves =
                             !properties.air && !properties.fluid});
        return id.has_value() ? *id : no_state;
    }
    const std::string key{name};
    if (const auto found = index_.find(key); found != index_.end()) {
        return found->second;
    }
    if (names_.size() >= no_state) {
        return no_state;
    }
    const auto id = static_cast<state_id>(names_.size());
    names_.push_back(key);
    properties_.push_back(properties);
    index_.emplace(key, id);
    return id;
}

auto block_states::find(std::string_view name) const
    -> std::optional<state_id> {
    if (registry_ != nullptr) {
        return registry_->find(name);
    }
    const auto found = index_.find(std::string{name});
    return found == index_.end() ? std::nullopt
                                 : std::optional<state_id>{found->second};
}

auto block_states::name(state_id id) const -> std::string_view {
    if (registry_ != nullptr) {
        return registry_->name(id);
    }
    return names_[id];
}

auto block_states::properties(state_id id) const -> state_properties {
    if (registry_ != nullptr) {
        const auto traits = registry_->traits(id);
        return state_properties{
            .air = traits.air, .fluid = traits.fluid, .water = traits.water};
    }
    return properties_[id];
}

auto block_states::size() const noexcept -> std::size_t {
    return registry_ != nullptr ? registry_->size() : names_.size();
}

material_context::material_context(
    block_states &states, generation_bounds bounds,
    material_providers providers,
    const density::sampler_context &density_context)
    : states_{&states}, bounds_{bounds}, providers_{std::move(providers)},
      density_context_{&density_context} {}

void material_context::update_xz(jint block_x, jint block_z,
                                 jint surface_gradient_x,
                                 jint surface_gradient_z) {
    ++last_update_xz_;
    ++last_update_y_;
    block_x_ = block_x;
    block_z_ = block_z;
    surface_gradient_x_ = surface_gradient_x;
    surface_gradient_z_ = surface_gradient_z;
    surface_depth_ = providers_.surface_depth(block_x, block_z);
}

void material_context::update_y(jint stone_depth_above, jint stone_depth_below,
                                jint water_height, jint block_y) {
    ++last_update_y_;
    biome_.reset();
    block_y_ = block_y;
    water_height_ = water_height;
    stone_depth_below_ = stone_depth_below;
    stone_depth_above_ = stone_depth_above;
}

auto material_context::biome() -> std::string_view {
    if (!biome_.has_value()) {
        biome_ = std::string{providers_.biome(block_x_, block_y_, block_z_)};
    }
    return *biome_;
}

auto material_context::surface_secondary() -> double {
    if (last_surface_secondary_update_ != last_update_xz_) {
        last_surface_secondary_update_ = last_update_xz_;
        surface_secondary_ = providers_.surface_secondary(block_x_, block_z_);
    }
    return surface_secondary_;
}

auto material_context::min_surface_level() -> jint {
    if (last_min_surface_update_ != last_update_xz_) {
        last_min_surface_update_ = last_update_xz_;
        min_surface_level_ =
            mth::floor(static_cast<double>(
                providers_.preliminary_surface(block_x_, block_z_))) +
            surface_depth_ - 8;
    }
    return min_surface_level_;
}

auto material_context::band(jint x, jint y, jint z) -> state_id {
    return providers_.band(x, y, z);
}

auto material_context::resolve_anchor(const vertical_anchor &anchor) const
    -> jint {
    return anchor.resolve(bounds_.min_gen_y, bounds_.gen_depth,
                          bounds_.sea_level);
}

auto material_context::random_factory(std::string_view name)
    -> positional_random_factory & {
    return providers_.random_factory(name);
}

auto material_context::cold_enough_to_snow(std::string_view biome) -> bool {
    return providers_.cold_enough_to_snow(biome, block_x_, block_y_, block_z_);
}

auto material_context::noise_sampler(std::string_view id, bool is_3d)
    -> result<std::function<double()>> {
    std::string key{id};
    key += is_3d ? "#3d" : "#2d";
    if (const auto found = noise_samplers_.find(key);
        found != noise_samplers_.end()) {
        auto cache = found->second;
        return std::function<double()>([cache]() {
            auto *context = cache->context;
            const jlong now = cache->is_3d ? context->last_update_y_
                                           : context->last_update_xz_;
            if (cache->last_update != now) {
                cache->last_update = now;
                cache->value = cache->is_3d
                                   ? cache->stack->get(context->block_x_,
                                                       context->block_y_,
                                                       context->block_z_)
                                   : cache->stack->get(context->block_x_, 0.0,
                                                       context->block_z_);
            }
            return cache->value;
        });
    }
    const auto *stack = providers_.noise(id);
    if (stack == nullptr) {
        return std::unexpected{fail("unknown noise: " + std::string{id})};
    }
    auto cache = std::make_shared<noise_cache>();
    cache->stack = stack;
    cache->is_3d = is_3d;
    cache->context = this;
    cache->last_update = (is_3d ? last_update_y_ : last_update_xz_) - 1;
    noise_samplers_.emplace(std::move(key), cache);
    return std::function<double()>([cache]() {
        auto *context = cache->context;
        const jlong now =
            cache->is_3d ? context->last_update_y_ : context->last_update_xz_;
        if (cache->last_update != now) {
            cache->last_update = now;
            cache->value =
                cache->is_3d
                    ? cache->stack->get(context->block_x_, context->block_y_,
                                        context->block_z_)
                    : cache->stack->get(context->block_x_, 0.0,
                                        context->block_z_);
        }
        return cache->value;
    });
}

auto material_context::densities_in_chunk(std::string_view id, bool prefill)
    -> result<std::function<float(jint, jint, jint)>> {
    std::string key{id};
    key += prefill ? "#prefill" : "#scalar";
    if (const auto found = density_samplers_.find(key);
        found != density_samplers_.end()) {
        auto cache = found->second;
        return std::function<float(jint, jint, jint)>(
            [cache](jint x, jint y, jint z) -> float {
                if (!cache->prefill) {
                    return cache->sampler->sample_value(*cache->sampler_context,
                                                        x, y, z);
                }
                return cache->value_at(x, y, z);
            });
    }
    auto sampler = providers_.density(id);
    if (!sampler) {
        return std::unexpected{sampler.error()};
    }
    auto cache = std::make_shared<density_cache>();
    cache->sampler = std::move(*sampler);
    cache->prefill = prefill;
    cache->sampler_context = density_context_;
    if (prefill) {
        cache->size_x = 16;
        cache->size_z = 16;
        cache->size_y =
            static_cast<std::size_t>(bounds_.max_block_y - bounds_.min_y + 1);
        cache->min_x = bounds_.chunk_min_x;
        cache->min_y = bounds_.min_y;
        cache->min_z = bounds_.chunk_min_z;
        const volume vol{static_cast<jint>(cache->size_x),
                         static_cast<jint>(cache->size_y),
                         static_cast<jint>(cache->size_z),
                         cache->min_x,
                         cache->min_y,
                         cache->min_z};
        cache->values.resize(vol.size());
        buffer values{vol.size()};
        cache->sampler->sample_volume(*density_context_, values, vol);
        std::copy(values.data(), values.data() + values.size(),
                  cache->values.begin());
    }
    density_samplers_.emplace(std::move(key), cache);
    return std::function<float(jint, jint, jint)>(
        [cache](jint x, jint y, jint z) -> float {
            if (!cache->prefill) {
                return cache->sampler->sample_value(*cache->sampler_context, x,
                                                    y, z);
            }
            return cache->value_at(x, y, z);
        });
}

namespace {

auto find_rule_text(const material_documents &documents, std::string_view id)
    -> const std::string * {
    for (const auto &[key, text] : documents.rules) {
        if (key == id) {
            return &text;
        }
    }
    return nullptr;
}

auto find_condition_text(const material_documents &documents,
                         std::string_view id) -> const std::string * {
    for (const auto &[key, text] : documents.conditions) {
        if (key == id) {
            return &text;
        }
    }
    return nullptr;
}

} // namespace

rule_compiler::rule_compiler(const material_documents &documents,
                             material_context &context)
    : documents_{documents}, context_{context} {}

auto rule_compiler::resolve_rule(std::string_view id) -> result<const rule *> {
    const std::string key{id};
    if (const auto found = rule_cache_.find(key); found != rule_cache_.end()) {
        return &found->second;
    }
    if (active_rules_.contains(key)) {
        return std::unexpected{fail("material rule reference cycle")};
    }
    const auto *text = find_rule_text(documents_, id);
    if (text == nullptr) {
        return std::unexpected{fail("unknown material rule: " + key)};
    }
    active_rules_.insert(key);
    auto decoded = decode_rule(*text);
    active_rules_.erase(key);
    if (!decoded) {
        return std::unexpected{decoded.error()};
    }
    auto [entry, inserted] = rule_cache_.emplace(key, std::move(*decoded));
    (void)inserted;
    return &entry->second;
}

auto rule_compiler::resolve_condition(std::string_view id)
    -> result<const condition *> {
    const std::string key{id};
    if (const auto found = condition_cache_.find(key);
        found != condition_cache_.end()) {
        return &found->second;
    }
    if (active_conditions_.contains(key)) {
        return std::unexpected{fail("material condition reference cycle")};
    }
    const auto *text = find_condition_text(documents_, id);
    if (text == nullptr) {
        return std::unexpected{fail("unknown material condition: " + key)};
    }
    active_conditions_.insert(key);
    auto decoded = decode_condition(*text);
    active_conditions_.erase(key);
    if (!decoded) {
        return std::unexpected{decoded.error()};
    }
    auto [entry, inserted] = condition_cache_.emplace(key, std::move(*decoded));
    (void)inserted;
    return &entry->second;
}

auto rule_compiler::compile_condition(const condition &value)
    -> result<condition_evaluator> {
    using enum condition::kind;
    switch (value.type) {
    case reference: {
        const std::string key{value.reference};
        if (compiling_conditions_.contains(key)) {
            return std::unexpected{fail("material condition reference cycle")};
        }
        auto resolved = resolve_condition(key);
        if (!resolved) {
            return std::unexpected{resolved.error()};
        }
        compiling_conditions_.insert(key);
        auto compiled = compile_condition(**resolved);
        compiling_conditions_.erase(key);
        return compiled;
    }
    case biome: {
        auto keys = value.biome_is;
        const auto *possible = context_.possible_biomes();
        if (possible != nullptr) {
            const bool can_never = std::none_of(
                keys.begin(), keys.end(), [possible](const std::string &key) {
                    return possible->contains(key);
                });
            if (can_never) {
                return condition_evaluator{[]() { return false; }};
            }
            const bool always =
                std::all_of(possible->begin(), possible->end(),
                            [&keys](const std::string &key) {
                                return contains_biome(keys, key);
                            });
            if (always) {
                return condition_evaluator{[]() { return true; }};
            }
        }
        return lazy_y(context_,
                      [context = &context_, keys = std::move(keys)]() {
                          return contains_biome(keys, context->biome());
                      });
    }
    case noise_threshold: {
        auto sampler = context_.noise_sampler(value.noise, value.is_3d);
        if (!sampler) {
            return std::unexpected{sampler.error()};
        }
        const double min = value.min_threshold;
        const double max = value.max_threshold;
        return condition_evaluator{[sampler = std::move(*sampler), min, max]() {
            const double noise = sampler();
            return noise >= min && noise <= max;
        }};
    }
    case vertical_gradient: {
        const jint true_y = context_.resolve_anchor(value.true_at_and_below);
        const jint false_y = context_.resolve_anchor(value.false_at_and_above);
        auto &factory = context_.random_factory(value.random_name);
        return lazy_y(context_, [context = &context_, true_y, false_y,
                                 &factory]() {
            const jint y = context->block_y();
            if (y <= true_y) {
                return true;
            }
            if (y >= false_y) {
                return false;
            }
            const double probability =
                mth::map(static_cast<double>(y), static_cast<double>(true_y),
                         static_cast<double>(false_y), 1.0, 0.0);
            auto random = factory.at(context->block_x(), y, context->block_z());
            return random->next_float() < probability;
        });
    }
    case y_above: {
        const jint anchor_y = context_.resolve_anchor(value.true_at_and_below);
        const jint multiplier = value.surface_depth_multiplier;
        const bool add_stone = value.add_stone_depth;
        return lazy_y(
            context_, [context = &context_, anchor_y, multiplier, add_stone]() {
                return context->block_y() +
                           (add_stone ? context->stone_depth_above() : 0) >=
                       anchor_y + context->surface_depth() * multiplier;
            });
    }
    case water: {
        const jint offset = value.offset;
        const jint multiplier = value.surface_depth_multiplier;
        const bool add_stone = value.add_stone_depth;
        return lazy_y(context_, [context = &context_, offset, multiplier,
                                 add_stone]() {
            if (context->water_height() == std::numeric_limits<jint>::min()) {
                return true;
            }
            return context->block_y() +
                       (add_stone ? context->stone_depth_above() : 0) >=
                   context->water_height() + offset +
                       context->surface_depth() * multiplier;
        });
    }
    case stone_depth: {
        const bool ceiling = value.surface == cave_surface::ceiling;
        const jint offset = value.offset;
        const bool add_surface = value.add_surface_depth;
        const jint range = value.secondary_depth_range;
        return lazy_y(context_, [context = &context_, ceiling, offset,
                                 add_surface, range]() {
            const jint stone = ceiling ? context->stone_depth_below()
                                       : context->stone_depth_above();
            const jint surface = add_surface ? context->surface_depth() : 0;
            const jint secondary =
                range == 0
                    ? 0
                    : static_cast<jint>(mth::map(context->surface_secondary(),
                                                 -1.0, 1.0, 0.0,
                                                 static_cast<double>(range)));
            return stone <= 1 + offset + surface + secondary;
        });
    }
    case steep:
        return lazy_xz(context_, [context = &context_]() {
            return context->surface_gradient_x() <= -4 ||
                   context->surface_gradient_z() >= 4;
        });
    case hole:
        return lazy_xz(context_, [context = &context_]() {
            return context->surface_depth() <= 0;
        });
    case above_preliminary_surface:
        return condition_evaluator{[context = &context_]() {
            return context->block_y() >= context->min_surface_level();
        }};
    case temperature:
        return condition_evaluator{[context = &context_]() {
            return context->cold_enough_to_snow(context->biome());
        }};
    case not_: {
        if (value.target == nullptr) {
            return std::unexpected{fail("not condition has no target")};
        }
        auto target = compile_condition(*value.target);
        if (!target) {
            return std::unexpected{target.error()};
        }
        return condition_evaluator{
            [target = std::move(*target)]() { return !target(); }};
    }
    }
    return std::unexpected{fail("unknown material condition kind")};
}

auto rule_compiler::compile(const rule &value) -> result<rule_evaluator> {
    using enum rule::kind;
    switch (value.type) {
    case reference: {
        const std::string key{value.reference};
        if (compiling_rules_.contains(key)) {
            return std::unexpected{fail("material rule reference cycle")};
        }
        auto resolved = resolve_rule(key);
        if (!resolved) {
            return std::unexpected{resolved.error()};
        }
        compiling_rules_.insert(key);
        auto compiled = compile(**resolved);
        compiling_rules_.erase(key);
        return compiled;
    }
    case block: {
        const state_id id = context_.states().intern(value.state);
        return rule_evaluator{
            [id](jint, jint, jint) { return std::optional<state_id>{id}; }};
    }
    case bandlands:
        return rule_evaluator{[context = &context_](jint x, jint y, jint z) {
            return std::optional<state_id>{context->band(x, y, z)};
        }};
    case sequence: {
        std::vector<rule_evaluator> children;
        children.reserve(value.sequence.size());
        for (const auto &entry : value.sequence) {
            auto compiled = compile(entry);
            if (!compiled) {
                return std::unexpected{compiled.error()};
            }
            children.push_back(std::move(*compiled));
        }
        return rule_evaluator{
            [children = std::move(children)](jint x, jint y, jint z) {
                for (const auto &child : children) {
                    if (auto state = child(x, y, z); state.has_value()) {
                        return state;
                    }
                }
                return std::optional<state_id>{};
            }};
    }
    case condition: {
        if (value.if_true == nullptr || value.then_run == nullptr) {
            return std::unexpected{fail("condition rule is incomplete")};
        }
        auto condition = compile_condition(*value.if_true);
        if (!condition) {
            return std::unexpected{condition.error()};
        }
        auto next = compile(*value.then_run);
        if (!next) {
            return std::unexpected{next.error()};
        }
        return rule_evaluator{
            [condition = std::move(*condition),
             next = std::move(*next)](jint x, jint y, jint z) {
                return condition() ? next(x, y, z) : std::optional<state_id>{};
            }};
    }
    case ore_vein: {
        auto density = context_.densities_in_chunk(value.density, true);
        if (!density) {
            return std::unexpected{density.error()};
        }
        auto richness = context_.densities_in_chunk(value.richness, true);
        if (!richness) {
            return std::unexpected{richness.error()};
        }
        auto filler_gap = context_.densities_in_chunk(value.filler_gap, false);
        if (!filler_gap) {
            return std::unexpected{filler_gap.error()};
        }
        const state_id ore = context_.states().intern(value.ore_block);
        const state_id raw = context_.states().intern(value.raw_ore_block);
        const state_id filler = context_.states().intern(value.filler_block);
        const float chance = value.raw_ore_chance;
        auto &factory = context_.random_factory("minecraft:ore");
        return rule_evaluator{
            [density = std::move(*density), richness = std::move(*richness),
             filler_gap = std::move(*filler_gap), ore, raw, filler, chance,
             &factory](jint x, jint y, jint z) -> std::optional<state_id> {
                const float current_density = density(x, y, z);
                if (current_density <= 0.0F) {
                    return std::nullopt;
                }
                auto random = factory.at(x, y, z);
                if (random->next_float() > current_density) {
                    return std::nullopt;
                }
                const float current_richness = richness(x, y, z);
                if (random->next_float() < current_richness &&
                    filler_gap(x, y, z) < 0.0F) {
                    return random->next_float() < chance ? raw : ore;
                }
                return filler;
            }};
    }
    }
    return std::unexpected{fail("unknown material rule kind")};
}

} // namespace refrain::worldgen::surface

namespace refrain::worldgen::surface {

namespace {

using noise_json = data::json::value;

auto noise_double(const noise_json &node, std::string_view key, double fallback)
    -> double {
    const noise_json *found = node.find(key);
    if (found == nullptr) {
        return fallback;
    }
    const auto value = found->as_double();
    return value ? *value : fallback;
}

auto noise_int(const noise_json &node, std::string_view key, jint fallback)
    -> jint {
    return static_cast<jint>(
        noise_double(node, key, static_cast<double>(fallback)));
}

auto noise_bool(const noise_json &node, std::string_view key, bool fallback)
    -> bool {
    const noise_json *found = node.find(key);
    if (found == nullptr) {
        return fallback;
    }
    const auto value = found->as_bool();
    return value ? *value : fallback;
}

auto collect_noise_documents(
    std::string_view data_root,
    std::vector<std::pair<std::string, std::string>> &out) -> status {
    auto namespaces = core::io::list_directory(data_root);
    if (!namespaces) {
        return std::unexpected{namespaces.error()};
    }
    for (const auto &name : *namespaces) {
        const std::string base =
            std::string{data_root} + "/" + name + "/worldgen/noise";
        if (!core::io::is_directory(base)) {
            continue;
        }
        std::vector<std::pair<std::string, std::string>> pending;
        auto walk = [&](auto &&self, std::string_view path,
                        std::string_view prefix) -> status {
            auto entries = core::io::list_directory(path);
            if (!entries) {
                return std::unexpected{entries.error()};
            }
            for (const auto &entry : *entries) {
                const std::string child = std::string{path} + "/" + entry;
                const std::string id_path =
                    prefix.empty() ? entry : std::string{prefix} + "/" + entry;
                if (core::io::is_directory(child)) {
                    if (auto walked = self(self, child, id_path); !walked) {
                        return walked;
                    }
                } else if (entry.ends_with(".json")) {
                    auto text = core::io::read_text(child);
                    if (!text) {
                        return std::unexpected{text.error()};
                    }
                    pending.emplace_back(
                        name + ":" + id_path.substr(0, id_path.size() - 5),
                        std::move(*text));
                }
            }
            return status_ok();
        };
        if (auto walked = walk(walk, base, ""); !walked) {
            return walked;
        }
        std::sort(pending.begin(), pending.end());
        for (auto &entry : pending) {
            out.push_back(std::move(entry));
        }
    }
    return status_ok();
}

} // namespace

noise_registry::noise_registry(algorithm kind, jlong seed)
    : factory_{new_positional_factory(kind, seed)} {}

void noise_registry::add(std::string id, std::string_view json) {
    auto parsed = noise_json::parse(json);
    if (!parsed) {
        return;
    }
    normal_noise_parameters parameters;
    parameters.base_amplitude = noise_double(*parsed, "base_amplitude", 1.0);
    parameters.base_octave = noise_int(*parsed, "base_octave", 0);
    parameters.octave_count = noise_int(*parsed, "octave_count", 1);
    parameters.normalize = noise_bool(*parsed, "normalize", true);
    if (const noise_json *modifiers = parsed->find("amplitude_modifiers");
        modifiers != nullptr && modifiers->is_array()) {
        for (const auto &entry : *modifiers->as_array()) {
            if (const auto value = entry.as_double()) {
                parameters.amplitude_modifiers.push_back(*value);
            }
        }
    }
    auto source = factory_->from_hash_of(id);
    noises_.emplace(std::move(id), normal_noise{parameters}.create(*source));
}

auto noise_registry::load(std::string_view data_root) -> status {
    std::vector<std::pair<std::string, std::string>> documents;
    if (auto loaded = collect_noise_documents(data_root, documents); !loaded) {
        return loaded;
    }
    for (auto &[id, text] : documents) {
        add(id, text);
    }
    return status_ok();
}

auto noise_registry::get(std::string_view id) -> const noise_stack * {
    const auto found = noises_.find(std::string{id});
    return found == noises_.end() ? nullptr : &found->second;
}

surface_system::surface_system(block_states &states, generation_bounds bounds,
                               noise_registry &noises,
                               positional_random_factory &noise_random,
                               state_id default_block, state_id air)
    : states_{&states}, bounds_{bounds}, noises_{&noises},
      noise_random_{&noise_random}, default_block_{default_block}, air_{air},
      surface_noise_{noises.get("minecraft:surface")},
      surface_secondary_noise_{noises.get("minecraft:surface_secondary")},
      clay_bands_offset_noise_{noises.get("minecraft:clay_bands_offset")},
      badlands_surface_noise_{noises.get("minecraft:badlands_surface")},
      badlands_pillar_noise_{noises.get("minecraft:badlands_pillar")},
      badlands_pillar_roof_noise_{noises.get("minecraft:badlands_pillar_roof")},
      iceberg_surface_noise_{noises.get("minecraft:iceberg_surface")},
      iceberg_pillar_noise_{noises.get("minecraft:iceberg_pillar")},
      iceberg_pillar_roof_noise_{noises.get("minecraft:iceberg_pillar_roof")} {
    const state_id terracotta = states.intern("minecraft:terracotta");
    const state_id orange = states.intern("minecraft:orange_terracotta");
    const state_id yellow = states.intern("minecraft:yellow_terracotta");
    const state_id brown = states.intern("minecraft:brown_terracotta");
    const state_id red = states.intern("minecraft:red_terracotta");
    const state_id white = states.intern("minecraft:white_terracotta");
    const state_id light_gray =
        states.intern("minecraft:light_gray_terracotta");
    snow_block_ = states.intern("minecraft:snow_block");
    packed_ice_ = states.intern("minecraft:packed_ice");
    clay_bands_.fill(terracotta);
    auto random = noise_random_->from_hash_of("minecraft:clay_bands");
    for (jint i = 0; i < static_cast<jint>(clay_bands_.size()); ++i) {
        i += random->next_int(5) + 1;
        if (i < static_cast<jint>(clay_bands_.size())) {
            clay_bands_[static_cast<std::size_t>(i)] = orange;
        }
    }
    auto make_bands = [&](jint base_width, state_id state) {
        const jint count = random->next_int(15 - 6 + 1) + 6;
        for (jint i = 0; i < count; ++i) {
            const jint width = base_width + random->next_int(3);
            const jint start =
                random->next_int(static_cast<jint>(clay_bands_.size()));
            for (jint position = 0;
                 start + position < static_cast<jint>(clay_bands_.size()) &&
                 position < width;
                 ++position) {
                clay_bands_[static_cast<std::size_t>(start + position)] = state;
            }
        }
    };
    make_bands(1, yellow);
    make_bands(2, brown);
    make_bands(1, red);
    const jint white_bands = random->next_int(15 - 9 + 1) + 9;
    jint painted = 0;
    for (jint start = 0;
         painted < white_bands && start < static_cast<jint>(clay_bands_.size());
         start += random->next_int(16) + 4) {
        clay_bands_[static_cast<std::size_t>(start)] = white;
        if (start - 1 > 0 && random->next_boolean()) {
            clay_bands_[static_cast<std::size_t>(start - 1)] = light_gray;
        }
        if (start + 1 < static_cast<jint>(clay_bands_.size()) &&
            random->next_boolean()) {
            clay_bands_[static_cast<std::size_t>(start + 1)] = light_gray;
        }
        ++painted;
    }
}

auto surface_system::surface_depth(jint x, jint z) -> jint {
    const double value = static_cast<double>(surface_noise_->get(
        static_cast<double>(x), 0.0, static_cast<double>(z)));
    const double draw = noise_random_->at(x, 0, z)->next_double();
    return static_cast<jint>(value * 2.75 + 3.0 + draw * 0.25);
}

auto surface_system::surface_secondary(jint x, jint z) -> double {
    return static_cast<double>(surface_secondary_noise_->get(
        static_cast<double>(x), 0.0, static_cast<double>(z)));
}

auto surface_system::band(jint x, jint y, jint z) -> state_id {
    const float scaled =
        clay_bands_offset_noise_->get(static_cast<double>(x), 0.0,
                                      static_cast<double>(z)) *
        4.0F;
    const jint offset =
        static_cast<jint>(std::floor(static_cast<double>(scaled) + 0.5));
    const auto length = static_cast<jint>(clay_bands_.size());
    const jint index = floor_mod(y + offset + length, length);
    return clay_bands_[static_cast<std::size_t>(index)];
}

void surface_system::eroded_badlands_extension(
    jint block_x, jint block_z, jint height, jint min_y,
    const std::function<state_id(jint, jint, jint)> &get,
    const std::function<void(jint, jint, jint, state_id)> &set) {
    const double pillar_buffer = std::min(
        std::abs(static_cast<double>(badlands_surface_noise_->get(
                     static_cast<double>(block_x), 0.0,
                     static_cast<double>(block_z))) *
                 8.25),
        static_cast<double>(static_cast<float>(badlands_pillar_noise_->get(
                                static_cast<double>(block_x) * 0.2, 0.0,
                                static_cast<double>(block_z) * 0.2)) *
                            15.0F));
    if (!(pillar_buffer > 0.0)) {
        return;
    }
    const double pillar_floor =
        std::abs(static_cast<double>(badlands_pillar_roof_noise_->get(
                     static_cast<double>(block_x) * 0.75, 0.0,
                     static_cast<double>(block_z) * 0.75)) *
                 1.5);
    const double extension_top =
        64.0 + std::min(pillar_buffer * pillar_buffer * 2.5,
                        std::ceil(pillar_floor * 50.0) + 24.0);
    const jint start_y = mth::floor(extension_top);
    if (height <= start_y) {
        for (jint y = start_y; y >= min_y; --y) {
            const state_id old = get(block_x, y, block_z);
            if (old == default_block_) {
                break;
            }
            if (states_->properties(old).water) {
                return;
            }
        }
        const jint max_y = bounds_.min_y + bounds_.gen_depth - 1;
        for (jint y = start_y; y >= min_y; --y) {
            if (!states_->properties(get(block_x, y, block_z)).air) {
                break;
            }
            if (y >= bounds_.min_y && y <= max_y) {
                set(block_x, y, block_z, default_block_);
            }
        }
    }
}

auto surface_system::build_surface(
    proto_chunk &chunk, material_context &context, const rule_evaluator &rule,
    const biome_climates &climates, const biome_temperatures &temperatures,
    const std::function<std::string_view(jint, jint, jint)> &biome) -> status {
    const jint chunk_min_x = chunk.chunk_x() * 16;
    const jint chunk_min_z = chunk.chunk_z() * 16;
    auto get = [&chunk, chunk_min_x, chunk_min_z](jint x, jint y, jint z) {
        return static_cast<state_id>(
            chunk.state_at(x - chunk_min_x, y, z - chunk_min_z));
    };
    auto set = [&chunk, chunk_min_x, chunk_min_z](jint x, jint y, jint z,
                                                  state_id state) {
        chunk.set_state(x - chunk_min_x, y, z - chunk_min_z,
                        static_cast<block_state_id>(state));
    };
    auto world_surface = [&chunk](jint x, jint z) {
        return chunk.first_available(heightmap_kind::world_surface_wg, x, z) -
               1;
    };
    return build_surface(context, rule, climates, temperatures, biome, get, set,
                         world_surface);
}

auto surface_system::top_material(
    material_context &context, const rule_evaluator &rule, jint block_x,
    jint block_y, jint block_z, bool under_fluid,
    const std::function<jint(jint, jint)> &world_surface)
    -> std::optional<state_id> {
    const jint local_x = block_x - bounds_.chunk_min_x;
    const jint local_z = block_z - bounds_.chunk_min_z;
    const jint gradient_x = world_surface(std::min(local_x + 1, 15), local_z) -
                            world_surface(std::max(local_x - 1, 0), local_z);
    const jint gradient_z = world_surface(local_x, std::min(local_z + 1, 15)) -
                            world_surface(local_x, std::max(local_z - 1, 0));
    context.update_xz(block_x, block_z, gradient_x, gradient_z);
    context.update_y(
        1, 1, under_fluid ? block_y + 1 : std::numeric_limits<jint>::min(),
        block_y);
    return rule(block_x, block_y, block_z);
}

auto surface_system::build_surface(
    material_context &context, const rule_evaluator &rule,
    const biome_climates &climates, const biome_temperatures &temperatures,
    const std::function<std::string_view(jint, jint, jint)> &biome,
    const std::function<state_id(jint, jint, jint)> &get,
    const std::function<void(jint, jint, jint, state_id)> &set,
    const std::function<jint(jint, jint)> &world_surface) -> status {
    const jint min_y = bounds_.min_y;
    const jint max_y = bounds_.min_y + bounds_.gen_depth - 1;
    for (jint x = 0; x < 16; ++x) {
        for (jint z = 0; z < 16; ++z) {
            const jint block_x = bounds_.chunk_min_x + x;
            const jint block_z = bounds_.chunk_min_z + z;
            const jint starting_height = world_surface(x, z) + 1;
            const auto surface_biome = biome(block_x, starting_height, block_z);
            const biome_climate *frozen_climate = nullptr;
            if (surface_biome == "minecraft:frozen_ocean" ||
                surface_biome == "minecraft:deep_frozen_ocean") {
                frozen_climate = climates.get(surface_biome);
                if (frozen_climate == nullptr) {
                    return std::unexpected{
                        make_error(error_code::invalid_argument,
                                   "frozen biome has no climate settings")};
                }
            }
            if (surface_biome == "minecraft:eroded_badlands") {
                eroded_badlands_extension(block_x, block_z, starting_height,
                                          min_y, get, set);
            }
            const jint height = world_surface(x, z) + 1;
            const jint gradient_x = world_surface(std::min(x + 1, 15), z) -
                                    world_surface(std::max(x - 1, 0), z);
            const jint gradient_z = world_surface(x, std::min(z + 1, 15)) -
                                    world_surface(x, std::max(z - 1, 0));
            context.update_xz(block_x, block_z, gradient_x, gradient_z);
            jint stone_above = 0;
            jint water_height = std::numeric_limits<jint>::min();
            jint next_ceiling_stone_y = std::numeric_limits<jint>::max();

            for (jint y = height; y >= min_y; --y) {
                const state_id old = get(block_x, y, block_z);
                const auto old_properties = states_->properties(old);
                if (old_properties.air) {
                    stone_above = 0;
                    water_height = std::numeric_limits<jint>::min();
                } else if (old_properties.fluid) {
                    if (water_height == std::numeric_limits<jint>::min()) {
                        water_height = y + 1;
                    }
                } else {
                    if (next_ceiling_stone_y >= y) {
                        next_ceiling_stone_y = -1024;
                        for (jint lookahead = y - 1; lookahead >= min_y - 1;
                             --lookahead) {
                            const state_id next =
                                get(block_x, lookahead, block_z);
                            const auto next_properties =
                                states_->properties(next);
                            if (next_properties.air || next_properties.fluid) {
                                next_ceiling_stone_y = lookahead + 1;
                                break;
                            }
                        }
                    }
                    ++stone_above;
                    const jint stone_below = y - next_ceiling_stone_y + 1;
                    context.update_y(stone_above, stone_below, water_height, y);
                    if (y >= min_y && y <= max_y) {
                        const auto state = rule(block_x, y, block_z);
                        if (state.has_value()) {
                            set(block_x, y, block_z, *state);
                        }
                    }
                }
            }
            if (frozen_climate != nullptr) {
                frozen_ocean_extension(surface_biome, *frozen_climate,
                                       temperatures,
                                       context.min_surface_level(), block_x,
                                       block_z, starting_height, get, set);
            }
        }
    }
    return status_ok();
}

void surface_system::frozen_ocean_extension(
    std::string_view /*biome*/, const biome_climate &climate,
    const biome_temperatures &temperatures, jint min_surface_level,
    jint block_x, jint block_z, jint height,
    const std::function<state_id(jint, jint, jint)> &get,
    const std::function<void(jint, jint, jint, state_id)> &set) {
    const double iceberg = std::min(
        std::abs(static_cast<double>(iceberg_surface_noise_->get(
                     static_cast<double>(block_x), 0.0,
                     static_cast<double>(block_z))) *
                 8.25),
        static_cast<double>(static_cast<float>(iceberg_pillar_noise_->get(
                                static_cast<double>(block_x) * 1.28, 0.0,
                                static_cast<double>(block_z) * 1.28)) *
                            15.0F));
    if (!(iceberg > 1.8)) {
        return;
    }
    const double iceberg_roof =
        std::abs(static_cast<double>(iceberg_pillar_roof_noise_->get(
                     static_cast<double>(block_x) * 1.17, 0.0,
                     static_cast<double>(block_z) * 1.17)) *
                 1.5);
    double top = std::min(iceberg * iceberg * 1.2,
                          std::ceil(iceberg_roof * 40.0) + 14.0);
    if (temperatures.should_melt_iceberg(climate, block_x, bounds_.sea_level,
                                         block_z, bounds_.sea_level)) {
        top -= 2.0;
    }
    if (!(top > 2.0)) {
        return;
    }
    const auto sea_level = static_cast<double>(bounds_.sea_level);
    const double extension_bottom = sea_level - top - 7.0;
    top += sea_level;
    const double extension_top = top;
    const jint start_y = std::max(height, static_cast<jint>(top) + 1);
    auto random = noise_random_->at(block_x, 0, block_z);
    const jint max_snow_depth = 2 + random->next_int(4);
    const jint min_snow_height = bounds_.sea_level + 18 + random->next_int(10);
    jint snow_depth = 0;
    for (jint y = start_y; y >= min_surface_level; --y) {
        const auto properties = states_->properties(get(block_x, y, block_z));
        bool place = false;
        if (properties.air && y < static_cast<jint>(extension_top)) {
            place = random->next_double() > 0.01;
        }
        if (!place && properties.water &&
            y > static_cast<jint>(extension_bottom) && y < bounds_.sea_level) {
            place = random->next_double() > 0.15;
        }
        if (!place) {
            continue;
        }
        const jint max_y = bounds_.min_y + bounds_.gen_depth - 1;
        if (y < bounds_.min_y || y > max_y) {
            continue;
        }
        if (snow_depth <= max_snow_depth && y > min_snow_height) {
            set(block_x, y, block_z, snow_block_);
            ++snow_depth;
        } else {
            set(block_x, y, block_z, packed_ice_);
        }
    }
}

} // namespace refrain::worldgen::surface
