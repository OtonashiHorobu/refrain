// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Feature pool elements place a placed feature while their piece is placed.
// The placed feature writes through the structure placement region, so it may
// cross the owning piece's chunk boundary exactly like `FeaturePoolElement`.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/worldgen/feature/feature_data.hpp>
#include <refrain/worldgen/feature/feature_registry.hpp>
#include <refrain/worldgen/feature/feature_stage.hpp>
#include <refrain/worldgen/structure/jigsaw.hpp>
#include <refrain/worldgen/structure/placement_stage.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::worldgen::structure {

/// Collects the placed-feature ids of every feature element in an element tree
/// or piece record.
void collect_feature_ids(const jigsaw_element_info &element,
                         std::vector<std::string> &out);
void collect_feature_ids(const structure_piece_record &piece,
                         std::vector<std::string> &out);

/// Places `feature_pool_element` entries through the feature stage.
///
/// Only the requested placed features and their configured-feature closure are
/// decoded, so unrelated documents that name feature types or state providers
/// outside the supported set do not block a structure generation. The feature
/// context takes its region, chunk and state table from the structure
/// placement context; the context has no climate source, so snow and freeze
/// probes see a non-freezing atmosphere.
class structure_feature_placer {
  public:
    structure_feature_placer(const structure_place_context &context,
                             const feature::feature_data_set &data);

    /// Decodes the listed placed features. A missing placed feature or
    /// configured reference is reported as `not_found`.
    [[nodiscard]] auto load(std::span<const std::string> ids) -> status;

    /// Runs one placed feature with the caller's random stream at `position`.
    /// Returns false when the id was not loaded or the feature placed nothing.
    [[nodiscard]] auto place(std::string_view id, worldgen_random &random,
                             const block_pos &position) -> bool;

  private:
    const structure_place_context *context_{nullptr};
    const feature::feature_data_set *data_{nullptr};
    feature::feature_registry registry_;
    feature::feature_context feature_context_{};
};

} // namespace refrain::worldgen::structure
