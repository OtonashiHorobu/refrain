// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/feature/feature_registry.hpp>

#include <refrain/worldgen/chunk/block_state_codec.hpp>
#include <refrain/worldgen/feature/rule_test.hpp>
#include <refrain/worldgen/feature/tree.hpp>

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

auto feature_registry::decode_provider(const data::json::value &document)
    -> result<std::unique_ptr<state_provider>> {
    return decode_state_provider(
        document, *states_,
        [this](std::string_view id) -> const state_provider * {
            const auto found = providers_.find(std::string{id});
            return found == providers_.end() ? nullptr : found->second.get();
        });
}

auto feature_registry::nested_reference(const data::json::value &input)
    -> result<std::string> {
    if (const auto text = input.as_string(); text.has_value()) {
        return std::string{*text};
    }
    if (!input.is_object()) {
        return std::unexpected{make_error(
            error_code::parse_error, "nested placed reference is invalid")};
    }
    const auto *placement = input.find("placement");
    if (placement == nullptr || !placement->is_array()) {
        return std::unexpected{
            make_error(error_code::parse_error, "nested placement is missing")};
    }
    const auto *feature = input.find("feature");
    if (feature == nullptr) {
        return std::unexpected{
            make_error(error_code::parse_error, "nested feature is missing")};
    }
    std::string feature_id;
    if (const auto text = feature->as_string(); text.has_value()) {
        feature_id = std::string{*text};
    } else if (feature->is_object()) {
        const auto *type = feature->find("type");
        if (type == nullptr) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "inline feature has no type")};
        }
        const auto type_name = type->as_string();
        if (!type_name) {
            return std::unexpected{type_name.error()};
        }
        if (*type_name != "minecraft:simple_block") {
            return std::unexpected{make_error(
                error_code::unsupported, "unsupported inline feature type")};
        }
        const auto *to_place = feature->find("to_place");
        if (to_place == nullptr) {
            return std::unexpected{make_error(
                error_code::parse_error, "inline simple block has no state")};
        }
        auto provider = decode_provider(*to_place);
        if (!provider) {
            return std::unexpected{provider.error()};
        }
        bool schedule_tick = false;
        if (const auto *field = feature->find("schedule_tick");
            field != nullptr) {
            const auto value = field->as_bool();
            if (!value) {
                return std::unexpected{value.error()};
            }
            schedule_tick = *value;
        }
        feature_id = "#inline:" + std::to_string(next_nested_++);
        features_.emplace(feature_id, std::make_unique<simple_block_feature>(
                                          std::move(*provider), schedule_tick));
    } else {
        return std::unexpected{
            make_error(error_code::parse_error, "nested feature is invalid")};
    }
    const auto id = "#nested:" + std::to_string(next_nested_++);
    nested_placed_.push_back(nested_placed_entry{
        .id = id, .feature = std::move(feature_id), .placement = *placement});
    return id;
}

auto feature_registry::decode_fallen_decorators(const data::json::value &list)
    -> result<std::vector<std::unique_ptr<tree_decorator>>> {
    if (!list.is_array()) {
        return std::unexpected{make_error(error_code::parse_error,
                                          "fallen decorators is not a list")};
    }
    std::vector<std::unique_ptr<tree_decorator>> out;
    for (const auto &entry : *list.as_array()) {
        const auto *type = entry.find("type");
        if (type == nullptr || !type->as_string()) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "fallen decorator has no type")};
        }
        const auto name = *type->as_string();
        if (name == "minecraft:trunk_vine") {
            out.push_back(std::make_unique<trunk_vine_decorator>());
        } else if (name == "minecraft:shelf_mushroom") {
            const auto *probability = entry.find("probability");
            if (probability == nullptr || !probability->as_double()) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "shelf mushroom probability is invalid")};
            }
            out.push_back(std::make_unique<shelf_mushroom_decorator>(
                static_cast<float>(*probability->as_double())));
        } else if (name == "minecraft:leave_vine") {
            const auto *probability = entry.find("probability");
            if (probability == nullptr || !probability->as_double()) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "leave vine probability is invalid")};
            }
            out.push_back(std::make_unique<leave_vine_decorator>(
                static_cast<float>(*probability->as_double())));
        } else if (name == "minecraft:attached_to_logs") {
            const auto *probability = entry.find("probability");
            const auto *provider_value = entry.find("block_provider");
            const auto *directions = entry.find("directions");
            if (probability == nullptr || provider_value == nullptr ||
                directions == nullptr || !directions->is_array() ||
                !probability->as_double()) {
                return std::unexpected{make_error(
                    error_code::parse_error, "attached_to_logs is incomplete")};
            }
            auto provider = decode_provider(*provider_value);
            if (!provider) {
                return std::unexpected{provider.error()};
            }
            std::vector<std::string> names;
            for (const auto &direction : *directions->as_array()) {
                const auto text = direction.as_string();
                if (!text) {
                    return std::unexpected{
                        make_error(error_code::parse_error,
                                   "attached log direction is invalid")};
                }
                names.emplace_back(*text);
            }
            out.push_back(std::make_unique<attached_to_logs_decorator>(
                static_cast<float>(*probability->as_double()),
                std::move(*provider), std::move(names)));
        } else {
            return std::unexpected{make_error(error_code::unsupported,
                                              "unsupported fallen decorator")};
        }
    }
    return out;
}

auto feature_registry::load(const feature_data_set &data) -> status {
    nested_placed_.clear();
    next_nested_ = 0;
    providers_.clear();
    provider_documents_ = data.state_providers;
    std::set<std::string> visiting;
    std::optional<error> provider_failure;
    std::function<const state_provider *(std::string_view)> resolve =
        [&](std::string_view id) -> const state_provider * {
        const auto found = providers_.find(std::string{id});
        if (found != providers_.end()) {
            return found->second.get();
        }
        const auto document = provider_documents_.find(std::string{id});
        if (document == provider_documents_.end()) {
            return nullptr;
        }
        if (visiting.contains(std::string{id})) {
            provider_failure = make_error(error_code::parse_error,
                                          "state provider reference cycle");
            return nullptr;
        }
        visiting.insert(std::string{id});
        auto decoded =
            decode_state_provider(document->second.document, *states_, resolve);
        visiting.erase(std::string{id});
        if (!decoded) {
            provider_failure = decoded.error();
            return nullptr;
        }
        const auto inserted =
            providers_.emplace(std::string{id}, std::move(*decoded));
        return inserted.first->second.get();
    };
    for (const auto &[id, document] : provider_documents_) {
        if (resolve(id) == nullptr && provider_failure) {
            return std::unexpected{*provider_failure};
        }
    }

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
            auto provider = decode_provider(*to_place);
            if (!provider) {
                return std::unexpected{provider.error()};
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
                                      std::move(*provider), schedule_tick));
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
                auto layer_provider = decode_provider(*provider);
                if (!height_provider || !layer_provider) {
                    return std::unexpected{make_error(
                        error_code::parse_error, "column layer is invalid")};
                }
                layers.push_back(block_column_feature::layer{
                    .height = std::move(*height_provider),
                    .provider = std::move(*layer_provider)});
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
            auto default_id = nested_reference(*default_value);
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
                auto reference = nested_reference(*feature);
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
                auto reference = nested_reference(*data_field);
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
                auto reference = nested_reference(entry);
                if (!reference) {
                    return std::unexpected{reference.error()};
                }
                entries.push_back(std::move(*reference));
            }
            features_.emplace(id, std::make_unique<simple_selector_feature>(
                                      std::move(entries)));
            continue;
        }
        if (*type == "minecraft:overlay") {
            const auto *features = document.document.find("features");
            if (features == nullptr || !features->is_array()) {
                return std::unexpected{make_error(error_code::parse_error,
                                                  "overlay has no features")};
            }
            std::vector<std::string> ids;
            for (const auto &entry : *features->as_array()) {
                auto reference = nested_reference(entry);
                if (!reference) {
                    return std::unexpected{reference.error()};
                }
                ids.push_back(std::move(*reference));
            }
            features_.emplace(
                id, std::make_unique<overlay_feature>(std::move(ids)));
            continue;
        }
        if (*type == "minecraft:random_boolean_selector") {
            const auto *true_value = document.document.find("feature_true");
            const auto *false_value = document.document.find("feature_false");
            if (true_value == nullptr || false_value == nullptr) {
                return std::unexpected{make_error(
                    error_code::parse_error, "boolean selector is incomplete")};
            }
            auto true_id = nested_reference(*true_value);
            auto false_id = nested_reference(*false_value);
            if (!true_id || !false_id) {
                return std::unexpected{make_error(
                    error_code::parse_error, "boolean selector is invalid")};
            }
            features_.emplace(id,
                              std::make_unique<boolean_selector_feature>(
                                  std::move(*true_id), std::move(*false_id)));
            continue;
        }
        if (*type == "minecraft:block_pile") {
            const auto *provider_value =
                document.document.find("state_provider");
            if (provider_value == nullptr) {
                return std::unexpected{make_error(
                    error_code::parse_error, "block pile has no provider")};
            }
            auto provider = decode_provider(*provider_value);
            if (!provider) {
                return std::unexpected{provider.error()};
            }
            features_.emplace(
                id, std::make_unique<block_pile_feature>(std::move(*provider)));
            continue;
        }
        if (*type == "minecraft:fallen_tree") {
            const auto *trunk_value = document.document.find("trunk_provider");
            const auto *length_value = document.document.find("log_length");
            const auto *stump_value =
                document.document.find("stump_decorators");
            const auto *log_value = document.document.find("log_decorators");
            if (trunk_value == nullptr || length_value == nullptr ||
                stump_value == nullptr || log_value == nullptr) {
                return std::unexpected{make_error(error_code::parse_error,
                                                  "fallen tree is incomplete")};
            }
            auto trunk = decode_provider(*trunk_value);
            auto length = provider::int_provider::decode(*length_value);
            auto stump = decode_fallen_decorators(*stump_value);
            auto logs = decode_fallen_decorators(*log_value);
            if (!trunk || !length || !stump || !logs) {
                return std::unexpected{make_error(error_code::parse_error,
                                                  "fallen tree is invalid")};
            }
            features_.emplace(id, std::make_unique<fallen_tree_feature>(
                                      *states_, std::move(*trunk),
                                      std::move(*length), std::move(*stump),
                                      std::move(*logs)));
            continue;
        }
        if (*type == "minecraft:tree") {
            const auto *trunk_value = document.document.find("trunk_provider");
            const auto *trunk_placer = document.document.find("trunk_placer");
            const auto *foliage_value =
                document.document.find("foliage_provider");
            const auto *foliage_placer =
                document.document.find("foliage_placer");
            const auto *minimum_size = document.document.find("minimum_size");
            const auto *decorators = document.document.find("decorators");
            const auto *below_value =
                document.document.find("below_trunk_provider");
            if (trunk_value == nullptr || trunk_placer == nullptr ||
                foliage_value == nullptr || foliage_placer == nullptr ||
                minimum_size == nullptr || decorators == nullptr ||
                below_value == nullptr) {
                return std::unexpected{make_error(
                    error_code::parse_error, "tree feature is incomplete")};
            }
            if (document.document.find("root_placer") != nullptr) {
                return std::unexpected{
                    make_error(error_code::unsupported,
                               "tree root placers are not implemented")};
            }
            if (!decorators->is_array()) {
                return std::unexpected{make_error(
                    error_code::parse_error, "tree decorators is not a list")};
            }
            std::vector<std::unique_ptr<tree_decorator>> decoded_decorators;
            for (const auto &entry : *decorators->as_array()) {
                const auto *decorator_type = entry.find("type");
                if (decorator_type == nullptr || !decorator_type->as_string()) {
                    return std::unexpected{make_error(
                        error_code::parse_error, "tree decorator has no type")};
                }
                const auto decorator_name = *decorator_type->as_string();
                if (decorator_name == "minecraft:trunk_vine") {
                    decoded_decorators.push_back(
                        std::make_unique<trunk_vine_decorator>());
                } else if (decorator_name == "minecraft:attached_to_leaves") {
                    const auto *probability = entry.find("probability");
                    const auto *provider_value = entry.find("block_provider");
                    const auto *directions = entry.find("directions");
                    if (probability == nullptr || provider_value == nullptr ||
                        directions == nullptr || !directions->is_array()) {
                        return std::unexpected{
                            make_error(error_code::parse_error,
                                       "attached_to_leaves is incomplete")};
                    }
                    const auto probability_value = probability->as_double();
                    auto provider = decode_provider(*provider_value);
                    if (!probability_value || !provider) {
                        return std::unexpected{
                            make_error(error_code::parse_error,
                                       "attached_to_leaves is invalid")};
                    }
                    std::vector<std::string> direction_names;
                    for (const auto &direction : *directions->as_array()) {
                        const auto name = direction.as_string();
                        if (!name) {
                            return std::unexpected{
                                make_error(error_code::parse_error,
                                           "attached direction is invalid")};
                        }
                        direction_names.emplace_back(*name);
                    }
                    jint exclusion_xz = 0;
                    jint exclusion_y = 0;
                    jint required_empty = 1;
                    if (const auto *field = entry.find("exclusion_radius_xz");
                        field != nullptr) {
                        const auto value = field->as_int64();
                        if (!value) {
                            return std::unexpected{value.error()};
                        }
                        exclusion_xz = static_cast<jint>(*value);
                    }
                    if (const auto *field = entry.find("exclusion_radius_y");
                        field != nullptr) {
                        const auto value = field->as_int64();
                        if (!value) {
                            return std::unexpected{value.error()};
                        }
                        exclusion_y = static_cast<jint>(*value);
                    }
                    if (const auto *field = entry.find("required_empty_blocks");
                        field != nullptr) {
                        const auto value = field->as_int64();
                        if (!value) {
                            return std::unexpected{value.error()};
                        }
                        required_empty = static_cast<jint>(*value);
                    }
                    decoded_decorators.push_back(
                        std::make_unique<attached_to_leaves_decorator>(
                            static_cast<float>(*probability_value),
                            exclusion_xz, exclusion_y, std::move(*provider),
                            required_empty, std::move(direction_names)));
                } else if (decorator_name == "minecraft:cocoa") {
                    const auto *probability = entry.find("probability");
                    if (probability == nullptr) {
                        return std::unexpected{
                            make_error(error_code::parse_error,
                                       "cocoa has no probability")};
                    }
                    const auto value = probability->as_double();
                    if (!value) {
                        return std::unexpected{value.error()};
                    }
                    decoded_decorators.push_back(
                        std::make_unique<cocoa_decorator>(
                            static_cast<float>(*value)));
                } else if (decorator_name == "minecraft:place_on_ground") {
                    jint tries = 128;
                    jint radius = 2;
                    jint height = 1;
                    if (const auto *field = entry.find("tries");
                        field != nullptr) {
                        const auto value = field->as_int64();
                        if (!value) {
                            return std::unexpected{value.error()};
                        }
                        tries = static_cast<jint>(*value);
                    }
                    if (const auto *field = entry.find("radius");
                        field != nullptr) {
                        const auto value = field->as_int64();
                        if (!value) {
                            return std::unexpected{value.error()};
                        }
                        radius = static_cast<jint>(*value);
                    }
                    if (const auto *field = entry.find("height");
                        field != nullptr) {
                        const auto value = field->as_int64();
                        if (!value) {
                            return std::unexpected{value.error()};
                        }
                        height = static_cast<jint>(*value);
                    }
                    const auto *provider_value =
                        entry.find("block_state_provider");
                    if (provider_value == nullptr) {
                        return std::unexpected{
                            make_error(error_code::parse_error,
                                       "place_on_ground has no provider")};
                    }
                    auto provider = decode_provider(*provider_value);
                    if (!provider) {
                        return std::unexpected{provider.error()};
                    }
                    decoded_decorators.push_back(
                        std::make_unique<place_on_ground_decorator>(
                            tries, radius, height, std::move(*provider)));
                } else if (decorator_name == "minecraft:beehive") {
                    const auto *probability = entry.find("probability");
                    if (probability == nullptr) {
                        return std::unexpected{
                            make_error(error_code::parse_error,
                                       "beehive has no probability")};
                    }
                    const auto value = probability->as_double();
                    if (!value) {
                        return std::unexpected{value.error()};
                    }
                    decoded_decorators.push_back(
                        std::make_unique<beehive_decorator>(
                            static_cast<float>(*value)));
                } else if (decorator_name == "minecraft:alter_ground") {
                    const auto *provider_value = entry.find("provider");
                    if (provider_value == nullptr) {
                        return std::unexpected{
                            make_error(error_code::parse_error,
                                       "alter_ground has no provider")};
                    }
                    auto provider = decode_provider(*provider_value);
                    if (!provider) {
                        return std::unexpected{provider.error()};
                    }
                    decoded_decorators.push_back(
                        std::make_unique<alter_ground_decorator>(
                            std::move(*provider)));
                } else if (decorator_name == "minecraft:leave_vine") {
                    const auto *probability = entry.find("probability");
                    if (probability == nullptr) {
                        return std::unexpected{
                            make_error(error_code::parse_error,
                                       "leave_vine has no probability")};
                    }
                    const auto value = probability->as_double();
                    if (!value) {
                        return std::unexpected{value.error()};
                    }
                    decoded_decorators.push_back(
                        std::make_unique<leave_vine_decorator>(
                            static_cast<float>(*value)));
                } else {
                    return std::unexpected{make_error(
                        error_code::unsupported, "unsupported tree decorator")};
                }
            }
            const auto *trunk_type = trunk_placer->find("type");
            if (trunk_type == nullptr) {
                return std::unexpected{make_error(error_code::parse_error,
                                                  "trunk placer has no type")};
            }
            const auto trunk_name = trunk_type->as_string();
            if (!trunk_name) {
                return std::unexpected{trunk_name.error()};
            }
            trunk_kind kind = trunk_kind::straight;
            if (*trunk_name == "minecraft:forking_trunk_placer") {
                kind = trunk_kind::forking;
            } else if (*trunk_name == "minecraft:fancy_trunk_placer") {
                kind = trunk_kind::fancy;
            } else if (*trunk_name == "minecraft:dark_oak_trunk_placer") {
                kind = trunk_kind::dark_oak;
            } else if (*trunk_name == "minecraft:giant_trunk_placer") {
                kind = trunk_kind::giant;
            } else if (*trunk_name != "minecraft:straight_trunk_placer") {
                return std::unexpected{make_error(error_code::unsupported,
                                                  "unsupported trunk placer")};
            }
            const auto *foliage_type = foliage_placer->find("type");
            if (foliage_type == nullptr || !foliage_type->as_string()) {
                return std::unexpected{make_error(
                    error_code::parse_error, "foliage placer has no type")};
            }
            const auto foliage_name = *foliage_type->as_string();
            foliage_kind foliage_type_kind = foliage_kind::blob;
            if (foliage_name == "minecraft:acacia_foliage_placer") {
                foliage_type_kind = foliage_kind::acacia;
            } else if (foliage_name == "minecraft:spruce_foliage_placer") {
                foliage_type_kind = foliage_kind::spruce;
            } else if (foliage_name == "minecraft:mega_pine_foliage_placer") {
                foliage_type_kind = foliage_kind::mega_pine;
            } else if (foliage_name == "minecraft:pine_foliage_placer") {
                foliage_type_kind = foliage_kind::pine;
            } else if (foliage_name == "minecraft:fancy_foliage_placer") {
                foliage_type_kind = foliage_kind::fancy;
            } else if (foliage_name == "minecraft:dark_oak_foliage_placer") {
                foliage_type_kind = foliage_kind::dark_oak;
            } else if (foliage_name != "minecraft:blob_foliage_placer") {
                return std::unexpected{make_error(
                    error_code::unsupported, "unsupported foliage placer")};
            }
            const auto *minimum_type = minimum_size->find("type");
            if (minimum_type == nullptr || !minimum_type->as_string()) {
                return std::unexpected{make_error(error_code::parse_error,
                                                  "feature size has no type")};
            }
            const auto minimum_name = *minimum_type->as_string();
            bool three_layers = false;
            if (minimum_name == "minecraft:three_layers_feature_size") {
                three_layers = true;
            } else if (minimum_name != "minecraft:two_layers_feature_size") {
                return std::unexpected{make_error(error_code::unsupported,
                                                  "unsupported feature size")};
            }
            auto trunk = decode_provider(*trunk_value);
            auto foliage = decode_provider(*foliage_value);
            auto below = decode_provider(*below_value);
            const auto base_height = trunk_placer->find("base_height");
            const auto rand_a = trunk_placer->find("height_rand_a");
            const auto rand_b = trunk_placer->find("height_rand_b");
            const auto radius = foliage_placer->find("radius");
            const auto offset = foliage_placer->find("offset");
            const auto *height = foliage_placer->find("height");
            if (!trunk || !foliage || !below || base_height == nullptr ||
                rand_a == nullptr || rand_b == nullptr || radius == nullptr ||
                offset == nullptr ||
                (height == nullptr &&
                 foliage_type_kind == foliage_kind::blob)) {
                return std::unexpected{make_error(error_code::parse_error,
                                                  "tree feature is invalid")};
            }
            const auto base_value = base_height->as_int64();
            const auto rand_a_value = rand_a->as_int64();
            const auto rand_b_value = rand_b->as_int64();
            jint height_value = 0;
            if (height != nullptr && foliage_type_kind != foliage_kind::pine) {
                const auto parsed_height = height->as_int64();
                if (!parsed_height) {
                    return std::unexpected{parsed_height.error()};
                }
                height_value = static_cast<jint>(*parsed_height);
            }
            auto radius_provider = provider::int_provider::decode(*radius);
            auto offset_provider = provider::int_provider::decode(*offset);
            std::unique_ptr<provider::int_provider> mega_pine_crown;
            if (foliage_type_kind == foliage_kind::mega_pine) {
                const auto *crown_height = foliage_placer->find("crown_height");
                if (crown_height == nullptr) {
                    return std::unexpected{
                        make_error(error_code::parse_error,
                                   "mega pine foliage has no crown_height")};
                }
                auto decoded = provider::int_provider::decode(*crown_height);
                if (!decoded) {
                    return std::unexpected{decoded.error()};
                }
                mega_pine_crown = std::move(*decoded);
            }
            std::unique_ptr<provider::int_provider> spruce_trunk_height;
            if (foliage_type_kind == foliage_kind::spruce) {
                const auto *trunk_height = foliage_placer->find("trunk_height");
                if (trunk_height == nullptr) {
                    return std::unexpected{
                        make_error(error_code::parse_error,
                                   "spruce foliage has no trunk_height")};
                }
                auto decoded = provider::int_provider::decode(*trunk_height);
                if (!decoded) {
                    return std::unexpected{decoded.error()};
                }
                spruce_trunk_height = std::move(*decoded);
            }
            if (!base_value || !rand_a_value || !rand_b_value ||
                !radius_provider || !offset_provider) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "tree feature numbers are invalid")};
            }
            jint limit = 1;
            jint lower = 0;
            jint upper = 1;
            if (const auto *field = minimum_size->find("limit");
                field != nullptr) {
                const auto value = field->as_int64();
                if (!value) {
                    return std::unexpected{value.error()};
                }
                limit = static_cast<jint>(*value);
            }
            if (const auto *field = minimum_size->find("lower_size");
                field != nullptr) {
                const auto value = field->as_int64();
                if (!value) {
                    return std::unexpected{value.error()};
                }
                lower = static_cast<jint>(*value);
            }
            if (const auto *field = minimum_size->find("upper_size");
                field != nullptr) {
                const auto value = field->as_int64();
                if (!value) {
                    return std::unexpected{value.error()};
                }
                upper = static_cast<jint>(*value);
            }
            jint upper_limit = 1;
            jint middle = 1;
            if (three_layers) {
                if (const auto *field = minimum_size->find("upper_limit");
                    field != nullptr) {
                    const auto value = field->as_int64();
                    if (!value) {
                        return std::unexpected{value.error()};
                    }
                    upper_limit = static_cast<jint>(*value);
                }
                if (const auto *field = minimum_size->find("middle_size");
                    field != nullptr) {
                    const auto value = field->as_int64();
                    if (!value) {
                        return std::unexpected{value.error()};
                    }
                    middle = static_cast<jint>(*value);
                }
            }
            bool has_min_clipped = false;
            jint min_clipped = 0;
            if (const auto *field = minimum_size->find("min_clipped_height");
                field != nullptr) {
                const auto value = field->as_int64();
                if (!value) {
                    return std::unexpected{value.error()};
                }
                has_min_clipped = true;
                min_clipped = static_cast<jint>(*value);
            }
            bool ignore_vines = false;
            if (const auto *field = document.document.find("ignore_vines");
                field != nullptr) {
                const auto value = field->as_bool();
                if (!value) {
                    return std::unexpected{value.error()};
                }
                ignore_vines = *value;
            }
            features_.emplace(
                id,
                std::make_unique<tree_feature>(
                    *states_, kind, foliage_type_kind, std::move(*trunk),
                    std::move(*foliage), std::move(*below),
                    static_cast<jint>(*base_value),
                    static_cast<jint>(*rand_a_value),
                    static_cast<jint>(*rand_b_value),
                    std::move(*radius_provider), std::move(*offset_provider),
                    std::move(spruce_trunk_height), std::move(mega_pine_crown),
                    height_value, limit, lower, upper, three_layers,
                    upper_limit, middle, has_min_clipped, min_clipped,
                    ignore_vines, std::move(decoded_decorators)));
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
            auto modifier = decode_placement(entry, *states_);
            if (!modifier) {
                return std::unexpected{modifier.error()};
            }
            runtime.placement.push_back(std::move(*modifier));
        }
        placed_.emplace(id, std::move(runtime));
    }
    for (const auto &nested : nested_placed_) {
        const auto configured = features_.find(nested.feature);
        if (configured == features_.end()) {
            return std::unexpected{
                make_error(error_code::not_found,
                           "nested placed feature names a missing feature")};
        }
        placed_feature_runtime runtime;
        runtime.id = nested.id;
        runtime.configured = configured->second.get();
        for (const auto &entry : *nested.placement.as_array()) {
            auto modifier = decode_placement(entry, *states_);
            if (!modifier) {
                return std::unexpected{modifier.error()};
            }
            runtime.placement.push_back(std::move(*modifier));
        }
        placed_.emplace(nested.id, std::move(runtime));
    }
    return {};
}

} // namespace refrain::worldgen::feature
