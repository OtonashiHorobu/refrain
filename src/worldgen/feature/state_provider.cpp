// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horashi

#include <refrain/worldgen/feature/state_provider.hpp>

#include <refrain/data/json.hpp>
#include <refrain/worldgen/chunk/block_state_codec.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/feature/block_predicate.hpp>
#include <refrain/worldgen/feature/placement.hpp>
#include <refrain/worldgen/provider.hpp>

#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::worldgen::feature {

namespace {

auto resolve_state(block_state_registry &states, std::string_view key)
    -> result<block_state_id> {
    if (const auto found = states.find(key); found.has_value()) {
        return *found;
    }
    const auto block = state_block_name(key);
    const bool air = is_air_block_name(block);
    const bool water = block == "minecraft:water" ||
                       key.find("waterlogged=true") != std::string_view::npos;
    const bool fluid = water || block == "minecraft:lava" ||
                       block == "minecraft:bubble_column";
    return states.intern(
        key, state_traits{.air = air,
                          .fluid = fluid,
                          .water = water,
                          .blocks_motion = !air && !fluid,
                          .blocks_motion_no_leaves = !air && !fluid});
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
            if (!entry.predicate->test(*context.region, *context.states, pos)) {
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
    return std::unexpected{
        make_error(error_code::unsupported, "unsupported state provider")};
}

} // namespace refrain::worldgen::feature
