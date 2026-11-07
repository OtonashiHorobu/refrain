// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Surface material evaluation: the `MaterialRuleContext` state machine and the
// compiler that turns a decoded rule tree into evaluators. The block-state
// table interns the names the rules and the chunk exchange.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/worldgen/biome/biome_climate.hpp>
#include <refrain/worldgen/core/java_compat.hpp>
#include <refrain/worldgen/core/rng.hpp>
#include <refrain/worldgen/density/density.hpp>
#include <refrain/worldgen/noise/noise.hpp>
#include <refrain/worldgen/terrain/material.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace refrain::worldgen {
class block_state_registry;
class proto_chunk;
} // namespace refrain::worldgen

namespace refrain::worldgen::surface {

using state_id = std::uint16_t;
inline constexpr state_id no_state = std::numeric_limits<state_id>::max();

/// The two properties the surface stage inspects.
struct state_properties {
    bool air{false};
    bool fluid{false};
    bool water{false};
};

/// Interns block-state keys. The chunk and the material rules share one table,
/// so comparisons are id comparisons and the oracle can map either side back to
/// the key.
class block_states {
  public:
    block_states() = default;
    /// Binds the table to the generation-wide registry; every interned or
    /// looked-up key then shares ids with the chunk.
    explicit block_states(block_state_registry &registry)
        : registry_{&registry} {}

    [[nodiscard]] auto intern(std::string_view name) -> state_id;
    [[nodiscard]] auto intern(std::string_view name,
                              state_properties properties) -> state_id;
    [[nodiscard]] auto find(std::string_view name) const
        -> std::optional<state_id>;
    [[nodiscard]] auto name(state_id id) const -> std::string_view;
    [[nodiscard]] auto properties(state_id id) const -> state_properties;
    [[nodiscard]] auto size() const noexcept -> std::size_t;

  private:
    block_state_registry *registry_{nullptr};
    std::vector<std::string> names_{};
    std::vector<state_properties> properties_{};
    std::unordered_map<std::string, state_id> index_{};
};

using condition_evaluator = std::function<bool()>;
using rule_evaluator = std::function<std::optional<state_id>(jint, jint, jint)>;

/// The generation bounds `VerticalAnchor` and the surface loop resolve
/// against, plus the chunk origin for the ore-vein prefilled volume.
struct generation_bounds {
    jint min_gen_y{-64};
    jint gen_depth{384};
    jint sea_level{63};
    jint min_y{-64};
    jint max_block_y{319};
    jint chunk_min_x{0};
    jint chunk_min_z{0};
};

/// Everything the context reads from the chunk pipeline. The implementations
/// live in the surface system and in tests.
struct material_providers {
    std::function<std::string_view(jint, jint, jint)> biome{};
    /// `chunk_surface_level` sampled at `(x, 0, z)`.
    std::function<float(jint, jint)> preliminary_surface{};
    std::function<jint(jint, jint)> surface_depth{};
    std::function<double(jint, jint)> surface_secondary{};
    std::function<state_id(jint, jint, jint)> band{};
    std::function<const noise_stack *(std::string_view)> noise{};
    std::function<result<density::sampler_ptr>(std::string_view)> density{};
    std::function<positional_random_factory &(std::string_view)>
        random_factory{};
    /// `Biome.coldEnoughToSnow(pos, seaLevel)` for the current biome.
    std::function<bool(std::string_view, jint, jint, jint)>
        cold_enough_to_snow{};
    const std::set<std::string> *possible_biomes{nullptr};
};

/// Java `MaterialRuleContext`.
class material_context {
  public:
    material_context(block_states &states, generation_bounds bounds,
                     material_providers providers,
                     const density::sampler_context &density_context);

    void update_xz(jint block_x, jint block_z, jint surface_gradient_x,
                   jint surface_gradient_z);
    void update_y(jint stone_depth_above, jint stone_depth_below,
                  jint water_height, jint block_y);

    [[nodiscard]] auto last_update_xz() const noexcept -> jlong {
        return last_update_xz_;
    }
    [[nodiscard]] auto last_update_y() const noexcept -> jlong {
        return last_update_y_;
    }
    [[nodiscard]] auto block_x() const noexcept -> jint { return block_x_; }
    [[nodiscard]] auto block_y() const noexcept -> jint { return block_y_; }
    [[nodiscard]] auto block_z() const noexcept -> jint { return block_z_; }
    [[nodiscard]] auto surface_gradient_x() const noexcept -> jint {
        return surface_gradient_x_;
    }
    [[nodiscard]] auto surface_gradient_z() const noexcept -> jint {
        return surface_gradient_z_;
    }
    [[nodiscard]] auto surface_depth() const noexcept -> jint {
        return surface_depth_;
    }
    [[nodiscard]] auto water_height() const noexcept -> jint {
        return water_height_;
    }
    [[nodiscard]] auto stone_depth_above() const noexcept -> jint {
        return stone_depth_above_;
    }
    [[nodiscard]] auto stone_depth_below() const noexcept -> jint {
        return stone_depth_below_;
    }
    [[nodiscard]] auto sea_level() const noexcept -> jint {
        return bounds_.sea_level;
    }
    [[nodiscard]] auto bounds() const noexcept -> const generation_bounds & {
        return bounds_;
    }
    [[nodiscard]] auto states() noexcept -> block_states & { return *states_; }
    [[nodiscard]] auto possible_biomes() const noexcept
        -> const std::set<std::string> * {
        return providers_.possible_biomes;
    }

    [[nodiscard]] auto biome() -> std::string_view;
    [[nodiscard]] auto surface_secondary() -> double;
    [[nodiscard]] auto min_surface_level() -> jint;
    [[nodiscard]] auto band(jint x, jint y, jint z) -> state_id;

    /// `MaterialSystem.erodedBadlandsExtension`.
    void eroded_badlands_extension(
        jint block_x, jint block_z, jint height, jint min_y,
        const std::function<state_id(jint, jint, jint)> &get,
        const std::function<void(jint, jint, jint, state_id)> &set);
    [[nodiscard]] auto resolve_anchor(const vertical_anchor &anchor) const
        -> jint;
    [[nodiscard]] auto random_factory(std::string_view name)
        -> positional_random_factory &;
    [[nodiscard]] auto cold_enough_to_snow(std::string_view biome) -> bool;

    /// Java `getNoiseSampler`: the returned callable reads the current position
    /// once per update.
    [[nodiscard]] auto noise_sampler(std::string_view id, bool is_3d)
        -> result<std::function<double()>>;

    /// Java `getDensitiesInChunk`.
    [[nodiscard]] auto densities_in_chunk(std::string_view id, bool prefill)
        -> result<std::function<float(jint, jint, jint)>>;

  private:
    struct noise_cache {
        const noise_stack *stack{nullptr};
        jlong last_update{0};
        double value{0.0};
        bool is_3d{false};
        material_context *context{nullptr};
    };

    struct density_cache {
        density::sampler_ptr sampler{};
        const density::sampler_context *sampler_context{nullptr};
        bool prefill{false};
        std::vector<float> values{};
        std::size_t size_x{0};
        std::size_t size_y{0};
        std::size_t size_z{0};
        jint min_x{0};
        jint min_y{0};
        jint min_z{0};

        [[nodiscard]] auto value_at(jint x, jint y, jint z) const -> float;
    };

    block_states *states_{nullptr};
    generation_bounds bounds_{};
    material_providers providers_{};
    const density::sampler_context *density_context_{nullptr};

    jlong last_update_xz_{std::numeric_limits<jlong>::min() + 1};
    jlong last_update_y_{std::numeric_limits<jlong>::min() + 1};
    jint block_x_{0};
    jint block_z_{0};
    jint surface_gradient_x_{0};
    jint surface_gradient_z_{0};
    jint surface_depth_{0};
    jlong last_surface_secondary_update_{std::numeric_limits<jlong>::min()};
    double surface_secondary_{0.0};
    jlong last_min_surface_update_{std::numeric_limits<jlong>::min()};
    jint min_surface_level_{0};
    jint block_y_{0};
    jint water_height_{std::numeric_limits<jint>::min()};
    jint stone_depth_below_{0};
    jint stone_depth_above_{0};
    std::optional<std::string> biome_{};

    std::unordered_map<std::string, std::shared_ptr<noise_cache>>
        noise_samplers_{};
    std::unordered_map<std::string, std::shared_ptr<density_cache>>
        density_samplers_{};
};

/// Shared `NormalNoise` instances for the names the surface stage uses.
class noise_registry {
  public:
    noise_registry(algorithm kind, jlong seed);
    noise_registry(const noise_registry &) = delete;
    auto operator=(const noise_registry &) -> noise_registry & = delete;
    noise_registry(noise_registry &&) = delete;
    auto operator=(noise_registry &&) -> noise_registry & = delete;
    ~noise_registry() = default;

    /// Instantiates one `worldgen/noise` document.
    void add(std::string id, std::string_view json);

    /// Walks `<data_root>/<namespace>/worldgen/noise/**`.
    [[nodiscard]] auto load(std::string_view data_root) -> status;

    [[nodiscard]] auto get(std::string_view id) -> const noise_stack *;

  private:
    std::unique_ptr<positional_random_factory> factory_{};
    std::unordered_map<std::string, noise_stack> noises_{};
};

/// Java `MaterialSystem`: surface depth/secondary, clay bands and the
/// `buildSurface` loop.
class surface_system {
  public:
    surface_system(block_states &states, generation_bounds bounds,
                   noise_registry &noises,
                   positional_random_factory &noise_random,
                   state_id default_block, state_id air);

    [[nodiscard]] auto surface_depth(jint x, jint z) -> jint;
    [[nodiscard]] auto surface_secondary(jint x, jint z) -> double;
    [[nodiscard]] auto band(jint x, jint y, jint z) -> state_id;

    /// `MaterialSystem.erodedBadlandsExtension`.
    void eroded_badlands_extension(
        jint block_x, jint block_z, jint height, jint min_y,
        const std::function<state_id(jint, jint, jint)> &get,
        const std::function<void(jint, jint, jint, state_id)> &set);

    /// Runs one chunk's surface pass. `get`/`set` use absolute coordinates;
    /// `world_surface` is the write-time heightmap indexed by `x + z*16`.
    [[nodiscard]] auto build_surface(
        material_context &context, const rule_evaluator &rule,
        const biome_climates &climates, const biome_temperatures &temperatures,
        const std::function<std::string_view(jint, jint, jint)> &biome,
        const std::function<state_id(jint, jint, jint)> &get,
        const std::function<void(jint, jint, jint, state_id)> &set,
        const std::function<jint(jint, jint)> &world_surface) -> status;

    /// Runs one chunk's surface pass against a generation chunk. The chunk
    /// must be at `biomes` status so its worldgen heightmap keeps updating.
    [[nodiscard]] auto build_surface(
        proto_chunk &chunk, material_context &context,
        const rule_evaluator &rule, const biome_climates &climates,
        const biome_temperatures &temperatures,
        const std::function<std::string_view(jint, jint, jint)> &biome)
        -> status;

  private:
    void frozen_ocean_extension(
        std::string_view biome, const biome_climate &climate,
        const biome_temperatures &temperatures, jint min_surface_level,
        jint block_x, jint block_z, jint height,
        const std::function<state_id(jint, jint, jint)> &get,
        const std::function<void(jint, jint, jint, state_id)> &set);

    block_states *states_{nullptr};
    generation_bounds bounds_{};
    noise_registry *noises_{nullptr};
    positional_random_factory *noise_random_{nullptr};
    state_id default_block_{no_state};
    state_id air_{no_state};
    const noise_stack *surface_noise_{nullptr};
    const noise_stack *surface_secondary_noise_{nullptr};
    const noise_stack *clay_bands_offset_noise_{nullptr};
    const noise_stack *badlands_surface_noise_{nullptr};
    const noise_stack *badlands_pillar_noise_{nullptr};
    const noise_stack *badlands_pillar_roof_noise_{nullptr};
    const noise_stack *iceberg_surface_noise_{nullptr};
    const noise_stack *iceberg_pillar_noise_{nullptr};
    const noise_stack *iceberg_pillar_roof_noise_{nullptr};
    state_id snow_block_{no_state};
    state_id packed_ice_{no_state};
    std::array<state_id, 192> clay_bands_{};
};

/// Compiles a decoded tree against one chunk context. References are resolved
/// against `documents` and cached; cycles are rejected.
class rule_compiler {
  public:
    rule_compiler(const material_documents &documents,
                  material_context &context);

    [[nodiscard]] auto compile(const rule &value) -> result<rule_evaluator>;
    [[nodiscard]] auto compile_condition(const condition &value)
        -> result<condition_evaluator>;

  private:
    [[nodiscard]] auto resolve_rule(std::string_view id)
        -> result<const rule *>;
    [[nodiscard]] auto resolve_condition(std::string_view id)
        -> result<const condition *>;

    const material_documents &documents_;
    material_context &context_;
    std::unordered_map<std::string, rule> rule_cache_{};
    std::unordered_map<std::string, condition> condition_cache_{};
    std::unordered_set<std::string> active_rules_{};
    std::unordered_set<std::string> active_conditions_{};
    std::unordered_set<std::string> compiling_rules_{};
    std::unordered_set<std::string> compiling_conditions_{};
};

} // namespace refrain::worldgen::surface
