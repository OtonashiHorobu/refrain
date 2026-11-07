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
