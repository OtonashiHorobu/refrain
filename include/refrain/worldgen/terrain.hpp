// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Terrain fill: the `doFill` stage of `NoiseBasedChunkGenerator`. The chunk is
// filled from the final-density volume, with an aquifer deciding the block
// between stone and the fluid/air column.

#pragma once

#include <refrain/worldgen/density.hpp>
#include <refrain/worldgen/density_buffer.hpp>
#include <refrain/worldgen/java_compat.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace refrain::worldgen::terrain {

/// Block state ids the fill stage works with. The caller maps them to the
/// vanilla table; air must be the id that is skipped, not written.
struct block_ids {
    std::uint16_t air{0};
    std::uint16_t default_block{1};
    std::uint16_t default_fluid{2};
    std::uint16_t lava{3};
};

/// Java `Aquifer.FluidPicker`: the fluid column of a position. The reference's
/// global picker returns lava below `min(-54, sea level)` and the default
/// fluid/air column otherwise.
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

/// Java `Aquifer`: `computeSubstance` returning `nullopt` means "use the
/// default block"; otherwise the returned id is written (air included).
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

/// `Aquifer.createDisabled`: density above zero is the default block, the rest
/// is the fluid column.
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
/// written state matched the heightmap predicate, mirroring Java
/// `Heightmap.getHighestTaken`; an empty column reads `min_y - 1`.
struct chunk_blocks {
    jint min_y{0};
    jint height{0};
    std::vector<std::uint16_t> states{};
    std::array<jint, 256> ocean_floor{};
    std::array<jint, 256> world_surface{};

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

/// `NoiseBasedChunkGenerator.doFill` for one chunk: sample the final-density
/// volume, walk y from the top down and write the aquifer/default block result.
/// Air is skipped. `world_surface` records every written state, while
/// `ocean_floor` only records states in `minecraft:blocks_motion_in_heightmap`;
/// the fill writes stone and fluids, and no fluid belongs to that tag.
[[nodiscard]] auto fill_chunk(const fill_options &options,
                              const density::sampler &final_density,
                              const density::sampler_context &context,
                              jint chunk_x, jint chunk_z, aquifer &aquifer)
    -> chunk_blocks;

} // namespace refrain::worldgen::terrain
