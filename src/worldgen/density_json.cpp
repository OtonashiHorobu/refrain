// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/density_json.hpp>

#include <refrain/data/json.hpp>
#include <refrain/worldgen/noise.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <unordered_set>

namespace refrain::worldgen::density {

namespace {

using json = data::json::value;

auto fail(std::string_view message) -> error {
    return make_error(error_code::parse_error, message);
}

auto string_member(const json &node, std::string_view key)
    -> std::optional<std::string> {
    const json *found = node.find(key);
    if (found == nullptr) {
        return std::nullopt;
    }
    const auto value = found->as_string();
    return value ? std::optional<std::string>{std::string{*value}}
                 : std::nullopt;
}

auto double_member(const json &node, std::string_view key)
    -> std::optional<double> {
    const json *found = node.find(key);
    if (found == nullptr) {
        return std::nullopt;
    }
    const auto value = found->as_double();
    return value ? std::optional<double>{*value} : std::nullopt;
}

auto int_member(const json &node, std::string_view key) -> std::optional<jint> {
    const auto value = double_member(node, key);
    return value ? std::optional<jint>{static_cast<jint>(*value)}
                 : std::nullopt;
}

auto bool_member(const json &node, std::string_view key)
    -> std::optional<bool> {
    const json *found = node.find(key);
    if (found == nullptr) {
        return std::nullopt;
    }
    const auto value = found->as_bool();
    return value ? std::optional<bool>{*value} : std::nullopt;
}

class decoder {
  public:
    explicit decoder(const density_documents &documents)
        : documents_{documents},
          factory_{new_positional_factory(documents.random_algorithm,
                                          documents.seed)} {}

    auto decode(std::string_view text) -> result<sampler_ptr> {
        auto parsed = json::parse(text);
        if (!parsed) {
            return std::unexpected{parsed.error()};
        }
        return decode_value(*parsed);
    }

  private:
    auto decode_value(const json &node) -> result<sampler_ptr> {
        if (node.is_number()) {
            return constant(static_cast<float>(*node.as_double()));
        }
        if (node.is_string()) {
            return decode_reference(*node.as_string());
        }
        if (!node.is_object()) {
            return std::unexpected{
                fail("density function must be a number, string or object")};
        }
        const auto type = string_member(node, "type");
        if (!type) {
            return std::unexpected{fail("density function has no type")};
        }
        const std::string_view kind = *type;
        if (kind == "minecraft:constant") {
            const auto value = double_member(node, "value");
            if (!value) {
                return std::unexpected{fail("constant has no value")};
            }
            return constant(static_cast<float>(*value));
        }
        if (kind == "minecraft:add" || kind == "minecraft:sub" ||
            kind == "minecraft:mul" || kind == "minecraft:div" ||
            kind == "minecraft:min" || kind == "minecraft:max") {
            return decode_binary(kind, node);
        }
        if (kind == "minecraft:abs" || kind == "minecraft:square" ||
            kind == "minecraft:cube" || kind == "minecraft:sqrt" ||
            kind == "minecraft:half_negative" ||
            kind == "minecraft:quarter_negative" ||
            kind == "minecraft:reciprocal" || kind == "minecraft:negate" ||
            kind == "minecraft:squeeze" || kind == "minecraft:log" ||
            kind == "minecraft:sign") {
            return decode_unary(kind, node);
        }
        if (kind == "minecraft:clamp") {
            return decode_clamp(node);
        }
        if (kind == "minecraft:gradient" ||
            kind == "minecraft:y_clamped_gradient") {
            return decode_gradient(kind, node);
        }
        if (kind == "minecraft:noise") {
            return decode_noise(node);
        }
        if (kind == "minecraft:shift" || kind == "minecraft:shift_a" ||
            kind == "minecraft:shift_b") {
            return decode_shift(kind, node);
        }
        if (kind == "minecraft:range_choice") {
            return decode_range_choice(node);
        }
        if (kind == "minecraft:interpolated") {
            return decode_interpolated(node);
        }
        if (kind == "minecraft:cache") {
            return child(node, "input");
        }
        if (kind == "minecraft:blend_density") {
            auto input = child(node, "input");
            if (!input) {
                return input;
            }
            return blend_density(std::move(*input));
        }
        if (kind == "minecraft:blend_alpha") {
            return blend_alpha();
        }
        if (kind == "minecraft:blend_offset") {
            return blend_offset();
        }
        if (kind == "minecraft:beardifier") {
            return beardifier();
        }
        if (kind == "minecraft:lerp") {
            return decode_lerp(node);
        }
        if (kind == "minecraft:spline") {
            return decode_spline_node(node);
        }
        if (kind == "minecraft:old_blended_noise") {
            return decode_old_blended(node);
        }
        if (kind == "minecraft:find_top_surface") {
            return decode_find_top_surface(node);
        }
        return std::unexpected{fail("unsupported density function type")};
    }

    auto decode_reference(std::string_view id) -> result<sampler_ptr> {
        const std::string key{id};
        if (active_.contains(key)) {
            return std::unexpected{fail("density function reference cycle")};
        }
        const auto found = std::find_if(
            documents_.functions.begin(), documents_.functions.end(),
            [&](const auto &entry) { return entry.first == key; });
        if (found == documents_.functions.end()) {
            return std::unexpected{fail("unknown density function reference")};
        }
        auto parsed = json::parse(found->second);
        if (!parsed) {
            return std::unexpected{parsed.error()};
        }
        active_.insert(key);
        auto result = decode_value(*parsed);
        active_.erase(key);
        return result;
    }

    auto child(const json &node, std::string_view key) -> result<sampler_ptr> {
        const json *found = node.find(key);
        if (found == nullptr) {
            return std::unexpected{fail("missing density function member")};
        }
        return decode_value(*found);
    }

    auto decode_binary(std::string_view kind, const json &node)
        -> result<sampler_ptr> {
        auto left = child(node, "left");
        if (!left) {
            return left;
        }
        auto right = child(node, "right");
        if (!right) {
            return right;
        }
        if (kind == "minecraft:add") {
            return add(std::move(*left), std::move(*right));
        }
        if (kind == "minecraft:sub") {
            return sub(std::move(*left), std::move(*right));
        }
        if (kind == "minecraft:mul") {
            return mul(std::move(*left), std::move(*right));
        }
        if (kind == "minecraft:div") {
            return div(std::move(*left), std::move(*right));
        }
        if (kind == "minecraft:min") {
            return min(std::move(*left), std::move(*right));
        }
        return max(std::move(*left), std::move(*right));
    }

    auto decode_unary(std::string_view kind, const json &node)
        -> result<sampler_ptr> {
        auto input = child(node, "input");
        if (!input) {
            return input;
        }
        if (kind == "minecraft:abs") {
            return abs(std::move(*input));
        }
        if (kind == "minecraft:square") {
            return square(std::move(*input));
        }
        if (kind == "minecraft:cube") {
            return cube(std::move(*input));
        }
        if (kind == "minecraft:sqrt") {
            return sqrt(std::move(*input));
        }
        if (kind == "minecraft:half_negative") {
            return half_negative(std::move(*input));
        }
        if (kind == "minecraft:quarter_negative") {
            return quarter_negative(std::move(*input));
        }
        if (kind == "minecraft:reciprocal") {
            return reciprocal(std::move(*input));
        }
        if (kind == "minecraft:negate") {
            return negate(std::move(*input));
        }
        if (kind == "minecraft:squeeze") {
            return squeeze(std::move(*input));
        }
        if (kind == "minecraft:log") {
            return log(std::move(*input));
        }
        return sign(std::move(*input));
    }

    auto decode_clamp(const json &node) -> result<sampler_ptr> {
        auto input = child(node, "input");
        if (!input) {
            return input;
        }
        const auto min_value = double_member(node, "min");
        const auto max_value = double_member(node, "max");
        if (!min_value || !max_value) {
            return std::unexpected{fail("clamp needs min and max")};
        }
        return clamp(std::move(*input), static_cast<float>(*min_value),
                     static_cast<float>(*max_value));
    }

    auto decode_gradient(std::string_view kind, const json &node)
        -> result<sampler_ptr> {
        if (kind == "minecraft:y_clamped_gradient") {
            const auto from_y = int_member(node, "from_y");
            const auto to_y = int_member(node, "to_y");
            const auto from_value = double_member(node, "from_value");
            const auto to_value = double_member(node, "to_value");
            if (!from_y || !to_y || !from_value || !to_value) {
                return std::unexpected{
                    fail("y_clamped_gradient is incomplete")};
            }
            return gradient_y(*from_y, *to_y, static_cast<float>(*from_value),
                              static_cast<float>(*to_value));
        }
        const auto axis = string_member(node, "axis");
        if (!axis || *axis != "y") {
            return std::unexpected{fail("only Y gradients are supported")};
        }
        const auto from_coordinate = int_member(node, "from_coordinate");
        const auto to_coordinate = int_member(node, "to_coordinate");
        const auto from_value = double_member(node, "from_value");
        const auto to_value = double_member(node, "to_value");
        if (!from_coordinate || !to_coordinate || !from_value || !to_value) {
            return std::unexpected{fail("gradient is incomplete")};
        }
        return gradient_y(*from_coordinate, *to_coordinate,
                          static_cast<float>(*from_value),
                          static_cast<float>(*to_value));
    }

    auto make_noise_stack(std::string_view id) -> result<noise_stack> {
        const std::string key{id};
        const auto found =
            std::find_if(documents_.noises.begin(), documents_.noises.end(),
                         [&](const auto &entry) { return entry.first == key; });
        if (found == documents_.noises.end()) {
            return std::unexpected{fail("unknown noise reference")};
        }
        auto parsed = json::parse(found->second);
        if (!parsed) {
            return std::unexpected{parsed.error()};
        }
        normal_noise_parameters parameters;
        if (auto value = double_member(*parsed, "base_amplitude")) {
            parameters.base_amplitude = *value;
        }
        if (auto value = int_member(*parsed, "base_octave")) {
            parameters.base_octave = *value;
        }
        if (auto value = int_member(*parsed, "octave_count")) {
            parameters.octave_count = *value;
        }
        if (auto value = bool_member(*parsed, "normalize")) {
            parameters.normalize = *value;
        }
        if (const json *list = parsed->find("amplitude_modifiers");
            list != nullptr && list->is_array()) {
            for (const auto &entry : *list->as_array()) {
                parameters.amplitude_modifiers.push_back(*entry.as_double());
            }
        }
        const normal_noise normal{parameters};
        auto source = factory_->from_hash_of(key);
        return normal.create(*source);
    }

    auto decode_noise(const json &node) -> result<sampler_ptr> {
        const auto noise_id = string_member(node, "noise");
        const auto xz_scale = double_member(node, "xz_scale");
        const auto y_scale = double_member(node, "y_scale");
        if (!noise_id || !xz_scale || !y_scale) {
            return std::unexpected{fail("noise is incomplete")};
        }
        auto stack = make_noise_stack(*noise_id);
        if (!stack) {
            return std::unexpected{stack.error()};
        }
        auto shared = std::make_shared<noise_stack>(std::move(*stack));

        const json *shift_x = node.find("shift_x");
        const json *shift_y = node.find("shift_y");
        const json *shift_z = node.find("shift_z");
        const bool has_shift =
            (shift_x != nullptr && !is_zero_function(*shift_x)) ||
            (shift_y != nullptr && !is_zero_function(*shift_y)) ||
            (shift_z != nullptr && !is_zero_function(*shift_z));
        if (!has_shift) {
            return noise(std::move(shared), *xz_scale, *y_scale);
        }
        sampler_ptr sx = constant(0.0F);
        sampler_ptr sy = constant(0.0F);
        sampler_ptr sz = constant(0.0F);
        if (shift_x != nullptr) {
            auto decoded = decode_value(*shift_x);
            if (!decoded) {
                return decoded;
            }
            sx = std::move(*decoded);
        }
        const bool has_y = shift_y != nullptr;
        if (has_y) {
            auto decoded = decode_value(*shift_y);
            if (!decoded) {
                return decoded;
            }
            sy = std::move(*decoded);
        }
        if (shift_z != nullptr) {
            auto decoded = decode_value(*shift_z);
            if (!decoded) {
                return decoded;
            }
            sz = std::move(*decoded);
        }
        if (has_y) {
            return shifted_noise_xyz(std::move(sx), std::move(sy),
                                     std::move(sz), *xz_scale, *y_scale,
                                     std::move(shared));
        }
        return shifted_noise_xz(std::move(sx), std::move(sz), *xz_scale,
                                *y_scale, std::move(shared));
    }

    auto decode_shift(std::string_view kind, const json &node)
        -> result<sampler_ptr> {
        const auto noise_id = string_member(node, "noise");
        if (!noise_id) {
            return std::unexpected{fail("shift has no noise")};
        }
        auto stack = make_noise_stack(*noise_id);
        if (!stack) {
            return std::unexpected{stack.error()};
        }
        auto shared = std::make_shared<noise_stack>(std::move(*stack));
        if (kind == "minecraft:shift") {
            return shift(std::move(shared));
        }
        if (kind == "minecraft:shift_a") {
            return shift_a(std::move(shared));
        }
        return shift_b(std::move(shared));
    }

    auto decode_range_choice(const json &node) -> result<sampler_ptr> {
        auto input = child(node, "input");
        if (!input) {
            return input;
        }
        const auto min_value = double_member(node, "min_inclusive");
        const auto max_value = double_member(node, "max_exclusive");
        if (!min_value || !max_value) {
            return std::unexpected{fail("range_choice is incomplete")};
        }
        auto when_in = child(node, "when_in_range");
        if (!when_in) {
            return when_in;
        }
        auto when_out = child(node, "when_out_of_range");
        if (!when_out) {
            return when_out;
        }
        return range_choice(std::move(*input), static_cast<float>(*min_value),
                            static_cast<float>(*max_value), std::move(*when_in),
                            std::move(*when_out));
    }

    auto decode_interpolated(const json &node) -> result<sampler_ptr> {
        auto input = child(node, "input");
        if (!input) {
            return input;
        }
        const auto cell_xz = int_member(node, "cell_size_xz");
        const auto cell_y = int_member(node, "cell_size_y");
        if (!cell_xz || !cell_y) {
            return std::unexpected{fail("interpolated is incomplete")};
        }
        return interpolated(std::move(*input), *cell_xz, *cell_y);
    }

    auto decode_lerp(const json &node) -> result<sampler_ptr> {
        auto alpha = child(node, "alpha");
        if (!alpha) {
            return alpha;
        }
        auto first = child(node, "first");
        if (!first) {
            return first;
        }
        auto second = child(node, "second");
        if (!second) {
            return second;
        }
        return lerp(std::move(*alpha), std::move(*first), std::move(*second));
    }

    auto decode_spline_node(const json &node) -> result<sampler_ptr> {
        const json *found = node.find("spline");
        if (found == nullptr) {
            return std::unexpected{fail("spline node has no spline")};
        }
        auto value = decode_spline(*found);
        if (!value) {
            return std::unexpected{value.error()};
        }
        return spline_value(std::move(*value));
    }

    auto decode_spline(const json &node)
        -> result<std::shared_ptr<const spline>> {
        if (node.is_number()) {
            return spline::constant(static_cast<float>(*node.as_double()));
        }
        if (!node.is_object()) {
            return std::unexpected{fail("spline must be a number or object")};
        }
        const json *coordinate = node.find("coordinate");
        const json *points = node.find("points");
        if (coordinate == nullptr || points == nullptr || !points->is_array()) {
            return std::unexpected{fail("spline is incomplete")};
        }
        auto coordinate_sampler = decode_value(*coordinate);
        if (!coordinate_sampler) {
            return std::unexpected{coordinate_sampler.error()};
        }
        std::vector<spline_point> decoded_points;
        for (const auto &point : *points->as_array()) {
            const auto location = double_member(point, "location");
            const auto derivative = double_member(point, "derivative");
            const json *point_value = point.find("value");
            if (!location || !derivative || point_value == nullptr) {
                return std::unexpected{fail("spline point is incomplete")};
            }
            auto value = decode_spline(*point_value);
            if (!value) {
                return std::unexpected{value.error()};
            }
            decoded_points.push_back(spline_point{
                .location = static_cast<float>(*location),
                .derivative = static_cast<float>(*derivative),
                .value = std::move(*value),
            });
        }
        return spline::multipoint(
            std::shared_ptr<sampler>(std::move(*coordinate_sampler)),
            std::move(decoded_points));
    }

    auto decode_old_blended(const json &node) -> result<sampler_ptr> {
        const auto xz_scale = double_member(node, "xz_scale");
        const auto y_scale = double_member(node, "y_scale");
        const auto xz_factor = double_member(node, "xz_factor");
        const auto y_factor = double_member(node, "y_factor");
        const auto smear = double_member(node, "smear_scale_multiplier");
        if (!xz_scale || !y_scale || !xz_factor || !y_factor || !smear) {
            return std::unexpected{fail("old_blended_noise is incomplete")};
        }
        blended_noise_parameters parameters;
        parameters.xz_scale = *xz_scale;
        parameters.y_scale = *y_scale;
        parameters.xz_factor = *xz_factor;
        parameters.y_factor = *y_factor;
        parameters.smear_scale_multiplier = *smear;
        auto source = factory_->from_hash_of("minecraft:terrain");
        return blended_noise(parameters, *source);
    }

    auto decode_find_top_surface(const json &node) -> result<sampler_ptr> {
        auto density_input = child(node, "density");
        if (!density_input) {
            return density_input;
        }
        auto upper_bound = child(node, "upper_bound");
        if (!upper_bound) {
            return upper_bound;
        }
        const auto lower_bound = int_member(node, "lower_bound");
        const auto cell_height = int_member(node, "cell_height");
        if (!lower_bound || !cell_height) {
            return std::unexpected{fail("find_top_surface is incomplete")};
        }
        return find_top_surface(std::move(*density_input),
                                std::move(*upper_bound), *lower_bound,
                                *cell_height);
    }

    [[nodiscard]] static auto is_zero_function(const json &node) -> bool {
        if (node.is_number()) {
            return *node.as_double() == 0.0;
        }
        if (!node.is_object()) {
            return false;
        }
        const auto type = string_member(node, "type");
        if (!type || *type != "minecraft:constant") {
            return false;
        }
        const auto value = double_member(node, "value");
        return value && *value == 0.0;
    }

    const density_documents &documents_;
    std::unique_ptr<positional_random_factory> factory_;
    std::unordered_set<std::string> active_;
};

} // namespace

auto decode_density(std::string_view text, const density_documents &documents)
    -> result<sampler_ptr> {
    decoder decode{documents};
    return decode.decode(text);
}

} // namespace refrain::worldgen::density
