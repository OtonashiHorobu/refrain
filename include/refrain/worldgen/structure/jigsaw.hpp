// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The `minecraft:jigsaw` structure description: template pools, pool aliases,
// start heights and the element payload a jigsaw piece carries. Pools are
// expanded into their weighted selection table once at load time; every draw
// then matches the reference's pool selection.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/data/json.hpp>
#include <refrain/worldgen/core/java_compat.hpp>
#include <refrain/worldgen/core/rng.hpp>
#include <refrain/worldgen/structure/placement_types.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::worldgen {
struct block_pos;
} // namespace refrain::worldgen

namespace refrain::worldgen::structure {

class structure_template_manager;

/// Pool element projection. `terrain_matching` elements carry the
/// gravity processor that drops every block onto the world surface.
enum class pool_projection : std::uint8_t {
    rigid = 0,
    terrain_matching = 1,
};

/// Pool element kinds. Feature elements place a placed feature at the
/// piece position instead of template blocks.
enum class pool_element_kind : std::uint8_t {
    empty = 0,
    single = 1,
    legacy_single = 2,
    list = 3,
    feature = 4,
};

struct pool_element {
    pool_element_kind kind{pool_element_kind::empty};
    pool_projection projection{pool_projection::rigid};
    /// Template location for `single` and `legacy_single` elements.
    std::string template_id{};
    /// Processor list key; empty means the empty list. An element may instead
    /// carry an inline processor list, kept in `inline_processors`.
    std::string processor_list{};
    std::shared_ptr<const data::json::value> inline_processors{};
    /// Placed feature key for `feature` elements.
    std::string feature{};
    std::optional<liquid_settings> liquid_override{};
    /// `list` children. A list takes the first child's jigsaw blocks and the
    /// union of the children's bounding boxes.
    std::vector<pool_element> elements{};
};

struct weighted_pool_element {
    pool_element element{};
    jint weight{1};
};

/// One loaded `worldgen/template_pool` document. `templates` is the weighted
/// expansion: a pool of size `templates.size()` whose entries are indices into
/// `raw_elements`.
struct jigsaw_template_pool {
    std::string id{};
    std::string fallback{};
    std::vector<weighted_pool_element> raw_elements{};
    std::vector<std::size_t> templates{};
    mutable std::optional<jint> max_size{};

    void build_index();
    [[nodiscard]] auto size() const -> jint {
        return static_cast<jint>(templates.size());
    }
    [[nodiscard]] auto empty() const -> bool { return templates.empty(); }
    [[nodiscard]] auto at(std::size_t index) const -> const pool_element & {
        return raw_elements[templates[index]].element;
    }
    /// One uniform draw over the expanded table.
    [[nodiscard]] auto get_random_template(worldgen_random &random) const
        -> const pool_element *;
    /// One shuffled copy of the expanded table.
    [[nodiscard]] auto get_shuffled_templates(worldgen_random &random) const
        -> std::vector<const pool_element *>;
    /// `getMaxSize`: the tallest bounding box of any non-empty element at the
    /// origin with no rotation.
    [[nodiscard]] auto get_max_size(structure_template_manager &manager) const
        -> jint;
};

/// A pool alias binding. Aliases are resolved once per structure start from
/// start position and the level seed.
struct pool_alias_binding {
    enum class kind : std::uint8_t { direct = 0, random = 1, random_group = 2 };

    struct weighted_target {
        std::string target{};
        jint weight{1};
    };
    struct group {
        std::vector<pool_alias_binding> bindings{};
        jint weight{1};
    };

    kind type{kind::direct};
    std::string alias{};
    std::string target{};
    std::vector<weighted_target> targets{};
    std::vector<group> groups{};
};

/// Resolves alias -> target over the bindings with a positional random at the
/// start position.
class pool_alias_lookup {
  public:
    pool_alias_lookup() = default;

    [[nodiscard]] static auto
    create(const std::vector<pool_alias_binding> &bindings,
           const block_pos &pos, jlong seed) -> pool_alias_lookup;

    [[nodiscard]] auto lookup(std::string_view pool) const -> std::string;

  private:
    std::vector<std::pair<std::string, std::string>> aliases_{};
};

/// A height anchor: an absolute height or an offset from the dimension bottom,
/// top or sea level.
enum class height_anchor : std::uint8_t {
    absolute = 0,
    above_bottom = 1,
    below_top = 2,
    relative_to_sea_level = 3,
};

struct height_bound {
    jint value{0};
    height_anchor anchor{height_anchor::absolute};
};

/// Height provider subset: a constant or uniform inclusive height.
struct height_provider {
    enum class kind : std::uint8_t { constant = 0, uniform = 1 };

    kind type{kind::constant};
    height_bound min{};
    height_bound max{};

    [[nodiscard]] auto sample(worldgen_random &random, jint min_y, jint height,
                              jint sea_level) const -> jint;
};

/// Decodes a height provider: a bare vertical anchor (constant height) or a
/// constant/uniform provider document.
[[nodiscard]] auto decode_height_provider(const data::json::value &document)
    -> result<height_provider>;

/// Decoded fields of a `minecraft:jigsaw` structure document.
struct jigsaw_structure_config {
    std::string start_pool{};
    /// `start_jigsaw_name`; empty when the field is absent.
    std::string start_jigsaw_name{};
    jint max_depth{6};
    height_provider start_height{};
    bool use_expansion_hack{true};
    /// `project_start_to_heightmap`; empty when the field is absent.
    std::string project_start_to_heightmap{};
    jint max_distance_horizontal{80};
    jint max_distance_vertical{384};
    std::vector<pool_alias_binding> pool_aliases{};
    jint padding_bottom{0};
    jint padding_top{0};
    liquid_settings liquid{liquid_settings::apply_waterlogging};
};

/// A jigsaw connector direction, in face order.
enum class jigsaw_front : std::uint8_t {
    down = 0,
    up = 1,
    north = 2,
    south = 3,
    west = 4,
    east = 5,
};

/// One jigsaw block's connection parameters.
struct jigsaw_connector {
    jigsaw_front front{jigsaw_front::north};
    jigsaw_front top{jigsaw_front::up};
    bool rollable{false};
    /// `false` models a feature element's unnamed anchor.
    bool has_name{false};
    std::string name{};
    std::string target{};
};

/// Whether two connectors attach: opposite fronts, a matching top for aligned
/// joints, and the source target naming the destination when it is named.
[[nodiscard]] auto connectors_attach(const jigsaw_connector &source,
                                     const jigsaw_connector &target) -> bool;

/// One template element a jigsaw piece places. A `list` element carries its
/// children; placement visits them in order at the same origin and rotation.
struct jigsaw_element_info {
    pool_element_kind kind{pool_element_kind::single};
    pool_projection projection{pool_projection::rigid};
    std::string template_id{};
    std::string processor_list{};
    std::shared_ptr<const data::json::value> inline_processors{};
    std::string feature{};
    std::optional<liquid_settings> liquid_override{};
    std::vector<jigsaw_element_info> elements{};
};

/// The beardifier contribution a connection records on both of its pieces.
struct jigsaw_junction_info {
    jint source_x{0};
    jint source_ground_y{0};
    jint source_z{0};
    jint delta_y{0};
    pool_projection dest_projection{pool_projection::rigid};

    friend constexpr auto operator<=>(const jigsaw_junction_info &,
                                      const jigsaw_junction_info &) = default;
};

/// The payload a jigsaw piece carries into chunk placement.
struct jigsaw_piece_data {
    jigsaw_element_info element{};
    jint ground_level_delta{1};
    liquid_settings liquid{liquid_settings::apply_waterlogging};
    std::vector<jigsaw_junction_info> junctions{};
};

/// Decodes the jigsaw fields of a structure document. Missing optional fields
/// take the reference defaults.
[[nodiscard]] auto
decode_jigsaw_structure_config(const data::json::value &document)
    -> result<jigsaw_structure_config>;

[[nodiscard]] auto decode_pool_element(const data::json::value &document)
    -> result<pool_element>;

[[nodiscard]] auto decode_template_pool(std::string_view id,
                                        const data::json::value &document)
    -> result<jigsaw_template_pool>;

/// Resolves the alias bindings in order; later bindings win for a repeated
/// alias.
[[nodiscard]] auto decode_pool_alias_binding(const data::json::value &document)
    -> result<pool_alias_binding>;

} // namespace refrain::worldgen::structure
