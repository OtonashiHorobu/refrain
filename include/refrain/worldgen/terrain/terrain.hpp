// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Terrain fill: the fill stage of generation. The chunk is filled from the
// final-density volume, with an aquifer deciding the block between stone and
// the fluid/air column.

#pragma once

#include <refrain/worldgen/chunk/proto_chunk.hpp>
#include <refrain/worldgen/core/java_compat.hpp>
#include <refrain/worldgen/density/density.hpp>
#include <refrain/worldgen/density/density_buffer.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace refrain::worldgen {
class positional_random_factory;
} // namespace refrain::worldgen

namespace refrain::worldgen::terrain {

struct aquifer_config;

/// Block state ids the fill stage works with. The caller maps them to the
/// vanilla table; air must be the id that is skipped, not written.
struct block_ids {
    std::uint16_t air{0};
    std::uint16_t default_block{1};
    std::uint16_t default_fluid{2};
    std::uint16_t lava{3};
};

/// The fluid column of a position. The global picker returns lava below
/// `min(-54, sea level)` and the default fluid/air column otherwise.
struct fluid_status {
    jint fluid_level{0};
    std::uint16_t fluid_type{0};

    [[nodiscard]] auto at(jint block_y) const -> std::uint16_t {
        return block_y < fluid_level ? fluid_type : std::uint16_t{0};
    }
    [[nodiscard]] auto at(jint block_y, std::uint16_t air_type) const
        -> std::uint16_t {
        return block_y < fluid_level ? fluid_type : air_type;
    }
    [[nodiscard]] auto operator==(const fluid_status &other) const -> bool {
        return fluid_level == other.fluid_level &&
               fluid_type == other.fluid_type;
    }
};

struct fluid_picker {
    jint sea_level{63};
    std::uint16_t default_fluid{2};
    std::uint16_t lava{3};
    std::uint16_t air{0};

    [[nodiscard]] auto compute_fluid(jint /*x*/, jint y, jint /*z*/) const
        -> fluid_status {
        if (y < std::min(-54, sea_level)) {
            return fluid_status{.fluid_level = -54, .fluid_type = lava};
        }
        return fluid_status{.fluid_level = sea_level,
                            .fluid_type = default_fluid};
    }
};

/// An aquifer: returning `nullopt` means "use the default block"; otherwise
/// the returned id is written (air included).
class aquifer {
  public:
    aquifer() = default;
    aquifer(const aquifer &) = delete;
    auto operator=(const aquifer &) -> aquifer & = delete;
    aquifer(aquifer &&) = delete;
    auto operator=(aquifer &&) -> aquifer & = delete;
    virtual ~aquifer() = default;

    [[nodiscard]] virtual auto compute_substance(jint block_x, jint block_y,
                                                 jint block_z, double density)
        -> std::optional<std::uint16_t> = 0;
    [[nodiscard]] virtual auto should_schedule_fluid_update() const -> bool = 0;
};

/// Disabled aquifer: density above zero is the default block, the rest is the
/// fluid column.
class disabled_aquifer final : public aquifer {
  public:
    explicit disabled_aquifer(fluid_picker picker) : picker_{picker} {}

    [[nodiscard]] auto compute_substance(jint block_x, jint block_y,
                                         jint block_z, double density)
        -> std::optional<std::uint16_t> override {
        if (density > 0.0) {
            return std::nullopt;
        }
        const auto fluid = picker_.compute_fluid(block_x, block_y, block_z);
        return fluid.at(block_y);
    }
    [[nodiscard]] auto should_schedule_fluid_update() const -> bool override {
        return false;
    }

  private:
    fluid_picker picker_;
};

inline constexpr jint chunk_width = 16;
inline constexpr jint chunk_depth = 16;

/// One chunk's blocks laid out as `(y - min_y) * 256 + z * 16 + x`, plus the
/// two write-time heightmaps. A heightmap entry holds the highest `y` whose
/// written state matched the heightmap predicate, using the reference
/// highest-taken rule; an empty column reads `min_y - 1`.
struct chunk_blocks {
    jint min_y{0};
    jint height{0};
    std::vector<std::uint16_t> states{};
    std::array<jint, 256> ocean_floor{};
    std::array<jint, 256> world_surface{};
    /// Post-processing coordinates per section: packed `x | y<<4 | z<<8`
    /// positions of fluid blocks the aquifer asked to schedule. Section
    /// indices are relative to `min_y`.
    std::vector<std::vector<std::uint16_t>> post_processing{};

    [[nodiscard]] auto at(jint x, jint y, jint z) const -> std::uint16_t {
        return states[static_cast<std::size_t>(y - min_y) * 256U +
                      static_cast<std::size_t>(z) * 16U +
                      static_cast<std::size_t>(x)];
    }
    void set(jint x, jint y, jint z, std::uint16_t state) {
        states[static_cast<std::size_t>(y - min_y) * 256U +
               static_cast<std::size_t>(z) * 16U +
               static_cast<std::size_t>(x)] = state;
    }
};

struct fill_options {
    jint min_y{-64};
    jint height{384};
    jint sea_level{63};
    block_ids blocks{};
};

/// Fills one chunk: sample the final-density volume, walk y from the top down
/// and write the aquifer/default block result.
/// Air is skipped. `world_surface` records every written state, while
/// `ocean_floor` only records states in `minecraft:blocks_motion_in_heightmap`;
/// the fill writes stone and fluids, and no fluid belongs to that tag. Fluid
/// writes the aquifer asks to schedule are recorded in `post_processing`.
[[nodiscard]] auto fill_chunk(const fill_options &options,
                              const density::sampler &final_density,
                              const density::sampler_context &context,
                              jint chunk_x, jint chunk_z, aquifer &aquifer)
    -> chunk_blocks;

/// One column's first occupied height, one below the base height. The volume is
/// one block wide and tall, matching the reference column sampling; the
/// aquifer therefore sees the same cell anchors and surface levels as a
/// full-chunk fill.
struct column_height_options {
    jint min_y{-64};
    jint height{384};
    jint sea_level{63};
    block_ids blocks{};
    const aquifer_config *aquifer{nullptr};
    positional_random_factory *factory{nullptr};
};

/// Returns the highest `y` whose generated state matches `predicate`, or
/// `min_y - 1` when the column is empty. `predicate` models the heightmap
/// type; the caller supplies the block-tag lookup.
[[nodiscard]] auto first_occupied_height(
    const column_height_options &options, const density::sampler &final_density,
    const density::sampler_context &context, jint block_x, jint block_z,
    const std::function<bool(std::uint16_t)> &predicate) -> jint;

/// `doFill` into a generation chunk. The chunk must be at `biomes` status so
/// the writes update the two worldgen heightmaps; completing `terrain` primes
/// the final heightmaps. Fluid writes the aquifer asks to schedule are marked
/// for post-processing.
struct proto_fill_options {
    block_state_id air{0};
    block_state_id default_block{1};
    block_state_id default_fluid{2};
    block_state_id lava{3};
};

void fill_proto_chunk(proto_chunk &chunk, const proto_fill_options &options,
                      const density::sampler &final_density,
                      const density::sampler_context &context,
                      aquifer &aquifer);

} // namespace refrain::worldgen::terrain
