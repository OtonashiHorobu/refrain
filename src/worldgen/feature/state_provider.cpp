// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horashi

#include <refrain/worldgen/feature/state_provider.hpp>

#include <refrain/data/json.hpp>
#include <refrain/worldgen/chunk/block_state_codec.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/feature/block_predicate.hpp>
#include <refrain/worldgen/feature/placement.hpp>
#include <refrain/worldgen/noise/noise.hpp>
#include <refrain/worldgen/provider.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::worldgen::feature {

namespace {

auto resolve_state(block_state_registry &states, std::string_view key)
    -> result<block_state_id> {
    // Fluid JSON spells out the default level. The registry keys states by
    // their data spelling, so fold it to the plain block name that terrain
    // fluids use; the two are the same state in the reference.
    std::string canonical{key};
    for (const std::string_view fluid : {"minecraft:water", "minecraft:lava"}) {
        if (key == std::string{fluid} + "[level=0]") {
            canonical = std::string{fluid};
            break;
        }
    }
    if (const auto found = states.find(canonical); found.has_value()) {
        return *found;
    }
    key = canonical;
    // Take the motion traits from the block tags, which describe the shapes a
    // heightmap counts. A snow layer is the exception: the tag covers the
    // full block, and a partial layer has no collision shape.
    auto traits = states.infer_traits(key);
    const auto block = state_block_name(key);
    if (block == "minecraft:snow" &&
        key.find("layers=8") == std::string_view::npos) {
        traits.blocks_motion = false;
        traits.blocks_motion_no_leaves = false;
    }
    return states.intern(key, traits);
}

class reference_provider final : public state_provider {
  public:
    explicit reference_provider(const state_provider *target)
        : target_{target} {}
    [[nodiscard]] auto get(const placement_context &context,
                           worldgen_random &random, const block_pos &pos) const
        -> block_state_id override {
        return target_->get(context, random, pos);
    }

  private:
    const state_provider *target_{nullptr};
};

class rule_based_provider final : public state_provider {
  public:
    struct rule {
        std::unique_ptr<block_predicate> predicate{};
        std::unique_ptr<state_provider> provider{};
    };

    rule_based_provider(std::unique_ptr<state_provider> fallback,
                        std::vector<rule> rules)
        : fallback_{std::move(fallback)}, rules_{std::move(rules)} {}
    [[nodiscard]] auto get(const placement_context &context,
                           worldgen_random &random, const block_pos &pos) const
        -> block_state_id override {
        const auto state = get_optional(context, random, pos);
        return state.has_value()
                   ? *state
                   : context.region->state_at(pos.x, pos.y, pos.z);
    }
    [[nodiscard]] auto get_optional(const placement_context &context,
                                    worldgen_random &random,
                                    const block_pos &pos) const
        -> std::optional<block_state_id> override {
        for (const auto &entry : rules_) {
            if (!entry.predicate->test(context, pos)) {
                continue;
            }
            const auto state =
                entry.provider->get_optional(context, random, pos);
            if (state.has_value()) {
                return state;
            }
        }
        if (fallback_) {
            return fallback_->get_optional(context, random, pos);
        }
        return std::nullopt;
    }

  private:
    std::unique_ptr<state_provider> fallback_;
    std::vector<rule> rules_;
};

class simple_provider final : public state_provider {
  public:
    explicit simple_provider(block_state_id state) : state_{state} {}
    [[nodiscard]] auto get(const placement_context &, worldgen_random &,
                           const block_pos &) const -> block_state_id override {
        return state_;
    }

  private:
    block_state_id state_{no_block_state};
};

class weighted_provider final : public state_provider {
  public:
    weighted_provider(std::vector<std::pair<jint, block_state_id>> entries,
                      jint total)
        : entries_{std::move(entries)}, total_{total} {}
    [[nodiscard]] auto get(const placement_context &, worldgen_random &random,
                           const block_pos &) const -> block_state_id override {
        jint selection = random.next_int(total_);
        for (const auto &[weight, state] : entries_) {
            selection -= weight;
            if (selection < 0) {
                return state;
            }
        }
        return entries_.back().second;
    }

  private:
    std::vector<std::pair<jint, block_state_id>> entries_;
    jint total_{0};
};

/// `NoiseThresholdProvider`: one NormalNoise sample selects the low, high or
/// default branch; the high branch also draws the chance roll.
class noise_threshold_provider final : public state_provider {
  public:
    noise_threshold_provider(noise_stack noise, float scale, float threshold,
                             float high_chance, block_state_id default_state,
                             std::vector<block_state_id> low_states,
                             std::vector<block_state_id> high_states)
        : noise_{std::move(noise)}, scale_{scale}, threshold_{threshold},
          high_chance_{high_chance}, default_state_{default_state},
          low_states_{std::move(low_states)},
          high_states_{std::move(high_states)} {}

    [[nodiscard]] auto get(const placement_context &, worldgen_random &random,
                           const block_pos &pos) const
        -> block_state_id override {
        const auto local = noise_.get(
            static_cast<double>(pos.x) * static_cast<double>(scale_),
            static_cast<double>(pos.y) * static_cast<double>(scale_),
            static_cast<double>(pos.z) * static_cast<double>(scale_));
        if (local < threshold_) {
            return pick(low_states_, random);
        }
        if (random.next_float() < high_chance_) {
            return pick(high_states_, random);
        }
        return default_state_;
    }

  private:
    [[nodiscard]] static auto pick(const std::vector<block_state_id> &states,
                                   worldgen_random &random) -> block_state_id {
        return states[static_cast<std::size_t>(
            random.next_int(static_cast<jint>(states.size())))];
    }

    noise_stack noise_{};
    float scale_{1.0F};
    float threshold_{0.0F};
    float high_chance_{0.0F};
    block_state_id default_state_{no_block_state};
    std::vector<block_state_id> low_states_{};
    std::vector<block_state_id> high_states_{};
};

/// `RandomBlockProvider`: one uniform draw selects a block from a tag or an
/// explicit list and the provider returns its default state.
class random_block_provider final : public state_provider {
  public:
    explicit random_block_provider(std::vector<block_state_id> blocks)
        : blocks_{std::move(blocks)} {}

    [[nodiscard]] auto get(const placement_context &, worldgen_random &random,
                           const block_pos &) const -> block_state_id override {
        if (blocks_.empty()) {
            return no_block_state;
        }
        const auto index = static_cast<std::size_t>(
            random.next_int(static_cast<jint>(blocks_.size())));
        return blocks_[index];
    }

  private:
    std::vector<block_state_id> blocks_{};
};

/// `NoiseProvider`: one NormalNoise sample selects a state from the list by
/// index; it consumes no randomness.
class noise_provider : public state_provider {
  public:
    noise_provider(noise_stack noise, float scale,
                   std::vector<block_state_id> states)
        : noise_{std::move(noise)}, scale_{scale}, states_{std::move(states)} {}

    [[nodiscard]] auto get(const placement_context &, worldgen_random &,
                           const block_pos &pos) const
        -> block_state_id override {
        const float value = noise_value(pos);
        return pick(value);
    }

  protected:
    [[nodiscard]] auto noise_value(const block_pos &pos) const -> float {
        return noise_.get(
            static_cast<double>(pos.x) * static_cast<double>(scale_),
            static_cast<double>(pos.y) * static_cast<double>(scale_),
            static_cast<double>(pos.z) * static_cast<double>(scale_));
    }

    [[nodiscard]] auto pick(float value) const -> block_state_id {
        const float placement =
            std::clamp((1.0F + value) / 2.0F, 0.0F, 0.9999F);
        const auto index = static_cast<std::size_t>(
            placement * static_cast<float>(states_.size()));
        return states_[std::min(index, states_.size() - 1)];
    }

    noise_stack noise_{};
    float scale_{1.0F};
    std::vector<block_state_id> states_{};
};

/// `DualNoiseProvider`: a slow noise picks how many candidate states the fast
/// noise chooses from, and the fast noise then picks one of those candidates.
class dual_noise_provider final : public noise_provider {
  public:
    dual_noise_provider(noise_stack slow_noise, float slow_scale,
                        jint variety_min, jint variety_max, noise_stack noise,
                        float scale, std::vector<block_state_id> states)
        : noise_provider{std::move(noise), scale, std::move(states)},
          slow_noise_{std::move(slow_noise)}, slow_scale_{slow_scale},
          variety_min_{variety_min}, variety_max_{variety_max} {}

    [[nodiscard]] auto get(const placement_context &, worldgen_random &random,
                           const block_pos &pos) const
        -> block_state_id override {
        const float slow = slow_value(pos);
        const double mapped =
            clamped_map(static_cast<double>(slow), -1.0, 1.0,
                        static_cast<double>(variety_min_),
                        static_cast<double>(variety_max_ + 1));
        const auto variety = static_cast<jint>(mapped);
        std::vector<block_state_id> candidates;
        candidates.reserve(static_cast<std::size_t>(std::max(variety, 0)));
        for (jint i = 0; i < variety; ++i) {
            const block_pos offset{
                .x = pos.x + i * 54545, .y = pos.y, .z = pos.z + i * 34234};
            candidates.push_back(pick(slow_value(offset)));
        }
        if (candidates.empty()) {
            return states_.empty() ? no_block_state : states_.front();
        }
        const float value = noise_value(pos);
        const float placement =
            std::clamp((1.0F + value) / 2.0F, 0.0F, 0.9999F);
        const auto index =
            std::min(static_cast<std::size_t>(
                         placement * static_cast<float>(candidates.size())),
                     candidates.size() - 1);
        (void)random;
        return candidates[index];
    }

  private:
    [[nodiscard]] auto slow_value(const block_pos &pos) const -> float {
        return slow_noise_.get(
            static_cast<double>(pos.x) * static_cast<double>(slow_scale_),
            static_cast<double>(pos.y) * static_cast<double>(slow_scale_),
            static_cast<double>(pos.z) * static_cast<double>(slow_scale_));
    }

    [[nodiscard]] static auto clamped_map(double value, double from_min,
                                          double from_max, double to_min,
                                          double to_max) -> double {
        const double factor = (value - from_min) / (from_max - from_min);
        if (factor < 0.0) {
            return to_min;
        }
        if (factor > 1.0) {
            return to_max;
        }
        return factor * (to_max - to_min) + to_min;
    }

    noise_stack slow_noise_{};
    float slow_scale_{1.0F};
    jint variety_min_{1};
    jint variety_max_{1};
};

class rotated_provider final : public state_provider {
  public:
    rotated_provider(std::unique_ptr<state_provider> source,
                     block_state_registry &states, std::string direction)
        : source_{std::move(source)}, states_{&states},
          direction_{std::move(direction)} {}
    [[nodiscard]] auto get(const placement_context &context,
                           worldgen_random &random, const block_pos &pos) const
        -> block_state_id override {
        static constexpr std::array<std::string_view, 6> names{
            "down", "up", "north", "south", "west", "east"};
        std::string_view direction = direction_;
        if (direction.empty()) {
            direction = names[static_cast<std::size_t>(random.next_int(6))];
        }
        std::string_view axis = "y";
        if (direction == "north" || direction == "south") {
            axis = "z";
        } else if (direction == "west" || direction == "east") {
            axis = "x";
        }
        const auto state = source_->get(context, random, pos);
        auto key = std::string{context.states->name(state)};
        key = with_state_property(key, "axis", axis);
        key = with_state_property(key, "facing", direction);
        if (axis != "y") {
            key = with_state_property(key, "horizontal_facing", direction);
        }
        const auto resolved = resolve_state(*states_, key);
        return resolved.has_value() ? *resolved : no_block_state;
    }

  private:
    std::unique_ptr<state_provider> source_;
    block_state_registry *states_{nullptr};
    std::string direction_;
};

class randomized_int_provider final : public state_provider {
  public:
    randomized_int_provider(std::unique_ptr<state_provider> source,
                            block_state_registry &states, std::string property,
                            std::unique_ptr<provider::int_provider> values)
        : source_{std::move(source)}, states_{&states},
          property_{std::move(property)}, values_{std::move(values)} {}
    [[nodiscard]] auto get(const placement_context &context,
                           worldgen_random &random, const block_pos &pos) const
        -> block_state_id override {
        const auto state = source_->get(context, random, pos);
        auto key = std::string{context.states->name(state)};
        key = with_state_property(key, property_,
                                  std::to_string(values_->sample(random)));
        const auto resolved = resolve_state(*states_, key);
        return resolved.has_value() ? *resolved : no_block_state;
    }

  private:
    std::unique_ptr<state_provider> source_;
    block_state_registry *states_{nullptr};
    std::string property_;
    std::unique_ptr<provider::int_provider> values_;
};

auto decode_normal_noise(const data::json::value &document)
    -> result<normal_noise_parameters> {
    if (!document.is_object()) {
        return std::unexpected{make_error(
            error_code::parse_error, "noise parameters are not an object")};
    }
    normal_noise_parameters parameters;
    if (const auto *value = document.find("base_amplitude"); value != nullptr) {
        const auto number = value->as_double();
        if (!number) {
            return std::unexpected{number.error()};
        }
        parameters.base_amplitude = *number;
    }
    if (const auto *value = document.find("base_octave"); value != nullptr) {
        const auto number = value->as_int64();
        if (!number) {
            return std::unexpected{number.error()};
        }
        parameters.base_octave = static_cast<jint>(*number);
    }
    if (const auto *value = document.find("octave_count"); value != nullptr) {
        const auto number = value->as_int64();
        if (!number) {
            return std::unexpected{number.error()};
        }
        parameters.octave_count = static_cast<jint>(*number);
    }
    if (const auto *value = document.find("normalize"); value != nullptr) {
        const auto enabled = value->as_bool();
        if (!enabled) {
            return std::unexpected{make_error(
                error_code::parse_error, "noise normalize must be a boolean")};
        }
        parameters.normalize = *enabled;
    }
    if (const auto *value = document.find("amplitude_modifiers");
        value != nullptr) {
        const auto list = value->as_array();
        if (!list) {
            return std::unexpected{list.error()};
        }
        for (const auto &entry : *list) {
            const auto number = entry.as_double();
            if (!number) {
                return std::unexpected{number.error()};
            }
            parameters.amplitude_modifiers.push_back(*number);
        }
    }
    return parameters;
}

auto decode_noise_states(const data::json::value &document,
                         block_state_registry &states)
    -> result<std::vector<block_state_id>> {
    if (!document.is_array() || document.as_array()->empty()) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "noise provider needs a non-empty states list")};
    }
    std::vector<block_state_id> out;
    for (const auto &entry : *document.as_array()) {
        const auto key = decode_state_key(entry);
        if (!key) {
            return std::unexpected{key.error()};
        }
        const auto state = resolve_state(states, *key);
        if (!state) {
            return std::unexpected{state.error()};
        }
        out.push_back(*state);
    }
    return out;
}

} // namespace

auto simple_state(block_state_id state) -> std::unique_ptr<state_provider> {
    return std::make_unique<simple_provider>(state);
}

auto decode_state_provider(const data::json::value &document,
                           block_state_registry &states,
                           const state_provider_resolver &resolve)
    -> result<std::unique_ptr<state_provider>> {
    if (!document.is_object()) {
        const auto text = document.as_string();
        if (text && resolve) {
            if (const auto *target = resolve(*text); target != nullptr) {
                return std::unique_ptr<state_provider>{
                    std::make_unique<reference_provider>(target)};
            }
        }
        const auto key = decode_state_key(document);
        if (!key) {
            return std::unexpected{key.error()};
        }
        auto state = resolve_state(states, *key);
        if (!state) {
            return std::unexpected{state.error()};
        }
        return std::unique_ptr<state_provider>{
            std::make_unique<simple_provider>(*state)};
    }
    const auto *type_value = document.find("type");
    if (type_value == nullptr) {
        const auto key = decode_state_key(document);
        if (!key) {
            return std::unexpected{key.error()};
        }
        auto state = resolve_state(states, *key);
        if (!state) {
            return std::unexpected{state.error()};
        }
        return std::unique_ptr<state_provider>{
            std::make_unique<simple_provider>(*state)};
    }
    const auto type = type_value->as_string();
    if (!type) {
        return std::unexpected{make_error(
            error_code::parse_error, "state provider type is not a string")};
    }
    if (*type == "minecraft:simple" ||
        *type == "minecraft:simple_state_provider") {
        const auto *state_value = document.find("state");
        if (state_value == nullptr) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "simple provider has no state")};
        }
        const auto key = decode_state_key(*state_value);
        if (!key) {
            return std::unexpected{key.error()};
        }
        auto state = resolve_state(states, *key);
        if (!state) {
            return std::unexpected{state.error()};
        }
        return std::unique_ptr<state_provider>{
            std::make_unique<simple_provider>(*state)};
    }
    if (*type == "minecraft:weighted") {
        const auto *entries = document.find("entries");
        if (entries == nullptr || !entries->is_array()) {
            return std::unexpected{make_error(
                error_code::parse_error, "weighted provider has no entries")};
        }
        std::vector<std::pair<jint, block_state_id>> decoded;
        jint total = 0;
        for (const auto &entry : *entries->as_array()) {
            const auto *data = entry.find("data");
            const auto *weight = entry.find("weight");
            if (data == nullptr || weight == nullptr) {
                return std::unexpected{make_error(
                    error_code::parse_error, "weighted entry is incomplete")};
            }
            const auto weight_value = weight->as_int64();
            const auto key = decode_state_key(*data);
            if (!weight_value || !key) {
                return std::unexpected{make_error(error_code::parse_error,
                                                  "weighted entry is invalid")};
            }
            auto state = resolve_state(states, *key);
            if (!state) {
                return std::unexpected{state.error()};
            }
            total += static_cast<jint>(*weight_value);
            decoded.emplace_back(static_cast<jint>(*weight_value), *state);
        }
        return std::unique_ptr<state_provider>{
            std::make_unique<weighted_provider>(std::move(decoded), total)};
    }
    if (*type == "minecraft:rotated") {
        const auto *source = document.find("state");
        if (source == nullptr) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "rotated provider has no state")};
        }
        auto inner = decode_state_provider(*source, states, resolve);
        if (!inner) {
            return std::unexpected{inner.error()};
        }
        std::string direction;
        if (const auto *field = document.find("direction"); field != nullptr) {
            const auto text = field->as_string();
            if (!text) {
                return std::unexpected{make_error(
                    error_code::parse_error, "rotated direction is invalid")};
            }
            direction = std::string{*text};
        }
        return std::unique_ptr<state_provider>{
            std::make_unique<rotated_provider>(std::move(*inner), states,
                                               std::move(direction))};
    }
    if (*type == "minecraft:randomized_int") {
        const auto *source = document.find("source");
        const auto *property = document.find("property");
        const auto *values = document.find("values");
        if (source == nullptr || property == nullptr || values == nullptr) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "randomized_int is incomplete")};
        }
        auto inner = decode_state_provider(*source, states, resolve);
        auto property_text = property->as_string();
        auto value_provider = provider::int_provider::decode(*values);
        if (!inner || !property_text || !value_provider) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "randomized_int is invalid")};
        }
        return std::unique_ptr<state_provider>{
            std::make_unique<randomized_int_provider>(
                std::move(*inner), states, std::string{*property_text},
                std::move(*value_provider))};
    }
    if (*type == "minecraft:rule_based") {
        const auto *fallback_value = document.find("fallback");
        const auto *rules_value = document.find("rules");
        if (rules_value == nullptr || !rules_value->is_array()) {
            return std::unexpected{
                make_error(error_code::parse_error, "rule_based has no rules")};
        }
        std::unique_ptr<state_provider> fallback;
        if (fallback_value != nullptr) {
            auto decoded =
                decode_state_provider(*fallback_value, states, resolve);
            if (!decoded) {
                return std::unexpected{decoded.error()};
            }
            fallback = std::move(*decoded);
        }
        std::vector<rule_based_provider::rule> rules;
        for (const auto &entry : *rules_value->as_array()) {
            const auto *predicate_value = entry.find("if_true");
            const auto *then_value = entry.find("then");
            if (predicate_value == nullptr || then_value == nullptr) {
                return std::unexpected{make_error(
                    error_code::parse_error, "rule_based rule is incomplete")};
            }
            auto predicate = decode_block_predicate(*predicate_value, states);
            auto provider = decode_state_provider(*then_value, states, resolve);
            if (!predicate || !provider) {
                return std::unexpected{make_error(
                    error_code::parse_error, "rule_based rule is invalid")};
            }
            rules.push_back(
                rule_based_provider::rule{.predicate = std::move(*predicate),
                                          .provider = std::move(*provider)});
        }
        return std::unique_ptr<state_provider>{
            std::make_unique<rule_based_provider>(std::move(fallback),
                                                  std::move(rules))};
    }
    if (*type == "minecraft:random_block") {
        const auto *blocks = document.find("blocks");
        if (blocks == nullptr) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "random block has no blocks")};
        }
        std::vector<block_state_id> decoded;
        if (const auto tag = blocks->as_string();
            tag.has_value() && tag->starts_with("#")) {
            const auto found = states.tag_states(tag->substr(1));
            decoded.assign(found.begin(), found.end());
        } else {
            auto list = blocks->is_array()
                            ? std::vector<const data::json::value *>{}
                            : std::vector<const data::json::value *>{blocks};
            if (blocks->is_array()) {
                for (const auto &entry : *blocks->as_array()) {
                    list.push_back(&entry);
                }
            }
            for (const auto *entry : list) {
                const auto key = decode_state_key(*entry);
                if (!key) {
                    return std::unexpected{key.error()};
                }
                const auto state = resolve_state(states, *key);
                if (!state) {
                    return std::unexpected{state.error()};
                }
                decoded.push_back(*state);
            }
        }
        if (decoded.empty()) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "random block list is empty")};
        }
        return std::unique_ptr<state_provider>{
            std::make_unique<random_block_provider>(std::move(decoded))};
    }
    if (*type == "minecraft:noise") {
        const auto *seed = document.find("seed");
        const auto *noise_document = document.find("noise");
        const auto *scale = document.find("scale");
        const auto *states_field = document.find("states");
        if (seed == nullptr || noise_document == nullptr || scale == nullptr ||
            states_field == nullptr) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "noise provider is incomplete")};
        }
        const auto seed_value = seed->as_int64();
        const auto scale_value = scale->as_double();
        if (!seed_value || !scale_value) {
            return std::unexpected{make_error(
                error_code::parse_error, "noise provider numbers are invalid")};
        }
        auto parameters = decode_normal_noise(*noise_document);
        auto decoded_states = decode_noise_states(*states_field, states);
        if (!parameters || !decoded_states) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "noise provider is invalid")};
        }
        const normal_noise noise{*parameters};
        legacy_random_source source{static_cast<jlong>(*seed_value)};
        auto stack = noise.create(source);
        return std::unique_ptr<state_provider>{std::make_unique<noise_provider>(
            std::move(stack), static_cast<float>(*scale_value),
            std::move(*decoded_states))};
    }
    if (*type == "minecraft:dual_noise") {
        const auto *variety = document.find("variety");
        const auto *slow_noise_document = document.find("slow_noise");
        const auto *slow_scale = document.find("slow_scale");
        const auto *seed = document.find("seed");
        const auto *noise_document = document.find("noise");
        const auto *scale = document.find("scale");
        const auto *states_field = document.find("states");
        if (variety == nullptr || slow_noise_document == nullptr ||
            slow_scale == nullptr || seed == nullptr ||
            noise_document == nullptr || scale == nullptr ||
            states_field == nullptr) {
            return std::unexpected{make_error(
                error_code::parse_error, "dual noise provider is incomplete")};
        }
        const auto seed_value = seed->as_int64();
        const auto scale_value = scale->as_double();
        const auto slow_scale_value = slow_scale->as_double();
        if (!seed_value || !scale_value || !slow_scale_value) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "dual noise provider numbers are invalid")};
        }
        // The inclusive range accepts the two-element array and the explicit
        // bounds object form.
        std::optional<jint> min_value;
        std::optional<jint> max_value;
        if (const auto values = variety->as_array();
            values && values->size() == 2) {
            const auto min = (*values)[0].as_int64();
            const auto max = (*values)[1].as_int64();
            if (min && max) {
                min_value = static_cast<jint>(*min);
                max_value = static_cast<jint>(*max);
            }
        } else if (const auto *min_field = variety->find("min_inclusive");
                   min_field != nullptr) {
            const auto *max_field = variety->find("max_inclusive");
            if (max_field != nullptr) {
                const auto min = min_field->as_int64();
                const auto max = max_field->as_int64();
                if (min && max) {
                    min_value = static_cast<jint>(*min);
                    max_value = static_cast<jint>(*max);
                }
            }
        }
        if (!min_value || !max_value) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "dual noise provider variety is invalid")};
        }
        auto parameters = decode_normal_noise(*noise_document);
        auto slow_parameters = decode_normal_noise(*slow_noise_document);
        auto decoded_states = decode_noise_states(*states_field, states);
        if (!parameters || !slow_parameters || !decoded_states) {
            return std::unexpected{make_error(
                error_code::parse_error, "dual noise provider is invalid")};
        }
        const normal_noise noise{*parameters};
        const normal_noise slow{*slow_parameters};
        legacy_random_source source{static_cast<jlong>(*seed_value)};
        legacy_random_source slow_source{static_cast<jlong>(*seed_value)};
        auto stack = noise.create(source);
        auto slow_stack = slow.create(slow_source);
        return std::unique_ptr<state_provider>{
            std::make_unique<dual_noise_provider>(
                std::move(slow_stack), static_cast<float>(*slow_scale_value),
                static_cast<jint>(*min_value), static_cast<jint>(*max_value),
                std::move(stack), static_cast<float>(*scale_value),
                std::move(*decoded_states))};
    }
    if (*type == "minecraft:noise_threshold") {
        const auto *seed = document.find("seed");
        const auto *noise_document = document.find("noise");
        const auto *scale = document.find("scale");
        const auto *threshold = document.find("threshold");
        const auto *high_chance = document.find("high_chance");
        const auto *default_state = document.find("default_state");
        const auto *low_states = document.find("low_states");
        const auto *high_states = document.find("high_states");
        if (seed == nullptr || noise_document == nullptr || scale == nullptr ||
            threshold == nullptr || high_chance == nullptr ||
            default_state == nullptr || low_states == nullptr ||
            high_states == nullptr) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "noise_threshold is incomplete")};
        }
        const auto seed_value = seed->as_int64();
        const auto scale_value = scale->as_double();
        const auto threshold_value = threshold->as_double();
        const auto high_chance_value = high_chance->as_double();
        if (!seed_value || !scale_value || !threshold_value ||
            !high_chance_value) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "noise_threshold numbers are invalid")};
        }
        if (!noise_document->is_object() || !low_states->is_array() ||
            !high_states->is_array()) {
            return std::unexpected{make_error(
                error_code::parse_error, "noise_threshold lists are invalid")};
        }
        normal_noise_parameters parameters;
        if (const auto *value = noise_document->find("base_amplitude");
            value != nullptr) {
            const auto number = value->as_double();
            if (!number) {
                return std::unexpected{number.error()};
            }
            parameters.base_amplitude = *number;
        }
        if (const auto *value = noise_document->find("base_octave");
            value != nullptr) {
            const auto number = value->as_int64();
            if (!number) {
                return std::unexpected{number.error()};
            }
            parameters.base_octave = static_cast<jint>(*number);
        }
        if (const auto *value = noise_document->find("octave_count");
            value != nullptr) {
            const auto number = value->as_int64();
            if (!number) {
                return std::unexpected{number.error()};
            }
            parameters.octave_count = static_cast<jint>(*number);
        }
        if (const auto *value = noise_document->find("normalize");
            value != nullptr) {
            const auto enabled = value->as_bool();
            if (!enabled) {
                return std::unexpected{
                    make_error(error_code::unsupported,
                               "noise_threshold normalize must be a boolean")};
            }
            parameters.normalize = *enabled;
        }
        if (const auto *value = noise_document->find("amplitude_modifiers");
            value != nullptr) {
            const auto list = value->as_array();
            if (!list) {
                return std::unexpected{list.error()};
            }
            for (const auto &entry : *list) {
                const auto number = entry.as_double();
                if (!number) {
                    return std::unexpected{number.error()};
                }
                parameters.amplitude_modifiers.push_back(*number);
            }
        }
        const auto default_key = decode_state_key(*default_state);
        if (!default_key) {
            return std::unexpected{default_key.error()};
        }
        const auto default_resolved = resolve_state(states, *default_key);
        if (!default_resolved) {
            return std::unexpected{default_resolved.error()};
        }
        const auto decode_list = [&](const data::json::value &list)
            -> result<std::vector<block_state_id>> {
            std::vector<block_state_id> out;
            for (const auto &entry : *list.as_array()) {
                const auto key = decode_state_key(entry);
                if (!key) {
                    return std::unexpected{key.error()};
                }
                auto state = resolve_state(states, *key);
                if (!state) {
                    return std::unexpected{state.error()};
                }
                out.push_back(*state);
            }
            if (out.empty()) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "noise_threshold state list is empty")};
            }
            return out;
        };
        auto low = decode_list(*low_states);
        if (!low) {
            return std::unexpected{low.error()};
        }
        auto high = decode_list(*high_states);
        if (!high) {
            return std::unexpected{high.error()};
        }
        const normal_noise noise{parameters};
        legacy_random_source source{static_cast<jlong>(*seed_value)};
        auto stack = noise.create(source);
        return std::unique_ptr<state_provider>{
            std::make_unique<noise_threshold_provider>(
                std::move(stack), static_cast<float>(*scale_value),
                static_cast<float>(*threshold_value),
                static_cast<float>(*high_chance_value), *default_resolved,
                std::move(*low), std::move(*high))};
    }
    return std::unexpected{make_error(error_code::unsupported,
                                      "unsupported state provider type '" +
                                          std::string{*type} + "'")};
}

} // namespace refrain::worldgen::feature
