// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/feature/placement.hpp>

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

class fixed_placement final : public placement_modifier {
  public:
    explicit fixed_placement(std::vector<block_pos> positions)
        : positions_{std::move(positions)} {}
    void modify(const placement_context &, worldgen_random &, const block_pos &,
                std::vector<block_pos> &out) const override {
        out.insert(out.end(), positions_.begin(), positions_.end());
    }

  private:
    std::vector<block_pos> positions_;
};

} // namespace

auto decode_placement(const data::json::value &document)
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
    if (*type == "minecraft:fixed_placement") {
        return std::unique_ptr<placement_modifier>{
            std::make_unique<fixed_placement>(std::vector<block_pos>{})};
    }
    return std::unexpected{
        make_error(error_code::unsupported, "unsupported placement modifier")};
}

} // namespace refrain::worldgen::feature
