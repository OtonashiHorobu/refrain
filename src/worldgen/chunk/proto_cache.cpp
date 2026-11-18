// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/chunk/proto_cache.hpp>

#include <algorithm>
#include <vector>

namespace refrain::worldgen {

namespace {

/// Locks the cache mutex when the cache is shared. The guard owns a reference
/// to a mutex the cache does not own, so it stays trivial to construct.
class cache_lock {
  public:
    explicit cache_lock(std::mutex *lock) noexcept : lock_{lock} {
        if (lock_ != nullptr) {
            lock_->lock();
        }
    }
    cache_lock(const cache_lock &) = delete;
    auto operator=(const cache_lock &) -> cache_lock & = delete;
    cache_lock(cache_lock &&) = delete;
    auto operator=(cache_lock &&) -> cache_lock & = delete;
    ~cache_lock() {
        if (lock_ != nullptr) {
            lock_->unlock();
        }
    }

  private:
    std::mutex *lock_{nullptr};
};

} // namespace

auto proto_stage_name(proto_stage stage) -> std::string_view {
    switch (stage) {
    case proto_stage::terrain:
        return "terrain";
    case proto_stage::decorated:
        return "decorated";
    }
    return "terrain";
}

proto_chunk_cache::proto_chunk_cache(std::size_t capacity,
                                     std::mutex *lock) noexcept
    : capacity_{std::max<std::size_t>(capacity, 1U)}, lock_{lock} {}

auto proto_chunk_cache::stats() const noexcept -> proto_cache_stats {
    const cache_lock guard{lock_};
    std::size_t terrain_entries = 0;
    for (const auto &[position, entry] : entries_) {
        (void)position;
        if (entry.stage == proto_stage::terrain && !entry.sealed) {
            ++terrain_entries;
        }
    }
    return proto_cache_stats{
        .entries = entries_.size(),
        .terrain_entries = terrain_entries,
        .capacity = capacity_,
        .hits = hits_,
        .misses = misses_,
        .evictions = evictions_,
    };
}

void proto_chunk_cache::clear() {
    const cache_lock guard{lock_};
    entries_.clear();
    insertion_order_.clear();
}

auto proto_chunk_cache::find(const chunk_pos &pos) -> proto_cache_entry * {
    const cache_lock guard{lock_};
    const auto found = entries_.find(pos);
    if (found == entries_.end()) {
        ++misses_;
        return nullptr;
    }
    ++hits_;
    return &found->second;
}

auto proto_chunk_cache::find(const chunk_pos &pos) const
    -> const proto_cache_entry * {
    const cache_lock guard{lock_};
    const auto found = entries_.find(pos);
    return found == entries_.end() ? nullptr : &found->second;
}

auto proto_chunk_cache::contains(const chunk_pos &pos) const noexcept -> bool {
    const cache_lock guard{lock_};
    return entries_.contains(pos);
}

auto proto_chunk_cache::insert(const chunk_pos &pos,
                               std::unique_ptr<proto_chunk> chunk)
    -> proto_cache_entry & {
    const cache_lock guard{lock_};
    if (lock_ == nullptr) {
        // A private cache keeps its bound by dropping the oldest chunk; the
        // chunk's writes are reproducible on the single-generator paths that
        // use it, so insertion-order eviction is safe there.
        evict_over_capacity();
    }
    insertion_order_.push_back(pos);
    auto [found, inserted] =
        entries_.try_emplace(pos, proto_cache_entry{.chunk = std::move(chunk)});
    return found->second;
}

auto proto_chunk_cache::erase(const chunk_pos &pos) -> bool {
    const cache_lock guard{lock_};
    const auto erased = entries_.erase(pos);
    if (erased == 0U) {
        return false;
    }
    std::erase(insertion_order_, pos);
    return true;
}

auto proto_chunk_cache::evict_terrain_if(
    const std::function<bool(const chunk_pos &)> &keep) -> std::size_t {
    const cache_lock guard{lock_};
    std::vector<chunk_pos> victims;
    for (const auto &[position, entry] : entries_) {
        if (entry.stage == proto_stage::terrain && !entry.sealed &&
            !keep(position)) {
            victims.push_back(position);
        }
    }
    std::ranges::sort(victims);
    for (const auto &position : victims) {
        entries_.erase(position);
        std::erase(insertion_order_, position);
    }
    evictions_ += victims.size();
    return victims.size();
}

void proto_chunk_cache::evict_over_capacity() {
    while (entries_.size() >= capacity_ && !insertion_order_.empty()) {
        entries_.erase(insertion_order_.front());
        insertion_order_.pop_front();
        ++evictions_;
    }
}

} // namespace refrain::worldgen
