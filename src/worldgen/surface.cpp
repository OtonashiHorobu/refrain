// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/surface.hpp>

#include <refrain/worldgen/mth.hpp>

#include <algorithm>
#include <cmath>
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
    if (name == "minecraft:water" || name == "minecraft:lava") {
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
    return intern(name, default_properties(name));
}

auto block_states::intern(std::string_view name, state_properties properties)
    -> state_id {
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
    const auto found = index_.find(std::string{name});
    return found == index_.end() ? std::nullopt
                                 : std::optional<state_id>{found->second};
}

auto block_states::name(state_id id) const -> std::string_view {
    return names_[id];
}

auto block_states::properties(state_id id) const -> state_properties {
    return properties_[id];
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
        return std::unexpected{fail("temperature condition is not supported")};
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
