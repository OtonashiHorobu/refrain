// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/carver/carver.hpp>

#include <refrain/data/json.hpp>
#include <refrain/worldgen/core/mth.hpp>

#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>

namespace refrain::worldgen::carver {

namespace {

constexpr double pi = 3.14159265358979323846;

auto number(const data::json::value &input) -> result<float> {
    if (input.is_integer()) {
        const auto value = input.as_int64();
        if (!value) {
            return std::unexpected{value.error()};
        }
        return static_cast<float>(*value);
    }
    const auto value = input.as_double();
    if (!value) {
        return std::unexpected{value.error()};
    }
    return static_cast<float>(*value);
}

auto integer(const data::json::value &input) -> result<jint> {
    const auto value = input.as_int64();
    if (!value) {
        return std::unexpected{value.error()};
    }
    return static_cast<jint>(*value);
}

auto field(const data::json::value &input, std::string_view name)
    -> result<const data::json::value *> {
    const auto *found = input.find(name);
    if (found == nullptr) {
        return std::unexpected{
            make_error(error_code::parse_error, "carver field is missing")};
    }
    return found;
}

auto type_of(const data::json::value &input) -> result<std::string_view> {
    const auto *type = input.find("type");
    if (type == nullptr) {
        return std::unexpected{
            make_error(error_code::parse_error, "carver has no type")};
    }
    const auto text = type->as_string();
    if (!text) {
        return std::unexpected{
            make_error(error_code::parse_error, "carver type is not a string")};
    }
    return *text;
}

} // namespace

void carve_ellipsoid(const chunk_pos &chunk_pos, double x, double y, double z,
                     double horizontal_radius, double vertical_radius,
                     carver_output &output, const skip_checker &skip) {
    const double center_x = chunk_pos.x * 16 + 8;
    const double center_z = chunk_pos.z * 16 + 8;
    const double max_delta = 16.0 + horizontal_radius * 2.0;
    if (std::abs(x - center_x) > max_delta ||
        std::abs(z - center_z) > max_delta) {
        return;
    }

    const jint chunk_min_x = chunk_pos.x * 16;
    const jint chunk_min_z = chunk_pos.z * 16;
    const jint min_x =
        std::max(mth::floor(x - horizontal_radius) - chunk_min_x - 1, 0);
    const jint max_x =
        std::min(mth::floor(x + horizontal_radius) - chunk_min_x, 15);
    const jint min_z =
        std::max(mth::floor(z - horizontal_radius) - chunk_min_z - 1, 0);
    const jint max_z =
        std::min(mth::floor(z + horizontal_radius) - chunk_min_z, 15);
    const jint min_y =
        std::max(mth::floor(y - vertical_radius) - 1, output.min_y());
    const jint max_y =
        std::min(mth::floor(y + vertical_radius) + 1, output.max_y());

    for (jint x_index = min_x; x_index <= max_x; ++x_index) {
        const jint world_x = chunk_min_x + x_index;
        const double xd = (world_x + 0.5 - x) / horizontal_radius;
        for (jint z_index = min_z; z_index <= max_z; ++z_index) {
            const jint world_z = chunk_min_z + z_index;
            const double zd = (world_z + 0.5 - z) / horizontal_radius;
            if (xd * xd + zd * zd >= 1.0) {
                continue;
            }
            for (jint world_y = max_y; world_y > min_y; --world_y) {
                const double yd = (world_y - 0.5 - y) / vertical_radius;
                if (!skip(xd, yd, zd, world_y)) {
                    output.carve(x_index, world_y, z_index);
                }
            }
        }
    }
}

auto can_reach(const chunk_pos &chunk_pos, double x, double z,
               jint current_step, jint total_steps, float thickness) -> bool {
    const double center_x = chunk_pos.x * 16 + 8;
    const double center_z = chunk_pos.z * 16 + 8;
    const double remaining = total_steps - current_step;
    const double radius = thickness + 2.0F + 16.0F;
    const double xd = x - center_x;
    const double zd = z - center_z;
    return xd * xd + zd * zd - remaining * remaining <= radius * radius;
}

cave_carver::cave_carver(
    float probability, std::unique_ptr<provider::height_provider> y,
    std::unique_ptr<provider::int_provider> count,
    std::unique_ptr<provider::float_provider> thickness,
    bool weird_thickness_bias,
    std::unique_ptr<provider::float_provider> room_vertical_radius_multiplier,
    std::unique_ptr<provider::float_provider> horizontal_radius_multiplier,
    std::unique_ptr<provider::float_provider> vertical_radius_multiplier,
    std::unique_ptr<provider::float_provider> start_vertical_radius_multiplier,
    std::unique_ptr<provider::float_provider> floor_level)
    : probability_{probability}, y_{std::move(y)}, count_{std::move(count)},
      thickness_{std::move(thickness)},
      weird_thickness_bias_{weird_thickness_bias},
      room_vertical_radius_multiplier_{
          std::move(room_vertical_radius_multiplier)},
      horizontal_radius_multiplier_{std::move(horizontal_radius_multiplier)},
      vertical_radius_multiplier_{std::move(vertical_radius_multiplier)},
      start_vertical_radius_multiplier_{
          std::move(start_vertical_radius_multiplier)},
      floor_level_{std::move(floor_level)} {}

auto cave_carver::is_start_chunk(worldgen_random &random) const -> bool {
    return random.next_float() <= probability_;
}

auto cave_carver::get_thickness(worldgen_random &random) const -> float {
    float thickness = thickness_->sample(random);
    if (weird_thickness_bias_ && random.next_int(10) == 0) {
        thickness *= random.next_float() * random.next_float() * 3.0F + 1.0F;
    }
    return thickness;
}

auto cave_carver::should_skip(double xd, double yd, double zd,
                              double floor_level) -> bool {
    return yd <= floor_level || xd * xd + yd * yd + zd * zd >= 1.0;
}

auto cave_carver::carve(const provider::generation_context &context,
                        worldgen_random &random, const chunk_pos &chunk_pos,
                        const ::refrain::worldgen::chunk_pos &source_pos,
                        carver_output &output) const -> bool {
    const jint max_distance = 16 * (range() * 2 - 1);
    const jint cave_count = count_->sample(random);

    for (jint cave = 0; cave < cave_count; ++cave) {
        const double x = source_pos.x * 16 + random.next_int(16);
        const double y = y_->sample(random, context);
        const double z = source_pos.z * 16 + random.next_int(16);
        const double horizontal_radius_multiplier =
            horizontal_radius_multiplier_->sample(random);
        const double vertical_radius_multiplier =
            vertical_radius_multiplier_->sample(random);
        const double start_vertical_radius_multiplier =
            start_vertical_radius_multiplier_->sample(random);
        const double floor_level = floor_level_->sample(random);
        const skip_checker skip = [floor_level](double xd, double yd, double zd,
                                                jint) {
            return should_skip(xd, yd, zd, floor_level);
        };

        jint tunnels = 1;
        if (random.next_int(4) == 0) {
            const double y_scale =
                room_vertical_radius_multiplier_->sample(random);
            const float room_thickness = 1.0F + random.next_float() * 6.0F;
            create_room(chunk_pos, x, y, z, room_thickness, y_scale, output,
                        skip);
            tunnels += random.next_int(4);
        }

        for (jint tunnel = 0; tunnel < tunnels; ++tunnel) {
            const float horizontal_rotation =
                random.next_float() * static_cast<float>(pi * 2.0);
            const float vertical_rotation = (random.next_float() - 0.5F) / 4.0F;
            const float thickness = get_thickness(random);
            const jint distance =
                max_distance - random.next_int(max_distance / 4);
            create_tunnel(chunk_pos, random.next_long(), x, y, z,
                          horizontal_radius_multiplier,
                          vertical_radius_multiplier, thickness,
                          horizontal_rotation, vertical_rotation, 0, distance,
                          start_vertical_radius_multiplier, output, skip);
        }
    }
    return true;
}

void cave_carver::create_room(const chunk_pos &chunk_pos, double x, double y,
                              double z, float thickness, double y_scale,
                              carver_output &output,
                              const skip_checker &skip) const {
    const float half_pi = static_cast<float>(pi / 2.0);
    const double horizontal_radius = 1.5 + mth::sin(half_pi) * thickness;
    const double vertical_radius = horizontal_radius * y_scale;
    carve_ellipsoid(chunk_pos, x + 1.0, y, z, horizontal_radius,
                    vertical_radius, output, skip);
}

void cave_carver::create_tunnel(
    const chunk_pos &chunk_pos, jlong tunnel_seed, double x, double y, double z,
    double horizontal_radius_multiplier, double vertical_radius_multiplier,
    float thickness, float horizontal_rotation, float vertical_rotation,
    jint step, jint distance, double y_scale, carver_output &output,
    const skip_checker &skip) const {
    worldgen_random random{std::make_unique<legacy_random_source>(tunnel_seed)};
    const jint split_point = random.next_int(distance / 2) + distance / 4;
    const bool steep = random.next_int(6) == 0;
    float y_rota = 0.0F;
    float x_rota = 0.0F;

    for (jint current_step = step; current_step < distance; ++current_step) {
        const float angle = static_cast<float>(pi) *
                            static_cast<float>(current_step) /
                            static_cast<float>(distance);
        const double horizontal_radius = 1.5 + mth::sin(angle) * thickness;
        const double vertical_radius = horizontal_radius * y_scale;
        const float cos_x = mth::cos(vertical_rotation);
        x += static_cast<double>(mth::cos(horizontal_rotation) * cos_x);
        y += static_cast<double>(mth::sin(vertical_rotation));
        z += static_cast<double>(mth::sin(horizontal_rotation) * cos_x);
        vertical_rotation *= steep ? 0.92F : 0.7F;
        vertical_rotation += x_rota * 0.1F;
        horizontal_rotation += y_rota * 0.1F;
        x_rota *= 0.9F;
        y_rota *= 0.75F;
        x_rota += (random.next_float() - random.next_float()) *
                  random.next_float() * 2.0F;
        y_rota += (random.next_float() - random.next_float()) *
                  random.next_float() * 4.0F;

        if (current_step == split_point && thickness > 1.0F) {
            const jlong first_seed = random.next_long();
            const float first_thickness = random.next_float() * 0.5F + 0.5F;
            create_tunnel(chunk_pos, first_seed, x, y, z,
                          horizontal_radius_multiplier,
                          vertical_radius_multiplier, first_thickness,
                          horizontal_rotation - static_cast<float>(pi / 2.0),
                          vertical_rotation / 3.0F, current_step, distance, 1.0,
                          output, skip);
            const jlong second_seed = random.next_long();
            const float second_thickness = random.next_float() * 0.5F + 0.5F;
            create_tunnel(chunk_pos, second_seed, x, y, z,
                          horizontal_radius_multiplier,
                          vertical_radius_multiplier, second_thickness,
                          horizontal_rotation + static_cast<float>(pi / 2.0),
                          vertical_rotation / 3.0F, current_step, distance, 1.0,
                          output, skip);
            return;
        }

        if (random.next_int(4) != 0) {
            if (!can_reach(chunk_pos, x, z, current_step, distance,
                           thickness)) {
                return;
            }
            carve_ellipsoid(chunk_pos, x, y, z,
                            horizontal_radius * horizontal_radius_multiplier,
                            vertical_radius * vertical_radius_multiplier,
                            output, skip);
        }
    }
}

canyon_carver::canyon_carver(
    float probability, std::unique_ptr<provider::height_provider> y,
    std::unique_ptr<provider::float_provider> vertical_rotation,
    canyon_shape shape)
    : probability_{probability}, y_{std::move(y)},
      vertical_rotation_{std::move(vertical_rotation)},
      shape_{std::move(shape)} {}

auto canyon_carver::is_start_chunk(worldgen_random &random) const -> bool {
    return random.next_float() <= probability_;
}

auto canyon_carver::update_vertical_radius(worldgen_random &random,
                                           double vertical_radius,
                                           float distance, float current_step,
                                           const canyon_shape &shape)
    -> double {
    const float vertical_multiplier =
        1.0F - std::abs(0.5F - current_step / distance) * 2.0F;
    const float factor =
        shape.vertical_radius_default_factor +
        shape.vertical_radius_center_factor * vertical_multiplier;
    const float between = random.next_float() * (1.0F - 0.75F) + 0.75F;
    return factor * vertical_radius * between;
}

auto canyon_carver::init_width_factors(
    const provider::generation_context &context, worldgen_random &random) const
    -> std::vector<float> {
    const jint depth = context.height;
    std::vector<float> factors(static_cast<std::size_t>(depth), 1.0F);
    float width_factor = 1.0F;
    for (jint y_index = 0; y_index < depth; ++y_index) {
        if (y_index == 0 || random.next_int(shape_.width_smoothness) == 0) {
            width_factor = 1.0F + random.next_float() * random.next_float();
        }
        factors[static_cast<std::size_t>(y_index)] =
            width_factor * width_factor;
    }
    return factors;
}

auto canyon_carver::should_skip(
    const provider::generation_context &context,
    const std::vector<float> &width_factor_per_height, double xd, double yd,
    double zd, jint y) -> bool {
    const jint y_index = y - context.min_y;
    if (y_index <= 0 ||
        y_index > static_cast<jint>(width_factor_per_height.size())) {
        return true;
    }
    return (xd * xd + zd * zd) *
                   static_cast<double>(
                       width_factor_per_height[static_cast<std::size_t>(
                           y_index - 1)]) +
               yd * yd / 6.0 >=
           1.0;
}

auto canyon_carver::carve(const provider::generation_context &context,
                          worldgen_random &random, const chunk_pos &chunk_pos,
                          const ::refrain::worldgen::chunk_pos &source_pos,
                          carver_output &output) const -> bool {
    const jint max_distance = (range() * 2 - 1) * 16;
    const double x = source_pos.x * 16 + random.next_int(16);
    const double y = y_->sample(random, context);
    const double z = source_pos.z * 16 + random.next_int(16);
    const float horizontal_rotation =
        random.next_float() * static_cast<float>(pi * 2.0);
    const float vertical_rotation = vertical_rotation_->sample(random);
    const double y_scale = shape_.y_scale->sample(random);
    const float thickness = shape_.thickness->sample(random);
    const jint distance =
        static_cast<jint>(static_cast<float>(max_distance) *
                          shape_.distance_factor->sample(random));
    do_carve(context, chunk_pos, random.next_long(), x, y, z, thickness,
             horizontal_rotation, vertical_rotation, 0, distance, y_scale,
             output);
    return true;
}

void canyon_carver::do_carve(const provider::generation_context &context,
                             const chunk_pos &chunk_pos, jlong tunnel_seed,
                             double x, double y, double z, float thickness,
                             float horizontal_rotation, float vertical_rotation,
                             jint step, jint distance, double y_scale,
                             carver_output &output) const {
    worldgen_random random{std::make_unique<legacy_random_source>(tunnel_seed)};
    const auto width_factors = init_width_factors(context, random);
    float y_rota = 0.0F;
    float x_rota = 0.0F;

    for (jint current_step = step; current_step < distance; ++current_step) {
        const float angle = static_cast<float>(current_step) *
                            static_cast<float>(pi) /
                            static_cast<float>(distance);
        double horizontal_radius = 1.5 + mth::sin(angle) * thickness;
        double vertical_radius = horizontal_radius * y_scale;
        horizontal_radius *= static_cast<double>(
            shape_.horizontal_radius_factor->sample(random));
        vertical_radius = update_vertical_radius(
            random, vertical_radius, static_cast<float>(distance),
            static_cast<float>(current_step), shape_);
        const float cos_x = mth::cos(vertical_rotation);
        const float sin_x = mth::sin(vertical_rotation);
        x += static_cast<double>(mth::cos(horizontal_rotation) * cos_x);
        y += static_cast<double>(sin_x);
        z += static_cast<double>(mth::sin(horizontal_rotation) * cos_x);
        vertical_rotation *= 0.7F;
        vertical_rotation += x_rota * 0.05F;
        horizontal_rotation += y_rota * 0.05F;
        x_rota *= 0.8F;
        y_rota *= 0.5F;
        x_rota += (random.next_float() - random.next_float()) *
                  random.next_float() * 2.0F;
        y_rota += (random.next_float() - random.next_float()) *
                  random.next_float() * 4.0F;

        if (random.next_int(4) != 0) {
            if (!can_reach(chunk_pos, x, z, current_step, distance,
                           thickness)) {
                return;
            }
            carve_ellipsoid(chunk_pos, x, y, z, horizontal_radius,
                            vertical_radius, output,
                            [&context, &width_factors](
                                double xd, double yd, double zd, jint block_y) {
                                return should_skip(context, width_factors, xd,
                                                   yd, zd, block_y);
                            });
        }
    }
}

auto decode_carver(const data::json::value &document)
    -> result<std::unique_ptr<world_carver>> {
    const auto type = type_of(document);
    if (!type) {
        return std::unexpected{type.error()};
    }

    auto float_field = [&document](std::string_view name) -> result<float> {
        const auto found = field(document, name);
        if (!found) {
            return std::unexpected{found.error()};
        }
        return number(**found);
    };
    auto provider_field = [&document](std::string_view name)
        -> result<const data::json::value *> { return field(document, name); };

    if (*type == "minecraft:cave") {
        const auto probability = float_field("probability");
        const auto y_field = provider_field("y");
        const auto count_field = provider_field("count");
        const auto thickness_field = provider_field("thickness");
        const auto room_field =
            provider_field("room_vertical_radius_multiplier");
        const auto horizontal_field =
            provider_field("horizontal_radius_multiplier");
        const auto vertical_field =
            provider_field("vertical_radius_multiplier");
        const auto floor_field = provider_field("floor_level");
        if (!probability || !y_field || !count_field || !thickness_field ||
            !room_field || !horizontal_field || !vertical_field ||
            !floor_field) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "cave carver is incomplete")};
        }
        auto y = provider::height_provider::decode(**y_field);
        auto count = provider::int_provider::decode(**count_field);
        auto thickness = provider::float_provider::decode(**thickness_field);
        auto room = provider::float_provider::decode(**room_field);
        auto horizontal = provider::float_provider::decode(**horizontal_field);
        auto vertical = provider::float_provider::decode(**vertical_field);
        auto floor = provider::float_provider::decode(**floor_field);
        if (!y || !count || !thickness || !room || !horizontal || !vertical ||
            !floor) {
            return std::unexpected{make_error(
                error_code::parse_error, "cave carver provider is invalid")};
        }
        bool weird = false;
        if (const auto *value = document.find("weird_thickness_bias");
            value != nullptr) {
            const auto parsed = value->as_bool();
            if (!parsed) {
                return std::unexpected{parsed.error()};
            }
            weird = *parsed;
        }
        std::unique_ptr<provider::float_provider> start =
            provider::make_constant_float(1.0F);
        if (const auto *value =
                document.find("start_vertical_radius_multiplier");
            value != nullptr) {
            auto parsed = provider::float_provider::decode(*value);
            if (!parsed) {
                return std::unexpected{parsed.error()};
            }
            start = std::move(*parsed);
        }
        return std::unique_ptr<world_carver>{std::make_unique<cave_carver>(
            *probability, std::move(*y), std::move(*count),
            std::move(*thickness), weird, std::move(*room),
            std::move(*horizontal), std::move(*vertical), std::move(start),
            std::move(*floor))};
    }

    if (*type == "minecraft:canyon") {
        const auto probability = float_field("probability");
        const auto y_field = provider_field("y");
        const auto rotation_field = provider_field("vertical_rotation");
        const auto shape_field = provider_field("shape");
        if (!probability || !y_field || !rotation_field || !shape_field ||
            !(*shape_field)->is_object()) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "canyon carver is incomplete")};
        }
        auto y = provider::height_provider::decode(**y_field);
        auto rotation = provider::float_provider::decode(**rotation_field);
        if (!y || !rotation) {
            return std::unexpected{make_error(
                error_code::parse_error, "canyon carver provider is invalid")};
        }
        const auto &shape_json = **shape_field;
        auto shape_number =
            [&shape_json](std::string_view name) -> result<float> {
            const auto found = field(shape_json, name);
            if (!found) {
                return std::unexpected{found.error()};
            }
            return number(**found);
        };
        auto shape_int = [&shape_json](std::string_view name) -> result<jint> {
            const auto found = field(shape_json, name);
            if (!found) {
                return std::unexpected{found.error()};
            }
            return integer(**found);
        };
        auto shape_provider =
            [&shape_json](
                std::string_view name) -> result<const data::json::value *> {
            return field(shape_json, name);
        };
        const auto distance_factor = shape_provider("distance_factor");
        const auto thickness = shape_provider("thickness");
        const auto width_smoothness = shape_int("width_smoothness");
        const auto horizontal_factor =
            shape_provider("horizontal_radius_factor");
        const auto default_factor =
            shape_number("vertical_radius_default_factor");
        const auto center_factor =
            shape_number("vertical_radius_center_factor");
        const auto y_scale = shape_provider("y_scale");
        if (!distance_factor || !thickness || !width_smoothness ||
            !horizontal_factor || !default_factor || !center_factor ||
            !y_scale) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "canyon shape is incomplete")};
        }
        auto distance = provider::float_provider::decode(**distance_factor);
        auto shape_thickness = provider::float_provider::decode(**thickness);
        auto horizontal = provider::float_provider::decode(**horizontal_factor);
        auto scale = provider::float_provider::decode(**y_scale);
        if (!distance || !shape_thickness || !horizontal || !scale) {
            return std::unexpected{make_error(
                error_code::parse_error, "canyon shape provider is invalid")};
        }
        canyon_shape shape;
        shape.distance_factor = std::move(*distance);
        shape.thickness = std::move(*shape_thickness);
        shape.width_smoothness = *width_smoothness;
        shape.horizontal_radius_factor = std::move(*horizontal);
        shape.vertical_radius_default_factor = *default_factor;
        shape.vertical_radius_center_factor = *center_factor;
        shape.y_scale = std::move(*scale);
        return std::unique_ptr<world_carver>{std::make_unique<canyon_carver>(
            *probability, std::move(*y), std::move(*rotation),
            std::move(shape))};
    }

    return std::unexpected{
        make_error(error_code::unsupported, "unsupported carver type")};
}

} // namespace refrain::worldgen::carver
