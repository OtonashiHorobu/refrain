// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/noise/noise.hpp>

#include <refrain/worldgen/core/mth.hpp>

#include "noise_detail.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

namespace refrain::worldgen {

namespace {

using noise_detail::float_gradients;
using noise_detail::gradients;
using noise_detail::make_smear_scale;
using noise_detail::permute;
using noise_detail::smeared_fudge_y;
using noise_detail::wrap;

auto grad_dot(jint hash, float x, float y, float z) -> float {
    const auto &g = float_gradients[static_cast<std::size_t>(hash) & 15U];
    return g.x * x + g.y * y + g.z * z;
}

/// One sampled coordinate of one stack. A stack is immutable, so the value of
/// a coordinate is fixed for the lifetime of the instance and the instance id
/// makes the entry safe when an object is destroyed and another is placed at
/// the same address.
struct cached_sample {
    std::uint64_t stack{0};
    std::uint64_t x{0};
    std::uint64_t y{0};
    std::uint64_t z{0};
    float value{0.0F};
};

/// Direct-mapped per-thread table. A caller that walks a sampling volume along
/// an axis the noise does not depend on queries the same coordinate many times
/// in a row, and the layer loop is the cost of each repeat.
constexpr std::size_t sample_cache_size = 256;
thread_local std::array<cached_sample, sample_cache_size> sample_cache{};

[[nodiscard]] auto coordinate_bits(double value) -> std::uint64_t {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

[[nodiscard]] auto sample_cache_index(std::uint64_t stack, std::uint64_t x,
                                      std::uint64_t y, std::uint64_t z)
    -> std::size_t {
    std::uint64_t hash = x ^ (y >> 1U) ^ (z >> 2U) ^ stack;
    hash ^= hash >> 33U;
    hash *= 0xff51afd7ed558ccdULL;
    hash ^= hash >> 33U;
    return static_cast<std::size_t>(hash >> 56U);
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
    return sample_at(x, y, z);
}

auto perlin_noise::sample_at(double x, double y, double z) const -> float {
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

template <bool smeared>
void perlin_noise::add_perlin_volume(buffer &output, const volume &vol,
                                     double xz_scale, double y_scale,
                                     float amplitude,
                                     double fudge_y_scale) const {
    const noise_detail::perlin_volume_parameters parameters{
        .perms = perms_.data(),
        .offset_x = offset_x_,
        .offset_y = offset_y_,
        .offset_z = offset_z_,
        .fudge = make_smear_scale(fudge_y_scale),
        .smeared = smeared,
    };
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
    if (__builtin_cpu_supports("avx2")) {
        perlin_volume_avx2(parameters, output, vol, xz_scale, y_scale,
                           amplitude);
        return;
    }
#endif

    noise_detail::perlin_volume_scalar(parameters, output, vol, xz_scale,
                                       y_scale, amplitude);
}

void perlin_noise::add_to_volume(buffer &output, const volume &volume,
                                 double xz_scale, double y_scale,
                                 float amplitude) const {
    add_perlin_volume<false>(output, volume, xz_scale, y_scale, amplitude, 0.0);
}

auto noise_stack::add_to_volume(buffer &output, const volume &volume,
                                double xz_scale, double y_scale,
                                float amplitude) const -> void {
    for (const auto &l : layers_) {
        const double frequency = l.frequency;
        l.source->add_to_volume(output, volume, xz_scale * frequency,
                                y_scale * frequency, amplitude * l.amplitude);
    }
}

simplex_noise::simplex_noise(random_source &random, bool discard_noise_offset) {
    const double scale = discard_noise_offset ? 0.0 : 256.0;
    offset_x_ = random.next_double() * scale;
    offset_y_ = random.next_double() * scale;
    offset_z_ = random.next_double() * scale;
    for (std::size_t i = 0; i < perms_.size(); ++i) {
        perms_[i] = static_cast<std::uint8_t>(i);
    }
    for (jint i = 0; i < 256; ++i) {
        const jint offset = random.next_int(256 - i);
        std::swap(perms_[static_cast<std::size_t>(i)],
                  perms_[static_cast<std::size_t>(offset + i)]);
    }
}

auto simplex_noise::permute_value(jint value) const -> jint {
    return static_cast<jint>(perms_[static_cast<std::size_t>(value) & 0xFFU]);
}

auto simplex_noise::corner_noise3d(jint index, double x, double y, double z,
                                   double base) const -> double {
    double squared = base - x * x - y * y - z * z;
    if (squared < 0.0) {
        return 0.0;
    }
    squared *= squared;
    const auto &gradient = gradients[static_cast<std::size_t>(index)];
    const double dot = static_cast<double>(gradient.x) * x +
                       static_cast<double>(gradient.y) * y +
                       static_cast<double>(gradient.z) * z;
    return squared * squared * dot;
}

auto simplex_noise::get(double x_in, double y_in) const -> float {
    static const double sqrt_three = std::sqrt(3.0);
    static const double f2 = 0.5 * (sqrt_three - 1.0);
    static const double g2 = (3.0 - sqrt_three) / 6.0;
    const double x_in_offset = x_in + offset_x_;
    const double y_in_offset = y_in + offset_y_;
    const double s = (x_in_offset + y_in_offset) * f2;
    const jint i = mth::floor(x_in_offset + s);
    const jint j = mth::floor(y_in_offset + s);
    const double t = static_cast<double>(i + j) * g2;
    const double x0 = x_in_offset - (static_cast<double>(i) - t);
    const double y0 = y_in_offset - (static_cast<double>(j) - t);
    jint i1 = 0;
    jint j1 = 0;
    if (x0 > y0) {
        i1 = 1;
        j1 = 0;
    } else {
        i1 = 0;
        j1 = 1;
    }
    const double x1 = x0 - static_cast<double>(i1) + g2;
    const double y1 = y0 - static_cast<double>(j1) + g2;
    const double x2 = x0 - 1.0 + 2.0 * g2;
    const double y2 = y0 - 1.0 + 2.0 * g2;
    const jint ii = i & 0xFF;
    const jint jj = j & 0xFF;
    const jint gi0 = permute_value(ii + permute_value(jj)) % 12;
    const jint gi1 = permute_value(ii + i1 + permute_value(jj + j1)) % 12;
    const jint gi2 = permute_value(ii + 1 + permute_value(jj + 1)) % 12;
    const double n0 = corner_noise3d(gi0, x0, y0, 0.0, 0.5);
    const double n1 = corner_noise3d(gi1, x1, y1, 0.0, 0.5);
    const double n2 = corner_noise3d(gi2, x2, y2, 0.0, 0.5);
    return static_cast<float>(70.0 * (n0 + n1 + n2));
}

auto simplex_noise::get(double x_in, double y_in, double z_in) const -> float {
    const double x_offset_in = x_in + offset_x_;
    const double y_offset_in = y_in + offset_y_;
    const double z_offset_in = z_in + offset_z_;
    constexpr double f3 = 0.3333333333333333;
    const double s = (x_offset_in + y_offset_in + z_offset_in) * f3;
    const jint i = mth::floor(x_offset_in + s);
    const jint j = mth::floor(y_offset_in + s);
    const jint k = mth::floor(z_offset_in + s);
    constexpr double g3 = 0.16666666666666666;
    const double t = static_cast<double>(i + j + k) * g3;
    const double x0 = x_offset_in - (static_cast<double>(i) - t);
    const double y0 = y_offset_in - (static_cast<double>(j) - t);
    const double z0 = z_offset_in - (static_cast<double>(k) - t);
    jint i1 = 0;
    jint j1 = 0;
    jint k1 = 0;
    jint i2 = 0;
    jint j2 = 0;
    jint k2 = 0;
    if (x0 >= y0) {
        if (y0 >= z0) {
            i1 = 1;
            j1 = 0;
            k1 = 0;
            i2 = 1;
            j2 = 1;
            k2 = 0;
        } else if (x0 >= z0) {
            i1 = 1;
            j1 = 0;
            k1 = 0;
            i2 = 1;
            j2 = 0;
            k2 = 1;
        } else {
            i1 = 0;
            j1 = 0;
            k1 = 1;
            i2 = 1;
            j2 = 0;
            k2 = 1;
        }
    } else if (y0 < z0) {
        i1 = 0;
        j1 = 0;
        k1 = 1;
        i2 = 0;
        j2 = 1;
        k2 = 1;
    } else if (x0 < z0) {
        i1 = 0;
        j1 = 1;
        k1 = 0;
        i2 = 0;
        j2 = 1;
        k2 = 1;
    } else {
        i1 = 0;
        j1 = 1;
        k1 = 0;
        i2 = 1;
        j2 = 1;
        k2 = 0;
    }
    const double x1 = x0 - static_cast<double>(i1) + g3;
    const double y1 = y0 - static_cast<double>(j1) + g3;
    const double z1 = z0 - static_cast<double>(k1) + g3;
    const double x2 = x0 - static_cast<double>(i2) + 0.3333333333333333;
    const double y2 = y0 - static_cast<double>(j2) + 0.3333333333333333;
    const double z2 = z0 - static_cast<double>(k2) + 0.3333333333333333;
    const double x3 = x0 - 0.5;
    const double y3 = y0 - 0.5;
    const double z3 = z0 - 0.5;
    const jint ii = i & 0xFF;
    const jint jj = j & 0xFF;
    const jint kk = k & 0xFF;
    const jint gi0 =
        permute_value(ii + permute_value(jj + permute_value(kk))) % 12;
    const jint gi1 =
        permute_value(ii + i1 +
                      permute_value(jj + j1 + permute_value(kk + k1))) %
        12;
    const jint gi2 =
        permute_value(ii + i2 +
                      permute_value(jj + j2 + permute_value(kk + k2))) %
        12;
    const jint gi3 =
        permute_value(ii + 1 + permute_value(jj + 1 + permute_value(kk + 1))) %
        12;
    const double n0 = corner_noise3d(gi0, x0, y0, z0, 0.6);
    const double n1 = corner_noise3d(gi1, x1, y1, z1, 0.6);
    const double n2 = corner_noise3d(gi2, x2, y2, z2, 0.6);
    const double n3 = corner_noise3d(gi3, x3, y3, z3, 0.6);
    return static_cast<float>(32.0 * (n0 + n1 + n2 + n3));
}

void simplex_noise::add_to_volume(buffer &output, const volume &vol,
                                  double xz_scale, double y_scale,
                                  float amplitude) const {
    std::size_t index = 0;
    for (jint z = 0; z < vol.size_z(); ++z) {
        const double z_coord = static_cast<double>(vol.block_z(z)) * xz_scale;
        for (jint x = 0; x < vol.size_x(); ++x) {
            const double x_coord =
                static_cast<double>(vol.block_x(x)) * xz_scale;
            for (jint y = 0; y < vol.size_y(); ++y) {
                const double y_coord =
                    static_cast<double>(vol.block_y(y)) * y_scale;
                output.add_to(index,
                              amplitude * get(x_coord, y_coord, z_coord));
                ++index;
            }
        }
    }
}

auto simplex_noise::range() const -> interval {
    return interval::of_symmetric(2.0F);
}

namespace {
auto next_stack_id() -> std::uint64_t {
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}
} // namespace

noise_stack::noise_stack() : id_{next_stack_id()} {}

noise_stack::noise_stack(noise_stack &&other) noexcept
    : noise{std::move(other)}, layers_{std::move(other.layers_)},
      range_{other.range_}, id_{other.id_} {
    other.id_ = 0;
}

auto noise_stack::operator=(noise_stack &&other) noexcept -> noise_stack & {
    if (this != &other) {
        layers_ = std::move(other.layers_);
        range_ = other.range_;
        id_ = other.id_;
        other.id_ = 0;
    }
    return *this;
}

noise_stack::noise_stack(std::vector<layer> layers)
    : layers_{std::move(layers)}, id_{next_stack_id()} {
    for (const auto &l : layers_) {
        range_ = interval::add(range_,
                               interval::mul(interval::of_symmetric(2.0F),
                                             interval::of_exact(l.amplitude)));
    }
}

auto noise_stack::get(double x, double y, double z) const -> float {
    if (id_ == 0) {
        // A moved-from stack has no identity to key a cache entry by.
        float value = 0.0F;
        for (const auto &l : layers_) {
            value +=
                l.amplitude * l.source->get(x * l.frequency, y * l.frequency,
                                            z * l.frequency);
        }
        return value;
    }

    const std::uint64_t x_bits = coordinate_bits(x);
    const std::uint64_t y_bits = coordinate_bits(y);
    const std::uint64_t z_bits = coordinate_bits(z);
    cached_sample &entry =
        sample_cache[sample_cache_index(id_, x_bits, y_bits, z_bits)];
    if (entry.stack == id_ && entry.x == x_bits && entry.y == y_bits &&
        entry.z == z_bits) {
        return entry.value;
    }

    float value = 0.0F;
    for (const auto &l : layers_) {
        const double frequency = l.frequency;
        const float sample =
            l.plain != nullptr
                ? l.plain->sample_at(x * frequency, y * frequency,
                                     z * frequency)
                : l.source->get(x * frequency, y * frequency, z * frequency);
        value += l.amplitude * sample;
    }
    entry = cached_sample{
        .stack = id_,
        .x = x_bits,
        .y = y_bits,
        .z = z_bits,
        .value = value,
    };
    return value;
}

auto noise_stack::get(double x, double y) const -> float {
    float value = 0.0F;
    for (const auto &l : layers_) {
        const double frequency = l.frequency;
        value += l.amplitude * l.source->get(x * frequency, y * frequency);
    }
    return value;
}

smeared_perlin_noise::smeared_perlin_noise(random_source &random,
                                           double fudge_y_scale)
    : perlin_noise{random}, fudge_y_scale_{fudge_y_scale} {}

auto smeared_perlin_noise::range_for(double fudge_y_scale) -> interval {
    return interval::of_symmetric(
        static_cast<float>(std::abs(fudge_y_scale) + 2.0));
}

auto smeared_perlin_noise::range() const -> interval {
    return range_for(fudge_y_scale_);
}

auto smeared_perlin_noise::compute_fudge_y(double original_y,
                                           double relative_y) const -> double {
    return smeared_fudge_y(original_y, relative_y, fudge_y_scale_);
}

auto smeared_perlin_noise::get(double x, double y, double z) const -> float {
    const double wrapped_x = wrap(x) + offset_x_;
    const double wrapped_y = wrap(y) + offset_y_;
    const double wrapped_z = wrap(z) + offset_z_;
    const jint floor_x = mth::floor(wrapped_x);
    const jint floor_y = mth::floor(wrapped_y);
    const jint floor_z = mth::floor(wrapped_z);
    const float relative_x = static_cast<float>(wrapped_x - floor_x);
    const double relative_y = wrapped_y - floor_y;
    const float relative_z = static_cast<float>(wrapped_z - floor_z);
    const float fudged_relative_y =
        static_cast<float>(relative_y - compute_fudge_y(y, relative_y));
    return sample_and_lerp(floor_x, floor_y, floor_z, relative_x,
                           fudged_relative_y, relative_z,
                           static_cast<float>(relative_y));
}

auto smeared_perlin_noise::add_to_volume(buffer &output, const volume &vol,
                                         double xz_scale, double y_scale,
                                         float amplitude) const -> void {
    add_perlin_volume<true>(output, vol, xz_scale, y_scale, amplitude,
                            fudge_y_scale_);
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

auto normal_noise::create_legacy_nether_biome(random_source &random) const
    -> result<noise_stack> {
    std::vector<double> amplitudes = parameters_.amplitude_modifiers;
    if (amplitudes.empty()) {
        amplitudes.assign(static_cast<std::size_t>(parameters_.octave_count),
                          1.0);
    }
    auto first =
        build_legacy_nether_layers(random, parameters_.base_octave, amplitudes);
    if (!first) {
        return std::unexpected{first.error()};
    }
    auto second =
        build_legacy_nether_layers(random, parameters_.base_octave, amplitudes);
    if (!second) {
        return std::unexpected{second.error()};
    }

    // The outer layer scales the whole unnormalised stack; the standard
    // layout instead folds the normalised per-octave amplitudes into each
    // layer. Both produce the same total amplitude, but the multiplication
    // rounds in a different order.
    const float value_factor =
        static_cast<float>(normalization_factor_ * parameters_.base_amplitude);
    constexpr double second_frequency = 1.0181268882175227;

    std::vector<noise_stack::layer> layers;
    layers.reserve(first->size() + second->size());
    const auto append = [&layers, value_factor](auto &stack, double scale) {
        for (auto &layer : stack) {
            layers.push_back(noise_stack::layer{
                .source = std::move(layer.source),
                .frequency = layer.frequency * scale,
                .amplitude = layer.amplitude * value_factor,
                .plain = layer.plain,
            });
        }
    };
    append(*first, 1.0);
    append(*second, second_frequency);
    return noise_stack{std::move(layers)};
}

auto normal_noise::build_legacy_nether_layers(
    random_source &random, jint first_octave,
    const std::vector<double> &amplitudes)
    -> result<std::vector<noise_stack::layer>> {
    const auto octave_count = static_cast<jint>(amplitudes.size());
    const jint zero_octave_index = -first_octave;
    if (zero_octave_index < octave_count - 1) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "legacy nether layout cannot start at a positive "
                       "octave")};
    }

    // The generator for the zero octave is drawn even when the layout has no
    // slot for it; the draw is part of the historical seed sequence.
    std::vector<std::unique_ptr<perlin_noise>> levels(
        static_cast<std::size_t>(octave_count));
    auto zero_octave = std::make_unique<perlin_noise>(random);
    if (zero_octave_index >= 0 && zero_octave_index < octave_count &&
        amplitudes[static_cast<std::size_t>(zero_octave_index)] != 0.0) {
        levels[static_cast<std::size_t>(zero_octave_index)] =
            std::move(zero_octave);
    }

    for (jint i = zero_octave_index - 1; i >= 0; --i) {
        if (i < octave_count &&
            amplitudes[static_cast<std::size_t>(i)] != 0.0) {
            levels[static_cast<std::size_t>(i)] =
                std::make_unique<perlin_noise>(random);
        } else {
            random.consume_count(262);
        }
    }

    std::size_t non_zero = 0;
    for (const double amplitude : amplitudes) {
        if (amplitude != 0.0) {
            ++non_zero;
        }
    }
    const std::size_t created = static_cast<std::size_t>(
        std::count_if(levels.begin(), levels.end(),
                      [](const auto &level) { return level != nullptr; }));
    if (created != non_zero) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "legacy nether layout has an unfilled octave")};
    }

    double frequency = std::pow(2.0, -static_cast<double>(zero_octave_index));
    double amplitude_factor =
        std::pow(2.0, static_cast<double>(octave_count - 1)) /
        (std::pow(2.0, static_cast<double>(octave_count)) - 1.0);
    std::vector<noise_stack::layer> layers;
    layers.reserve(static_cast<std::size_t>(octave_count));
    for (jint i = 0; i < octave_count; ++i) {
        auto &level = levels[static_cast<std::size_t>(i)];
        if (level != nullptr) {
            const auto *plain = level.get();
            layers.push_back(noise_stack::layer{
                .source = std::move(level),
                .frequency = frequency,
                .amplitude = static_cast<float>(
                    amplitude_factor * amplitudes[static_cast<std::size_t>(i)]),
                .plain = plain,
            });
        }
        frequency *= 2.0;
        amplitude_factor /= 2.0;
    }
    return layers;
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

        auto first_layer = std::make_unique<perlin_noise>(*first_source);
        const auto *first_plain = first_layer.get();
        layers.push_back(noise_stack::layer{
            .source = std::move(first_layer),
            .frequency = octave.frequency,
            .amplitude = value_factor,
            .plain = first_plain,
        });
        auto second_layer = std::make_unique<perlin_noise>(*second_source);
        const auto *second_plain = second_layer.get();
        layers.push_back(noise_stack::layer{
            .source = std::move(second_layer),
            .frequency = octave.frequency * 1.0181268882175227,
            .amplitude = value_factor,
            .plain = second_plain,
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

auto biome_info_noise() -> const noise & {
    static const auto instance = [] {
        legacy_random_source random{2345};
        return std::make_unique<simplex_noise>(random, true);
    }();
    return *instance;
}

} // namespace refrain::worldgen
