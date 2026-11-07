// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/provider.hpp>

#include <refrain/data/json.hpp>
#include <refrain/worldgen/terrain/material.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::worldgen::provider {

namespace {

using data::json::value;

auto invalid(std::string_view message) -> error {
    return make_error(error_code::invalid_argument, message);
}

auto parse_error(std::string_view message) -> error {
    return make_error(error_code::parse_error, message);
}

auto as_jint(const value &input) -> result<jint> {
    const auto number = input.as_int64();
    if (!number) {
        return std::unexpected{number.error()};
    }
    if (*number < std::numeric_limits<jint>::min() ||
        *number > std::numeric_limits<jint>::max()) {
        return std::unexpected{invalid("provider integer is out of range")};
    }
    return static_cast<jint>(*number);
}

auto as_float(const value &input) -> result<float> {
    if (input.is_integer()) {
        const auto number = input.as_int64();
        if (!number) {
            return std::unexpected{number.error()};
        }
        return static_cast<float>(*number);
    }
    const auto number = input.as_double();
    if (!number) {
        return std::unexpected{number.error()};
    }
    return static_cast<float>(*number);
}

auto type_of(const value &input) -> result<std::string_view> {
    const auto *type = input.find("type");
    if (type == nullptr) {
        return std::unexpected{parse_error("provider has no type")};
    }
    const auto text = type->as_string();
    if (!text) {
        return std::unexpected{parse_error("provider type is not a string")};
    }
    return *text;
}

auto require(const value &input, std::string_view name)
    -> result<const value *> {
    const auto *field = input.find(name);
    if (field == nullptr) {
        return std::unexpected{parse_error("provider field is missing")};
    }
    return field;
}

auto decode_anchor(const value &input) -> result<surface::vertical_anchor> {
    if (!input.is_object()) {
        return std::unexpected{parse_error("height anchor is not an object")};
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
            auto number = as_jint(*field);
            if (!number) {
                return std::unexpected{number.error()};
            }
            return surface::vertical_anchor{.type = candidate.kind,
                                            .value = *number};
        }
    }
    return std::unexpected{parse_error("unknown height anchor")};
}

class constant_int final : public int_provider {
  public:
    explicit constant_int(jint value) : value_{value} {}
    [[nodiscard]] auto sample(worldgen_random &) const -> jint override {
        return value_;
    }

  private:
    jint value_{0};
};

class uniform_int final : public int_provider {
  public:
    uniform_int(jint min, jint max) : min_{min}, max_{max} {}
    [[nodiscard]] auto sample(worldgen_random &random) const -> jint override {
        return random.next_int(max_ - min_ + 1) + min_;
    }

  private:
    jint min_{0};
    jint max_{0};
};

class biased_int final : public int_provider {
  public:
    biased_int(jint min, jint max) : min_{min}, max_{max} {}
    [[nodiscard]] auto sample(worldgen_random &random) const -> jint override {
        return min_ + random.next_int(random.next_int(max_ - min_ + 1) + 1);
    }

  private:
    jint min_{0};
    jint max_{0};
};

class very_biased_int final : public int_provider {
  public:
    very_biased_int(jint min, jint max) : min_{min}, max_{max} {}
    [[nodiscard]] auto sample(worldgen_random &random) const -> jint override {
        return min_ +
               random.next_int(
                   random.next_int(random.next_int(max_ - min_ + 1) + 1) + 1);
    }

  private:
    jint min_{0};
    jint max_{0};
};

class trapezoid_int final : public int_provider {
  public:
    trapezoid_int(jint min, jint max, jint plateau)
        : min_{min}, max_{max}, plateau_{plateau} {}
    [[nodiscard]] auto sample(worldgen_random &random) const -> jint override {
        if (plateau_ == 0 && max_ == -min_) {
            return random.next_int(max_ + 1) - random.next_int(max_ + 1);
        }
        const jint range = max_ - min_;
        if (plateau_ == range) {
            return random.next_int(range + 1) + min_;
        }
        const jint plateau_start = (range - plateau_) / 2;
        const jint plateau_end = range - plateau_start;
        return min_ + random.next_int(plateau_end + 1) +
               random.next_int(plateau_start + 1);
    }

  private:
    jint min_{0};
    jint max_{0};
    jint plateau_{0};
};

class clamped_int final : public int_provider {
  public:
    clamped_int(std::unique_ptr<int_provider> source, jint min, jint max)
        : source_{std::move(source)}, min_{min}, max_{max} {}
    [[nodiscard]] auto sample(worldgen_random &random) const -> jint override {
        return std::clamp(source_->sample(random), min_, max_);
    }

  private:
    std::unique_ptr<int_provider> source_;
    jint min_{0};
    jint max_{0};
};

class weighted_int final : public int_provider {
  public:
    struct entry {
        std::unique_ptr<int_provider> value;
        jint weight{0};
    };

    explicit weighted_int(std::vector<entry> entries)
        : entries_{std::move(entries)} {
        for (const auto &item : entries_) {
            total_ += item.weight;
        }
    }
    [[nodiscard]] auto sample(worldgen_random &random) const -> jint override {
        jint selection = random.next_int(total_);
        for (const auto &item : entries_) {
            selection -= item.weight;
            if (selection < 0) {
                return item.value->sample(random);
            }
        }
        return entries_.back().value->sample(random);
    }

  private:
    std::vector<entry> entries_;
    jint total_{0};
};

class constant_float final : public float_provider {
  public:
    explicit constant_float(float value) : value_{value} {}
    [[nodiscard]] auto sample(worldgen_random &) const -> float override {
        return value_;
    }

  private:
    float value_{0.0F};
};

class uniform_float final : public float_provider {
  public:
    uniform_float(float min, float max) : min_{min}, max_{max} {}
    [[nodiscard]] auto sample(worldgen_random &random) const -> float override {
        return random.next_float() * (max_ - min_) + min_;
    }

  private:
    float min_{0.0F};
    float max_{0.0F};
};

class trapezoid_float final : public float_provider {
  public:
    trapezoid_float(float min, float max, float plateau)
        : min_{min}, max_{max}, plateau_{plateau} {}
    [[nodiscard]] auto sample(worldgen_random &random) const -> float override {
        const float range = max_ - min_;
        const float plateau_start = (range - plateau_) / 2.0F;
        const float plateau_end = range - plateau_start;
        return min_ + random.next_float() * plateau_end +
               random.next_float() * plateau_start;
    }

  private:
    float min_{0.0F};
    float max_{0.0F};
    float plateau_{0.0F};
};

class constant_height final : public height_provider {
  public:
    explicit constant_height(surface::vertical_anchor anchor)
        : anchor_{anchor} {}
    [[nodiscard]] auto sample(worldgen_random &,
                              const generation_context &context) const
        -> jint override {
        return anchor_.resolve(context.min_y, context.height,
                               context.sea_level);
    }

  private:
    surface::vertical_anchor anchor_{};
};

class uniform_height final : public height_provider {
  public:
    uniform_height(surface::vertical_anchor min, surface::vertical_anchor max)
        : min_{min}, max_{max} {}
    [[nodiscard]] auto sample(worldgen_random &random,
                              const generation_context &context) const
        -> jint override {
        const jint min =
            min_.resolve(context.min_y, context.height, context.sea_level);
        const jint max =
            max_.resolve(context.min_y, context.height, context.sea_level);
        if (min > max) {
            return min;
        }
        return random.next_int(max - min + 1) + min;
    }

  private:
    surface::vertical_anchor min_{};
    surface::vertical_anchor max_{};
};

class biased_height final : public height_provider {
  public:
    biased_height(surface::vertical_anchor min, surface::vertical_anchor max,
                  jint inner)
        : min_{min}, max_{max}, inner_{inner} {}
    [[nodiscard]] auto sample(worldgen_random &random,
                              const generation_context &context) const
        -> jint override {
        const jint min =
            min_.resolve(context.min_y, context.height, context.sea_level);
        const jint max =
            max_.resolve(context.min_y, context.height, context.sea_level);
        if (max - min - inner_ + 1 <= 0) {
            return min;
        }
        const jint limit = random.next_int(max - min - inner_ + 1);
        return random.next_int(limit + inner_) + min;
    }

  private:
    surface::vertical_anchor min_{};
    surface::vertical_anchor max_{};
    jint inner_{1};
};

class very_biased_height final : public height_provider {
  public:
    very_biased_height(surface::vertical_anchor min,
                       surface::vertical_anchor max, jint inner)
        : min_{min}, max_{max}, inner_{inner} {}
    [[nodiscard]] auto sample(worldgen_random &random,
                              const generation_context &context) const
        -> jint override {
        const jint min =
            min_.resolve(context.min_y, context.height, context.sea_level);
        const jint max =
            max_.resolve(context.min_y, context.height, context.sea_level);
        if (max - min - inner_ + 1 <= 0) {
            return min;
        }
        const jint upper =
            random.next_int(max - (min + inner_) + 1) + min + inner_;
        const jint biased_upper = random.next_int(upper - 1 - min + 1) + min;
        return random.next_int(biased_upper - 1 + inner_ - min + 1) + min;
    }

  private:
    surface::vertical_anchor min_{};
    surface::vertical_anchor max_{};
    jint inner_{1};
};

class trapezoid_height final : public height_provider {
  public:
    trapezoid_height(surface::vertical_anchor min, surface::vertical_anchor max,
                     jint plateau)
        : min_{min}, max_{max}, plateau_{plateau} {}
    [[nodiscard]] auto sample(worldgen_random &random,
                              const generation_context &context) const
        -> jint override {
        const jint min =
            min_.resolve(context.min_y, context.height, context.sea_level);
        const jint max =
            max_.resolve(context.min_y, context.height, context.sea_level);
        if (min > max) {
            return min;
        }
        const jint range = max - min;
        if (plateau_ >= range) {
            return random.next_int(range + 1) + min;
        }
        const jint plateau_start = (range - plateau_) / 2;
        const jint plateau_end = range - plateau_start;
        return min + random.next_int(plateau_end + 1) +
               random.next_int(plateau_start + 1);
    }

  private:
    surface::vertical_anchor min_{};
    surface::vertical_anchor max_{};
    jint plateau_{0};
};

auto decode_int(const value &input) -> result<std::unique_ptr<int_provider>>;
auto decode_float(const value &input)
    -> result<std::unique_ptr<float_provider>>;
auto decode_height(const value &input)
    -> result<std::unique_ptr<height_provider>>;

auto decode_int(const value &input) -> result<std::unique_ptr<int_provider>> {
    if (input.is_integer()) {
        auto constant = as_jint(input);
        if (!constant) {
            return std::unexpected{constant.error()};
        }
        return std::make_unique<constant_int>(*constant);
    }
    auto type = type_of(input);
    if (!type) {
        return std::unexpected{type.error()};
    }

    auto int_field = [&input](std::string_view name) -> result<jint> {
        auto field = require(input, name);
        if (!field) {
            return std::unexpected{field.error()};
        }
        return as_jint(**field);
    };

    if (*type == "minecraft:constant") {
        auto constant = int_field("value");
        if (!constant) {
            return std::unexpected{constant.error()};
        }
        return std::make_unique<constant_int>(*constant);
    }
    if (*type == "minecraft:uniform") {
        auto min = int_field("min_inclusive");
        auto max = int_field("max_inclusive");
        if (!min || !max) {
            return std::unexpected{invalid("uniform int has no range")};
        }
        if (*max < *min) {
            return std::unexpected{invalid("uniform int range is inverted")};
        }
        return std::make_unique<uniform_int>(*min, *max);
    }
    if (*type == "minecraft:biased_to_bottom") {
        auto min = int_field("min_inclusive");
        auto max = int_field("max_inclusive");
        if (!min || !max) {
            return std::unexpected{invalid("biased int has no range")};
        }
        return std::make_unique<biased_int>(*min, *max);
    }
    if (*type == "minecraft:very_biased_to_bottom") {
        auto min = int_field("min_inclusive");
        auto max = int_field("max_inclusive");
        if (!min || !max) {
            return std::unexpected{invalid("very biased int has no range")};
        }
        return std::make_unique<very_biased_int>(*min, *max);
    }
    if (*type == "minecraft:trapezoid") {
        auto min = int_field("min");
        auto max = int_field("max");
        auto plateau = int_field("plateau");
        if (!min || !max || !plateau) {
            return std::unexpected{invalid("trapezoid int has no range")};
        }
        return std::make_unique<trapezoid_int>(*min, *max, *plateau);
    }
    if (*type == "minecraft:clamped") {
        auto source_field = require(input, "source");
        if (!source_field) {
            return std::unexpected{source_field.error()};
        }
        auto source = decode_int(**source_field);
        auto min = int_field("min_inclusive");
        auto max = int_field("max_inclusive");
        if (!source || !min || !max) {
            return std::unexpected{invalid("clamped int is incomplete")};
        }
        return std::make_unique<clamped_int>(std::move(*source), *min, *max);
    }
    if (*type == "minecraft:weighted_list") {
        auto distribution = require(input, "distribution");
        if (!distribution) {
            return std::unexpected{distribution.error()};
        }
        if (!(*distribution)->is_array()) {
            return std::unexpected{parse_error("distribution is not an array")};
        }
        std::vector<weighted_int::entry> entries;
        for (const auto &item : *(*distribution)->as_array()) {
            auto data = require(item, "data");
            auto weight = require(item, "weight");
            if (!data || !weight) {
                return std::unexpected{invalid("weighted entry is incomplete")};
            }
            auto value = decode_int(**data);
            auto amount = as_jint(**weight);
            if (!value || !amount) {
                return std::unexpected{invalid("weighted entry is invalid")};
            }
            entries.push_back(weighted_int::entry{.value = std::move(*value),
                                                  .weight = *amount});
        }
        return std::make_unique<weighted_int>(std::move(entries));
    }
    return std::unexpected{invalid("unsupported integer provider")};
}

auto decode_float(const value &input)
    -> result<std::unique_ptr<float_provider>> {
    if (input.is_integer() || input.is_real()) {
        auto constant = as_float(input);
        if (!constant) {
            return std::unexpected{constant.error()};
        }
        return std::make_unique<constant_float>(*constant);
    }
    auto type = type_of(input);
    if (!type) {
        return std::unexpected{type.error()};
    }
    auto float_field = [&input](std::string_view name) -> result<float> {
        auto field = require(input, name);
        if (!field) {
            return std::unexpected{field.error()};
        }
        return as_float(**field);
    };

    if (*type == "minecraft:constant") {
        auto constant = float_field("value");
        if (!constant) {
            return std::unexpected{constant.error()};
        }
        return std::make_unique<constant_float>(*constant);
    }
    if (*type == "minecraft:uniform") {
        auto min = float_field("min_inclusive");
        auto max = float_field("max_exclusive");
        if (!min || !max) {
            return std::unexpected{invalid("uniform float has no range")};
        }
        if (*max <= *min) {
            return std::unexpected{invalid("uniform float range is inverted")};
        }
        return std::make_unique<uniform_float>(*min, *max);
    }
    if (*type == "minecraft:trapezoid") {
        auto min = float_field("min");
        auto max = float_field("max");
        auto plateau = float_field("plateau");
        if (!min || !max || !plateau) {
            return std::unexpected{invalid("trapezoid float has no range")};
        }
        return std::make_unique<trapezoid_float>(*min, *max, *plateau);
    }
    return std::unexpected{invalid("unsupported float provider")};
}

auto decode_height(const value &input)
    -> result<std::unique_ptr<height_provider>> {
    if (!input.is_object()) {
        return std::unexpected{parse_error("height provider is not an object")};
    }
    if (const auto *type_field = input.find("type"); type_field == nullptr) {
        auto anchor = decode_anchor(input);
        if (!anchor) {
            return std::unexpected{anchor.error()};
        }
        return std::make_unique<constant_height>(*anchor);
    }

    auto type = type_of(input);
    if (!type) {
        return std::unexpected{type.error()};
    }
    auto anchor_field =
        [&input](std::string_view name) -> result<surface::vertical_anchor> {
        auto field = require(input, name);
        if (!field) {
            return std::unexpected{field.error()};
        }
        return decode_anchor(**field);
    };

    if (*type == "minecraft:constant") {
        auto anchor = anchor_field("value");
        if (!anchor) {
            return std::unexpected{anchor.error()};
        }
        return std::make_unique<constant_height>(*anchor);
    }
    if (*type == "minecraft:uniform") {
        auto min = anchor_field("min_inclusive");
        auto max = anchor_field("max_inclusive");
        if (!min || !max) {
            return std::unexpected{invalid("uniform height has no range")};
        }
        return std::make_unique<uniform_height>(*min, *max);
    }
    if (*type == "minecraft:biased_to_bottom" ||
        *type == "minecraft:very_biased_to_bottom") {
        auto min = anchor_field("min_inclusive");
        auto max = anchor_field("max_inclusive");
        if (!min || !max) {
            return std::unexpected{invalid("biased height has no range")};
        }
        jint inner = 1;
        if (const auto *field = input.find("inner"); field != nullptr) {
            auto parsed = as_jint(*field);
            if (!parsed) {
                return std::unexpected{parsed.error()};
            }
            inner = *parsed;
        }
        if (*type == "minecraft:biased_to_bottom") {
            return std::make_unique<biased_height>(*min, *max, inner);
        }
        return std::make_unique<very_biased_height>(*min, *max, inner);
    }
    if (*type == "minecraft:trapezoid") {
        auto min = anchor_field("min_inclusive");
        auto max = anchor_field("max_inclusive");
        if (!min || !max) {
            return std::unexpected{invalid("trapezoid height has no range")};
        }
        jint plateau = 0;
        if (const auto *field = input.find("plateau"); field != nullptr) {
            auto parsed = as_jint(*field);
            if (!parsed) {
                return std::unexpected{parsed.error()};
            }
            plateau = *parsed;
        }
        return std::make_unique<trapezoid_height>(*min, *max, plateau);
    }
    return std::unexpected{invalid("unsupported height provider")};
}

} // namespace

auto make_constant_float(float value) -> std::unique_ptr<float_provider> {
    return std::make_unique<constant_float>(value);
}

auto int_provider::decode(const data::json::value &value)
    -> result<std::unique_ptr<int_provider>> {
    return decode_int(value);
}

auto float_provider::decode(const data::json::value &value)
    -> result<std::unique_ptr<float_provider>> {
    return decode_float(value);
}

auto height_provider::decode(const data::json::value &value)
    -> result<std::unique_ptr<height_provider>> {
    return decode_height(value);
}

} // namespace refrain::worldgen::provider
