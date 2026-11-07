// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/feature/placement.hpp>

#include <refrain/worldgen/chunk/block_state_codec.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/feature/block_predicate.hpp>

#include <refrain/data/json.hpp>

#include <string_view>
#include <utility>

namespace refrain::worldgen::feature {

namespace {

auto type_of(const data::json::value &input) -> result<std::string_view> {
    const auto *type = input.find("type");
    if (type == nullptr) {
        return std::unexpected{
            make_error(error_code::parse_error, "placement has no type")};
    }
    const auto text = type->as_string();
    if (!text) {
        return std::unexpected{make_error(error_code::parse_error,
                                          "placement type is not a string")};
    }
    return *text;
}

auto field(const data::json::value &input, std::string_view name)
    -> result<const data::json::value *> {
    const auto *found = input.find(name);
    if (found == nullptr) {
        return std::unexpected{
            make_error(error_code::parse_error, "placement field is missing")};
    }
    return found;
}

auto as_int(const data::json::value &input) -> result<jint> {
    const auto value = input.as_int64();
    if (!value) {
        return std::unexpected{value.error()};
    }
    return static_cast<jint>(*value);
}

auto as_float(const data::json::value &input) -> result<float> {
    const auto value = input.as_double();
    if (!value) {
        return std::unexpected{value.error()};
    }
    return static_cast<float>(*value);
}

auto heightmap_kind(std::string_view name) -> result<worldgen::heightmap_kind> {
    if (name == "WORLD_SURFACE_WG") {
        return worldgen::heightmap_kind::world_surface_wg;
    }
    if (name == "WORLD_SURFACE") {
        return worldgen::heightmap_kind::world_surface;
    }
    if (name == "OCEAN_FLOOR_WG") {
        return worldgen::heightmap_kind::ocean_floor_wg;
    }
    if (name == "OCEAN_FLOOR") {
        return worldgen::heightmap_kind::ocean_floor;
    }
    if (name == "MOTION_BLOCKING") {
        return worldgen::heightmap_kind::motion_blocking;
    }
    if (name == "MOTION_BLOCKING_NO_LEAVES") {
        return worldgen::heightmap_kind::motion_blocking_no_leaves;
    }
    return std::unexpected{
        make_error(error_code::invalid_argument, "unknown heightmap type")};
}

class count_placement final : public placement_modifier {
  public:
    explicit count_placement(std::unique_ptr<provider::int_provider> count)
        : count_{std::move(count)} {}
    void modify(const placement_context &, worldgen_random &random,
                const block_pos &origin,
                std::vector<block_pos> &out) const override {
        const jint count = count_->sample(random);
        for (jint i = 0; i < count; ++i) {
            out.push_back(origin);
        }
    }

  private:
    std::unique_ptr<provider::int_provider> count_;
};

class in_square_placement final : public placement_modifier {
  public:
    void modify(const placement_context &, worldgen_random &random,
                const block_pos &origin,
                std::vector<block_pos> &out) const override {
        out.push_back(block_pos{.x = origin.x + random.next_int(16),
                                .y = origin.y,
                                .z = origin.z + random.next_int(16)});
    }
};

class heightmap_placement final : public placement_modifier {
  public:
    explicit heightmap_placement(worldgen::heightmap_kind kind) : kind_{kind} {}
    void modify(const placement_context &context, worldgen_random &,
                const block_pos &origin,
                std::vector<block_pos> &out) const override {
        const jint local_x = origin.x - context.chunk->chunk_x() * 16;
        const jint local_z = origin.z - context.chunk->chunk_z() * 16;
        const jint height =
            context.chunk->first_available(kind_, local_x, local_z);
        if (height > context.chunk->min_y()) {
            out.push_back(block_pos{.x = origin.x, .y = height, .z = origin.z});
        }
    }

  private:
    worldgen::heightmap_kind kind_;
};

class height_range_placement final : public placement_modifier {
  public:
    explicit height_range_placement(
        std::unique_ptr<provider::height_provider> height)
        : height_{std::move(height)} {}
    void modify(const placement_context &context, worldgen_random &random,
                const block_pos &origin,
                std::vector<block_pos> &out) const override {
        out.push_back(block_pos{
            .x = origin.x,
            .y = height_->sample(random, context.generation),
            .z = origin.z,
        });
    }

  private:
    std::unique_ptr<provider::height_provider> height_;
};

class rarity_filter final : public placement_modifier {
  public:
    explicit rarity_filter(float chance) : chance_{chance} {}
    void modify(const placement_context &, worldgen_random &random,
                const block_pos &origin,
                std::vector<block_pos> &out) const override {
        if (random.next_float() < 1.0F / chance_) {
            out.push_back(origin);
        }
    }

  private:
    float chance_{1.0F};
};

class offset_placement final : public placement_modifier {
  public:
    offset_placement(jint x, jint y, jint z) : x_{x}, y_{y}, z_{z} {}
    void modify(const placement_context &, worldgen_random &,
                const block_pos &origin,
                std::vector<block_pos> &out) const override {
        out.push_back(block_pos{
            .x = origin.x + x_, .y = origin.y + y_, .z = origin.z + z_});
    }

  private:
    jint x_{0};
    jint y_{0};
    jint z_{0};
};

class random_chance final : public placement_modifier {
  public:
    explicit random_chance(float chance) : chance_{chance} {}
    void modify(const placement_context &, worldgen_random &random,
                const block_pos &position,
                std::vector<block_pos> &out) const override {
        if (random.next_float() < chance_) {
            out.push_back(position);
        }
    }

  private:
    float chance_{0.0F};
};

class count_on_every_layer final : public placement_modifier {
  public:
    explicit count_on_every_layer(std::unique_ptr<provider::int_provider> count)
        : count_{std::move(count)} {}
    void modify(const placement_context &context, worldgen_random &random,
                const block_pos &origin,
                std::vector<block_pos> &out) const override {
        jint layer = 0;
        bool found_any = false;
        do {
            found_any = false;
            const jint count = count_->sample(random);
            for (jint i = 0; i < count; ++i) {
                const jint x = random.next_int(16) + origin.x;
                const jint z = random.next_int(16) + origin.z;
                const jint local_x = x - context.chunk->chunk_x() * 16;
                const jint local_z = z - context.chunk->chunk_z() * 16;
                if (local_x < 0 || local_x > 15 || local_z < 0 ||
                    local_z > 15) {
                    continue;
                }
                const jint start_y = context.chunk->first_available(
                    heightmap_kind::motion_blocking, local_x, local_z);
                jint y = start_y;
                jint current_layer = 0;
                bool current_air = false;
                const auto current = context.chunk->state_at(
                    static_cast<jint>(x), start_y, static_cast<jint>(z));
                current_air = is_empty(context, current);
                for (; y >= context.chunk->min_y() + 1; --y) {
                    const auto below = context.chunk->state_at(
                        static_cast<jint>(x), y - 1, static_cast<jint>(z));
                    if (!is_empty(context, below) && current_air &&
                        state_block_name(context.states->name(below)) !=
                            "minecraft:bedrock") {
                        if (current_layer == layer) {
                            out.push_back(block_pos{.x = x, .y = y, .z = z});
                            found_any = true;
                            break;
                        }
                        ++current_layer;
                    }
                    current_air = is_empty(context, below);
                }
            }
            ++layer;
        } while (found_any);
    }

  private:
    static auto is_empty(const placement_context &context, block_state_id state)
        -> bool {
        const auto name = context.states->name(state);
        return is_air_block_name(name) || name == "minecraft:water" ||
               name == "minecraft:lava";
    }

    std::unique_ptr<provider::int_provider> count_;
};

class environment_scan final : public placement_modifier {
  public:
    environment_scan(jint direction, std::unique_ptr<block_predicate> target,
                     std::unique_ptr<block_predicate> allowed, jint max_steps)
        : direction_{direction}, target_{std::move(target)},
          allowed_{std::move(allowed)}, max_steps_{max_steps} {}
    void modify(const placement_context &context, worldgen_random &,
                const block_pos &origin,
                std::vector<block_pos> &out) const override {
        block_pos pos = origin;
        const auto allowed = [&](const block_pos &value) {
            return allowed_ == nullptr ||
                   allowed_->test(*context.region, *context.states, value);
        };
        const auto target = [&](const block_pos &value) {
            return target_->test(*context.region, *context.states, value);
        };
        if (!allowed(pos)) {
            return;
        }
        for (jint i = 0; i < max_steps_; ++i) {
            if (target(pos)) {
                out.push_back(pos);
                return;
            }
            pos.y += direction_;
            if (pos.y < context.chunk->min_y() ||
                pos.y >= context.chunk->min_y() + context.chunk->height()) {
                return;
            }
            if (!allowed(pos)) {
                break;
            }
        }
        if (target(pos)) {
            out.push_back(pos);
        }
    }

  private:
    jint direction_{-1};
    std::unique_ptr<block_predicate> target_;
    std::unique_ptr<block_predicate> allowed_;
    jint max_steps_{1};
};

class surface_water_depth_filter final : public placement_modifier {
  public:
    explicit surface_water_depth_filter(jint max_depth)
        : max_depth_{max_depth} {}
    void modify(const placement_context &context, worldgen_random &,
                const block_pos &position,
                std::vector<block_pos> &out) const override {
        const jint local_x = position.x - context.chunk->chunk_x() * 16;
        const jint local_z = position.z - context.chunk->chunk_z() * 16;
        if (local_x < 0 || local_x > 15 || local_z < 0 || local_z > 15) {
            return;
        }
        const jint surface = context.chunk->first_available(
            heightmap_kind::world_surface, local_x, local_z);
        const jint floor = context.chunk->first_available(
            heightmap_kind::ocean_floor, local_x, local_z);
        if (surface - floor <= max_depth_) {
            out.push_back(position);
        }
    }

  private:
    jint max_depth_{0};
};

class block_predicate_filter final : public placement_modifier {
  public:
    explicit block_predicate_filter(std::unique_ptr<block_predicate> predicate)
        : predicate_{std::move(predicate)} {}
    void modify(const placement_context &context, worldgen_random &,
                const block_pos &position,
                std::vector<block_pos> &out) const override {
        if (predicate_->test(*context.region, *context.states, position)) {
            out.push_back(position);
        }
    }

  private:
    std::unique_ptr<block_predicate> predicate_;
};

class fixed_placement final : public placement_modifier {
  public:
    explicit fixed_placement(std::vector<block_pos> positions)
        : positions_{std::move(positions)} {}
    void modify(const placement_context &, worldgen_random &,
                const block_pos &origin,
                std::vector<block_pos> &out) const override {
        const auto chunk_of = [](jint coordinate) {
            return coordinate >= 0 ? coordinate / 16
                                   : -(((-coordinate) + 15) / 16);
        };
        const jint chunk_x = chunk_of(origin.x);
        const jint chunk_z = chunk_of(origin.z);
        for (const auto &position : positions_) {
            if (chunk_of(position.x) == chunk_x &&
                chunk_of(position.z) == chunk_z) {
                out.push_back(position);
            }
        }
    }

  private:
    std::vector<block_pos> positions_;
};

} // namespace

auto decode_placement(const data::json::value &document,
                      block_state_registry &states)
    -> result<std::unique_ptr<placement_modifier>> {
    const auto type = type_of(document);
    if (!type) {
        return std::unexpected{type.error()};
    }
    if (*type == "minecraft:count") {
        const auto count_field = field(document, "count");
        if (!count_field) {
            return std::unexpected{count_field.error()};
        }
        auto count = provider::int_provider::decode(**count_field);
        if (!count) {
            return std::unexpected{count.error()};
        }
        return std::unique_ptr<placement_modifier>{
            std::make_unique<count_placement>(std::move(*count))};
    }
    if (*type == "minecraft:in_square") {
        return std::unique_ptr<placement_modifier>{
            std::make_unique<in_square_placement>()};
    }
    if (*type == "minecraft:heightmap") {
        const auto heightmap_field = field(document, "heightmap");
        if (!heightmap_field) {
            return std::unexpected{heightmap_field.error()};
        }
        const auto name = (*heightmap_field)->as_string();
        if (!name) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "heightmap is not a string")};
        }
        const auto kind = heightmap_kind(*name);
        if (!kind) {
            return std::unexpected{kind.error()};
        }
        return std::unique_ptr<placement_modifier>{
            std::make_unique<heightmap_placement>(*kind)};
    }
    if (*type == "minecraft:height_range") {
        const auto height_field = field(document, "height");
        if (!height_field) {
            return std::unexpected{height_field.error()};
        }
        auto height = provider::height_provider::decode(**height_field);
        if (!height) {
            return std::unexpected{height.error()};
        }
        return std::unique_ptr<placement_modifier>{
            std::make_unique<height_range_placement>(std::move(*height))};
    }
    if (*type == "minecraft:rarity_filter") {
        const auto chance_field = field(document, "chance");
        if (!chance_field) {
            return std::unexpected{chance_field.error()};
        }
        const auto chance = as_float(**chance_field);
        if (!chance) {
            return std::unexpected{chance.error()};
        }
        return std::unique_ptr<placement_modifier>{
            std::make_unique<rarity_filter>(*chance)};
    }
    if (*type == "minecraft:offset") {
        jint x = 0;
        jint y = 0;
        jint z = 0;
        for (const auto &[name, slot] :
             {std::pair{"x", &x}, std::pair{"y", &y}, std::pair{"z", &z}}) {
            if (const auto *value = document.find(name); value != nullptr) {
                auto parsed = as_int(*value);
                if (!parsed) {
                    return std::unexpected{parsed.error()};
                }
                *slot = *parsed;
            }
        }
        return std::unique_ptr<placement_modifier>{
            std::make_unique<offset_placement>(x, y, z)};
    }
    if (*type == "minecraft:environment_scan") {
        const auto *direction = document.find("direction_of_search");
        const auto *target = document.find("target_condition");
        const auto *max_steps = document.find("max_steps");
        if (direction == nullptr || target == nullptr || max_steps == nullptr) {
            return std::unexpected{make_error(
                error_code::parse_error, "environment_scan is incomplete")};
        }
        const auto direction_name = direction->as_string();
        if (!direction_name ||
            (*direction_name != "up" && *direction_name != "down")) {
            return std::unexpected{make_error(
                error_code::parse_error, "environment direction is invalid")};
        }
        auto target_predicate = decode_block_predicate(*target, states);
        if (!target_predicate) {
            return std::unexpected{target_predicate.error()};
        }
        std::unique_ptr<block_predicate> allowed_predicate;
        if (const auto *allowed = document.find("allowed_search_condition");
            allowed != nullptr) {
            auto decoded = decode_block_predicate(*allowed, states);
            if (!decoded) {
                return std::unexpected{decoded.error()};
            }
            allowed_predicate = std::move(*decoded);
        }
        const auto steps = max_steps->as_int64();
        if (!steps) {
            return std::unexpected{steps.error()};
        }
        return std::unique_ptr<placement_modifier>{
            std::make_unique<environment_scan>(
                *direction_name == "up" ? 1 : -1, std::move(*target_predicate),
                std::move(allowed_predicate), static_cast<jint>(*steps))};
    }
    if (*type == "minecraft:random_chance") {
        const auto *chance = document.find("chance");
        if (chance == nullptr) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "random_chance has no chance")};
        }
        const auto value = chance->as_double();
        if (!value) {
            return std::unexpected{value.error()};
        }
        return std::unique_ptr<placement_modifier>{
            std::make_unique<random_chance>(static_cast<float>(*value))};
    }
    if (*type == "minecraft:count_on_every_layer") {
        const auto *count_field = document.find("count");
        if (count_field == nullptr) {
            return std::unexpected{make_error(
                error_code::parse_error, "count_on_every_layer has no count")};
        }
        auto count = provider::int_provider::decode(*count_field);
        if (!count) {
            return std::unexpected{count.error()};
        }
        return std::unique_ptr<placement_modifier>{
            std::make_unique<count_on_every_layer>(std::move(*count))};
    }
    if (*type == "minecraft:surface_water_depth_filter") {
        const auto *depth = document.find("max_water_depth");
        if (depth == nullptr) {
            return std::unexpected{make_error(
                error_code::parse_error,
                "surface_water_depth_filter has no max_water_depth")};
        }
        const auto value = depth->as_int64();
        if (!value) {
            return std::unexpected{value.error()};
        }
        return std::unique_ptr<placement_modifier>{
            std::make_unique<surface_water_depth_filter>(
                static_cast<jint>(*value))};
    }
    if (*type == "minecraft:block_predicate_filter") {
        const auto *predicate_field = document.find("predicate");
        if (predicate_field == nullptr) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "block_predicate_filter has no predicate")};
        }
        auto predicate = decode_block_predicate(*predicate_field, states);
        if (!predicate) {
            return std::unexpected{predicate.error()};
        }
        return std::unique_ptr<placement_modifier>{
            std::make_unique<block_predicate_filter>(std::move(*predicate))};
    }
    if (*type == "minecraft:fixed_placement") {
        const auto *positions = document.find("positions");
        if (positions == nullptr || !positions->is_array()) {
            return std::unexpected{make_error(
                error_code::parse_error, "fixed_placement has no positions")};
        }
        std::vector<block_pos> decoded;
        for (const auto &entry : *positions->as_array()) {
            if (!entry.is_array() || entry.size() != 3) {
                return std::unexpected{make_error(error_code::parse_error,
                                                  "fixed position is not xyz")};
            }
            const auto values = entry.as_array();
            if (!values) {
                return std::unexpected{values.error()};
            }
            auto x = (*values)[0].as_int64();
            auto y = (*values)[1].as_int64();
            auto z = (*values)[2].as_int64();
            if (!x || !y || !z) {
                return std::unexpected{make_error(
                    error_code::parse_error, "fixed position is not integer")};
            }
            decoded.push_back(block_pos{.x = static_cast<jint>(*x),
                                        .y = static_cast<jint>(*y),
                                        .z = static_cast<jint>(*z)});
        }
        return std::unique_ptr<placement_modifier>{
            std::make_unique<fixed_placement>(std::move(decoded))};
    }
    return std::unexpected{
        make_error(error_code::unsupported, "unsupported placement modifier")};
}

} // namespace refrain::worldgen::feature
