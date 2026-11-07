// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Datapack material rules (`worldgen/material_rule`) and conditions
// (`worldgen/material_condition`). The decoder produces a neutral tree with
// references left as ids; compilation against a chunk context happens in
// `surface_system`.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/worldgen/core/java_compat.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::worldgen::surface {

/// `VerticalAnchor`: a Y coordinate resolved against the generation bounds.
struct vertical_anchor {
    enum class kind : std::uint8_t {
        absolute,
        above_bottom,
        below_top,
        relative_to_sea_level,
    };

    kind type{kind::absolute};
    jint value{0};

    [[nodiscard]] auto resolve(jint min_gen_y, jint gen_depth,
                               jint sea_level) const noexcept -> jint;
};

/// `CaveSurface`.
enum class cave_surface : std::uint8_t { floor, ceiling };

/// One material condition. `kind::reference` defers to a document id; every
/// other kind is inline data.
struct condition {
    enum class kind : std::uint8_t {
        biome,
        noise_threshold,
        vertical_gradient,
        y_above,
        water,
        temperature,
        steep,
        not_,
        hole,
        above_preliminary_surface,
        stone_depth,
        reference,
    };

    kind type{kind::hole};

    /// biome: holder-set entries (keys, `#tags`, or lists flattened here).
    std::vector<std::string> biome_is{};

    /// noise_threshold.
    std::string noise{};
    double min_threshold{0.0};
    double max_threshold{0.0};
    bool is_3d{false};

    /// vertical_gradient.
    std::string random_name{};
    vertical_anchor true_at_and_below{};
    vertical_anchor false_at_and_above{};

    /// y_above / water / stone_depth.
    jint offset{0};
    jint surface_depth_multiplier{0};
    bool add_stone_depth{false};
    jint secondary_depth_range{0};
    bool add_surface_depth{false};
    cave_surface surface{cave_surface::floor};

    /// not.
    std::unique_ptr<condition> target{};

    /// reference.
    std::string reference{};
};

/// One material rule.
struct rule {
    enum class kind : std::uint8_t {
        block,
        bandlands,
        sequence,
        condition,
        ore_vein,
        reference,
    };

    kind type{kind::block};

    /// block / ore_vein: block state keys (`minecraft:stone`).
    std::string state{};
    std::string ore_block{};
    std::string raw_ore_block{};
    std::string filler_block{};
    float raw_ore_chance{0.0F};
    std::string density{};
    std::string richness{};
    std::string filler_gap{};

    /// sequence.
    std::vector<rule> sequence{};

    /// condition.
    std::unique_ptr<surface::condition> if_true{};
    std::unique_ptr<rule> then_run{};

    /// reference.
    std::string reference{};
};

/// Documents the decoder resolves references against. Ids are full
/// identifiers (`minecraft:overworld/surface`).
struct material_documents {
    std::vector<std::pair<std::string, std::string>> rules{};
    std::vector<std::pair<std::string, std::string>> conditions{};
};

/// Loads both document sets from `<data_root>/<namespace>/worldgen/...`.
[[nodiscard]] auto load_material_documents(std::string_view data_root)
    -> result<material_documents>;

/// Decodes one document body. Strings are references (kept as ids); inline
/// objects are decoded recursively.
[[nodiscard]] auto decode_rule(std::string_view json) -> result<rule>;
[[nodiscard]] auto decode_condition(std::string_view json) -> result<condition>;

} // namespace refrain::worldgen::surface
