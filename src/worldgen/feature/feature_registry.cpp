// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/feature/feature_registry.hpp>

#include <refrain/worldgen/chunk/block_state_codec.hpp>
#include <refrain/worldgen/feature/rule_test.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::worldgen::feature {

namespace {

auto type_of(const data::json::value &input) -> result<std::string_view> {
    const auto *type = input.find("type");
    if (type == nullptr) {
        return std::unexpected{
            make_error(error_code::parse_error, "feature has no type")};
    }
    const auto text = type->as_string();
    if (!text) {
        return std::unexpected{make_error(error_code::parse_error,
                                          "feature type is not a string")};
    }
    return *text;
}

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

auto decode_simple_state(const data::json::value &provider,
                         block_state_registry &states)
    -> result<block_state_id> {
    const data::json::value *state = &provider;
    if (const auto *type = provider.find("type"); type != nullptr) {
        const auto type_name = type->as_string();
        if (!type_name || (*type_name != "minecraft:simple" &&
                           *type_name != "minecraft:simple_state_provider")) {
            return std::unexpected{
                make_error(error_code::unsupported,
                           "only simple state providers are implemented")};
        }
        state = provider.find("state");
        if (state == nullptr) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "state provider has no state")};
        }
    }
    const auto key = decode_state_key(*state);
    if (!key) {
        return std::unexpected{key.error()};
    }
    return resolve_state(states, *key);
}

auto reference_id(const data::json::value &input) -> result<std::string> {
    if (const auto text = input.as_string(); text.has_value()) {
        return std::string{*text};
    }
    if (!input.is_object()) {
        return std::unexpected{make_error(
            error_code::parse_error, "placed feature reference is invalid")};
    }
    const auto *feature = input.find("feature");
    if (feature == nullptr) {
        return std::unexpected{make_error(error_code::parse_error,
                                          "placed feature has no feature")};
    }
    if (!feature->is_string()) {
        return std::unexpected{
            make_error(error_code::unsupported,
                       "inline nested feature objects are not implemented")};
    }
    const auto *placement = input.find("placement");
    if (placement != nullptr) {
        if (!placement->is_array() || !placement->as_array()->empty()) {
            return std::unexpected{
                make_error(error_code::unsupported,
                           "nested placement lists are not implemented")};
        }
    }
    return std::string{*feature->as_string()};
}

} // namespace

auto feature_registry::load(const feature_data_set &data) -> status {
    for (const auto &[id, document] : data.features) {
        const auto type = type_of(document.document);
        if (!type) {
            return std::unexpected{type.error()};
        }
        if (*type == "minecraft:simple_block") {
            const auto *to_place = document.document.find("to_place");
            if (to_place == nullptr) {
                return std::unexpected{make_error(
                    error_code::parse_error, "simple block has no to_place")};
            }
            auto state = decode_simple_state(*to_place, *states_);
            if (!state) {
                return std::unexpected{state.error()};
            }
            bool schedule_tick = false;
            if (const auto *field = document.document.find("schedule_tick");
                field != nullptr) {
                const auto value = field->as_bool();
                if (!value) {
                    return std::unexpected{value.error()};
                }
                schedule_tick = *value;
            }
            features_.emplace(id, std::make_unique<simple_block_feature>(
                                      *state, schedule_tick));
            continue;
        }
        if (*type == "minecraft:disk") {
            const auto *state_provider =
                document.document.find("state_provider");
            const auto *target = document.document.find("target");
            const auto *radius = document.document.find("radius");
            const auto *half_height = document.document.find("half_height");
            if (state_provider == nullptr || target == nullptr ||
                radius == nullptr || half_height == nullptr) {
                return std::unexpected{make_error(
                    error_code::parse_error, "disk feature is incomplete")};
            }
            auto state = decode_simple_state(*state_provider, *states_);
            auto predicate = decode_block_predicate(*target, *states_);
            auto radius_provider = provider::int_provider::decode(*radius);
            const auto half = half_height->as_int64();
            if (!state || !predicate || !radius_provider || !half) {
                return std::unexpected{make_error(error_code::parse_error,
                                                  "disk feature is invalid")};
            }
            features_.emplace(id, std::make_unique<disk_feature>(
                                      *state, std::move(*predicate),
                                      std::move(*radius_provider),
                                      static_cast<jint>(*half)));
            continue;
        }
        if (*type == "minecraft:ore") {
            const auto *targets = document.document.find("targets");
            const auto *size = document.document.find("size");
            if (targets == nullptr || !targets->is_array() || size == nullptr) {
                return std::unexpected{make_error(error_code::parse_error,
                                                  "ore feature is incomplete")};
            }
            const auto size_value = size->as_int64();
            if (!size_value) {
                return std::unexpected{size_value.error()};
            }
            float discard = 0.0F;
            if (const auto *field =
                    document.document.find("discard_chance_on_air_exposure");
                field != nullptr) {
                const auto value = field->as_double();
                if (!value) {
                    return std::unexpected{value.error()};
                }
                discard = static_cast<float>(*value);
            }
            std::vector<block_replacement> replacements;
            for (const auto &entry : *targets->as_array()) {
                const auto *state = entry.find("state");
                const auto *target = entry.find("target");
                if (state == nullptr || target == nullptr) {
                    return std::unexpected{make_error(
                        error_code::parse_error, "ore target is incomplete")};
                }
                std::string name;
                if (const auto text = state->as_string(); text.has_value()) {
                    name = std::string{*text};
                } else if (const auto *id_field = state->find("id");
                           id_field != nullptr) {
                    const auto id_text = id_field->as_string();
                    if (!id_text) {
                        return std::unexpected{
                            make_error(error_code::parse_error,
                                       "ore state id is invalid")};
                    }
                    name = std::string{*id_text};
                } else {
                    return std::unexpected{make_error(error_code::parse_error,
                                                      "ore state is invalid")};
                }
                const auto state_id = resolve_state(*states_, name);
                if (!state_id) {
                    return std::unexpected{state_id.error()};
                }
                auto rule = decode_rule_test(*target, *states_);
                if (!rule) {
                    return std::unexpected{rule.error()};
                }
                replacements.push_back(block_replacement{
                    .target = std::move(*rule), .state = *state_id});
            }
            features_.emplace(id, std::make_unique<ore_feature>(
                                      std::move(replacements),
                                      static_cast<jint>(*size_value), discard));
            continue;
        }
        if (*type == "minecraft:spring_feature") {
            const auto *state_value = document.document.find("state");
            const auto *valid_value = document.document.find("valid_blocks");
            if (state_value == nullptr || valid_value == nullptr) {
                return std::unexpected{make_error(
                    error_code::parse_error, "spring feature is incomplete")};
            }
            const auto state_key = decode_state_key(*state_value);
            if (!state_key) {
                return std::unexpected{state_key.error()};
            }
            const auto state = resolve_state(*states_, *state_key);
            if (!state) {
                return std::unexpected{state.error()};
            }
            std::vector<std::string> valid;
            if (const auto single = valid_value->as_string();
                single.has_value()) {
                valid.emplace_back(*single);
            } else if (valid_value->is_array()) {
                for (const auto &entry : *valid_value->as_array()) {
                    const auto name = entry.as_string();
                    if (!name) {
                        return std::unexpected{
                            make_error(error_code::parse_error,
                                       "spring valid block is not an id")};
                    }
                    valid.emplace_back(*name);
                }
            } else {
                return std::unexpected{make_error(
                    error_code::parse_error, "spring valid_blocks is invalid")};
            }
            bool requires_below = true;
            if (const auto *field =
                    document.document.find("requires_block_below");
                field != nullptr) {
                const auto value = field->as_bool();
                if (!value) {
                    return std::unexpected{value.error()};
                }
                requires_below = *value;
            }
            jint rocks = 4;
            jint holes = 1;
            if (const auto *field = document.document.find("rock_count");
                field != nullptr) {
                const auto value = field->as_int64();
                if (!value) {
                    return std::unexpected{value.error()};
                }
                rocks = static_cast<jint>(*value);
            }
            if (const auto *field = document.document.find("hole_count");
                field != nullptr) {
                const auto value = field->as_int64();
                if (!value) {
                    return std::unexpected{value.error()};
                }
                holes = static_cast<jint>(*value);
            }
            features_.emplace(id, std::make_unique<spring_feature>(
                                      *state, requires_below, rocks, holes,
                                      std::move(valid)));
            continue;
        }
        if (*type == "minecraft:block_blob") {
            const auto *state_value = document.document.find("state");
            const auto *predicate_value =
                document.document.find("can_place_on");
            if (state_value == nullptr || predicate_value == nullptr) {
                return std::unexpected{make_error(error_code::parse_error,
                                                  "block blob is incomplete")};
            }
            const auto key = decode_state_key(*state_value);
            if (!key) {
                return std::unexpected{key.error()};
            }
            const auto state = resolve_state(*states_, *key);
            auto predicate = decode_block_predicate(*predicate_value, *states_);
            if (!state || !predicate) {
                return std::unexpected{make_error(error_code::parse_error,
                                                  "block blob is invalid")};
            }
            features_.emplace(id, std::make_unique<block_blob_feature>(
                                      *state, std::move(*predicate)));
            continue;
        }
        if (*type == "minecraft:block_column") {
            const auto *layers_value = document.document.find("layers");
            const auto *direction_value = document.document.find("direction");
            const auto *allowed_value =
                document.document.find("allowed_placement");
            const auto *prioritize_value =
                document.document.find("prioritize_tip");
            if (layers_value == nullptr || !layers_value->is_array() ||
                direction_value == nullptr || allowed_value == nullptr ||
                prioritize_value == nullptr) {
                return std::unexpected{make_error(
                    error_code::parse_error, "block column is incomplete")};
            }
            const auto direction_name = direction_value->as_string();
            if (!direction_name) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "column direction is not a string")};
            }
            block_direction direction = block_direction::up;
            if (*direction_name == "up") {
                direction = block_direction::up;
            } else if (*direction_name == "down") {
                direction = block_direction::down;
            } else if (*direction_name == "north") {
                direction = block_direction::north;
            } else if (*direction_name == "south") {
                direction = block_direction::south;
            } else if (*direction_name == "west") {
                direction = block_direction::west;
            } else if (*direction_name == "east") {
                direction = block_direction::east;
            } else {
                return std::unexpected{make_error(error_code::parse_error,
                                                  "unknown column direction")};
            }
            std::vector<block_column_feature::layer> layers;
            for (const auto &entry : *layers_value->as_array()) {
                const auto *height = entry.find("height");
                const auto *provider = entry.find("provider");
                if (height == nullptr || provider == nullptr) {
                    return std::unexpected{make_error(
                        error_code::parse_error, "column layer is incomplete")};
                }
                auto height_provider = provider::int_provider::decode(*height);
                auto state = decode_simple_state(*provider, *states_);
                if (!height_provider || !state) {
                    return std::unexpected{make_error(
                        error_code::parse_error, "column layer is invalid")};
                }
                layers.push_back(block_column_feature::layer{
                    .height = std::move(*height_provider), .state = *state});
            }
            auto allowed = decode_block_predicate(*allowed_value, *states_);
            const auto prioritize = prioritize_value->as_bool();
            if (!allowed || !prioritize) {
                return std::unexpected{make_error(error_code::parse_error,
                                                  "block column is invalid")};
            }
            features_.emplace(id, std::make_unique<block_column_feature>(
                                      std::move(layers), direction,
                                      std::move(*allowed), *prioritize));
            continue;
        }
        if (*type == "minecraft:random_selector") {
            const auto *features = document.document.find("features");
            const auto *default_value = document.document.find("default");
            if (features == nullptr || !features->is_array() ||
                default_value == nullptr) {
                return std::unexpected{make_error(
                    error_code::parse_error, "random selector is incomplete")};
            }
            auto default_id = reference_id(*default_value);
            if (!default_id) {
                return std::unexpected{default_id.error()};
            }
            std::vector<std::pair<float, std::string>> entries;
            for (const auto &entry : *features->as_array()) {
                const auto *chance = entry.find("chance");
                const auto *feature = entry.find("feature");
                if (chance == nullptr || feature == nullptr) {
                    return std::unexpected{
                        make_error(error_code::parse_error,
                                   "selector entry is incomplete")};
                }
                const auto chance_value = chance->as_double();
                auto reference = reference_id(*feature);
                if (!chance_value || !reference) {
                    return std::unexpected{make_error(
                        error_code::parse_error, "selector entry is invalid")};
                }
                entries.emplace_back(static_cast<float>(*chance_value),
                                     std::move(*reference));
            }
            features_.emplace(id,
                              std::make_unique<random_selector_feature>(
                                  std::move(entries), std::move(*default_id)));
            continue;
        }
        if (*type == "minecraft:weighted_random_selector") {
            const auto *features = document.document.find("features");
            if (features == nullptr || !features->is_array()) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "weighted selector is incomplete")};
            }
            std::vector<std::pair<jint, std::string>> entries;
            jint total = 0;
            for (const auto &entry : *features->as_array()) {
                const auto *data_field = entry.find("data");
                const auto *weight = entry.find("weight");
                if (data_field == nullptr || weight == nullptr) {
                    return std::unexpected{
                        make_error(error_code::parse_error,
                                   "selector entry is incomplete")};
                }
                const auto weight_value = weight->as_int64();
                auto reference = reference_id(*data_field);
                if (!weight_value || !reference) {
                    return std::unexpected{make_error(
                        error_code::parse_error, "selector entry is invalid")};
                }
                total += static_cast<jint>(*weight_value);
                entries.emplace_back(static_cast<jint>(*weight_value),
                                     std::move(*reference));
            }
            features_.emplace(id, std::make_unique<weighted_selector_feature>(
                                      std::move(entries), total));
            continue;
        }
        if (*type == "minecraft:simple_random_selector") {
            const auto *features = document.document.find("features");
            if (features == nullptr || !features->is_array()) {
                return std::unexpected{make_error(
                    error_code::parse_error, "simple selector is incomplete")};
            }
            std::vector<std::string> entries;
            for (const auto &entry : *features->as_array()) {
                auto reference = reference_id(entry);
                if (!reference) {
                    return std::unexpected{reference.error()};
                }
                entries.push_back(std::move(*reference));
            }
            features_.emplace(id, std::make_unique<simple_selector_feature>(
                                      std::move(entries)));
            continue;
        }
        return std::unexpected{
            make_error(error_code::unsupported, "unsupported feature type")};
    }

    for (const auto &[id, document] : data.placed) {
        const auto configured = features_.find(document.feature);
        if (configured == features_.end()) {
            return std::unexpected{
                make_error(error_code::not_found,
                           "placed feature names a missing feature")};
        }
        placed_feature_runtime runtime;
        runtime.id = id;
        runtime.configured = configured->second.get();
        for (const auto &entry : *document.placement.as_array()) {
            auto modifier = decode_placement(entry);
            if (!modifier) {
                return std::unexpected{modifier.error()};
            }
            runtime.placement.push_back(std::move(*modifier));
        }
        placed_.emplace(id, std::move(runtime));
    }
    return {};
}

} // namespace refrain::worldgen::feature
