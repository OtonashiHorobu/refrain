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
#include <set>
#include <span>
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
    /// A structure piece or feature wrote into the chunk while it was still at
    /// terrain stage. Its content is then no longer reproducible by filling
    /// the chunk alone, so it is not an eviction candidate until it is
    /// decorated.
    bool written{false};
};

/// Counters a caller reads to confirm the cache is doing the work.
///
/// Every field is maintained while the cache mutex is held, so a reader can
/// poll the counters while workers generate.
struct proto_cache_stats {
    std::size_t entries{0};
    /// Unsealed entries still at `terrain` stage, written or not.
    std::size_t terrain_entries{0};
    /// Unsealed, unwritten terrain entries: the reproducible fills, and the
    /// only eviction candidates.
    std::size_t pristine_entries{0};
    std::size_t capacity{0};
    /// High-water mark of `entries` since construction. The scheduler reports
    /// it so a bound the cache documents can be checked against a real run
    /// instead of against the counter's value after the work drained.
    std::size_t peak_entries{0};
    /// High-water mark of `terrain_entries`.
    std::size_t peak_terrain_entries{0};
    /// High-water mark of `pristine_entries`, recorded after insertion
    /// evicted back down to `capacity`. A reader never observes more than
    /// `capacity` entries: the insertion transients and the pin caveat are
    /// stated on the class, and `stats` reads under the same mutex as the
    /// insertion.
    std::size_t peak_pristine_entries{0};
    std::uint64_t hits{0};
    std::uint64_t misses{0};
    std::uint64_t evictions{0};
    /// Entries the owner unloaded through `unload_if`, separately from the
    /// eviction rule that bounds the terrain working set.
    std::uint64_t unloads{0};
};

/// A bounded map from chunk position to cached proto chunk.
///
/// The cache has two ownership models. A per-generator cache is private to one
/// generator and not locked; it is touched by one thread at a time, so it
/// evicts in insertion order and the caller keeps its bound above the working
/// set. A shared cache is owned by a scheduler and its workers reach it through
/// one instance with a mutex: the mutex is the lock the scheduler names in its
/// ownership contract. Only terrain entries are eviction candidates, while a
/// decorated chunk carries the feature writes its neighbours already made into
/// it.
///
/// Eviction never changes served content. Only two kinds of terrain entry are
/// candidates: an entry the owner unloads with `unload_if` because it lies
/// outside the demand window, and an unwritten terrain entry that the owner can
/// reproduce. Reproduction is not the chunk's own seed noise alone: a terrain
/// fill may carry structure beard pieces, and the scheduler refills an evicted
/// position from the starts window of the demand that first published it, so
/// the refill recomputes the same beard. A terrain entry that already received
/// a structure piece or a neighbour feature write is marked written and is kept
/// until the pipeline decorates it, so a write can never be dropped by an
/// eviction. The scheduler pins the dependency window of its ordered decoration
/// lane while it reads it; a pinned entry is not a candidate, so a pointer
/// handed out by `find` stays valid for the job.
///
/// The bound is a scheduler-level guarantee, not a property of the cache in
/// isolation. The scheduler holds at most one five-by-five dependency window
/// (25 entries) pinned and clamps `capacity` to at least 25, so its inserts
/// alone can not hide enough pristine entries from eviction to overshoot the
/// bound: an insertion evicts pristine entries in ascending position order
/// until the count is back at `capacity`. Inside one insertion the count is
/// transiently `capacity + 1` before that eviction runs; `stats` never observes
/// that state because it reads under the same mutex. A caller that pins more
/// entries than `capacity` can exceed the bound, and a caller that uses the
/// cache directly without that pinning discipline must size `capacity` above
/// its pinned set. The written dependency ring and the decorated entries sit
/// outside the number altogether; the owner states them separately and drops
/// them with `unload_if` when the demand window moves. The per-generator
/// private cache keeps its stricter but lossy insertion-order eviction: its
/// caller must hold the bound above the working set it needs.
class proto_chunk_cache {
  public:
    /// One cached chunk costs about one terrain chunk (roughly 200 KiB
    /// for a full-height dimension), so the default bound is a few tens
    /// of megabytes of terrain working set per scheduler.
    static constexpr std::size_t default_capacity = 256;

    /// `lock` non-null makes the cache shared: every operation takes the mutex,
    /// and insertion evicts unpinned pristine entries to keep the bound. The
    /// mutex must outlive the cache.
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

    /// Erases every unpinned entry for which `drop` returns true and returns
    /// the number erased. The scheduler uses it to unload entries outside its
    /// demand window; a pinned entry is skipped and reconsidered by the next
    /// call. Candidates are erased in ascending position order.
    auto unload_if(const std::function<bool(const chunk_pos &)> &drop)
        -> std::size_t;

    /// Protects the entries of `positions` from eviction until the matching
    /// `unpin`. The ordered decoration lane pins the dependency window it is
    /// about to read, so an insertion on another worker cannot erase a chunk
    /// under its pointers. A position may be pinned before it is cached: the
    /// pin also covers the insertion that fills it.
    void pin(std::span<const chunk_pos> positions);
    void unpin(std::span<const chunk_pos> positions);

    /// Advances a cached entry to `decorated` stage under the cache mutex. The
    /// stage is part of the published statistics, so it is written here rather
    /// than through a `find` pointer.
    auto mark_decorated(const chunk_pos &pos) -> bool;
    /// Seals a cached entry under the cache mutex: writes into a served chunk
    /// are dropped, and the entry leaves the terrain statistics.
    auto seal(const chunk_pos &pos) -> bool;

    /// Marks cached entries as written, so their content is no longer
    /// reproducible from the fill alone and an eviction must keep it. The
    /// ordered lane calls it for the chunks a structure or feature write
    /// reached; a written entry stops being an eviction candidate until it is
    /// decorated.
    void mark_written(std::span<const chunk_pos> positions);

    /// Looks an entry up and counts the query. Returns null on a miss.
    [[nodiscard]] auto find(const chunk_pos &pos) -> proto_cache_entry *;
    /// Looks an entry up without counting the query.
    [[nodiscard]] auto find(const chunk_pos &pos) const
        -> const proto_cache_entry *;
    [[nodiscard]] auto contains(const chunk_pos &pos) const noexcept -> bool;

    /// Inserts a chunk that is not cached yet and returns its entry. A chunk of
    /// the same position must not be cached already. A private cache evicts the
    /// oldest entries when the cache is over capacity. A shared cache evicts
    /// pristine entries in ascending position order until its pristine bound
    /// holds again; written and pinned entries are never chosen. The returned
    /// pointer is null when the cache dropped the inserted chunk in that pass.
    [[nodiscard]] auto insert(const chunk_pos &pos,
                              std::unique_ptr<proto_chunk> chunk)
        -> proto_cache_entry *;

    auto erase(const chunk_pos &pos) -> bool;

  private:
    void evict_over_capacity();
    /// Evicts pristine entries in ascending position order until the pristine
    /// count is back at `capacity_`.
    void evict_pristine_over_capacity();
    /// Erases `pos`; the mutex is held and the entry is never pinned.
    void erase_entry(const chunk_pos &pos);
    /// Recomputes whether `pos` is an eviction candidate.
    void refresh_candidate(const chunk_pos &pos);

    std::size_t capacity_{default_capacity};
    /// Non-null on a shared cache: the mutex every operation takes.
    std::mutex *lock_{nullptr};
    std::unordered_map<chunk_pos, proto_cache_entry, chunk_pos_hash> entries_{};
    std::deque<chunk_pos> insertion_order_{};
    /// Unsealed, unwritten, unpinned terrain entries, ascending position. An
    /// eviction takes its victims from this set, so the choice never depends on
    /// hash-map order or on which worker inserted first.
    std::set<chunk_pos> candidates_{};
    /// Pin counts of positions the ordered lane reads. A pinned position may
    /// not be cached yet.
    std::unordered_map<chunk_pos, std::size_t, chunk_pos_hash> pins_{};
    std::size_t terrain_entries_{0};
    std::size_t pristine_entries_{0};
    std::size_t peak_entries_{0};
    std::size_t peak_terrain_entries_{0};
    std::size_t peak_pristine_entries_{0};
    std::uint64_t hits_{0};
    std::uint64_t misses_{0};
    std::uint64_t evictions_{0};
    std::uint64_t unloads_{0};
};

} // namespace refrain::worldgen
