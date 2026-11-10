// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// 26.3 density-function samplers.
//
// Every density-function node compiles into a sampler with a scalar and a bulk
// sampling path. The two paths do not always agree: for example `mul`
// short-circuits a zero left operand on the scalar path but not in bulk, and
// `sub` accumulates the right operand negated in bulk. Both are implemented
// separately, because terrain generation reads the bulk path.
//
// The compiler also rewrites the graph (constant specialisations, `div` by a
// constant becoming a multiply by the reciprocal, min/max range shortcuts).
// Those rewrites are part of the observable float results, so the factories
// below reproduce them.
//
// A referenced density document is compiled once and shared by every node that
// refers to it, and a `cache` node keeps the values it sampled for one volume
// in the sampling context. Both reproduce the reference's compiled graph: the
// number of evaluations of a shared node no longer depends on how often the
// document is referenced.

#pragma once

#include <refrain/worldgen/core/java_compat.hpp>
#include <refrain/worldgen/density/density_buffer.hpp>
#include <refrain/worldgen/density/interval.hpp>
#include <refrain/worldgen/noise/noise.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace refrain::worldgen::density {

class sampler;

/// Context fields the reference binds through named sampler keys. Terrain
/// generation without blending/beardifier leaves them unset, which makes the
/// corresponding samplers use their fallbacks.
enum class context_key : std::uint8_t {
    blend_alpha,
    blend_offset,
    beardifier,
};

/// One sampling pass's state: the bound context fields and the buffers
/// `cache` nodes keep. A context is single-threaded; copies share the cache.
class sampler_context {
  public:
    sampler_context();
    sampler_context(const sampler_context &) = default;
    auto operator=(const sampler_context &) -> sampler_context & = default;
    sampler_context(sampler_context &&) noexcept = default;
    auto operator=(sampler_context &&) noexcept -> sampler_context & = default;
    ~sampler_context();

    /// Scratch storage for one sampling pass. The reference uses a pooled arena
    /// with scoped lifetimes; a fresh allocation has the same values.
    [[nodiscard]] auto acquire(std::size_t size) const -> buffer {
        return buffer{size};
    }

    /// The buffer a `cache` node keeps for `volume`, and whether it already
    /// holds that volume's values. On a miss the buffer is resized for the new
    /// volume and the caller must sample it before reading; the reference keeps
    /// one buffer per cache node and samples it again when the volume changes.
    [[nodiscard]] auto cached_volume(std::size_t id, const volume &vol) const
        -> std::pair<buffer *, bool>;

    /// Marks a cache entry as being filled, so eviction keeps its buffer.
    void mark_filling(std::size_t id, bool value) const;

    [[nodiscard]] auto get_field(context_key key) const -> const sampler * {
        return fields_[static_cast<std::size_t>(key)];
    }
    void set_field(context_key key, const sampler *value) {
        fields_[static_cast<std::size_t>(key)] = value;
    }

  private:
    struct volume_cache;

    std::array<const sampler *, 3> fields_{};
    mutable std::shared_ptr<volume_cache> cache_;
};

class sampler {
  public:
    sampler() = default;
    sampler(const sampler &) = delete;
    auto operator=(const sampler &) -> sampler & = delete;
    sampler(sampler &&) = delete;
    auto operator=(sampler &&) -> sampler & = delete;
    virtual ~sampler() = default;

    [[nodiscard]] virtual auto sample_value(const sampler_context &context,
                                            jint block_x, jint block_y,
                                            jint block_z) const -> float = 0;
    virtual void sample_volume(const sampler_context &context, buffer &output,
                               const volume &volume) const = 0;

    [[nodiscard]] virtual auto range() const -> interval = 0;

    /// Non-null when the node is a constant; the compiler uses this for the
    /// constant specialisations.
    [[nodiscard]] virtual auto constant_value() const -> const float * {
        return nullptr;
    }
};

/// Samplers are shared: a referenced document compiles once, and a `cache`
/// node's identity therefore stays the same at every reference to it.
using sampler_ptr = std::shared_ptr<sampler>;

[[nodiscard]] auto constant(float value) -> sampler_ptr;

[[nodiscard]] auto add(sampler_ptr left, sampler_ptr right) -> sampler_ptr;
[[nodiscard]] auto sub(sampler_ptr left, sampler_ptr right) -> sampler_ptr;
[[nodiscard]] auto mul(sampler_ptr left, sampler_ptr right) -> sampler_ptr;
[[nodiscard]] auto div(sampler_ptr left, sampler_ptr right) -> sampler_ptr;
[[nodiscard]] auto min(sampler_ptr left, sampler_ptr right) -> sampler_ptr;
[[nodiscard]] auto max(sampler_ptr left, sampler_ptr right) -> sampler_ptr;

[[nodiscard]] auto abs(sampler_ptr input) -> sampler_ptr;
[[nodiscard]] auto square(sampler_ptr input) -> sampler_ptr;
[[nodiscard]] auto cube(sampler_ptr input) -> sampler_ptr;
[[nodiscard]] auto sqrt(sampler_ptr input) -> sampler_ptr;
[[nodiscard]] auto half_negative(sampler_ptr input) -> sampler_ptr;
[[nodiscard]] auto quarter_negative(sampler_ptr input) -> sampler_ptr;
[[nodiscard]] auto reciprocal(sampler_ptr input) -> sampler_ptr;
[[nodiscard]] auto negate(sampler_ptr input) -> sampler_ptr;
[[nodiscard]] auto squeeze(sampler_ptr input) -> sampler_ptr;
[[nodiscard]] auto log(sampler_ptr input) -> sampler_ptr;
[[nodiscard]] auto sign(sampler_ptr input) -> sampler_ptr;

[[nodiscard]] auto clamp(sampler_ptr input, float min_value, float max_value)
    -> sampler_ptr;

/// `gradient` / `y_clamped_gradient` with `CLAMP_TO_EDGE`. Only the Y axis is
/// needed for the overworld router; the other axes follow the same formula.
[[nodiscard]] auto gradient_y(jint from_coordinate, jint to_coordinate,
                              float from_value, float to_value) -> sampler_ptr;

/// `minecraft:noise`: samples the noise at `(x * xz, y * y, z * xz)`.
[[nodiscard]] auto noise(std::shared_ptr<const noise_stack> stack,
                         double xz_scale, double y_scale) -> sampler_ptr;

/// `minecraft:noise` with shift functions; `y_scale` is used for the Y
/// coordinate and the X/Z shifts are evaluated per sample.
[[nodiscard]] auto shifted_noise_xz(sampler_ptr shift_x, sampler_ptr shift_z,
                                    double xz_scale, double y_scale,
                                    std::shared_ptr<const noise_stack> stack)
    -> sampler_ptr;
[[nodiscard]] auto shifted_noise_xyz(sampler_ptr shift_x, sampler_ptr shift_y,
                                     sampler_ptr shift_z, double xz_scale,
                                     double y_scale,
                                     std::shared_ptr<const noise_stack> stack)
    -> sampler_ptr;

struct spline;
struct spline_point {
    float location{0.0F};
    float derivative{0.0F};
    std::shared_ptr<const spline> value{};
};

/// A constant or a multipoint Hermite spline. Coordinates are density samplers;
/// identical samplers are sampled once per point/volume.
struct spline {
    [[nodiscard]] static auto constant(float value)
        -> std::shared_ptr<const spline>;
    [[nodiscard]] static auto multipoint(std::shared_ptr<sampler> coordinate,
                                         std::vector<spline_point> points)
        -> std::shared_ptr<const spline>;

    [[nodiscard]] auto range() const -> interval;

    bool constant_{false};
    float constant_value_{0.0F};
    std::shared_ptr<sampler> coordinate_;
    std::vector<float> locations_;
    std::vector<float> derivatives_;
    std::vector<std::shared_ptr<const spline>> values_;
};

/// `minecraft:spline`.
[[nodiscard]] auto spline_value(std::shared_ptr<const spline> value)
    -> sampler_ptr;

/// `minecraft:interpolated`: sample the input on a coarse cell grid and
/// trilinearly interpolate the block values.
[[nodiscard]] auto interpolated(sampler_ptr input, jint cell_size_xz,
                                jint cell_size_y) -> sampler_ptr;

/// `minecraft:range_choice`: `input` selects which branch to evaluate.
[[nodiscard]] auto range_choice(sampler_ptr input, float min_inclusive,
                                float max_exclusive, sampler_ptr when_in_range,
                                sampler_ptr when_out_of_range) -> sampler_ptr;

/// `minecraft:interval_select`: threshold table selecting one of N functions.
[[nodiscard]] auto interval_select(sampler_ptr input,
                                   std::vector<float> thresholds,
                                   std::vector<sampler_ptr> functions)
    -> sampler_ptr;

/// `minecraft:lerp`: linear interpolation of the two branches with the
/// reference's 0/1 alpha short circuits and constant specialisations.
[[nodiscard]] auto lerp(sampler_ptr alpha, sampler_ptr first,
                        sampler_ptr second) -> sampler_ptr;

/// `minecraft:blend_alpha`, `blend_offset` and `beardifier`: context-bound
/// values with the reference fallbacks (1.0, 0.0, 0.0).
[[nodiscard]] auto blend_alpha() -> sampler_ptr;
[[nodiscard]] auto blend_offset() -> sampler_ptr;
[[nodiscard]] auto beardifier() -> sampler_ptr;

/// `minecraft:blend_density`: applies the blender from the context when one is
/// bound; without a blender it is the identity.
[[nodiscard]] auto blend_density(sampler_ptr input) -> sampler_ptr;

/// `minecraft:old_blended_noise` parameters.
struct blended_noise_parameters {
    double xz_scale{0.25};
    double y_scale{0.125};
    double xz_factor{80.0};
    double y_factor{160.0};
    double smear_scale_multiplier{8.0};
};

/// `minecraft:old_blended_noise`: three smeared-Perlin FBM stacks combined by
/// a clamped lerp. `random` is the stream derived from the
/// `"minecraft:terrain"` name hash.
[[nodiscard]] auto blended_noise(const blended_noise_parameters &parameters,
                                 random_source &random) -> sampler_ptr;

/// `minecraft:slice`: evaluate `input` at a fixed coordinate of one axis and
/// broadcast the result along that axis.
enum class slice_axis : std::uint8_t { x, y, z };

[[nodiscard]] auto slice(slice_axis axis, jint coordinate, sampler_ptr input)
    -> sampler_ptr;

/// `minecraft:slice` on Y. Kept as the constructor the surface probe uses.
[[nodiscard]] auto slice_y(sampler_ptr input, jint y) -> sampler_ptr;

/// The distance a `minecraft:distance_to_point` function reports.
enum class distance_metric : std::uint8_t {
    euclidean,
    euclidean_squared,
    manhattan,
    chebyshev,
};

/// `minecraft:distance_to_point`: the metric between a fixed point and the
/// sampled block position.
[[nodiscard]] auto distance_to_point(jint x, jint y, jint z,
                                     distance_metric metric) -> sampler_ptr;

/// `minecraft:end_outer_islands`: the outer-island height field. The stream is
/// a legacy source seeded with the world seed that has consumed the 17292
/// draws the reference discards before building the simplex noise.
[[nodiscard]] auto end_outer_islands(random_source &random) -> sampler_ptr;

/// `minecraft:find_top_surface`: probe the density along Y in `cell_height`
/// steps and return the first cell above zero (or `lower_bound`).
[[nodiscard]] auto find_top_surface(sampler_ptr density,
                                    sampler_ptr upper_bound, jint lower_bound,
                                    jint cell_height) -> sampler_ptr;

/// `minecraft:cache`: keeps the values its input sampled for one grid in the
/// sampling context, so several references to the same node sample it once per
/// grid. A different grid replaces the entry. Only the bulk path reads the
/// entry; scalar sampling evaluates the input, because the scalar and bulk
/// paths of some nodes round differently and terrain generation reads bulk.
[[nodiscard]] auto cache(sampler_ptr input) -> sampler_ptr;

/// `minecraft:shift`: the 3D noise at a quarter scale, times four.
[[nodiscard]] auto shift(std::shared_ptr<const noise_stack> stack)
    -> sampler_ptr;
[[nodiscard]] auto shift_a(std::shared_ptr<const noise_stack> stack)
    -> sampler_ptr;
[[nodiscard]] auto shift_b(std::shared_ptr<const noise_stack> stack)
    -> sampler_ptr;

} // namespace refrain::worldgen::density
