// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/feature/feature_registry.hpp>

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

auto decode_simple_state(const data::json::value &provider,
                         const block_state_registry &states)
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
    const auto *name = state->find("id");
    if (name == nullptr) {
        name = state->find("Name");
    }
    if (name == nullptr) {
        return std::unexpected{
            make_error(error_code::parse_error, "state has no id")};
    }
    const auto text = name->as_string();
    if (!text) {
        return std::unexpected{
            make_error(error_code::parse_error, "state id is not a string")};
    }
    const auto id = states.find(*text);
    if (!id) {
        return std::unexpected{make_error(
            error_code::not_found, "state provider names an unknown state")};
    }
    return *id;
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
                const auto state_id = states_->find(name);
                if (!state_id) {
                    return std::unexpected{make_error(error_code::not_found,
                                                      "ore state is unknown")};
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
