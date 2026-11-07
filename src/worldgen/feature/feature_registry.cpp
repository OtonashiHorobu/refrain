// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/feature/feature_registry.hpp>

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
