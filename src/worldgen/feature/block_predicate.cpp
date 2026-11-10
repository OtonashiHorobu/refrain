// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/feature/block_predicate.hpp>

#include <refrain/worldgen/feature/block_shape.hpp>

#include <refrain/data/json.hpp>
#include <refrain/worldgen/chunk/block_state_codec.hpp>
#include <refrain/worldgen/feature/placement.hpp>
#include <refrain/worldgen/terrain/material.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
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
            make_error(error_code::parse_error, "block predicate has no type")};
    }
    const auto text = type->as_string();
    if (!text) {
        return std::unexpected{make_error(
            error_code::parse_error, "block predicate type is not a string")};
    }
    return *text;
}

/// Decodes an optional `offset` vector. The reference limits each component
/// to sixteen blocks.
auto decode_offset(const data::json::value &input, block_pos &out) -> status {
    const auto *field = input.find("offset");
    if (field == nullptr) {
        return status_ok();
    }
    const auto values = field->as_array();
    if (!values || values->size() != 3) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "predicate offset is not a three-element array")};
    }
    const auto x = (*values)[0].as_int64();
    const auto y = (*values)[1].as_int64();
    const auto z = (*values)[2].as_int64();
    if (!x || !y || !z) {
        return std::unexpected{
            make_error(error_code::parse_error, "predicate offset is invalid")};
    }
    if (std::abs(*x) > 16 || std::abs(*y) > 16 || std::abs(*z) > 16) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "predicate offset exceeds sixteen blocks")};
    }
    out = block_pos{.x = static_cast<jint>(*x),
                    .y = static_cast<jint>(*y),
                    .z = static_cast<jint>(*z)};
    return status_ok();
}

auto names(const data::json::value &input) -> result<std::vector<std::string>> {
    if (const auto single = input.as_string(); single.has_value()) {
        return std::vector<std::string>{std::string{*single}};
    }
    if (!input.is_array()) {
        return std::unexpected{
            make_error(error_code::parse_error, "block list is not an array")};
    }
    std::vector<std::string> out;
    for (const auto &entry : *input.as_array()) {
        const auto name = entry.as_string();
        if (!name) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "block name is not a string")};
        }
        out.emplace_back(*name);
    }
    return out;
}

/// Decodes one vertical anchor object, the same four forms the height
/// providers use.
auto decode_anchor(const data::json::value &input)
    -> result<surface::vertical_anchor> {
    if (!input.is_object()) {
        return std::unexpected{make_error(error_code::parse_error,
                                          "height anchor is not an object")};
    }
    struct entry {
        std::string_view name;
        surface::vertical_anchor::kind kind;
    };
    constexpr std::array<entry, 4> entries{{
        {"absolute", surface::vertical_anchor::kind::absolute},
        {"above_bottom", surface::vertical_anchor::kind::above_bottom},
        {"below_top", surface::vertical_anchor::kind::below_top},
        {"relative_to_sea_level",
         surface::vertical_anchor::kind::relative_to_sea_level},
    }};
    for (const auto &candidate : entries) {
        if (const auto *field = input.find(candidate.name); field != nullptr) {
            const auto value = field->as_int64();
            if (!value) {
                return std::unexpected{value.error()};
            }
            return surface::vertical_anchor{.type = candidate.kind,
                                            .value = static_cast<jint>(*value)};
        }
    }
    return std::unexpected{
        make_error(error_code::parse_error, "unknown height anchor")};
}

class true_predicate final : public block_predicate {
  protected:
    [[nodiscard]] auto test_impl(const block_predicate_context &) const
        -> bool override {
        return true;
    }
};

class matching_blocks final : public block_predicate {
  public:
    explicit matching_blocks(std::vector<std::string> blocks)
        : blocks_{std::move(blocks)} {}

  protected:
    [[nodiscard]] auto test_impl(const block_predicate_context &context) const
        -> bool override {
        const auto name = context.states->name(context.region->state_at(
            context.pos->x, context.pos->y, context.pos->z));
        return std::find(blocks_.begin(), blocks_.end(), name) != blocks_.end();
    }

  private:
    std::vector<std::string> blocks_;
};

class matching_tag final : public block_predicate {
  public:
    explicit matching_tag(std::string tag) : tag_{std::move(tag)} {}

  protected:
    [[nodiscard]] auto test_impl(const block_predicate_context &context) const
        -> bool override {
        return context.states->in_tag(context.region->state_at(context.pos->x,
                                                               context.pos->y,
                                                               context.pos->z),
                                      tag_);
    }

  private:
    std::string tag_;
};

class not_predicate final : public block_predicate {
  public:
    explicit not_predicate(std::unique_ptr<block_predicate> inner)
        : inner_{std::move(inner)} {}

  protected:
    [[nodiscard]] auto test_impl(const block_predicate_context &context) const
        -> bool override {
        return !inner_->evaluate(context);
    }

  private:
    std::unique_ptr<block_predicate> inner_;
};

class any_of final : public block_predicate {
  public:
    explicit any_of(std::vector<std::unique_ptr<block_predicate>> rules)
        : rules_{std::move(rules)} {}

  protected:
    [[nodiscard]] auto test_impl(const block_predicate_context &context) const
        -> bool override {
        return std::any_of(rules_.begin(), rules_.end(), [&](const auto &rule) {
            return rule->evaluate(context);
        });
    }

  private:
    std::vector<std::unique_ptr<block_predicate>> rules_;
};

class solid_predicate final : public block_predicate {
  protected:
    [[nodiscard]] auto test_impl(const block_predicate_context &context) const
        -> bool override {
        return context.states
            ->traits(context.region->state_at(context.pos->x, context.pos->y,
                                              context.pos->z))
            .blocks_motion;
    }
};

class has_sturdy_face final : public block_predicate {
  public:
    has_sturdy_face(block_face direction, const block_pos &offset)
        : direction_{direction}, offset_{offset} {}

  protected:
    [[nodiscard]] auto test_impl(const block_predicate_context &context) const
        -> bool override {
        const block_pos target{.x = context.pos->x + offset_.x,
                               .y = context.pos->y + offset_.y,
                               .z = context.pos->z + offset_.z};
        return is_sturdy_face(
            *context.states,
            context.region->state_at(target.x, target.y, target.z), direction_);
    }

  private:
    block_face direction_{block_face::up};
    block_pos offset_{};
};

class matching_fluids final : public block_predicate {
  public:
    matching_fluids(bool water, bool lava) : water_{water}, lava_{lava} {}

  protected:
    [[nodiscard]] auto test_impl(const block_predicate_context &context) const
        -> bool override {
        const auto traits = context.states->traits(context.region->state_at(
            context.pos->x, context.pos->y, context.pos->z));
        return (water_ && traits.water) ||
               (lava_ && traits.fluid && !traits.water);
    }

  private:
    bool water_{false};
    bool lava_{false};
};

enum class survival_rule : std::uint8_t {
    vegetation,
    hanging_mangrove,
    cactus,
    sugar_cane
};

/// `WouldSurvivePredicate`: asks whether `state` could stay at the offset
/// position. The rules implemented here are the ground checks of the decoded
/// vegetation, hanging propagule, cactus and sugar cane states; other block
/// families have no support model here and are rejected at decode time.
class would_survive final : public block_predicate {
  public:
    would_survive(survival_rule rule, block_pos offset)
        : rule_{rule}, offset_{offset} {}

  protected:
    [[nodiscard]] auto test_impl(const block_predicate_context &context) const
        -> bool override {
        const auto &region = *context.region;
        const auto &states = *context.states;
        const block_pos at{.x = context.pos->x + offset_.x,
                           .y = context.pos->y + offset_.y,
                           .z = context.pos->z + offset_.z};
        if (rule_ == survival_rule::hanging_mangrove) {
            const block_pos above{.x = at.x, .y = at.y + 1, .z = at.z};
            return states.in_tag(
                region.state_at(above.x, above.y, above.z),
                "minecraft:supports_hanging_mangrove_propagule");
        }
        if (rule_ == survival_rule::sugar_cane) {
            const auto below = region.state_at(at.x, at.y - 1, at.z);
            if (state_block_name(states.name(below)) ==
                "minecraft:sugar_cane") {
                return true;
            }
            if (!states.in_tag(below, "minecraft:supports_sugar_cane")) {
                return false;
            }
            constexpr std::array<std::pair<jint, jint>, 4> horizontal{{
                {0, -1},
                {0, 1},
                {-1, 0},
                {1, 0},
            }};
            for (const auto &[dx, dz] : horizontal) {
                const auto neighbor =
                    region.state_at(at.x + dx, at.y - 1, at.z + dz);
                if (states.traits(neighbor).water ||
                    states.in_tag(neighbor,
                                  "minecraft:supports_sugar_cane_adjacently")) {
                    return true;
                }
            }
            return false;
        }
        if (rule_ == survival_rule::cactus) {
            constexpr std::array<std::pair<jint, jint>, 4> horizontal{{
                {0, -1},
                {0, 1},
                {-1, 0},
                {1, 0},
            }};
            for (const auto &[dx, dz] : horizontal) {
                const auto neighbor =
                    region.state_at(at.x + dx, at.y, at.z + dz);
                const auto traits = states.traits(neighbor);
                if (traits.blocks_motion || (traits.fluid && !traits.water)) {
                    return false;
                }
            }
            const auto below = region.state_at(at.x, at.y - 1, at.z);
            const bool supported =
                state_block_name(states.name(below)) == "minecraft:cactus" ||
                states.in_tag(below, "minecraft:supports_cactus");
            return supported &&
                   !states.traits(region.state_at(at.x, at.y + 1, at.z)).fluid;
        }
        const block_pos below{.x = at.x, .y = at.y - 1, .z = at.z};
        return states.in_tag(region.state_at(below.x, below.y, below.z),
                             "minecraft:supports_vegetation");
    }

  private:
    survival_rule rule_{survival_rule::vegetation};
    block_pos offset_{};
};

class replaceable_predicate final : public block_predicate {
  public:
    explicit replaceable_predicate(block_pos offset) : offset_{offset} {}

  protected:
    [[nodiscard]] auto test_impl(const block_predicate_context &context) const
        -> bool override {
        const block_pos target{.x = context.pos->x + offset_.x,
                               .y = context.pos->y + offset_.y,
                               .z = context.pos->z + offset_.z};
        return context.states->in_tag(
            context.region->state_at(target.x, target.y, target.z),
            "minecraft:replaceable");
    }

  private:
    block_pos offset_{};
};

class inside_world_bounds_predicate final : public block_predicate {
  public:
    explicit inside_world_bounds_predicate(block_pos offset)
        : offset_{offset} {}

  protected:
    [[nodiscard]] auto test_impl(const block_predicate_context &context) const
        -> bool override {
        const jint y = context.pos->y + offset_.y;
        const jint min_y = context.region->center().min_y();
        return y >= min_y && y < min_y + context.region->center().height();
    }

  private:
    block_pos offset_{};
};

class height_range_predicate final : public block_predicate {
  public:
    height_range_predicate(surface::vertical_anchor min,
                           surface::vertical_anchor max)
        : min_{min}, max_{max} {}

  protected:
    [[nodiscard]] auto test_impl(const block_predicate_context &context) const
        -> bool override {
        const auto &generation = context.generation;
        const jint min = min_.resolve(generation.min_y, generation.height,
                                      generation.sea_level);
        const jint max = max_.resolve(generation.min_y, generation.height,
                                      generation.sea_level);
        return context.pos->y >= min && context.pos->y <= max;
    }

  private:
    surface::vertical_anchor min_{};
    surface::vertical_anchor max_{};
};

/// World generation has no entities, and the reference asks the level whether
/// any entity would obstruct a block-shaped volume at the position. That query
/// is always empty during generation, so the predicate admits every position.
class unobstructed_predicate final : public block_predicate {
  public:
    explicit unobstructed_predicate(block_pos offset) : offset_{offset} {}

  protected:
    [[nodiscard]] auto test_impl(const block_predicate_context &) const
        -> bool override {
        return true;
    }

  private:
    block_pos offset_{};
};

class matching_biomes_predicate final : public block_predicate {
  public:
    explicit matching_biomes_predicate(std::vector<std::string> biomes)
        : biomes_{std::move(biomes)} {}

  protected:
    [[nodiscard]] auto test_impl(const block_predicate_context &context) const
        -> bool override {
        if (!context.biome) {
            return false;
        }
        const auto biome = context.biome(*context.pos);
        return std::find(biomes_.begin(), biomes_.end(), biome) !=
               biomes_.end();
    }

  private:
    std::vector<std::string> biomes_;
};

class volume_match_predicate final : public block_predicate {
  public:
    volume_match_predicate(block_pos min, block_pos max,
                           std::unique_ptr<block_predicate> match)
        : min_{min}, max_{max}, match_{std::move(match)} {}

  protected:
    [[nodiscard]] auto test_impl(const block_predicate_context &context) const
        -> bool override {
        for (jint x = min_.x; x <= max_.x; ++x) {
            for (jint z = min_.z; z <= max_.z; ++z) {
                for (jint y = min_.y; y <= max_.y; ++y) {
                    auto shifted = context;
                    const block_pos pos{.x = context.pos->x + x,
                                        .y = context.pos->y + y,
                                        .z = context.pos->z + z};
                    shifted.pos = &pos;
                    if (!match_->evaluate(shifted)) {
                        return false;
                    }
                }
            }
        }
        return true;
    }

  private:
    block_pos min_{};
    block_pos max_{};
    std::unique_ptr<block_predicate> match_{};
};

class all_of final : public block_predicate {
  public:
    explicit all_of(std::vector<std::unique_ptr<block_predicate>> rules)
        : rules_{std::move(rules)} {}

  protected:
    [[nodiscard]] auto test_impl(const block_predicate_context &context) const
        -> bool override {
        return std::all_of(rules_.begin(), rules_.end(), [&](const auto &rule) {
            return rule->evaluate(context);
        });
    }

  private:
    std::vector<std::unique_ptr<block_predicate>> rules_;
};

} // namespace

auto block_predicate::test(const chunk_region &region,
                           const block_state_table &states,
                           const block_pos &pos) const -> bool {
    return evaluate(block_predicate_context{
        .region = &region,
        .states = &states,
        .pos = &pos,
    });
}

auto block_predicate::test(const placement_context &context,
                           const block_pos &pos) const -> bool {
    return evaluate(block_predicate_context{
        .region = context.region,
        .states = context.states,
        .pos = &pos,
        .generation = context.generation,
        .biome = context.biome,
    });
}

auto decode_block_predicate(const data::json::value &document,
                            const block_state_table &states)
    -> result<std::unique_ptr<block_predicate>> {
    const auto type = type_of(document);
    if (!type) {
        return std::unexpected{type.error()};
    }
    if (*type == "minecraft:true") {
        return std::unique_ptr<block_predicate>{
            std::make_unique<true_predicate>()};
    }
    if (*type == "minecraft:matching_blocks" ||
        *type == "minecraft:matching_block_tag") {
        const bool tag = *type == "minecraft:matching_block_tag";
        const auto *field = document.find(tag ? "tag" : "blocks");
        if (field == nullptr) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "block predicate has no list")};
        }
        if (tag) {
            const auto name = field->as_string();
            if (!name) {
                return std::unexpected{
                    make_error(error_code::parse_error, "tag is not a string")};
            }
            return std::unique_ptr<block_predicate>{
                std::make_unique<matching_tag>(std::string{*name})};
        }
        auto list = names(*field);
        if (!list) {
            return std::unexpected{list.error()};
        }
        return std::unique_ptr<block_predicate>{
            std::make_unique<matching_blocks>(std::move(*list))};
    }
    if (*type == "minecraft:has_sturdy_face") {
        const auto *direction = document.find("direction");
        if (direction == nullptr) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "has_sturdy_face is incomplete")};
        }
        const auto direction_text = direction->as_string();
        if (!direction_text) {
            return std::unexpected{direction_text.error()};
        }
        const auto face = parse_block_face(*direction_text);
        if (!face) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "has_sturdy_face direction is not a block face")};
        }
        block_pos offset{};
        if (const auto *offset_value = document.find("offset");
            offset_value != nullptr) {
            const auto values = offset_value->as_array();
            if (!values || values->size() != 3) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "has_sturdy_face offset is invalid")};
            }
            const auto x = (*values)[0].as_int64();
            const auto y = (*values)[1].as_int64();
            const auto z = (*values)[2].as_int64();
            if (!x || !y || !z) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "has_sturdy_face offset is invalid")};
            }
            offset = block_pos{.x = static_cast<jint>(*x),
                               .y = static_cast<jint>(*y),
                               .z = static_cast<jint>(*z)};
        }
        return std::unique_ptr<block_predicate>{
            std::make_unique<has_sturdy_face>(*face, offset)};
    }
    if (*type == "minecraft:would_survive") {
        const auto *state_value = document.find("state");
        if (state_value == nullptr) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "would_survive has no state")};
        }
        const auto key = decode_state_key(*state_value);
        if (!key) {
            return std::unexpected{key.error()};
        }
        const auto block = std::string{state_block_name(*key)};
        const bool sapling = block.ends_with("_sapling");
        const bool firefly = block == "minecraft:firefly_bush";
        const bool mangrove = block == "minecraft:mangrove_propagule";
        const bool cactus = block == "minecraft:cactus";
        const bool sugar_cane = block == "minecraft:sugar_cane";
        if (!sapling && !firefly && !mangrove && !cactus && !sugar_cane) {
            return std::unexpected{make_error(
                error_code::unsupported,
                "would_survive has no survival rule for '" + block + "'")};
        }
        block_pos offset{};
        if (const auto *offset_value = document.find("offset");
            offset_value != nullptr) {
            const auto values = offset_value->as_array();
            if (!values || values->size() != 3) {
                return std::unexpected{make_error(
                    error_code::parse_error,
                    "would_survive offset is not a three-element array")};
            }
            const auto x = (*values)[0].as_int64();
            const auto y = (*values)[1].as_int64();
            const auto z = (*values)[2].as_int64();
            if (!x || !y || !z) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "would_survive offset is invalid")};
            }
            if (std::abs(*x) > 16 || std::abs(*y) > 16 || std::abs(*z) > 16) {
                return std::unexpected{
                    make_error(error_code::invalid_argument,
                               "would_survive offset exceeds sixteen blocks")};
            }
            offset = block_pos{.x = static_cast<jint>(*x),
                               .y = static_cast<jint>(*y),
                               .z = static_cast<jint>(*z)};
        }
        survival_rule rule = survival_rule::vegetation;
        if (mangrove && key->find("hanging=true") != std::string::npos) {
            rule = survival_rule::hanging_mangrove;
        } else if (cactus) {
            rule = survival_rule::cactus;
        } else if (sugar_cane) {
            rule = survival_rule::sugar_cane;
        }
        return std::unique_ptr<block_predicate>{
            std::make_unique<would_survive>(rule, offset)};
    }
    if (*type == "minecraft:solid") {
        return std::unique_ptr<block_predicate>{
            std::make_unique<solid_predicate>()};
    }
    if (*type == "minecraft:matching_fluids") {
        const auto *fluids = document.find("fluids");
        if (fluids == nullptr) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "matching_fluids has no fluids")};
        }
        auto list = names(*fluids);
        if (!list) {
            return std::unexpected{list.error()};
        }
        bool water = false;
        bool lava = false;
        for (const auto &name : *list) {
            if (name == "minecraft:water") {
                water = true;
            } else if (name == "minecraft:lava") {
                lava = true;
            }
        }
        return std::unique_ptr<block_predicate>{
            std::make_unique<matching_fluids>(water, lava)};
    }
    if (*type == "minecraft:replaceable") {
        block_pos offset{};
        if (const auto decoded = decode_offset(document, offset); !decoded) {
            return std::unexpected{decoded.error()};
        }
        return std::unique_ptr<block_predicate>{
            std::make_unique<replaceable_predicate>(offset)};
    }
    if (*type == "minecraft:inside_world_bounds") {
        block_pos offset{};
        if (const auto decoded = decode_offset(document, offset); !decoded) {
            return std::unexpected{decoded.error()};
        }
        return std::unique_ptr<block_predicate>{
            std::make_unique<inside_world_bounds_predicate>(offset)};
    }
    if (*type == "minecraft:height_range") {
        const auto *min_field = document.find("min_inclusive");
        const auto *max_field = document.find("max_inclusive");
        if (min_field == nullptr || max_field == nullptr) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "height_range is incomplete")};
        }
        const auto min = decode_anchor(*min_field);
        const auto max = decode_anchor(*max_field);
        if (!min || !max) {
            return std::unexpected{make_error(
                error_code::parse_error, "height_range anchor is invalid")};
        }
        return std::unique_ptr<block_predicate>{
            std::make_unique<height_range_predicate>(*min, *max)};
    }
    if (*type == "minecraft:unobstructed") {
        block_pos offset{};
        if (const auto decoded = decode_offset(document, offset); !decoded) {
            return std::unexpected{decoded.error()};
        }
        return std::unique_ptr<block_predicate>{
            std::make_unique<unobstructed_predicate>(offset)};
    }
    if (*type == "minecraft:matching_biomes") {
        const auto *field = document.find("biomes");
        if (field == nullptr) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "matching_biomes has no biomes")};
        }
        std::vector<std::string> decoded;
        if (const auto single = field->as_string(); single.has_value()) {
            decoded.emplace_back(*single);
        } else if (field->is_array()) {
            for (const auto &entry : *field->as_array()) {
                const auto name = entry.as_string();
                if (!name) {
                    return std::unexpected{make_error(
                        error_code::parse_error, "biome name is not a string")};
                }
                decoded.emplace_back(*name);
            }
        } else {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "matching_biomes list is not an array")};
        }
        for (const auto &name : decoded) {
            if (name.starts_with("#")) {
                return std::unexpected{make_error(
                    error_code::unsupported,
                    "matching_biomes cannot resolve biome tag '" + name + "'")};
            }
        }
        if (decoded.empty()) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "matching_biomes list is empty")};
        }
        return std::unique_ptr<block_predicate>{
            std::make_unique<matching_biomes_predicate>(std::move(decoded))};
    }
    if (*type == "minecraft:volume_match") {
        const auto *min_field = document.find("min");
        const auto *max_field = document.find("max");
        const auto *match = document.find("match");
        if (min_field == nullptr || max_field == nullptr || match == nullptr) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "volume_match is missing a required field")};
        }
        const auto read_point = [](const data::json::value &field,
                                   block_pos &out) -> status {
            const auto values = field.as_array();
            if (!values || values->size() != 3) {
                return std::unexpected{make_error(
                    error_code::parse_error, "volume_match bound is not xyz")};
            }
            const auto x = (*values)[0].as_int64();
            const auto y = (*values)[1].as_int64();
            const auto z = (*values)[2].as_int64();
            if (!x || !y || !z) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "volume_match bound is not integer")};
            }
            if (std::abs(*x) > 16 || std::abs(*y) > 16 || std::abs(*z) > 16) {
                return std::unexpected{
                    make_error(error_code::invalid_argument,
                               "volume_match bound exceeds sixteen blocks")};
            }
            out = block_pos{.x = static_cast<jint>(*x),
                            .y = static_cast<jint>(*y),
                            .z = static_cast<jint>(*z)};
            return status_ok();
        };
        block_pos min{};
        block_pos max{};
        if (const auto decoded = read_point(*min_field, min); !decoded) {
            return std::unexpected{decoded.error()};
        }
        if (const auto decoded = read_point(*max_field, max); !decoded) {
            return std::unexpected{decoded.error()};
        }
        if (min.x > max.x || min.y > max.y || min.z > max.z) {
            return std::unexpected{make_error(
                error_code::parse_error,
                "volume_match min bound is larger than the max bound")};
        }
        auto inner = decode_block_predicate(*match, states);
        if (!inner) {
            return std::unexpected{inner.error()};
        }
        return std::unique_ptr<block_predicate>{
            std::make_unique<volume_match_predicate>(min, max,
                                                     std::move(*inner))};
    }
    if (*type == "minecraft:not") {
        const auto *predicate = document.find("predicate");
        if (predicate == nullptr) {
            return std::unexpected{make_error(
                error_code::parse_error, "not predicate has no predicate")};
        }
        auto inner = decode_block_predicate(*predicate, states);
        if (!inner) {
            return std::unexpected{inner.error()};
        }
        return std::unique_ptr<block_predicate>{
            std::make_unique<not_predicate>(std::move(*inner))};
    }
    if (*type == "minecraft:any_of") {
        const auto *rules = document.find("predicates");
        if (rules == nullptr || !rules->is_array()) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "any_of has no predicate list")};
        }
        std::vector<std::unique_ptr<block_predicate>> decoded;
        for (const auto &entry : *rules->as_array()) {
            auto predicate = decode_block_predicate(entry, states);
            if (!predicate) {
                return std::unexpected{predicate.error()};
            }
            decoded.push_back(std::move(*predicate));
        }
        return std::unique_ptr<block_predicate>{
            std::make_unique<any_of>(std::move(decoded))};
    }
    if (*type == "minecraft:all_of") {
        const auto *rules = document.find("predicates");
        if (rules == nullptr) {
            rules = document.find("rules");
        }
        if (rules == nullptr || !rules->is_array()) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "all_of has no predicate list")};
        }
        std::vector<std::unique_ptr<block_predicate>> decoded;
        for (const auto &entry : *rules->as_array()) {
            auto predicate = decode_block_predicate(entry, states);
            if (!predicate) {
                return std::unexpected{predicate.error()};
            }
            decoded.push_back(std::move(*predicate));
        }
        return std::unique_ptr<block_predicate>{
            std::make_unique<all_of>(std::move(decoded))};
    }
    return std::unexpected{
        make_error(error_code::unsupported,
                   "unsupported block predicate '" + std::string{*type} + "'")};
}

} // namespace refrain::worldgen::feature
