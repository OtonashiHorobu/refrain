// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Java `Beardifier`: the structure-driven density contribution. The structure
// manager side (`forStructuresInChunk`) is not implemented yet; the sampler is
// built from rigid pieces and jigsaw junctions, and `empty()` reproduces the
// no-structure case.

#pragma once

#include <refrain/worldgen/density.hpp>
#include <refrain/worldgen/interval.hpp>
#include <refrain/worldgen/java_compat.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace refrain::worldgen {

/// Java `BoundingBox` (inclusive bounds).
struct bounding_box {
    jint min_x{0};
    jint min_y{0};
    jint min_z{0};
    jint max_x{0};
    jint max_y{0};
    jint max_z{0};

    [[nodiscard]] auto is_inside(jint x, jint y, jint z) const noexcept -> bool;
    [[nodiscard]] auto intersects(jint other_min_x, jint other_min_y,
                                  jint other_min_z, jint other_max_x,
                                  jint other_max_y,
                                  jint other_max_z) const noexcept -> bool;
    [[nodiscard]] auto inflated_by(jint amount) const noexcept -> bounding_box;
    [[nodiscard]] static auto encapsulating(const bounding_box &first,
                                            const bounding_box &second) noexcept
        -> bounding_box;
};

/// Java `TerrainAdjustment`.
enum class terrain_adjustment : std::uint8_t {
    none,
    bury,
    beard_thin,
    beard_box,
    encapsulate,
};

/// Java `JigsawJunction` (only the source position affects terrain).
struct jigsaw_junction {
    jint source_x{0};
    jint source_ground_y{0};
    jint source_z{0};
};

/// Java `Beardifier.Rigid`.
struct beardifier_rigid {
    bounding_box box{};
    terrain_adjustment adjustment{terrain_adjustment::none};
    jint ground_level_delta{0};
};

/// Java `Beardifier`: a `DensitySampler` added to the final density.
class beardifier final : public density::sampler {
  public:
    beardifier(std::vector<beardifier_rigid> pieces,
               std::vector<jigsaw_junction> junctions,
               std::optional<bounding_box> affected_box);

    /// The reference `Beardifier.EMPTY`.
    [[nodiscard]] static auto empty() -> std::unique_ptr<beardifier>;

    [[nodiscard]] auto sample_value(const density::sampler_context &context,
                                    jint block_x, jint block_y,
                                    jint block_z) const -> float override;
    void sample_volume(const density::sampler_context &context, buffer &output,
                       const volume &vol) const override;
    [[nodiscard]] auto range() const -> interval override;

    [[nodiscard]] auto kernel_entry(jint xi, jint yi, jint zi) const noexcept
        -> float;

  private:
    [[nodiscard]] auto sample_unchecked(jint block_x, jint block_y,
                                        jint block_z) const -> float;

    std::vector<beardifier_rigid> pieces_{};
    std::vector<jigsaw_junction> junctions_{};
    std::optional<bounding_box> affected_box_{};
};

} // namespace refrain::worldgen
