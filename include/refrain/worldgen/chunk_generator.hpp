// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The production full-chunk generation service. One object owns the terrain
// engine, the feature registry, the structure template manager and the mutable
// search state of a dimension, and shares the loaded immutable worldgen data
// with the other workers of a batch. Generating a chunk runs the stages in
// dependency order and returns the mutable chunk the caller can turn into a
// level chunk.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/chunk/proto_chunk.hpp>
#include <refrain/worldgen/dimension_generator.hpp>
#include <refrain/worldgen/feature/feature_registry.hpp>
#include <refrain/worldgen/structure/template.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace refrain::worldgen {

class stage_profile;

/// How far a generated chunk advances along the generation chain.
enum class generation_target : std::uint8_t {
    /// Biomes, fill, surface and carvers. The chunk holds no structures and
    /// no features.
    terrain = 0,
    /// Terrain plus structure starts, references, placed pieces and features.
    features = 1,
    /// The features target plus the final status.
    full = 2,
};

/// An inclusive rectangle in chunk coordinates. An empty rectangle has a
/// minimum beyond its maximum.
struct chunk_rectangle {
    jint min_x{0};
    jint min_z{0};
    jint max_x{-1};
    jint max_z{-1};

    [[nodiscard]] auto empty() const noexcept -> bool {
        return min_x > max_x || min_z > max_z;
    }
};

struct chunk_generator_config {
    /// The datapack `data` directory (`<version>/data`).
    std::string data_root{};
    /// Extracted structure templates, laid out as
    /// `<root>/<namespace>/structure/**.nbt`. An empty root still generates
    /// procedural pieces and features; template-backed pieces resolve to air.
    std::string templates_root{};
    jlong seed{0};
    dimension_kind kind{dimension_kind::overworld};
    /// Fixed biome key. Empty selects the dimension's biome source.
    std::string fixed_biome{};
    generation_target target{generation_target::full};
    /// Structure set ids that may generate. Empty enables every set; naming
    /// no existing set turns structure starts off.
    std::vector<std::string> structure_sets{};
    /// Reproduces the reference worlds of the structure oracle: terrain fill
    /// plus structure pieces only, with no surface pass, carvers or features.
    /// The chunk still reaches the configured target status.
    bool reference_world{false};
    /// Second reference mode for a `reference_world` run: keep the real
    /// surface pass and the dimension's carvers instead of stripping them.
    /// Features stay off because `reference_world` already disables them. Use
    /// `generation_target::terrain` when the reference world disables
    /// structures as well. A production run always executes the dimension's
    /// carvers.
    bool reference_carvers{false};
    /// Feature reference mode: decorate the target chunk and then the eight
    /// neighbour chunks through the same region. A reference world saves every
    /// chunk of a generated area, so its chunks contain the feature writes of
    /// their neighbours, which reach a few blocks across a border. Production
    /// generation decorates the target chunk only. Requires a fixed biome,
    /// because every decorated chunk uses the target's biome plan.
    bool decorate_neighbours{false};
    /// With `decorate_neighbours`, decorate the eight neighbours before the
    /// target chunk. The target's own writes then win where two chunks touch;
    /// the default decorates the target first, so neighbour writes win.
    bool neighbours_before_target{false};
    /// With `decorate_neighbours`, order the decorated chunks the way a
    /// force-load ticket queue does: ascending x, then ascending z. Every
    /// chunk that precedes the target in that order decorates first, then the
    /// target, then the chunks that follow. A saved reference world whose area
    /// was force-loaded contains exactly this interleaving, so a feature that
    /// writes across a border resolves contacts the same way.
    bool neighbours_in_generation_order{false};
    /// With `decorate_neighbours` and a `neighbour_box`, decorate the chunks
    /// in the order a force-load ticket queue runs them instead of ascending
    /// x and z. A ticket keeps its own two rings at the full status, so the
    /// task of every force-loaded chunk first runs the feature layer of the
    /// three-by-three around it; the propagated tickets two rings out then
    /// run one more layer each. `force_load_decoration_order` returns that
    /// sequence. A reference world built by force-loading a rectangle saves
    /// the whole area the tickets reached, and a feature that writes across a
    /// border resolves contacts in this order, so a single-chunk comparison
    /// of such a world needs it. Requires a non-empty `neighbour_box`.
    bool neighbours_ticket_sweep{false};
    /// With `decorate_neighbours`, how many chunks around the target are
    /// decorated. Zero decorates only the target, one the eight surrounding
    /// chunks (the default). Two adds the next ring, which a force-loaded
    /// reference area also generated: a feature of an outer chunk can read a
    /// chunk outside the target's three-by-three region, and a patch spread
    /// from there consumes a different number of random draws when that chunk
    /// reads as air. The runtime always generates one more terrain ring than
    /// it decorates, so every decorated chunk can read the chunks its own
    /// generation region covers, and it attaches no terrain outside the
    /// symmetric square. The value is limited to eight chunks.
    jint neighbour_radius{1};
    /// With `decorate_neighbours`, the reference world's generated chunk
    /// rectangle in absolute chunk coordinates. The whole rectangle decorates
    /// in force-load order, which reproduces features whose writes cross more
    /// than one border, such as a spreading sculk patch. Chunks outside the
    /// rectangle are attached as terrain only, because the reference world
    /// generated them as terrain dependencies without features. An empty
    /// rectangle keeps the symmetric `neighbour_radius` behaviour. The target
    /// chunk must lie inside the rectangle.
    chunk_rectangle neighbour_box{};
};

/// The chunks a force-load of `box` runs features for, in the order the
/// ticket queue runs them. Every chunk of the rectangle is force-loaded in
/// ascending x and then z; each ticket keeps its own two rings at the full
/// status, so its generation task runs the feature layer of the three-by-three
/// around it, and the tickets propagated two rings out run one more layer
/// each. The result covers the rectangle expanded by three chunks.
[[nodiscard]] auto force_load_decoration_order(const chunk_rectangle &box)
    -> std::vector<chunk_pos>;

/// A reusable full-chunk generator. The object is not thread-safe: a single
/// instance must not generate concurrently. The batch entry points create one
/// independent instance per worker, so no random stream, block-state registry,
/// biome search or template cache is shared between workers. The loaded,
/// immutable datapack data is shared by reference.
class chunk_generator {
  public:
    [[nodiscard]] static auto create(chunk_generator_config config)
        -> result<std::unique_ptr<chunk_generator>>;

    /// Runs the configured stages for one chunk. Calling it again with the
    /// same position produces the same chunk: every multi-noise search is
    /// reset before the chunk's first biome is selected. The returned chunk
    /// borrows this object's block state table, so it must not outlive the
    /// generator.
    [[nodiscard]] auto generate(const chunk_pos &pos)
        -> result<std::unique_ptr<proto_chunk>>;

    /// Generates `positions` in parallel and returns the chunks in input
    /// order. `workers` is clamped to the position count; zero selects the
    /// hardware concurrency. Every worker owns its mutable state and shares
    /// only immutable data, so the result is identical to serial generation.
    /// Worker instances are kept for later batches, so returned chunks stay
    /// valid as long as this object does.
    [[nodiscard]] auto generate_batch(std::span<const chunk_pos> positions,
                                      std::size_t workers = 0)
        -> result<std::vector<std::unique_ptr<proto_chunk>>>;

    /// Generates a `width` by `depth` rectangle of chunks starting at
    /// `origin`, x outer to the inner z loop, and returns them in that order.
    [[nodiscard]] auto generate_area(chunk_pos origin, jint width, jint depth,
                                     std::size_t workers = 0)
        -> result<std::vector<std::unique_ptr<proto_chunk>>>;

    [[nodiscard]] auto config() const noexcept
        -> const chunk_generator_config & {
        return config_;
    }
    [[nodiscard]] auto profile() const noexcept -> const dimension_profile & {
        return dimension_->profile();
    }
    /// Biome keys indexed by the ids the generated chunks carry. The storage
    /// bridge maps those ids to names.
    [[nodiscard]] auto biome_names() const -> const std::vector<std::string> & {
        return dimension_->biome_source().biomes();
    }

    [[nodiscard]] auto states() noexcept -> block_state_registry & {
        return dimension_->states();
    }
    [[nodiscard]] auto target() const noexcept -> generation_target {
        return config_.target;
    }

    /// Records the wall-clock time of every stage of the chunks generated
    /// afterwards into `profile`. Null disables recording. The profile never
    /// changes the generated chunk. A profile is not thread-safe, so each
    /// worker needs its own instance.
    void set_stage_profile(stage_profile *profile) noexcept {
        profile_ = profile;
    }

  private:
    /// Loaded data every worker of one configuration shares. It is immutable
    /// after `create`, so workers reference it instead of parsing the datapack
    /// again. Per-worker state (block-state registry, biome search, template
    /// cache, start-probe caches) stays in the generator instance.
    struct shared_generator_data;

    chunk_generator(
        chunk_generator_config config,
        std::shared_ptr<const shared_generator_data> shared,
        std::unique_ptr<dimension_generator> dimension,
        std::unique_ptr<feature::feature_registry> features,
        std::unique_ptr<structure::structure_template_manager> templates);

    [[nodiscard]] static auto
    create_with_shared(chunk_generator_config config,
                       std::shared_ptr<const shared_generator_data> shared)
        -> result<std::unique_ptr<chunk_generator>>;

    [[nodiscard]] auto generate_impl(const chunk_pos &pos)
        -> result<std::unique_ptr<proto_chunk>>;

    /// One cached unbearded start column: the six heightmap rows the starts
    /// stage reads. The column is filled at `biomes` status, so the four final
    /// heightmaps stay at their empty value, exactly as a freshly generated
    /// column reports them.
    struct start_height_column {
        std::array<std::array<jint, 16 * 16>, heightmap_kind_count> heights{};
    };

    /// One vertical block column a structure start reads. The fossil start
    /// scans a single world column, so the cache stores that column instead of
    /// the generated chunk.
    struct block_column_key {
        chunk_pos chunk{};
        jint x{0};
        jint z{0};

        friend constexpr auto operator==(const block_column_key &,
                                         const block_column_key &)
            -> bool = default;
    };

    struct chunk_pos_hash {
        [[nodiscard]] auto operator()(const chunk_pos &pos) const noexcept
            -> std::size_t {
            const auto x =
                static_cast<std::uint64_t>(static_cast<std::uint32_t>(pos.x));
            const auto z =
                static_cast<std::uint64_t>(static_cast<std::uint32_t>(pos.z));
            return static_cast<std::size_t>(
                (x * 0x9E3779B97F4A7C15ULL) ^
                (z + 0x165667B19E3779F9ULL + (x << 6U) + (x >> 2U)));
        }
    };

    struct block_column_hash {
        [[nodiscard]] auto
        operator()(const block_column_key &key) const noexcept -> std::size_t {
            const auto base = chunk_pos_hash{}(key.chunk);
            return base ^ (static_cast<std::size_t>(
                               static_cast<std::uint32_t>(key.x)) *
                               0x9E3779B97F4A7C15ULL +
                           static_cast<std::size_t>(
                               static_cast<std::uint32_t>(key.z)));
        }
    };

    /// Returns the cached column, generating it on the first request. The
    /// cache is per generator instance, so its size covers the sliding start
    /// window of a contiguous batch without growing with the batch length.
    /// Worker instances for a batch: this generator plus the extra ones the
    /// previous batches created, created up to `worker_count` if needed.
    [[nodiscard]] auto worker_instances(std::size_t worker_count)
        -> result<std::vector<chunk_generator *>>;
    [[nodiscard]] auto start_height_at(const chunk_pos &column)
        -> result<const start_height_column *>;
    /// Unbearded block state of one position. Reads a cached vertical column
    /// and only generates the height column when that block column is new.
    [[nodiscard]] auto start_block_at(const chunk_pos &column, jint x, jint y,
                                      jint z) -> result<block_state_id>;
    auto store_height_column(const chunk_pos &column, start_height_column value)
        -> const start_height_column *;
    auto store_block_column(block_column_key key,
                            std::vector<block_state_id> states)
        -> const std::vector<block_state_id> *;

    /// Pristine terrain of one neighbour, or null when it is not cached. The
    /// returned chunk is shared and must be cloned before a stage writes to
    /// it.
    [[nodiscard]] auto cached_terrain(const chunk_pos &pos) const
        -> const proto_chunk *;
    /// Keeps a pristine terrain chunk for the neighbour fills of later
    /// targets. Chunks that carry a beardifier are never stored, because their
    /// fill depends on the structure pieces around the target rather than on
    /// the position alone.
    void store_terrain(const chunk_pos &pos,
                       std::unique_ptr<proto_chunk> chunk);
    /// Number of pristine chunks a worker keeps. A target fills its eight
    /// neighbours, so the sliding window of a batch is at most three columns
    /// of chunks; the bound covers that window without growing with the batch.
    static constexpr std::size_t terrain_cache_capacity = 64;
    std::unordered_map<chunk_pos, std::unique_ptr<proto_chunk>, chunk_pos_hash>
        terrain_cache_{};
    std::deque<chunk_pos> terrain_cache_order_{};

    chunk_generator_config config_{};
    std::shared_ptr<const shared_generator_data> shared_{};
    std::unique_ptr<dimension_generator> dimension_{};
    std::unique_ptr<feature::feature_registry> features_{};
    std::unique_ptr<structure::structure_template_manager> templates_{};
    /// Extra generators created by the batch entry points. They share
    /// `shared_` and keep their registries alive for the chunks they returned.
    std::vector<std::unique_ptr<chunk_generator>> workers_{};
    /// Bounded per-worker caches for the structure-start probes. Height
    /// columns are pure functions of the seed and data, so reusing them does
    /// not change the generated chunk.
    std::unordered_map<chunk_pos, start_height_column, chunk_pos_hash>
        height_columns_{};
    std::deque<chunk_pos> height_column_order_{};
    std::unordered_map<block_column_key, std::vector<block_state_id>,
                       block_column_hash>
        block_columns_{};
    std::deque<block_column_key> block_column_order_{};
    /// Optional per-stage accounting owned by the caller. Null in production.
    stage_profile *profile_{nullptr};
    bool has_structures_{false};
    bool has_features_{false};
};

} // namespace refrain::worldgen
