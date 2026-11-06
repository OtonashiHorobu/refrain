// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/noise.hpp>

#include <refrain/worldgen/mth.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace refrain::worldgen {

namespace {

struct gradient {
    int x;
    int y;
    int z;
};

// Java `GradientNoise.GRADIENT`: 16 entries, including the four duplicates at
// positions 12..15.
constexpr std::array<gradient, 16> gradients{{
    {1, 1, 0},
    {-1, 1, 0},
    {1, -1, 0},
    {-1, -1, 0},
    {1, 0, 1},
    {-1, 0, 1},
    {1, 0, -1},
    {-1, 0, -1},
    {0, 1, 1},
    {0, -1, 1},
    {0, 1, -1},
    {0, -1, -1},
    {1, 1, 0},
    {0, -1, 1},
    {-1, 1, 0},
    {0, -1, -1},
}};

constexpr double wrap_period = 33554432.0; // Java 3.3554432E7
constexpr double half_wrap = 16777216.0;   // Java 1.6777216E7

/// Java `GradientNoise.wrap`: keeps coordinates inside the stable range so the
/// integer floor does not overflow far from spawn. Farländs are never enabled.
auto wrap(double value) -> double {
    static const double half_round_off = std::nextafter(half_wrap, 0.0);
    if (value >= -half_round_off && value < half_round_off) {
        return value;
    }
    return value - std::floor(value / wrap_period + 0.5) * wrap_period;
}

auto permute(const std::array<std::uint8_t, 256> &perms, jint value) -> jint {
    return static_cast<jint>(perms[static_cast<std::size_t>(value) & 0xFFU]);
}

auto grad_dot(jint hash, float x, float y, float z) -> float {
    const auto &g = gradients[static_cast<std::size_t>(hash) & 15U];
    return static_cast<float>(g.x) * x + static_cast<float>(g.y) * y +
           static_cast<float>(g.z) * z;
}

} // namespace

perlin_noise::perlin_noise(random_source &random) {
    offset_x_ = random.next_double() * 256.0;
    offset_y_ = random.next_double() * 256.0;
    offset_z_ = random.next_double() * 256.0;

    for (std::size_t i = 0; i < perms_.size(); ++i) {
        perms_[i] = static_cast<std::uint8_t>(i);
    }
    for (jint i = 0; i < 256; ++i) {
        const jint offset = random.next_int(256 - i);
        std::swap(perms_[static_cast<std::size_t>(i)],
                  perms_[static_cast<std::size_t>(offset + i)]);
    }
}

auto perlin_noise::get(double x, double y) const -> float {
    return get(wrap(x), 0.0, wrap(y));
}

auto perlin_noise::get(double x, double y, double z) const -> float {
    const double wrapped_x = wrap(x) + offset_x_;
    const double wrapped_y = wrap(y) + offset_y_;
    const double wrapped_z = wrap(z) + offset_z_;

    const jint floor_x = mth::floor(wrapped_x);
    const jint floor_y = mth::floor(wrapped_y);
    const jint floor_z = mth::floor(wrapped_z);

    const float relative_x = static_cast<float>(wrapped_x - floor_x);
    const float relative_y = static_cast<float>(wrapped_y - floor_y);
    const float relative_z = static_cast<float>(wrapped_z - floor_z);

    return sample_and_lerp(floor_x, floor_y, floor_z, relative_x, relative_y,
                           relative_z, relative_y);
}

auto perlin_noise::sample_and_lerp(jint x, jint y, jint z, float relative_x,
                                   float relative_y, float relative_z,
                                   float original_relative_y) const -> float {
    const jint x0 = permute(perms_, x);
    const jint x1 = permute(perms_, x + 1);
    const jint xy00 = permute(perms_, x0 + y);
    const jint xy01 = permute(perms_, x0 + y + 1);
    const jint xy10 = permute(perms_, x1 + y);
    const jint xy11 = permute(perms_, x1 + y + 1);

    const float d000 =
        grad_dot(permute(perms_, xy00 + z), relative_x, relative_y, relative_z);
    const float d100 = grad_dot(permute(perms_, xy10 + z), relative_x - 1.0F,
                                relative_y, relative_z);
    const float d010 = grad_dot(permute(perms_, xy01 + z), relative_x,
                                relative_y - 1.0F, relative_z);
    const float d110 = grad_dot(permute(perms_, xy11 + z), relative_x - 1.0F,
                                relative_y - 1.0F, relative_z);
    const float d001 = grad_dot(permute(perms_, xy00 + z + 1), relative_x,
                                relative_y, relative_z - 1.0F);
    const float d101 =
        grad_dot(permute(perms_, xy10 + z + 1), relative_x - 1.0F, relative_y,
                 relative_z - 1.0F);
    const float d011 = grad_dot(permute(perms_, xy01 + z + 1), relative_x,
                                relative_y - 1.0F, relative_z - 1.0F);
    const float d111 =
        grad_dot(permute(perms_, xy11 + z + 1), relative_x - 1.0F,
                 relative_y - 1.0F, relative_z - 1.0F);

    const float x_alpha = mth::smoothstep(relative_x);
    const float y_alpha = mth::smoothstep(original_relative_y);
    const float z_alpha = mth::smoothstep(relative_z);

    return mth::lerp3(x_alpha, y_alpha, z_alpha, d000, d100, d010, d110, d001,
                      d101, d011, d111);
}

auto noise_stack::get(double x, double y, double z) const -> float {
    float value = 0.0F;
    for (const auto &l : layers_) {
        const double frequency = l.frequency;
        value += l.amplitude *
                 l.noise.get(x * frequency, y * frequency, z * frequency);
    }
    return value;
}

auto noise_stack::get(double x, double y) const -> float {
    float value = 0.0F;
    for (const auto &l : layers_) {
        const double frequency = l.frequency;
        value += l.amplitude * l.noise.get(x * frequency, y * frequency);
    }
    return value;
}

normal_noise::normal_noise(normal_noise_parameters parameters)
    : parameters_{std::move(parameters)} {
    octaves_ =
        build_octaves(parameters_.base_octave, parameters_.base_amplitude,
                      parameters_.octave_count, parameters_.normalize,
                      parameters_.amplitude_modifiers);

    double target_amplitude = 0.0;
    for (const auto &octave : octaves_) {
        target_amplitude += std::abs(octave.amplitude);
    }
    normalization_factor_ =
        compute_normalization_factor(target_amplitude, octaves_);
    range_bound_ =
        static_cast<float>(target_amplitude * 0.3333333333333333 * 6.0);
}

auto normal_noise::create_parity(jint first_octave,
                                 std::span<const double> amplitudes)
    -> normal_noise {
    normal_noise_parameters parameters;
    parameters.base_octave = first_octave;
    parameters.octave_count = static_cast<jint>(amplitudes.size());
    parameters.base_amplitude =
        compute_parity_base_amplitude(first_octave, amplitudes);
    parameters.amplitude_modifiers.assign(amplitudes.begin(), amplitudes.end());
    return normal_noise{std::move(parameters)};
}

auto normal_noise::create(random_source &random) const -> noise_stack {
    auto first_random = random.fork_positional();
    auto second_random = random.fork_positional();

    std::vector<noise_stack::layer> layers;
    layers.reserve(octaves_.size() * 2);
    for (const auto &octave : octaves_) {
        const std::string seed = "octave_" + std::to_string(octave.index);
        auto first_source = first_random->from_hash_of(seed);
        auto second_source = second_random->from_hash_of(seed);
        const auto value_factor =
            static_cast<float>(normalization_factor_ * octave.amplitude);

        layers.push_back(noise_stack::layer{
            .noise = perlin_noise{*first_source},
            .frequency = octave.frequency,
            .amplitude = value_factor,
        });
        layers.push_back(noise_stack::layer{
            .noise = perlin_noise{*second_source},
            .frequency = octave.frequency * 1.0181268882175227,
            .amplitude = value_factor,
        });
    }
    return noise_stack{std::move(layers)};
}

auto normal_noise::build_octaves(jint base_octave, double base_amplitude,
                                 jint octave_count, bool normalize,
                                 const std::vector<double> &amplitude_modifiers)
    -> std::vector<normal_noise::octave_info> {
    double frequency = std::pow(2.0, static_cast<double>(base_octave));
    double amplitude = base_amplitude;
    if (normalize) {
        amplitude = base_amplitude *
                    (std::pow(0.5, -static_cast<double>(octave_count - 1)) /
                     (std::pow(0.5, -static_cast<double>(octave_count)) - 1.0));
    }

    std::vector<octave_info> octaves;
    octaves.reserve(static_cast<std::size_t>(octave_count));
    for (jint i = 0; i < octave_count; ++i) {
        const double modifier = amplitude_modifier(amplitude_modifiers, i);
        if (modifier != 0.0) {
            octaves.push_back(octave_info{
                .index = base_octave + i,
                .frequency = frequency,
                .amplitude = amplitude * modifier,
            });
        }
        frequency *= 2.0;
        amplitude *= 0.5;
    }
    return octaves;
}

auto normal_noise::amplitude_modifier(
    const std::vector<double> &amplitude_modifiers, jint index) -> double {
    return amplitude_modifiers.empty()
               ? 1.0
               : amplitude_modifiers[static_cast<std::size_t>(index)];
}

auto normal_noise::estimate_deviation(const std::vector<octave_info> &octaves)
    -> double {
    double variance = 0.0;
    for (const auto &octave : octaves) {
        const double layer_deviation =
            0.2702247831245211 * std::abs(octave.amplitude);
        variance += mth::square(layer_deviation);
    }
    return std::sqrt(variance);
}

auto normal_noise::compute_normalization_factor(
    double target_amplitude, const std::vector<octave_info> &octaves)
    -> double {
    const double input_deviation = estimate_deviation(octaves);
    if (input_deviation == 0.0) {
        return 0.0;
    }
    const double input_sum_deviation = input_deviation * std::sqrt(2.0);
    const double target_deviation = target_amplitude * 0.3333333333333333;
    return target_deviation / input_sum_deviation;
}

auto normal_noise::compute_parity_base_amplitude(
    jint base_octave, std::span<const double> amplitudes) -> double {
    const std::vector<double> modifiers(amplitudes.begin(), amplitudes.end());
    const auto octaves =
        build_octaves(base_octave, 1.0, static_cast<jint>(amplitudes.size()),
                      true, modifiers);
    double target_amplitude = 0.0;
    for (const auto &octave : octaves) {
        target_amplitude += std::abs(octave.amplitude);
    }
    const double new_normalization_factor =
        compute_normalization_factor(target_amplitude, octaves);
    if (new_normalization_factor == 0.0) {
        return 1.0;
    }
    const double old_normalization_factor = compute_parity_normalization_factor(
        1.0, static_cast<jint>(amplitudes.size()), modifiers);
    return old_normalization_factor / new_normalization_factor;
}

auto normal_noise::compute_parity_normalization_factor(
    double base_amplitude, jint octave_count,
    const std::vector<double> &amplitude_modifiers) -> double {
    jint min_octave = std::numeric_limits<jint>::max();
    jint max_octave = std::numeric_limits<jint>::min();
    for (jint i = 0; i < octave_count; ++i) {
        const double modifier = amplitude_modifier(amplitude_modifiers, i);
        if (modifier != 0.0) {
            min_octave = std::min(min_octave, i);
            max_octave = std::max(max_octave, i);
        }
    }
    return base_amplitude * 0.5 * 0.3333333333333333 /
           parity_expected_deviation(max_octave - min_octave);
}

auto normal_noise::parity_expected_deviation(jint octave_span) -> double {
    return 0.1 * (1.0 + 1.0 / static_cast<double>(octave_span + 1));
}

} // namespace refrain::worldgen
