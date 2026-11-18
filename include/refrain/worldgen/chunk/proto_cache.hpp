// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The working set of the per-chunk generation pipeline. A chunk is kept after
// its terrain pass so the neighbours that decorate around it read the same
// blocks instead of re-running their own terrain, and feature writes from one
// chunk stay visible to the chunks generated after it. A cache is either
// private to one generator instance, or shared by the workers of a scheduler
// through the mutex the scheduler passes in.

#pragma once

#include <refrain/worldgen/chunk/chunk_region.hpp>
#include <refrain/worldgen/chunk/proto_chunk.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace refrain::worldgen {

/// How far a cached chunk has advanced. A decorated chunk ran its own
/// structure and feature pass and read the writes of the neighbours processed
/// before it; it keeps accepting writes from the neighbours processed after
/// it until it is served.
enum class proto_stage : std::uint8_t {
    /// Biomes, fill, surface and carvers, with the final heightmaps primed.
    terrain = 0,
    /// The chunk's own structure pieces and features are placed.
    decorated = 1,
};

[[nodiscard]] auto proto_stage_name(proto_stage stage) -> std::string_view;

/// One cached chunk and the bookkeeping the pipeline needs besides its blocks.
struct proto_cache_entry {
    std::unique_ptr<proto_chunk> chunk{};
    proto_stage stage{proto_stage::terrain};
    /// The pieces of every structure start that reaches this chunk are placed.
    bool structures{false};
    /// A piece reached this chunk, so its border walls, stairs and fences need
    /// a repair once the neighbours' pieces are placed too.
    bool shape_dirty{false};
    /// The shape-sensitive blocks were repaired after the pieces landed.
    bool shapes{false};
    /// The chunk was handed to a caller. Writes into a served chunk are
    /// dropped, so a repeated request returns the same blocks while a later
    /// neighbour still reads them.
    bool sealed{false};
};

/// Counters a caller reads to confirm the cache is doing the work.
struct proto_cache_stats {
    std::size_t entries{0};
    /// Entries still at `terrain` stage: the only eviction candidates.
    std::size_t terrain_entries{0};
    std::size_t capacity{0};
    std::uint64_t hits{0};
    std::uint64_t misses{0};
    std::uint64_t evictions{0};
};

/// A bounded map from chunk position to cached proto chunk.
///
/// The cache has two ownership models. A per-generator cache is private to one
/// generator and not locked. A shared cache is owned by a scheduler and its
/// workers reach it through one instance with a mutex: the mutex is the lock
/// the scheduler names in its ownership contract. Shared mode never evicts on
/// insertion, because an eviction could erase a chunk another worker's
/// decoration region points into; the owner evicts between ordered steps with
/// `evict_terrain_if`. Only terrain entries are eviction candidates: their
/// content is a pure function of seed, data and position, while a decorated
/// chunk carries the feature writes its neighbours already made into it.
class proto_chunk_cache {
  public:
    /// One cached chunk costs about one terrain chunk (roughly 200 KiB
    /// for a full-height dimension), so the default bound is a few tens
    /// of megabytes per worker.
    static constexpr std::size_t default_capacity = 256;

    /// `lock` non-null makes the cache shared: every operation takes the mutex
    /// and insertion never evicts on its own. The mutex must outlive the
    /// cache.
    explicit proto_chunk_cache(std::size_t capacity = default_capacity,
                               std::mutex *lock = nullptr) noexcept;
    proto_chunk_cache(const proto_chunk_cache &) = delete;
    auto operator=(const proto_chunk_cache &) -> proto_chunk_cache & = delete;
    proto_chunk_cache(proto_chunk_cache &&) = delete;
    auto operator=(proto_chunk_cache &&) -> proto_chunk_cache & = delete;
    ~proto_chunk_cache() = default;

    [[nodiscard]] auto capacity() const noexcept -> std::size_t {
        return capacity_;
    }
    [[nodiscard]] auto size() const noexcept -> std::size_t {
        return entries_.size();
    }
    [[nodiscard]] auto empty() const noexcept -> bool {
        return entries_.empty();
    }
    [[nodiscard]] auto stats() const noexcept -> proto_cache_stats;
    void clear();

    /// Erases the terrain entries for which `keep` returns false. Returns the
    /// number erased. Only unsealed `terrain` entries are candidates, so the
    /// content of every decorated chunk and of every served snapshot is
    /// unaffected. The candidates are erased in ascending position order, so
    /// the eviction order never follows hash-map iteration.
    auto evict_terrain_if(const std::function<bool(const chunk_pos &)> &keep)
        -> std::size_t;

    /// Looks an entry up and counts the query. Returns null on a miss.
    [[nodiscard]] auto find(const chunk_pos &pos) -> proto_cache_entry *;
    /// Looks an entry up without counting the query.
    [[nodiscard]] auto find(const chunk_pos &pos) const
        -> const proto_cache_entry *;
    [[nodiscard]] auto contains(const chunk_pos &pos) const noexcept -> bool;

    /// Inserts a chunk that is not cached yet and returns its entry. A chunk
    /// of the same position must not be cached already. Evicts the oldest
    /// entries when the cache is over capacity.
    [[nodiscard]] auto insert(const chunk_pos &pos,
                              std::unique_ptr<proto_chunk> chunk)
        -> proto_cache_entry &;

    auto erase(const chunk_pos &pos) -> bool;

  private:
    void evict_over_capacity();

    std::size_t capacity_{default_capacity};
    /// Non-null on a shared cache: the mutex every operation takes.
    std::mutex *lock_{nullptr};
    std::unordered_map<chunk_pos, proto_cache_entry, chunk_pos_hash> entries_{};
    std::deque<chunk_pos> insertion_order_{};
    std::uint64_t hits_{0};
    std::uint64_t misses_{0};
    std::uint64_t evictions_{0};
};

} // namespace refrain::worldgen
