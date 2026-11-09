// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/feature/feature_registry.hpp>

#include <refrain/worldgen/chunk/block_state_codec.hpp>
#include <refrain/worldgen/feature/rule_test.hpp>
#include <refrain/worldgen/feature/tree.hpp>

#include <limits>
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

/// Decodes a template feature body. The body is reused for registry entries
/// and for the inline features an overlay or selector can embed.
auto decode_template_feature_document(block_state_registry &states,
                                      const data::json::value &document)
    -> result<std::unique_ptr<feature>> {
    const auto *templates = document.find("templates");
    if (templates == nullptr || !templates->is_array() ||
        templates->as_array()->empty()) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "template feature needs a non-empty templates list")};
    }
    std::vector<std::pair<jint, template_feature::entry>> entries;
    jint total_weight = 0;
    for (const auto &raw : *templates->as_array()) {
        const auto *data = raw.find("data");
        const auto *weight = raw.find("weight");
        if (data == nullptr || !data->is_object() || weight == nullptr) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "template entry is missing data or weight")};
        }
        const auto weight_value = weight->as_int64();
        if (!weight_value || *weight_value < 0 ||
            *weight_value > std::numeric_limits<jint>::max() - total_weight) {
            return std::unexpected{make_error(
                error_code::parse_error, "template entry weight is invalid")};
        }
        const auto *template_id = data->find("id");
        if (template_id == nullptr) {
            return std::unexpected{make_error(
                error_code::parse_error, "template entry has no template id")};
        }
        const auto template_text = template_id->as_string();
        if (!template_text) {
            return std::unexpected{template_text.error()};
        }
        std::vector<structure::template_rotation> rotations;
        if (const auto *rotation_list = data->find("rotations");
            rotation_list != nullptr) {
            if (!rotation_list->is_array() ||
                rotation_list->as_array()->empty()) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "template rotations must be a non-empty list")};
            }
            for (const auto &rotation : *rotation_list->as_array()) {
                const auto name = rotation.as_string();
                if (!name) {
                    return std::unexpected{name.error()};
                }
                if (*name == "none") {
                    rotations.push_back(structure::template_rotation::none);
                } else if (*name == "clockwise_90") {
                    rotations.push_back(
                        structure::template_rotation::clockwise_90);
                } else if (*name == "180") {
                    rotations.push_back(
                        structure::template_rotation::clockwise_180);
                } else if (*name == "counterclockwise_90") {
                    rotations.push_back(
                        structure::template_rotation::counterclockwise_90);
                } else {
                    return std::unexpected{
                        make_error(error_code::parse_error,
                                   "template rotation name is invalid")};
                }
            }
        } else {
            rotations = {structure::template_rotation::none,
                         structure::template_rotation::clockwise_90,
                         structure::template_rotation::clockwise_180,
                         structure::template_rotation::counterclockwise_90};
        }
        total_weight += static_cast<jint>(*weight_value);
        entries.push_back(std::pair{
            static_cast<jint>(*weight_value),
            template_feature::entry{.id = std::string{*template_text},
                                    .rotations = std::move(rotations)}});
    }
    if (total_weight == 0) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "template feature has no positive weight")};
    }
    std::shared_ptr<structure::structure_processor_list> processors;
    std::string processor_id;
    if (const auto *processor_value = document.find("processors");
        processor_value != nullptr) {
        if (const auto text = processor_value->as_string(); text.has_value()) {
            processor_id = std::string{*text};
        } else if (processor_value->is_object()) {
            auto decoded =
                structure::decode_processor_list(states, *processor_value);
            if (!decoded) {
                return std::unexpected{decoded.error()};
            }
            processors = std::move(*decoded);
        } else {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "template processors must be a list or an id")};
        }
    }
    return std::unique_ptr<feature>{std::make_unique<template_feature>(
        std::move(entries), total_weight, std::move(processors),
        std::move(processor_id))};
}

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
    // heightmap counts. The listed states have a collision shape the tag
    // counts but a heightmap must ignore; a partial snow layer has no
    // collision shape at all.
    auto traits = states.infer_traits(key);
    const auto block = state_block_name(key);
    if (block == "minecraft:snow" &&
        key.find("layers=8") == std::string_view::npos) {
        traits.blocks_motion = false;
        traits.blocks_motion_no_leaves = false;
    }
    const bool non_solid =
        block == "minecraft:end_portal" || block == "minecraft:end_gateway" ||
        block == "minecraft:fire" || block == "minecraft:torch" ||
        block == "minecraft:wall_torch" || block == "minecraft:soul_torch" ||
        block == "minecraft:soul_wall_torch" ||
        block == "minecraft:glow_lichen" || block == "minecraft:sculk_vein";
    if (non_solid) {
        traits.blocks_motion = false;
        traits.blocks_motion_no_leaves = false;
    }
    return states.intern(key, traits);
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
    provider_error_.reset();
    auto decoded = decode_state_provider(
        document, *states_,
        [this](std::string_view id) -> const state_provider * {
            return resolve_provider(id);
        });
    if (provider_error_) {
        return std::unexpected{*provider_error_};
    }
    return decoded;
}

auto feature_registry::resolve_provider(std::string_view id)
    -> const state_provider * {
    const std::string key{id};
    if (const auto found = providers_.find(key); found != providers_.end()) {
        return found->second.get();
    }
    const auto document = provider_documents_.find(key);
    if (document == provider_documents_.end()) {
        return nullptr;
    }
    if (!decoding_providers_.insert(key).second) {
        if (!provider_error_) {
            provider_error_ = make_error(error_code::parse_error,
                                         "state provider reference cycle");
        }
        return nullptr;
    }
    auto decoded = decode_state_provider(
        document->second.document, *states_,
        [this](std::string_view nested) -> const state_provider * {
            return resolve_provider(nested);
        });
    decoding_providers_.erase(key);
    if (!decoded) {
        if (!provider_error_) {
            provider_error_ = decoded.error();
        }
        return nullptr;
    }
    return providers_.emplace(key, std::move(*decoded)).first->second.get();
}

auto feature_registry::nested_reference(const data::json::value &input,
                                        bool entry_reference)
    -> result<std::string> {
    if (const auto text = input.as_string(); text.has_value()) {
        if (entry_reference) {
            referenced_placed_.insert(std::string{*text});
        }
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
        if (feature->find("type") == nullptr) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "inline feature has no type")};
        }
        if (pending_documents_ == nullptr) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "inline feature is not allowed in this position")};
        }
        // An inline configured feature is decoded by the same dispatch as a
        // document from the data set; the synthetic id keeps the reference
        // stable for the nesting entry built below.
        feature_id = "#inline:" + std::to_string(next_nested_++);
        pending_documents_->push_back(configured_feature_document{
            .id = feature_id, .document = *feature});
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

namespace {

/// Collects string values that name a configured feature. Selectors, nested
/// feature lists and overlay documents spell their configured references as
/// plain strings, so a structural walk finds the closure without decoding the
/// documents first.
void collect_feature_references(
    const data::json::value &document,
    const std::unordered_map<std::string, configured_feature_document>
        &features,
    std::set<std::string> &out) {
    if (document.is_string()) {
        const auto text = document.as_string();
        if (!text) {
            return;
        }
        const std::string id{*text};
        if (features.contains(id)) {
            out.insert(id);
        }
        return;
    }
    if (document.is_array()) {
        const auto values = document.as_array();
        if (!values) {
            return;
        }
        for (const auto &entry : *values) {
            collect_feature_references(entry, features, out);
        }
        return;
    }
    if (document.is_object()) {
        const auto members = document.as_object();
        if (!members) {
            return;
        }
        for (const auto &[key, value] : members->get()) {
            (void)key;
            collect_feature_references(value, features, out);
        }
    }
}

} // namespace

auto feature_registry::load_placed(const feature_data_set &data,
                                   std::span<const std::string> ids) -> status {
    std::set<std::string> placed_ids(ids.begin(), ids.end());
    // Selectors name placed features directly, and a placed feature can point
    // at a configured feature with a different name. Decode, collect the
    // placed references that turned up, and repeat until the closure is
    // stable.
    for (;;) {
        feature_data_set subset;
        subset.state_providers = data.state_providers;
        std::set<std::string> pending;
        for (const auto &id : placed_ids) {
            const auto found = data.placed.find(id);
            if (found == data.placed.end()) {
                return std::unexpected{
                    make_error(error_code::not_found, "placed feature '" + id +
                                                          "' is not in the "
                                                          "data pack")};
            }
            subset.placed.emplace(found->first, found->second);
            pending.insert(found->second.feature);
        }
        std::set<std::string> decoded;
        while (!pending.empty()) {
            const std::string feature = *pending.begin();
            pending.erase(pending.begin());
            if (!decoded.insert(feature).second) {
                continue;
            }
            const auto found = data.features.find(feature);
            if (found == data.features.end()) {
                return std::unexpected{make_error(
                    error_code::not_found,
                    "placed feature names missing configured feature '" +
                        feature + "'")};
            }
            subset.features.emplace(found->first, found->second);
            collect_feature_references(found->second.document, data.features,
                                       pending);
        }
        if (const auto loaded = load(subset); !loaded) {
            return loaded;
        }
        std::set<std::string> discovered;
        for (const auto &id : referenced_placed_) {
            if (!data.placed.contains(id)) {
                return std::unexpected{make_error(
                    error_code::not_found, "nested placed feature '" + id +
                                               "' is not in the data pack")};
            }
            if (!placed_ids.contains(id)) {
                discovered.insert(id);
            }
        }
        if (discovered.empty()) {
            return status_ok();
        }
        placed_ids.insert(discovered.begin(), discovered.end());
    }
}

auto feature_registry::configured(std::string_view id) const
    -> const feature * {
    const auto found = features_.find(std::string{id});
    return found == features_.end() ? nullptr : found->second.get();
}

auto feature_registry::load(const feature_data_set &data) -> status {
    nested_placed_.clear();
    pending_documents_ = nullptr;
    referenced_placed_.clear();
    features_.clear();
    placed_.clear();
    next_nested_ = 0;
    providers_.clear();
    decoding_providers_.clear();
    provider_error_.reset();
    provider_documents_ = data.state_providers;
    // Inline configured features append themselves to this list while the
    // selected documents decode, so one pass decodes the whole closure.
    std::deque<configured_feature_document> documents;
    for (const auto &[id, document] : data.features) {
        (void)id;
        documents.push_back(document);
    }
    pending_documents_ = &documents;
    for (std::size_t index = 0; index < documents.size(); ++index) {
        const auto &id = documents[index].id;
        const auto &document = documents[index];
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
        if (*type == "minecraft:sequence") {
            const auto *features = document.document.find("features");
            if (features == nullptr || !features->is_array()) {
                return std::unexpected{make_error(
                    error_code::parse_error, "sequence feature is incomplete")};
            }
            std::vector<std::string> nested;
            for (const auto &entry : *features->as_array()) {
                auto reference = nested_reference(entry, true);
                if (!reference) {
                    return std::unexpected{reference.error()};
                }
                nested.push_back(std::move(*reference));
            }
            features_.emplace(
                id, std::make_unique<sequence_feature>(std::move(nested)));
            continue;
        }
        if (*type == "minecraft:netherrack_replace_blobs" ||
            *type == "minecraft:replace_blobs") {
            const auto *target = document.document.find("target");
            const auto *replacement = document.document.find("state");
            const auto *radius = document.document.find("radius");
            if (target == nullptr || replacement == nullptr ||
                radius == nullptr) {
                return std::unexpected{make_error(
                    error_code::parse_error, "replace blobs is incomplete")};
            }
            const auto target_key = decode_state_key(*target);
            const auto replacement_key = decode_state_key(*replacement);
            auto radius_provider = provider::int_provider::decode(*radius);
            if (!target_key || !replacement_key || !radius_provider) {
                return std::unexpected{make_error(error_code::parse_error,
                                                  "replace blobs is invalid")};
            }
            auto target_state = resolve_state(*states_, *target_key);
            auto replacement_state = resolve_state(*states_, *replacement_key);
            if (!target_state || !replacement_state) {
                return std::unexpected{make_error(
                    error_code::parse_error, "replace blobs state is invalid")};
            }
            features_.emplace(id, std::make_unique<replace_blobs_feature>(
                                      *target_state, *replacement_state,
                                      std::move(*radius_provider)));
            continue;
        }
        if (*type == "minecraft:blue_ice") {
            const auto blue_ice = resolve_state(*states_, "minecraft:blue_ice");
            const auto water = resolve_state(*states_, "minecraft:water");
            const auto packed_ice =
                resolve_state(*states_, "minecraft:packed_ice");
            const auto ice = resolve_state(*states_, "minecraft:ice");
            const auto air = resolve_state(*states_, "minecraft:air");
            if (!blue_ice || !water || !packed_ice || !ice || !air) {
                return std::unexpected{make_error(
                    error_code::parse_error, "blue ice states are invalid")};
            }
            features_.emplace(id,
                              std::make_unique<blue_ice_feature>(
                                  *blue_ice, *water, *packed_ice, *ice, *air));
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
            auto state = decode_provider(*state_provider);
            auto predicate = decode_block_predicate(*target, *states_);
            auto radius_provider = provider::int_provider::decode(*radius);
            const auto half = half_height->as_int64();
            if (!state || !predicate || !radius_provider || !half) {
                return std::unexpected{make_error(error_code::parse_error,
                                                  "disk feature is invalid")};
            }
            features_.emplace(id, std::make_unique<disk_feature>(
                                      std::move(*state), std::move(*predicate),
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
        if (*type == "minecraft:large_dripstone") {
            const auto *replaceable =
                document.document.find("replaceable_blocks");
            const auto *radius = document.document.find("column_radius");
            const auto *height_scale = document.document.find("height_scale");
            const auto *stalactite =
                document.document.find("stalactite_bluntness");
            const auto *stalagmite =
                document.document.find("stalagmite_bluntness");
            const auto *wind = document.document.find("wind_speed");
            if (replaceable == nullptr || radius == nullptr ||
                height_scale == nullptr || stalactite == nullptr ||
                stalagmite == nullptr || wind == nullptr) {
                return std::unexpected{make_error(
                    error_code::parse_error, "large dripstone is incomplete")};
            }
            const auto tag = replaceable->as_string();
            auto radius_provider = provider::int_provider::decode(*radius);
            auto height_provider =
                provider::float_provider::decode(*height_scale);
            auto stalactite_provider =
                provider::float_provider::decode(*stalactite);
            auto stalagmite_provider =
                provider::float_provider::decode(*stalagmite);
            auto wind_provider = provider::float_provider::decode(*wind);
            if (!tag || !radius_provider || !height_provider ||
                !stalactite_provider || !stalagmite_provider ||
                !wind_provider) {
                return std::unexpected{make_error(
                    error_code::parse_error, "large dripstone is invalid")};
            }
            jint radius_min = 1;
            jint radius_max = 16;
            if (const auto *value = radius->find("min_inclusive");
                value != nullptr) {
                if (const auto parsed = value->as_int64(); parsed) {
                    radius_min = static_cast<jint>(*parsed);
                }
            }
            if (const auto *value = radius->find("max_inclusive");
                value != nullptr) {
                if (const auto parsed = value->as_int64(); parsed) {
                    radius_max = static_cast<jint>(*parsed);
                }
            }
            const auto read_float = [&](std::string_view name, float fallback) {
                const auto *field = document.document.find(name);
                if (field == nullptr) {
                    return fallback;
                }
                const auto value = field->as_double();
                return value ? static_cast<float>(*value) : fallback;
            };
            const auto read_int = [&](std::string_view name, jint fallback) {
                const auto *field = document.document.find(name);
                if (field == nullptr) {
                    return fallback;
                }
                const auto value = field->as_int64();
                return value ? static_cast<jint>(*value) : fallback;
            };
            features_.emplace(
                id,
                std::make_unique<large_dripstone_feature>(
                    std::string{tag->starts_with('#') ? tag->substr(1) : *tag},
                    read_int("floor_to_ceiling_search_range", 30),
                    std::move(*radius_provider), radius_min, radius_max,
                    std::move(*height_provider),
                    read_float("max_column_radius_to_cave_height_ratio", 0.33F),
                    std::move(*stalactite_provider),
                    std::move(*stalagmite_provider), std::move(*wind_provider),
                    read_int("min_radius_for_wind", 4),
                    read_float("min_bluntness_for_wind", 0.6F)));
            continue;
        }
        if (*type == "minecraft:geode") {
            const auto *blocks = document.document.find("blocks");
            const auto *layers = document.document.find("layers");
            const auto *crack = document.document.find("crack");
            if (blocks == nullptr) {
                return std::unexpected{make_error(error_code::parse_error,
                                                  "geode blocks are missing")};
            }
            geode_parameters parameters;
            const auto decode_provider_field =
                [&](std::string_view name) -> std::unique_ptr<state_provider> {
                const auto *field = blocks->find(name);
                if (field == nullptr) {
                    return nullptr;
                }
                auto provider = decode_provider(*field);
                if (!provider) {
                    return nullptr;
                }
                return std::move(*provider);
            };
            parameters.filling = decode_provider_field("filling_provider");
            parameters.inner = decode_provider_field("inner_layer_provider");
            parameters.alternate_inner =
                decode_provider_field("alternate_inner_layer_provider");
            parameters.middle = decode_provider_field("middle_layer_provider");
            parameters.outer = decode_provider_field("outer_layer_provider");
            const auto *cannot = blocks->find("cannot_replace");
            const auto *invalid = blocks->find("invalid_blocks");
            if (!parameters.filling || !parameters.inner ||
                !parameters.alternate_inner || !parameters.middle ||
                !parameters.outer || cannot == nullptr || invalid == nullptr) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "geode block settings are invalid")};
            }
            const auto cannot_name = cannot->as_string();
            const auto invalid_name = invalid->as_string();
            if (!cannot_name || !invalid_name) {
                return std::unexpected{make_error(
                    error_code::parse_error, "geode tags are not strings")};
            }
            parameters.cannot_replace_tag = std::string{
                cannot_name->starts_with('#') ? cannot_name->substr(1)
                                              : *cannot_name};
            parameters.invalid_blocks_tag = std::string{
                invalid_name->starts_with('#') ? invalid_name->substr(1)
                                               : *invalid_name};
            if (const auto *placements = blocks->find("inner_placements");
                placements != nullptr && placements->is_array()) {
                for (const auto &entry : *placements->as_array()) {
                    const auto key = decode_state_key(entry);
                    if (!key) {
                        return std::unexpected{key.error()};
                    }
                    const auto state = resolve_state(*states_, *key);
                    if (!state) {
                        return std::unexpected{state.error()};
                    }
                    parameters.inner_placements.push_back(*state);
                }
            }
            if (layers != nullptr) {
                const auto read_double = [&](std::string_view name,
                                             double fallback) {
                    const auto *field = layers->find(name);
                    if (field == nullptr) {
                        return fallback;
                    }
                    const auto value = field->as_double();
                    return value ? *value : fallback;
                };
                parameters.filling_threshold =
                    read_double("filling", parameters.filling_threshold);
                parameters.inner_threshold =
                    read_double("inner_layer", parameters.inner_threshold);
                parameters.middle_threshold =
                    read_double("middle_layer", parameters.middle_threshold);
                parameters.outer_threshold =
                    read_double("outer_layer", parameters.outer_threshold);
            }
            if (crack != nullptr) {
                const auto read_double = [&](std::string_view name,
                                             double fallback) {
                    const auto *field = crack->find(name);
                    if (field == nullptr) {
                        return fallback;
                    }
                    const auto value = field->as_double();
                    return value ? *value : fallback;
                };
                parameters.crack_chance = read_double("generate_crack_chance",
                                                      parameters.crack_chance);
                parameters.base_crack_size =
                    read_double("base_crack_size", parameters.base_crack_size);
                if (const auto *field = crack->find("crack_point_offset");
                    field != nullptr) {
                    const auto value = field->as_int64();
                    if (value) {
                        parameters.crack_point_offset =
                            static_cast<jint>(*value);
                    }
                }
            }
            const auto read_optional_double = [&](std::string_view name,
                                                  double fallback) {
                const auto *field = document.document.find(name);
                if (field == nullptr) {
                    return fallback;
                }
                const auto value = field->as_double();
                return value ? *value : fallback;
            };
            parameters.use_potential_placements_chance = read_optional_double(
                "use_potential_placements_chance",
                parameters.use_potential_placements_chance);
            parameters.use_alternate_layer0_chance =
                read_optional_double("use_alternate_layer0_chance",
                                     parameters.use_alternate_layer0_chance);
            if (const auto *field = document.document.find(
                    "placements_require_layer0_alternate");
                field != nullptr) {
                const auto value = field->as_bool();
                if (value) {
                    parameters.placements_require_layer0_alternate = *value;
                }
            }
            const auto read_int = [&](std::string_view name, jint fallback) {
                const auto *field = document.document.find(name);
                if (field == nullptr) {
                    return fallback;
                }
                const auto value = field->as_int64();
                return value ? static_cast<jint>(*value) : fallback;
            };
            parameters.min_gen_offset =
                read_int("min_gen_offset", parameters.min_gen_offset);
            parameters.max_gen_offset =
                read_int("max_gen_offset", parameters.max_gen_offset);
            parameters.noise_multiplier = read_optional_double(
                "noise_multiplier", parameters.noise_multiplier);
            parameters.invalid_blocks_threshold =
                read_int("invalid_blocks_threshold",
                         parameters.invalid_blocks_threshold);
            const auto read_bounds = [&](std::string_view name, jint &min,
                                         jint &max) {
                const auto *field = document.document.find(name);
                if (field == nullptr) {
                    return;
                }
                if (const auto *value = field->find("value");
                    value != nullptr) {
                    if (const auto parsed = value->as_int64(); parsed) {
                        min = max = static_cast<jint>(*parsed);
                    }
                    return;
                }
                if (const auto *value = field->find("min_inclusive");
                    value != nullptr) {
                    if (const auto parsed = value->as_int64(); parsed) {
                        min = static_cast<jint>(*parsed);
                    }
                }
                if (const auto *value = field->find("max_inclusive");
                    value != nullptr) {
                    if (const auto parsed = value->as_int64(); parsed) {
                        max = static_cast<jint>(*parsed);
                    }
                }
            };
            read_bounds("outer_wall_distance", parameters.outer_wall_min,
                        parameters.outer_wall_max);
            read_bounds("distribution_points", parameters.distribution_min,
                        parameters.distribution_max);
            read_bounds("point_offset", parameters.point_offset_min,
                        parameters.point_offset_max);
            if (const auto *field =
                    document.document.find("outer_wall_distance");
                field != nullptr) {
                auto decoded = provider::int_provider::decode(*field);
                if (decoded) {
                    parameters.outer_wall_provider = std::move(*decoded);
                }
            }
            if (const auto *field =
                    document.document.find("distribution_points");
                field != nullptr) {
                auto decoded = provider::int_provider::decode(*field);
                if (decoded) {
                    parameters.distribution_provider = std::move(*decoded);
                }
            }
            if (const auto *field = document.document.find("point_offset");
                field != nullptr) {
                auto decoded = provider::int_provider::decode(*field);
                if (decoded) {
                    parameters.point_offset_provider = std::move(*decoded);
                }
            }
            features_.emplace(
                id, std::make_unique<geode_feature>(std::move(parameters)));
            continue;
        }
        if (*type == "minecraft:delta_feature") {
            const auto *contents = document.document.find("contents");
            const auto *rim = document.document.find("rim");
            const auto *size = document.document.find("size");
            const auto *rim_size = document.document.find("rim_size");
            if (contents == nullptr || rim == nullptr || size == nullptr ||
                rim_size == nullptr) {
                return std::unexpected{make_error(
                    error_code::parse_error, "delta feature is incomplete")};
            }
            const auto contents_key = decode_state_key(*contents);
            const auto rim_key = decode_state_key(*rim);
            auto size_provider = provider::int_provider::decode(*size);
            auto rim_provider = provider::int_provider::decode(*rim_size);
            if (!contents_key || !rim_key || !size_provider || !rim_provider) {
                return std::unexpected{make_error(error_code::parse_error,
                                                  "delta feature is invalid")};
            }
            const auto contents_state = resolve_state(*states_, *contents_key);
            const auto rim_state = resolve_state(*states_, *rim_key);
            if (!contents_state || !rim_state) {
                return std::unexpected{make_error(error_code::parse_error,
                                                  "delta states are invalid")};
            }
            features_.emplace(
                id, std::make_unique<delta_feature>(*contents_state, *rim_state,
                                                    std::move(*size_provider),
                                                    std::move(*rim_provider)));
            continue;
        }
        if (*type == "minecraft:multiface_growth") {
            const auto *block = document.document.find("block");
            const auto *search = document.document.find("search_range");
            const auto *floor = document.document.find("can_place_on_floor");
            const auto *ceiling =
                document.document.find("can_place_on_ceiling");
            const auto *wall = document.document.find("can_place_on_wall");
            const auto *chance = document.document.find("chance_of_spreading");
            const auto *place_on = document.document.find("can_be_placed_on");
            const auto string_of = [](const data::json::value *value)
                -> std::optional<std::string> {
                if (value == nullptr) {
                    return std::nullopt;
                }
                const auto text = value->as_string();
                if (!text) {
                    return std::nullopt;
                }
                return std::string{*text};
            };
            const auto array_of = [](const data::json::value *value)
                -> std::optional<std::span<const data::json::value>> {
                if (value == nullptr) {
                    return std::nullopt;
                }
                const auto array = value->as_array();
                if (!array) {
                    return std::nullopt;
                }
                return *array;
            };
            const auto block_text = string_of(block);
            const auto place_array = array_of(place_on);
            if (!block_text || !place_array) {
                return std::unexpected{make_error(
                    error_code::parse_error, "multiface growth is invalid")};
            }
            std::vector<std::string> place_on_ids;
            for (const auto &item : *place_array) {
                const auto item_text = item.as_string();
                if (!item_text) {
                    return std::unexpected{make_error(
                        error_code::parse_error, "multiface block is invalid")};
                }
                place_on_ids.emplace_back(*item_text);
            }
            const jint search_value =
                search == nullptr ? 10 : search->as_int64().value_or(10);
            const bool floor_value =
                floor != nullptr && floor->as_bool().value_or(false);
            const bool ceiling_value =
                ceiling != nullptr && ceiling->as_bool().value_or(false);
            const bool wall_value =
                wall != nullptr && wall->as_bool().value_or(false);
            const float chance_value = static_cast<float>(
                chance == nullptr ? 0.5 : chance->as_double().value_or(0.5));
            features_.emplace(id, std::make_unique<multiface_growth_feature>(
                                      std::string{*block_text}, search_value,
                                      floor_value, ceiling_value, wall_value,
                                      chance_value, std::move(place_on_ids)));
            continue;
        }
        if (*type == "minecraft:void_start_platform") {
            features_.emplace(id,
                              std::make_unique<void_start_platform_feature>());
            continue;
        }
        if (*type == "minecraft:end_platform") {
            features_.emplace(id, std::make_unique<end_platform_feature>());
            continue;
        }
        if (*type == "minecraft:end_spike") {
            std::vector<end_spike> spikes;
            const auto *list = document.document.find("spikes");
            if (list != nullptr && list->as_array()) {
                for (const auto &entry : *list->as_array()) {
                    end_spike spike{};
                    const auto read_int = [&](const char *key, jint fallback) {
                        const auto *value = entry.find(key);
                        return value == nullptr
                                   ? fallback
                                   : static_cast<jint>(
                                         value->as_int64().value_or(fallback));
                    };
                    spike.center_x = read_int("centerX", 0);
                    spike.center_z = read_int("centerZ", 0);
                    spike.radius = read_int("radius", 0);
                    spike.height = read_int("height", 0);
                    if (const auto *guarded = entry.find("guarded");
                        guarded != nullptr) {
                        spike.guarded = guarded->as_bool().value_or(false);
                    }
                    spikes.push_back(spike);
                }
            }
            bool invulnerable = false;
            if (const auto *value =
                    document.document.find("crystal_invulnerable");
                value != nullptr) {
                invulnerable = value->as_bool().value_or(false);
            }
            features_.emplace(id, std::make_unique<end_spike_feature>(
                                      std::move(spikes), invulnerable));
            continue;
        }
        if (*type == "minecraft:bonus_chest") {
            const auto chest = resolve_state(*states_, "minecraft:chest");
            const auto torch = resolve_state(*states_, "minecraft:torch");
            if (!chest || !torch) {
                return std::unexpected{make_error(
                    error_code::parse_error, "bonus chest states are invalid")};
            }
            features_.emplace(
                id, std::make_unique<bonus_chest_feature>(*chest, *torch));
            continue;
        }
        if (*type == "minecraft:chorus_plant") {
            const auto plant =
                resolve_state(*states_, "minecraft:chorus_plant");
            const auto flower =
                resolve_state(*states_, "minecraft:chorus_flower[age=5]");
            if (!plant || !flower) {
                return std::unexpected{make_error(error_code::parse_error,
                                                  "chorus states are invalid")};
            }
            features_.emplace(
                id, std::make_unique<chorus_plant_feature>(*plant, *flower));
            continue;
        }
        if (*type == "minecraft:root_system") {
            const auto *feature_field = document.document.find("feature");
            if (feature_field == nullptr) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "root system has no nested feature")};
            }
            auto nested = nested_reference(*feature_field, true);
            const auto *root_provider =
                document.document.find("root_state_provider");
            const auto *hanging_provider =
                document.document.find("hanging_root_state_provider");
            const auto *predicate =
                document.document.find("allowed_tree_position");
            if (root_provider == nullptr || hanging_provider == nullptr ||
                predicate == nullptr) {
                return std::unexpected{make_error(
                    error_code::parse_error, "root system states are missing")};
            }
            auto root_state = decode_provider(*root_provider);
            auto hanging_state = decode_provider(*hanging_provider);
            auto allowed = decode_block_predicate(*predicate, *states_);
            if (!nested || !root_state || !hanging_state || !allowed) {
                return std::unexpected{make_error(
                    error_code::parse_error, "root system states are invalid")};
            }
            const auto *replaceable_field =
                document.document.find("root_replaceable");
            const std::string replaceable =
                replaceable_field == nullptr
                    ? std::string{}
                    : std::string{replaceable_field->as_string().value_or("")};
            const auto read_int = [&](const char *key, jint fallback) {
                const auto *value = document.document.find(key);
                return value == nullptr
                           ? fallback
                           : static_cast<jint>(
                                 value->as_int64().value_or(fallback));
            };
            features_.emplace(
                id,
                std::make_unique<root_system_feature>(
                    std::move(*nested), std::move(*root_state),
                    std::move(*hanging_state), std::move(*allowed), replaceable,
                    read_int("required_vertical_space_for_tree", 3),
                    read_int("level_test_distance", 0),
                    read_int("max_level_deviation", 0),
                    read_int("root_radius", 3),
                    read_int("root_placement_attempts", 20),
                    read_int("root_column_max_height", 100),
                    read_int("hanging_root_radius", 3),
                    read_int("hanging_roots_vertical_span", 2),
                    read_int("hanging_root_placement_attempts", 20),
                    read_int("allowed_vertical_water_for_tree", 2)));
            continue;
        }
        if (*type == "minecraft:vegetation_patch" ||
            *type == "minecraft:waterlogged_vegetation_patch") {
            const auto *nested = document.document.find("vegetation_feature");
            const auto *ground = document.document.find("ground_state");
            const auto *depth = document.document.find("depth");
            const auto *radius = document.document.find("xz_radius");
            const auto *replaceable = document.document.find("replaceable");
            const auto *surface = document.document.find("surface");
            if (nested == nullptr || ground == nullptr || depth == nullptr ||
                radius == nullptr || replaceable == nullptr ||
                surface == nullptr) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "vegetation patch config is incomplete")};
            }
            auto nested_id = nested_reference(*nested, true);
            auto ground_state = decode_provider(*ground);
            auto depth_provider = provider::int_provider::decode(*depth);
            auto radius_provider = provider::int_provider::decode(*radius);
            if (!nested_id || !ground_state || !depth_provider ||
                !radius_provider) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "vegetation patch config is invalid")};
            }
            const auto surface_name = surface->as_string().value_or("floor");
            const auto read_float = [&](const char *key) {
                const auto *value = document.document.find(key);
                return value == nullptr ? 0.0F
                                        : static_cast<float>(
                                              value->as_double().value_or(0.0));
            };
            const auto read_int = [&](const char *key, jint fallback) {
                const auto *value = document.document.find(key);
                return value == nullptr
                           ? fallback
                           : static_cast<jint>(
                                 value->as_int64().value_or(fallback));
            };
            features_.emplace(
                id,
                std::make_unique<vegetation_patch_feature>(
                    std::move(*nested_id), std::move(*ground_state),
                    std::string{replaceable->as_string().value_or("")},
                    std::move(*depth_provider), std::move(*radius_provider),
                    surface_name == "ceiling", read_int("vertical_range", 5),
                    read_float("vegetation_chance"),
                    read_float("extra_bottom_block_chance"),
                    read_float("extra_edge_column_chance"),
                    *type == "minecraft:waterlogged_vegetation_patch"));
            continue;
        }
        if (*type == "minecraft:end_podium") {
            const auto *active = document.document.find("active");
            const bool is_active =
                active != nullptr && active->as_bool().value_or(false);
            const auto bedrock = resolve_state(*states_, "minecraft:bedrock");
            const auto end_stone =
                resolve_state(*states_, "minecraft:end_stone");
            const auto portal = resolve_state(*states_, "minecraft:end_portal");
            const std::array<std::string, 4> torch_keys{
                "minecraft:wall_torch[facing=south]",
                "minecraft:wall_torch[facing=north]",
                "minecraft:wall_torch[facing=east]",
                "minecraft:wall_torch[facing=west]"};
            std::array<block_state_id, 4> torches{};
            bool torches_ok = true;
            for (std::size_t i = 0; i < 4; ++i) {
                const auto torch = resolve_state(*states_, torch_keys[i]);
                torches_ok = torches_ok && torch.has_value();
                torches[i] = torch.value_or(no_block_state);
            }
            if (!bedrock || !end_stone || !portal || !torches_ok) {
                return std::unexpected{make_error(
                    error_code::parse_error, "end podium states are invalid")};
            }
            features_.emplace(
                id, std::make_unique<end_podium_feature>(
                        is_active, *bedrock, *end_stone, *portal, torches));
            continue;
        }
        if (*type == "minecraft:end_island") {
            const auto end_stone =
                resolve_state(*states_, "minecraft:end_stone");
            if (!end_stone) {
                return std::unexpected{make_error(
                    error_code::parse_error, "end island state is invalid")};
            }
            features_.emplace(id,
                              std::make_unique<end_island_feature>(*end_stone));
            continue;
        }
        if (*type == "minecraft:end_gateway") {
            const auto gateway =
                resolve_state(*states_, "minecraft:end_gateway");
            const auto bedrock = resolve_state(*states_, "minecraft:bedrock");
            if (!gateway || !bedrock) {
                return std::unexpected{make_error(
                    error_code::parse_error, "end gateway states are invalid")};
            }
            std::optional<block_pos> exit;
            if (const auto *exit_value = document.document.find("exit");
                exit_value != nullptr) {
                const auto values = exit_value->as_array();
                if (values && values->size() == 3) {
                    exit = block_pos{.x = static_cast<jint>(
                                         (*values)[0].as_int64().value_or(0)),
                                     .y = static_cast<jint>(
                                         (*values)[1].as_int64().value_or(0)),
                                     .z = static_cast<jint>(
                                         (*values)[2].as_int64().value_or(0))};
                }
            }
            bool exact = false;
            if (const auto *exact_value = document.document.find("exact");
                exact_value != nullptr) {
                exact = exact_value->as_bool().value_or(false);
            }
            features_.emplace(id, std::make_unique<end_gateway_feature>(
                                      *gateway, *bedrock, exit, exact));
            continue;
        }
        if (*type == "minecraft:fossil") {
            const auto *fossils = document.document.find("fossil_structures");
            const auto *overlays = document.document.find("overlay_structures");
            const auto *fossil_processors =
                document.document.find("fossil_processors");
            const auto *overlay_processors =
                document.document.find("overlay_processors");
            const auto *max_corners =
                document.document.find("max_empty_corners_allowed");
            if (fossils == nullptr || overlays == nullptr ||
                fossil_processors == nullptr || overlay_processors == nullptr ||
                max_corners == nullptr) {
                return std::unexpected{make_error(error_code::parse_error,
                                                  "fossil is incomplete")};
            }
            const auto fossil_array = fossils->as_array();
            const auto overlay_array = overlays->as_array();
            const auto fossil_processor = fossil_processors->as_string();
            const auto overlay_processor = overlay_processors->as_string();
            const auto max_corners_value = max_corners->as_int64();
            if (!fossil_array || !overlay_array || !fossil_processor ||
                !overlay_processor || !max_corners_value) {
                return std::unexpected{
                    make_error(error_code::parse_error, "fossil is invalid")};
            }
            std::vector<std::string> fossil_ids;
            std::vector<std::string> overlay_ids;
            for (const auto &item : *fossil_array) {
                const auto text = item.as_string();
                if (!text) {
                    return std::unexpected{make_error(error_code::parse_error,
                                                      "fossil id is invalid")};
                }
                fossil_ids.emplace_back(*text);
            }
            for (const auto &item : *overlay_array) {
                const auto text = item.as_string();
                if (!text) {
                    return std::unexpected{make_error(error_code::parse_error,
                                                      "overlay id is invalid")};
                }
                overlay_ids.emplace_back(*text);
            }
            if (fossil_ids.size() != overlay_ids.size()) {
                return std::unexpected{
                    make_error(error_code::parse_error, "fossil lists differ")};
            }
            features_.emplace(id,
                              std::make_unique<fossil_feature>(
                                  std::move(fossil_ids), std::move(overlay_ids),
                                  std::string{*fossil_processor},
                                  std::string{*overlay_processor},
                                  static_cast<jint>(*max_corners_value)));
            continue;
        }
        if (*type == "minecraft:speleothem_cluster") {
            const auto *base = document.document.find("base_block");
            const auto *pointed = document.document.find("pointed_block");
            const auto *replaceable =
                document.document.find("replaceable_blocks");
            const auto *search =
                document.document.find("floor_to_ceiling_search_range");
            const auto *height = document.document.find("height");
            const auto *radius = document.document.find("radius");
            const auto *max_diff =
                document.document.find("max_stalagmite_stalactite_height_diff");
            const auto *deviation = document.document.find("height_deviation");
            const auto *layer =
                document.document.find("speleothem_block_layer_thickness");
            const auto *density = document.document.find("density");
            const auto *wetness = document.document.find("wetness");
            const auto *chance = document.document.find(
                "chance_of_speleothem_at_max_distance_from_center");
            const auto *edge = document.document.find(
                "max_distance_from_edge_affecting_chance_of_speleothem");
            const auto *center = document.document.find(
                "max_distance_from_center_affecting_height_bias");
            if (base == nullptr || pointed == nullptr ||
                replaceable == nullptr || search == nullptr ||
                height == nullptr || radius == nullptr || max_diff == nullptr ||
                deviation == nullptr || layer == nullptr ||
                density == nullptr || wetness == nullptr || chance == nullptr ||
                edge == nullptr || center == nullptr) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "speleothem cluster is incomplete")};
            }
            const auto base_state = decode_simple_state(*base, *states_);
            const auto pointed_state = decode_simple_state(*pointed, *states_);
            const auto replaceable_text = replaceable->as_string();
            const auto search_value = search->as_int64();
            const auto max_diff_value = max_diff->as_int64();
            const auto deviation_value = deviation->as_int64();
            const auto chance_value = chance->as_double();
            const auto edge_value = edge->as_int64();
            const auto center_value = center->as_int64();
            auto height_provider = provider::int_provider::decode(*height);
            auto radius_provider = provider::int_provider::decode(*radius);
            auto layer_provider = provider::int_provider::decode(*layer);
            auto density_provider = provider::float_provider::decode(*density);
            auto wetness_provider = provider::float_provider::decode(*wetness);
            if (!base_state || !pointed_state || !replaceable_text ||
                !search_value || !max_diff_value || !deviation_value ||
                !chance_value || !edge_value || !center_value ||
                !height_provider || !radius_provider || !layer_provider ||
                !density_provider || !wetness_provider) {
                return std::unexpected{make_error(
                    error_code::parse_error, "speleothem cluster is invalid")};
            }
            features_.emplace(
                id,
                std::make_unique<speleothem_cluster_feature>(
                    *base_state, *pointed_state, std::string{*replaceable_text},
                    static_cast<jint>(*search_value),
                    std::move(*height_provider), std::move(*radius_provider),
                    static_cast<jint>(*max_diff_value),
                    static_cast<jint>(*deviation_value),
                    std::move(*layer_provider), std::move(*density_provider),
                    std::move(*wetness_provider),
                    static_cast<float>(*chance_value),
                    static_cast<jint>(*edge_value),
                    static_cast<jint>(*center_value)));
            continue;
        }
        if (*type == "minecraft:monster_room") {
            features_.emplace(id, std::make_unique<monster_room_feature>());
            continue;
        }
        if (*type == "minecraft:spike") {
            const auto *state = document.document.find("state");
            const auto *can_place = document.document.find("can_place_on");
            const auto *can_replace = document.document.find("can_replace");
            if (state == nullptr || can_place == nullptr ||
                can_replace == nullptr) {
                return std::unexpected{
                    make_error(error_code::parse_error, "spike is incomplete")};
            }
            const auto state_id = decode_simple_state(*state, *states_);
            auto place_predicate = decode_block_predicate(*can_place, *states_);
            auto replace_predicate =
                decode_block_predicate(*can_replace, *states_);
            if (!state_id || !place_predicate || !replace_predicate) {
                return std::unexpected{
                    make_error(error_code::parse_error, "spike is invalid")};
            }
            features_.emplace(id, std::make_unique<spike_feature>(
                                      *state_id, std::move(*place_predicate),
                                      std::move(*replace_predicate)));
            continue;
        }
        if (*type == "minecraft:underwater_magma") {
            const auto *range = document.document.find("floor_search_range");
            const auto *radius =
                document.document.find("placement_radius_around_floor");
            const auto *probability = document.document.find(
                "placement_probability_per_valid_position");
            if (range == nullptr || radius == nullptr ||
                probability == nullptr) {
                return std::unexpected{make_error(
                    error_code::parse_error, "underwater magma is incomplete")};
            }
            const auto range_value = range->as_int64();
            const auto radius_value = radius->as_int64();
            const auto probability_value = probability->as_double();
            const auto magma = resolve_state(*states_, "minecraft:magma_block");
            if (!range_value || !radius_value || !probability_value || !magma) {
                return std::unexpected{make_error(
                    error_code::parse_error, "underwater magma is invalid")};
            }
            features_.emplace(id, std::make_unique<underwater_magma_feature>(
                                      static_cast<jint>(*range_value),
                                      static_cast<jint>(*radius_value),
                                      static_cast<float>(*probability_value),
                                      *magma));
            continue;
        }
        if (*type == "minecraft:lake") {
            const auto *fluid = document.document.find("fluid");
            const auto *barrier = document.document.find("barrier");
            const auto *can_place = document.document.find("can_place_feature");
            const auto *can_replace_air =
                document.document.find("can_replace_with_air_or_fluid");
            const auto *can_replace_barrier =
                document.document.find("can_replace_with_barrier");
            if (fluid == nullptr || barrier == nullptr ||
                can_place == nullptr || can_replace_air == nullptr ||
                can_replace_barrier == nullptr) {
                return std::unexpected{make_error(
                    error_code::parse_error, "lake feature is incomplete")};
            }
            auto fluid_provider = decode_provider(*fluid);
            auto barrier_provider = decode_provider(*barrier);
            auto place_predicate = decode_block_predicate(*can_place, *states_);
            auto air_predicate =
                decode_block_predicate(*can_replace_air, *states_);
            auto barrier_predicate =
                decode_block_predicate(*can_replace_barrier, *states_);
            const auto cave_air = resolve_state(*states_, "minecraft:cave_air");
            const auto ice = resolve_state(*states_, "minecraft:ice");
            if (!fluid_provider || !barrier_provider || !place_predicate ||
                !air_predicate || !barrier_predicate || !cave_air || !ice) {
                return std::unexpected{make_error(error_code::parse_error,
                                                  "lake feature is invalid")};
            }
            features_.emplace(
                id,
                std::make_unique<lake_feature>(
                    std::move(*fluid_provider), std::move(*barrier_provider),
                    std::move(*place_predicate), std::move(*air_predicate),
                    std::move(*barrier_predicate), *cave_air, *ice));
            continue;
        }
        if (*type == "minecraft:freeze_top_layer") {
            const auto ice = resolve_state(*states_, "minecraft:ice");
            const auto snow =
                resolve_state(*states_, "minecraft:snow[layers=1]");
            if (!ice || !snow) {
                return std::unexpected{make_error(error_code::parse_error,
                                                  "freeze states are invalid")};
            }
            features_.emplace(
                id, std::make_unique<freeze_top_layer_feature>(*ice, *snow));
            continue;
        }
        if (*type == "minecraft:iceberg") {
            const auto *state = document.document.find("state");
            if (state == nullptr) {
                return std::unexpected{make_error(error_code::parse_error,
                                                  "iceberg state is missing")};
            }
            const auto main_key = decode_state_key(*state);
            const auto snow_block =
                resolve_state(*states_, "minecraft:snow_block");
            const auto snow_layer =
                resolve_state(*states_, "minecraft:snow[layers=1]");
            const auto ice = resolve_state(*states_, "minecraft:ice");
            const auto packed = resolve_state(*states_, "minecraft:packed_ice");
            const auto blue = resolve_state(*states_, "minecraft:blue_ice");
            const auto water = resolve_state(*states_, "minecraft:water");
            if (!main_key || !snow_block || !snow_layer || !ice || !packed ||
                !blue || !water) {
                return std::unexpected{make_error(
                    error_code::parse_error, "iceberg states are invalid")};
            }
            const auto main_state = resolve_state(*states_, *main_key);
            if (!main_state) {
                return std::unexpected{main_state.error()};
            }
            features_.emplace(id, std::make_unique<iceberg_feature>(
                                      *main_state, *snow_block, *snow_layer,
                                      *ice, *packed, *blue, *water));
            continue;
        }
        if (*type == "minecraft:huge_red_mushroom" ||
            *type == "minecraft:huge_brown_mushroom") {
            const auto *cap = document.document.find("cap_provider");
            const auto *stem = document.document.find("stem_provider");
            const auto *can_place = document.document.find("can_place_on");
            if (cap == nullptr || stem == nullptr || can_place == nullptr) {
                return std::unexpected{make_error(
                    error_code::parse_error, "huge mushroom is incomplete")};
            }
            auto cap_provider = decode_provider(*cap);
            auto stem_provider = decode_provider(*stem);
            auto predicate = decode_block_predicate(*can_place, *states_);
            if (!cap_provider || !stem_provider || !predicate) {
                return std::unexpected{make_error(error_code::parse_error,
                                                  "huge mushroom is invalid")};
            }
            jint radius = 2;
            if (const auto *field = document.document.find("foliage_radius");
                field != nullptr) {
                const auto value = field->as_int64();
                if (!value) {
                    return std::unexpected{value.error()};
                }
                radius = static_cast<jint>(*value);
            }
            features_.emplace(id, std::make_unique<huge_mushroom_feature>(
                                      *type == "minecraft:huge_red_mushroom",
                                      std::move(*cap_provider),
                                      std::move(*stem_provider), radius,
                                      std::move(*predicate)));
            continue;
        }
        if (*type == "minecraft:huge_fungus") {
            const auto *base = document.document.find("valid_base_block");
            const auto *stem = document.document.find("stem_state");
            const auto *hat = document.document.find("hat_state");
            const auto *decor = document.document.find("decor_state");
            const auto *replaceable =
                document.document.find("replaceable_blocks");
            if (base == nullptr || stem == nullptr || hat == nullptr ||
                decor == nullptr || replaceable == nullptr) {
                return std::unexpected{make_error(error_code::parse_error,
                                                  "huge fungus is incomplete")};
            }
            const auto base_key = decode_state_key(*base);
            const auto stem_key = decode_state_key(*stem);
            const auto hat_key = decode_state_key(*hat);
            const auto decor_key = decode_state_key(*decor);
            auto filter = decode_block_predicate(*replaceable, *states_);
            if (!base_key || !stem_key || !hat_key || !decor_key || !filter) {
                return std::unexpected{make_error(error_code::parse_error,
                                                  "huge fungus is invalid")};
            }
            const auto base_state = resolve_state(*states_, *base_key);
            const auto stem_state = resolve_state(*states_, *stem_key);
            const auto hat_state = resolve_state(*states_, *hat_key);
            const auto decor_state = resolve_state(*states_, *decor_key);
            if (!base_state || !stem_state || !hat_state || !decor_state) {
                return std::unexpected{make_error(
                    error_code::parse_error, "huge fungus state is invalid")};
            }
            bool planted = false;
            if (const auto *field = document.document.find("planted");
                field != nullptr) {
                const auto value = field->as_bool();
                if (!value) {
                    return std::unexpected{value.error()};
                }
                planted = *value;
            }
            std::array<block_state_id, 3> heads{};
            for (jint age = 0; age < 3; ++age) {
                const auto key =
                    "minecraft:weeping_vines[age=" + std::to_string(23 + age) +
                    "]";
                const auto state = resolve_state(*states_, key);
                if (!state) {
                    return std::unexpected{make_error(error_code::parse_error,
                                                      "weeping vines state")};
                }
                heads[static_cast<std::size_t>(age)] = *state;
            }
            const auto plant_state =
                resolve_state(*states_, "minecraft:weeping_vines_plant");
            if (!plant_state) {
                return std::unexpected{make_error(error_code::parse_error,
                                                  "weeping vines plant state")};
            }
            const bool place_vines =
                state_block_name(*hat_key) == "minecraft:nether_wart_block";
            features_.emplace(id,
                              std::make_unique<huge_fungus_feature>(
                                  *base_state, *stem_state, *hat_state,
                                  *decor_state, heads, *plant_state,
                                  std::move(*filter), planted, place_vines));
            continue;
        }
        if (*type == "minecraft:vines") {
            std::array<block_state_id, 6> states{};
            constexpr std::array<std::string_view, 6> keys{{
                "minecraft:vine[east=false,north=false,south=false,up=false,"
                "west=false]",
                "minecraft:vine[east=false,north=false,south=false,up=true,"
                "west=false]",
                "minecraft:vine[east=false,north=true,south=false,up=false,"
                "west=false]",
                "minecraft:vine[east=false,north=false,south=true,up=false,"
                "west=false]",
                "minecraft:vine[east=false,north=false,south=false,up=false,"
                "west=true]",
                "minecraft:vine[east=true,north=false,south=false,up=false,"
                "west=false]",
            }};
            for (std::size_t i = 0; i < keys.size(); ++i) {
                const auto state = resolve_state(*states_, keys[i]);
                if (!state) {
                    return std::unexpected{make_error(error_code::parse_error,
                                                      "vine state is invalid")};
                }
                states[i] = *state;
            }
            features_.emplace(id, std::make_unique<vines_feature>(states));
            continue;
        }
        if (*type == "minecraft:bamboo") {
            const auto *probability = document.document.find("probability");
            if (probability == nullptr) {
                return std::unexpected{make_error(
                    error_code::parse_error, "bamboo probability is missing")};
            }
            const auto value = probability->as_double();
            if (!value) {
                return std::unexpected{value.error()};
            }
            const auto trunk = resolve_state(
                *states_, "minecraft:bamboo[age=1,leaves=none,stage=0]");
            const auto final_large = resolve_state(
                *states_, "minecraft:bamboo[age=1,leaves=large,stage=1]");
            const auto top_large = resolve_state(
                *states_, "minecraft:bamboo[age=1,leaves=large,stage=0]");
            const auto top_small = resolve_state(
                *states_, "minecraft:bamboo[age=1,leaves=small,stage=0]");
            const auto podzol = resolve_state(*states_, "minecraft:podzol");
            if (!trunk || !final_large || !top_large || !top_small || !podzol) {
                return std::unexpected{make_error(error_code::parse_error,
                                                  "bamboo states are invalid")};
            }
            features_.emplace(id, std::make_unique<bamboo_feature>(
                                      static_cast<float>(*value), *trunk,
                                      *final_large, *top_large, *top_small,
                                      *podzol));
            continue;
        }
        if (*type == "minecraft:random_neighbor_spread") {
            const auto *block = document.document.find("block");
            const auto *accepted = document.document.find("accepted_neighbors");
            const auto *can_replace = document.document.find("can_replace");
            const auto *attempts = document.document.find("attempts");
            const auto *xz_offset = document.document.find("xz_offset");
            const auto *y_offset = document.document.find("y_offset");
            if (block == nullptr || accepted == nullptr ||
                can_replace == nullptr || attempts == nullptr ||
                xz_offset == nullptr || y_offset == nullptr) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "random neighbor spread is incomplete")};
            }
            auto provider = decode_provider(*block);
            auto predicate = decode_block_predicate(*can_replace, *states_);
            auto attempts_provider = provider::int_provider::decode(*attempts);
            auto xz_provider = provider::int_provider::decode(*xz_offset);
            auto y_provider = provider::int_provider::decode(*y_offset);
            if (!provider || !predicate || !attempts_provider || !xz_provider ||
                !y_provider) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "random neighbor spread is invalid")};
            }
            std::vector<std::string> accepted_names;
            if (const auto text = accepted->as_string(); text) {
                accepted_names.emplace_back(*text);
            } else if (accepted->is_array()) {
                for (const auto &entry : *accepted->as_array()) {
                    const auto entry_name = entry.as_string();
                    if (!entry_name) {
                        return std::unexpected{
                            make_error(error_code::parse_error,
                                       "accepted neighbor is not a block id")};
                    }
                    accepted_names.emplace_back(*text);
                }
            } else {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "accepted neighbors is not a list")};
            }
            for (auto &name : accepted_names) {
                if (!name.empty() && name.front() == '#') {
                    name.erase(name.begin());
                }
            }
            features_.emplace(
                id, std::make_unique<random_neighbor_spread_feature>(
                        std::move(*provider), std::move(accepted_names),
                        std::move(*predicate), std::move(*attempts_provider),
                        std::move(*xz_provider), std::move(*y_provider)));
            continue;
        }
        if (*type == "minecraft:scattered_ore") {
            const auto *targets = document.document.find("targets");
            const auto *size = document.document.find("size");
            if (targets == nullptr || !targets->is_array() || size == nullptr) {
                return std::unexpected{make_error(
                    error_code::parse_error, "scattered ore is incomplete")};
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
                const auto *target = entry.find("target");
                const auto *state = entry.find("state");
                if (target == nullptr || state == nullptr) {
                    return std::unexpected{
                        make_error(error_code::parse_error,
                                   "scattered ore replacement is incomplete")};
                }
                auto test = decode_rule_test(*target, *states_);
                const auto key = decode_state_key(*state);
                if (!test || !key) {
                    return std::unexpected{
                        make_error(error_code::parse_error,
                                   "scattered ore replacement is invalid")};
                }
                auto state_id = resolve_state(*states_, *key);
                if (!state_id) {
                    return std::unexpected{state_id.error()};
                }
                replacements.push_back(block_replacement{
                    .target = std::move(*test), .state = *state_id});
            }
            features_.emplace(id, std::make_unique<scattered_ore_feature>(
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
            auto default_id = nested_reference(*default_value, true);
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
                auto reference = nested_reference(*feature, true);
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
                auto reference = nested_reference(*data_field, true);
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
                auto reference = nested_reference(entry, true);
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
                auto reference = nested_reference(entry, true);
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
            auto true_id = nested_reference(*true_value, true);
            auto false_id = nested_reference(*false_value, true);
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
            tree_placer_options options;
            const auto read_holder_set =
                [](const data::json::value &value, std::string &tag,
                   std::vector<std::string> &names) -> status {
                if (const auto single = value.as_string(); single.has_value()) {
                    if (single->starts_with("#")) {
                        tag = std::string{single->substr(1)};
                    } else {
                        names.emplace_back(*single);
                    }
                    return status_ok();
                }
                if (!value.is_array()) {
                    return std::unexpected{make_error(
                        error_code::parse_error, "holder set is not a list")};
                }
                for (const auto &entry : *value.as_array()) {
                    const auto name = entry.as_string();
                    if (!name) {
                        return std::unexpected{
                            make_error(error_code::parse_error,
                                       "holder set entry is not a string")};
                    }
                    if (name->starts_with("#")) {
                        if (!tag.empty()) {
                            return std::unexpected{
                                make_error(error_code::parse_error,
                                           "holder set has more than one tag")};
                        }
                        tag = std::string{name->substr(1)};
                    } else {
                        names.emplace_back(*name);
                    }
                }
                return status_ok();
            };
            const auto read_uniform_bounds = [](const data::json::value &value,
                                                jint &minimum,
                                                jint &maximum) -> status {
                if (const auto values = value.as_array();
                    values && values->size() == 2) {
                    const auto min = (*values)[0].as_int64();
                    const auto max = (*values)[1].as_int64();
                    if (!min || !max) {
                        return std::unexpected{
                            make_error(error_code::parse_error,
                                       "uniform bounds are not integers")};
                    }
                    minimum = static_cast<jint>(*min);
                    maximum = static_cast<jint>(*max);
                    return status_ok();
                }
                const auto *min_field = value.find("min_inclusive");
                const auto *max_field = value.find("max_inclusive");
                if (min_field == nullptr || max_field == nullptr) {
                    return std::unexpected{make_error(
                        error_code::parse_error, "uniform bounds are missing")};
                }
                const auto min = min_field->as_int64();
                const auto max = max_field->as_int64();
                if (!min || !max) {
                    return std::unexpected{
                        make_error(error_code::parse_error,
                                   "uniform bounds are not integers")};
                }
                minimum = static_cast<jint>(*min);
                maximum = static_cast<jint>(*max);
                return status_ok();
            };
            if (const auto *root = document.document.find("root_placer");
                root != nullptr) {
                const auto *root_type = root->find("type");
                std::optional<std::string_view> root_type_name;
                if (root_type != nullptr) {
                    const auto decoded_type = root_type->as_string();
                    if (decoded_type) {
                        root_type_name = *decoded_type;
                    }
                }
                if (!root_type_name ||
                    *root_type_name != "minecraft:mangrove_root_placer") {
                    return std::unexpected{
                        make_error(error_code::unsupported,
                                   "unsupported tree root placer")};
                }
                const auto *trunk_offset = root->find("trunk_offset_y");
                const auto *root_provider = root->find("root_provider");
                const auto *mangrove = root->find("mangrove_root_placement");
                if (trunk_offset == nullptr || root_provider == nullptr ||
                    mangrove == nullptr) {
                    return std::unexpected{
                        make_error(error_code::parse_error,
                                   "mangrove root placer is incomplete")};
                }
                auto offset_provider =
                    provider::int_provider::decode(*trunk_offset);
                auto provider_value = decode_provider(*root_provider);
                if (!offset_provider || !provider_value) {
                    return std::unexpected{
                        make_error(error_code::parse_error,
                                   "mangrove root placer is invalid")};
                }
                options.root_trunk_offset_y = std::move(*offset_provider);
                options.root_provider = std::move(*provider_value);
                if (const auto *above = root->find("above_root_placement");
                    above != nullptr) {
                    const auto *above_provider =
                        above->find("above_root_provider");
                    const auto *above_chance =
                        above->find("above_root_placement_chance");
                    if (above_provider == nullptr || above_chance == nullptr) {
                        return std::unexpected{
                            make_error(error_code::parse_error,
                                       "above root placement is incomplete")};
                    }
                    auto decoded = decode_provider(*above_provider);
                    const auto chance = above_chance->as_double();
                    if (!decoded || !chance) {
                        return std::unexpected{
                            make_error(error_code::parse_error,
                                       "above root placement is invalid")};
                    }
                    options.above_root_provider = std::move(*decoded);
                    options.above_root_chance = static_cast<float>(*chance);
                }
                const auto *can_grow = mangrove->find("can_grow_through");
                const auto *muddy_in = mangrove->find("muddy_roots_in");
                const auto *muddy_provider =
                    mangrove->find("muddy_roots_provider");
                const auto *max_width = mangrove->find("max_root_width");
                const auto *max_length = mangrove->find("max_root_length");
                const auto *skew = mangrove->find("random_skew_chance");
                if (can_grow == nullptr || muddy_in == nullptr ||
                    muddy_provider == nullptr || max_width == nullptr ||
                    max_length == nullptr || skew == nullptr) {
                    return std::unexpected{
                        make_error(error_code::parse_error,
                                   "mangrove root placement is incomplete")};
                }
                std::string can_grow_tag;
                std::vector<std::string> can_grow_names;
                if (const auto read = read_holder_set(*can_grow, can_grow_tag,
                                                      can_grow_names);
                    !read) {
                    return std::unexpected{read.error()};
                }
                if (!can_grow_names.empty() || can_grow_tag.empty()) {
                    return std::unexpected{make_error(
                        error_code::unsupported,
                        "mangrove roots need a can_grow_through tag")};
                }
                std::string muddy_tag;
                if (const auto read = read_holder_set(
                        *muddy_in, muddy_tag, options.mangrove_muddy_roots_in);
                    !read) {
                    return std::unexpected{read.error()};
                }
                auto decoded_muddy = decode_provider(*muddy_provider);
                const auto width = max_width->as_int64();
                const auto length = max_length->as_int64();
                const auto skew_value = skew->as_double();
                if (!decoded_muddy || !width || !length || !skew_value) {
                    return std::unexpected{
                        make_error(error_code::parse_error,
                                   "mangrove root placement is invalid")};
                }
                options.mangrove_can_grow_through = std::move(can_grow_tag);
                options.muddy_roots_provider = std::move(*decoded_muddy);
                options.mangrove_max_root_width = static_cast<jint>(*width);
                options.mangrove_max_root_length = static_cast<jint>(*length);
                options.mangrove_random_skew_chance =
                    static_cast<float>(*skew_value);
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
                } else if (decorator_name == "minecraft:shelf_mushroom") {
                    const auto *probability = entry.find("probability");
                    if (probability == nullptr) {
                        return std::unexpected{
                            make_error(error_code::parse_error,
                                       "shelf mushroom has no probability")};
                    }
                    const auto value = probability->as_double();
                    if (!value) {
                        return std::unexpected{value.error()};
                    }
                    decoded_decorators.push_back(
                        std::make_unique<shelf_mushroom_decorator>(
                            static_cast<float>(*value)));
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
                } else if (decorator_name == "minecraft:pale_moss") {
                    const auto read =
                        [&](std::string_view name) -> std::optional<float> {
                        const auto *value = entry.find(name);
                        if (value == nullptr) {
                            return std::nullopt;
                        }
                        const auto number = value->as_double();
                        if (!number) {
                            return std::nullopt;
                        }
                        return static_cast<float>(*number);
                    };
                    const auto leaves = read("leaves_probability");
                    const auto trunk = read("trunk_probability");
                    const auto ground = read("ground_probability");
                    if (!leaves || !trunk || !ground) {
                        return std::unexpected{
                            make_error(error_code::parse_error,
                                       "pale moss decorator is incomplete")};
                    }
                    decoded_decorators.push_back(
                        std::make_unique<pale_moss_decorator>(
                            *leaves, *trunk, *ground,
                            "minecraft:pale_moss_patch"));
                } else if (decorator_name == "minecraft:creaking_heart") {
                    const auto *probability = entry.find("probability");
                    if (probability == nullptr) {
                        return std::unexpected{
                            make_error(error_code::parse_error,
                                       "creaking heart has no probability")};
                    }
                    const auto value = probability->as_double();
                    if (!value) {
                        return std::unexpected{value.error()};
                    }
                    decoded_decorators.push_back(
                        std::make_unique<creaking_heart_decorator>(
                            static_cast<float>(*value)));
                } else {
                    return std::unexpected{
                        make_error(error_code::unsupported,
                                   "unsupported tree decorator '" +
                                       std::string{decorator_name} + "'")};
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
            } else if (*trunk_name == "minecraft:mega_jungle_trunk_placer") {
                kind = trunk_kind::mega_jungle;
            } else if (*trunk_name == "minecraft:cherry_trunk_placer") {
                kind = trunk_kind::cherry;
                const auto *count = trunk_placer->find("branch_count");
                const auto *horizontal =
                    trunk_placer->find("branch_horizontal_length");
                const auto *start =
                    trunk_placer->find("branch_start_offset_from_top");
                const auto *end =
                    trunk_placer->find("branch_end_offset_from_top");
                if (count == nullptr || horizontal == nullptr ||
                    start == nullptr || end == nullptr) {
                    return std::unexpected{
                        make_error(error_code::parse_error,
                                   "cherry trunk placer is incomplete")};
                }
                auto count_provider = provider::int_provider::decode(*count);
                auto horizontal_provider =
                    provider::int_provider::decode(*horizontal);
                auto end_provider = provider::int_provider::decode(*end);
                jint start_min = 0;
                jint start_max = 0;
                const auto bounds =
                    read_uniform_bounds(*start, start_min, start_max);
                if (!count_provider || !horizontal_provider || !end_provider ||
                    !bounds) {
                    return std::unexpected{
                        make_error(error_code::parse_error,
                                   "cherry trunk placer is invalid")};
                }
                options.cherry_branch_count = std::move(*count_provider);
                options.cherry_branch_horizontal_length =
                    std::move(*horizontal_provider);
                options.cherry_branch_start_min = start_min;
                options.cherry_branch_start_max = start_max;
                options.cherry_branch_end_offset = std::move(*end_provider);
            } else if (*trunk_name == "minecraft:poplar_trunk_placer") {
                kind = trunk_kind::poplar;
                const auto *above =
                    trunk_placer->find("trunk_height_above_branches");
                const auto *amount = trunk_placer->find("branch_amount");
                if (above == nullptr || amount == nullptr) {
                    return std::unexpected{
                        make_error(error_code::parse_error,
                                   "poplar trunk placer is incomplete")};
                }
                auto above_provider = provider::int_provider::decode(*above);
                auto amount_provider = provider::int_provider::decode(*amount);
                if (!above_provider || !amount_provider) {
                    return std::unexpected{
                        make_error(error_code::parse_error,
                                   "poplar trunk placer is invalid")};
                }
                options.poplar_trunk_above_branches =
                    std::move(*above_provider);
                options.poplar_branch_amount = std::move(*amount_provider);
            } else if (*trunk_name == "minecraft:bending_trunk_placer") {
                kind = trunk_kind::bending;
                const auto *bend = trunk_placer->find("bend_length");
                if (bend == nullptr) {
                    return std::unexpected{
                        make_error(error_code::parse_error,
                                   "bending trunk placer has no bend_length")};
                }
                auto bend_provider = provider::int_provider::decode(*bend);
                if (!bend_provider) {
                    return std::unexpected{bend_provider.error()};
                }
                options.bending_bend_length = std::move(*bend_provider);
                if (const auto *minimum =
                        trunk_placer->find("min_height_for_leaves");
                    minimum != nullptr) {
                    const auto value = minimum->as_int64();
                    if (!value) {
                        return std::unexpected{value.error()};
                    }
                    options.bending_min_height_for_leaves =
                        static_cast<jint>(*value);
                }
            } else if (*trunk_name ==
                       "minecraft:upwards_branching_trunk_placer") {
                kind = trunk_kind::upwards_branching;
                const auto *steps = trunk_placer->find("extra_branch_steps");
                const auto *length = trunk_placer->find("extra_branch_length");
                const auto *probability =
                    trunk_placer->find("place_branch_per_log_probability");
                const auto *grow = trunk_placer->find("can_grow_through");
                if (steps == nullptr || length == nullptr ||
                    probability == nullptr || grow == nullptr) {
                    return std::unexpected{make_error(
                        error_code::parse_error,
                        "upwards branching trunk placer is incomplete")};
                }
                auto steps_provider = provider::int_provider::decode(*steps);
                auto length_provider = provider::int_provider::decode(*length);
                const auto probability_value = probability->as_double();
                std::string grow_tag;
                std::vector<std::string> grow_names;
                const auto read = read_holder_set(*grow, grow_tag, grow_names);
                if (!steps_provider || !length_provider || !probability_value ||
                    !read) {
                    return std::unexpected{make_error(
                        error_code::parse_error,
                        "upwards branching trunk placer is invalid")};
                }
                if (!grow_names.empty() || grow_tag.empty()) {
                    return std::unexpected{make_error(
                        error_code::unsupported,
                        "upwards branching needs a can_grow_through tag")};
                }
                options.upwards_extra_branch_steps = std::move(*steps_provider);
                options.upwards_extra_branch_length =
                    std::move(*length_provider);
                options.upwards_branch_probability =
                    static_cast<float>(*probability_value);
                options.upwards_can_grow_through = std::move(grow_tag);
            } else if (*trunk_name != "minecraft:straight_trunk_placer") {
                return std::unexpected{make_error(error_code::unsupported,
                                                  "unsupported trunk placer '" +
                                                      std::string{*trunk_name} +
                                                      "'")};
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
            } else if (foliage_name == "minecraft:jungle_foliage_placer") {
                foliage_type_kind = foliage_kind::jungle;
            } else if (foliage_name == "minecraft:bush_foliage_placer") {
                foliage_type_kind = foliage_kind::bush;
            } else if (foliage_name == "minecraft:cherry_foliage_placer") {
                foliage_type_kind = foliage_kind::cherry;
            } else if (foliage_name == "minecraft:poplar_foliage_placer") {
                foliage_type_kind = foliage_kind::poplar;
            } else if (foliage_name ==
                       "minecraft:random_spread_foliage_placer") {
                foliage_type_kind = foliage_kind::random_spread;
            } else if (foliage_name != "minecraft:blob_foliage_placer") {
                return std::unexpected{
                    make_error(error_code::unsupported,
                               "unsupported foliage placer '" +
                                   std::string{foliage_name} + "'")};
            }
            if (foliage_type_kind == foliage_kind::cherry ||
                foliage_type_kind == foliage_kind::poplar ||
                foliage_type_kind == foliage_kind::random_spread) {
                const auto *height_field = foliage_placer->find(
                    foliage_type_kind == foliage_kind::random_spread
                        ? "foliage_height"
                        : "height");
                if (height_field == nullptr) {
                    return std::unexpected{
                        make_error(error_code::parse_error,
                                   "foliage placer has no foliage height")};
                }
                auto decoded = provider::int_provider::decode(*height_field);
                if (!decoded) {
                    return std::unexpected{decoded.error()};
                }
                options.foliage_height = std::move(*decoded);
                const auto read_chance = [&](std::string_view name,
                                             float &out) -> status {
                    const auto *value = foliage_placer->find(name);
                    if (value == nullptr) {
                        return std::unexpected{
                            make_error(error_code::parse_error,
                                       "foliage placer chance is missing")};
                    }
                    const auto number = value->as_double();
                    if (!number) {
                        return std::unexpected{number.error()};
                    }
                    out = static_cast<float>(*number);
                    return status_ok();
                };
                if (foliage_type_kind == foliage_kind::cherry &&
                    (!read_chance("wide_bottom_layer_hole_chance",
                                  options.cherry_wide_bottom_hole_chance) ||
                     !read_chance("corner_hole_chance",
                                  options.cherry_corner_hole_chance) ||
                     !read_chance("hanging_leaves_chance",
                                  options.cherry_hanging_leaves_chance) ||
                     !read_chance("hanging_leaves_extension_chance",
                                  options.cherry_hanging_extension_chance))) {
                    return std::unexpected{
                        make_error(error_code::parse_error,
                                   "cherry foliage placer is invalid")};
                }
                if (foliage_type_kind == foliage_kind::poplar &&
                    !read_chance("side_hole_chance",
                                 options.poplar_side_hole_chance)) {
                    return std::unexpected{
                        make_error(error_code::parse_error,
                                   "poplar foliage placer is invalid")};
                }
                if (foliage_type_kind == foliage_kind::random_spread) {
                    const auto *attempts =
                        foliage_placer->find("leaf_placement_attempts");
                    if (attempts == nullptr) {
                        return std::unexpected{make_error(
                            error_code::parse_error,
                            "random spread foliage has no attempts")};
                    }
                    const auto value = attempts->as_int64();
                    if (!value) {
                        return std::unexpected{value.error()};
                    }
                    options.random_spread_leaf_attempts =
                        static_cast<jint>(*value);
                }
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
            if (height != nullptr && foliage_type_kind != foliage_kind::pine &&
                foliage_type_kind != foliage_kind::cherry &&
                foliage_type_kind != foliage_kind::poplar &&
                foliage_type_kind != foliage_kind::random_spread) {
                const auto parsed_height = height->as_int64();
                if (!parsed_height) {
                    return std::unexpected{parsed_height.error()};
                }
                height_value = static_cast<jint>(*parsed_height);
            }
            auto radius_provider = provider::int_provider::decode(*radius);
            auto offset_provider = provider::int_provider::decode(*offset);
            // Both pine placers take the canopy height from a provider: the
            // mega pine names it `crown_height`, the regular pine `height`.
            std::unique_ptr<provider::int_provider> pine_foliage_height;
            if (foliage_type_kind == foliage_kind::mega_pine ||
                foliage_type_kind == foliage_kind::pine) {
                const char *field = foliage_type_kind == foliage_kind::pine
                                        ? "height"
                                        : "crown_height";
                const auto *height_field = foliage_placer->find(field);
                if (height_field == nullptr) {
                    return std::unexpected{make_error(
                        error_code::parse_error,
                        std::string{"pine foliage has no "} + field)};
                }
                auto decoded = provider::int_provider::decode(*height_field);
                if (!decoded) {
                    return std::unexpected{decoded.error()};
                }
                pine_foliage_height = std::move(*decoded);
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
                    std::move(spruce_trunk_height),
                    std::move(pine_foliage_height), height_value, limit, lower,
                    upper, three_layers, upper_limit, middle, has_min_clipped,
                    min_clipped, ignore_vines, std::move(decoded_decorators),
                    std::move(options)));
            continue;
        }
        if (*type == "minecraft:no_op") {
            features_.emplace(id, std::make_unique<no_op_feature>());
            continue;
        }
        if (*type == "minecraft:fill_layer") {
            const auto *height_value = document.document.find("height");
            const auto *state_value = document.document.find("state");
            if (height_value == nullptr || state_value == nullptr) {
                return std::unexpected{make_error(error_code::parse_error,
                                                  "fill layer is incomplete")};
            }
            const auto height = height_value->as_int64();
            const auto key = decode_state_key(*state_value);
            if (!height || !key) {
                return std::unexpected{make_error(error_code::parse_error,
                                                  "fill layer is invalid")};
            }
            auto state = resolve_state(*states_, *key);
            if (!state) {
                return std::unexpected{state.error()};
            }
            features_.emplace(id, std::make_unique<fill_layer_feature>(
                                      static_cast<jint>(*height), *state));
            continue;
        }
        if (*type == "minecraft:single_block_pillar") {
            const auto *block_value = document.document.find("block");
            const auto *direction_value = document.document.find("direction");
            if (block_value == nullptr || direction_value == nullptr) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "single block pillar is incomplete")};
            }
            auto block = decode_provider(*block_value);
            if (!block) {
                return std::unexpected{block.error()};
            }
            const auto direction_name = direction_value->as_string();
            if (!direction_name ||
                (*direction_name != "up" && *direction_name != "down")) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "single block pillar direction is invalid")};
            }
            std::unique_ptr<block_predicate> can_replace;
            if (const auto *value = document.document.find("can_replace");
                value != nullptr) {
                auto decoded = decode_block_predicate(*value, *states_);
                if (!decoded) {
                    return std::unexpected{decoded.error()};
                }
                can_replace = std::move(*decoded);
            }
            float chance = 1.0F;
            if (const auto *value =
                    document.document.find("chance_to_continue");
                value != nullptr) {
                const auto number = value->as_double();
                if (!number) {
                    return std::unexpected{number.error()};
                }
                chance = static_cast<float>(*number);
            }
            std::string cap;
            if (const auto *value = document.document.find("cap_feature");
                value != nullptr) {
                auto reference = nested_reference(*value, true);
                if (!reference) {
                    return std::unexpected{reference.error()};
                }
                cap = std::move(*reference);
            }
            features_.emplace(id, std::make_unique<single_block_pillar_feature>(
                                      std::move(*block), std::move(can_replace),
                                      *direction_name == "up" ? 1 : -1, chance,
                                      std::move(cap)));
            continue;
        }
        if (*type == "minecraft:speleothem") {
            const auto *base_value = document.document.find("base_block");
            const auto *pointed_value = document.document.find("pointed_block");
            const auto *replaceable_value =
                document.document.find("replaceable_blocks");
            if (base_value == nullptr || pointed_value == nullptr ||
                replaceable_value == nullptr) {
                return std::unexpected{make_error(error_code::parse_error,
                                                  "speleothem is incomplete")};
            }
            const auto base_key = decode_state_key(*base_value);
            const auto pointed_key = decode_state_key(*pointed_value);
            const auto replaceable = replaceable_value->as_string();
            if (!base_key || !pointed_key || !replaceable ||
                !replaceable->starts_with("#")) {
                return std::unexpected{make_error(error_code::parse_error,
                                                  "speleothem is invalid")};
            }
            auto base = resolve_state(*states_, *base_key);
            auto pointed = resolve_state(*states_, *pointed_key);
            if (!base || !pointed) {
                return std::unexpected{make_error(
                    error_code::parse_error, "speleothem state is invalid")};
            }
            float taller = 0.2F;
            float directional = 0.7F;
            float radius2 = 0.5F;
            float radius3 = 0.5F;
            const auto read_chance = [&](std::string_view name,
                                         float &out) -> status {
                const auto *value = document.document.find(name);
                if (value == nullptr) {
                    return status_ok();
                }
                const auto number = value->as_double();
                if (!number) {
                    return std::unexpected{number.error()};
                }
                out = static_cast<float>(*number);
                return status_ok();
            };
            if (const auto read =
                    read_chance("chance_of_taller_generation", taller);
                !read) {
                return std::unexpected{read.error()};
            }
            if (const auto read =
                    read_chance("chance_of_directional_spread", directional);
                !read) {
                return std::unexpected{read.error()};
            }
            if (const auto read =
                    read_chance("chance_of_spread_radius2", radius2);
                !read) {
                return std::unexpected{read.error()};
            }
            if (const auto read =
                    read_chance("chance_of_spread_radius3", radius3);
                !read) {
                return std::unexpected{read.error()};
            }
            features_.emplace(id, std::make_unique<speleothem_feature>(
                                      *base, *pointed,
                                      std::string{replaceable->substr(1)},
                                      taller, directional, radius2, radius3));
            continue;
        }
        if (*type == "minecraft:coral_tree" ||
            *type == "minecraft:coral_claw") {
            const auto *value = document.document.find("feature");
            if (value == nullptr) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "coral feature has no nested feature")};
            }
            auto reference = nested_reference(*value, true);
            if (!reference) {
                return std::unexpected{reference.error()};
            }
            if (*type == "minecraft:coral_tree") {
                features_.emplace(id, std::make_unique<coral_tree_feature>(
                                          std::move(*reference)));
            } else {
                features_.emplace(id, std::make_unique<coral_claw_feature>(
                                          std::move(*reference)));
            }
            continue;
        }
        if (*type == "minecraft:projected_random_patchy_square") {
            const auto *block_value = document.document.find("block");
            const auto *predicate_value =
                document.document.find("project_through");
            const auto *size_value = document.document.find("size");
            const auto *height_value =
                document.document.find("max_projection_height");
            if (block_value == nullptr || predicate_value == nullptr ||
                size_value == nullptr || height_value == nullptr) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "projected random patchy square is incomplete")};
            }
            auto block = decode_provider(*block_value);
            auto predicate = decode_block_predicate(*predicate_value, *states_);
            auto size = provider::int_provider::decode(*size_value);
            const auto height = height_value->as_int64();
            if (!block || !predicate || !size || !height) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "projected random patchy square is invalid")};
            }
            features_.emplace(
                id, std::make_unique<projected_random_patchy_square_feature>(
                        std::move(*block), std::move(*predicate),
                        std::move(*size), static_cast<jint>(*height)));
            continue;
        }
        if (*type == "minecraft:stepped_column_cluster") {
            const auto *block_value = document.document.find("block");
            const auto *continue_value =
                document.document.find("continue_through");
            const auto *replace_value = document.document.find("can_replace");
            const auto *cannot_value =
                document.document.find("cannot_place_on");
            const auto *reach_value = document.document.find("cluster_reach");
            const auto *count_value = document.document.find("column_count");
            const auto *column_reach_value =
                document.document.find("column_reach");
            const auto *height_value = document.document.find("height");
            if (block_value == nullptr || continue_value == nullptr ||
                replace_value == nullptr || cannot_value == nullptr ||
                reach_value == nullptr || count_value == nullptr ||
                column_reach_value == nullptr || height_value == nullptr) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "stepped column cluster is incomplete")};
            }
            auto block = decode_provider(*block_value);
            auto continue_through =
                decode_block_predicate(*continue_value, *states_);
            auto can_replace = decode_block_predicate(*replace_value, *states_);
            auto reach = provider::int_provider::decode(*reach_value);
            auto count = provider::int_provider::decode(*count_value);
            auto column_reach =
                provider::int_provider::decode(*column_reach_value);
            auto height = provider::int_provider::decode(*height_value);
            if (!block || !continue_through || !can_replace || !reach ||
                !count || !column_reach || !height) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "stepped column cluster is invalid")};
            }
            std::string tag;
            std::vector<std::string> blocks;
            if (const auto single = cannot_value->as_string();
                single.has_value()) {
                if (single->starts_with("#")) {
                    tag = single->substr(1);
                } else {
                    blocks.emplace_back(*single);
                }
            } else if (cannot_value->is_array()) {
                for (const auto &entry : *cannot_value->as_array()) {
                    const auto name = entry.as_string();
                    if (!name || name->starts_with("#")) {
                        return std::unexpected{
                            make_error(error_code::parse_error,
                                       "cannot_place_on entry is invalid")};
                    }
                    blocks.emplace_back(*name);
                }
            } else {
                return std::unexpected{make_error(
                    error_code::parse_error, "cannot_place_on is invalid")};
            }
            features_.emplace(
                id, std::make_unique<stepped_column_cluster_feature>(
                        std::move(*block), std::move(*continue_through),
                        std::move(*can_replace), std::move(tag),
                        std::move(blocks), std::move(*reach), std::move(*count),
                        std::move(*column_reach), std::move(*height)));
            continue;
        }
        if (*type == "minecraft:sculk_patch") {
            const auto read_int =
                [&](std::string_view name) -> std::optional<jint> {
                const auto *value = document.document.find(name);
                if (value == nullptr) {
                    return std::nullopt;
                }
                const auto number = value->as_int64();
                if (!number) {
                    return std::nullopt;
                }
                return static_cast<jint>(*number);
            };
            const auto charge_count = read_int("charge_count");
            const auto amount = read_int("amount_per_charge");
            const auto attempts = read_int("spread_attempts");
            const auto growth = read_int("growth_rounds");
            const auto spread = read_int("spread_rounds");
            if (!charge_count || !amount || !attempts || !growth || !spread) {
                return std::unexpected{make_error(error_code::parse_error,
                                                  "sculk patch is incomplete")};
            }
            features_.emplace(
                id, std::make_unique<sculk_patch_feature>(
                        *charge_count, *amount, *attempts, *growth, *spread));
            continue;
        }
        if (*type == "minecraft:template") {
            auto decoded =
                decode_template_feature_document(*states_, document.document);
            if (!decoded) {
                return std::unexpected{decoded.error()};
            }
            features_.emplace(id, std::move(*decoded));
            continue;
        }
        return std::unexpected{
            make_error(error_code::unsupported, "unsupported feature type '" +
                                                    std::string{*type} + "'")};
    }

    pending_documents_ = nullptr;
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
