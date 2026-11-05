// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Event bus implementation.
//
// Thread-safety contract: the bus is owned by a single thread (the owner /
// kernel thread). subscribe/unsubscribe/publish/post/dispatch must all be
// called from that thread; workers hand messages back through queues instead of
// touching the bus directly. `dispatch()` is not re-entrant: a deferred
// callback must not call dispatch() again.
//
// Mutation from inside a listener is supported: removals are deferred
// (tombstoned) until the outermost publish returns, so unsubscribing a listener
// - including the one currently running - can never shift the iteration cursor
// and skip the listener that follows it. Subscriptions added during a
// publish do not receive the in-flight event (the listener count is snapshotted
// when the publish starts).
//
// Event types are RTTI-free: `type_of<Event>()` in event.hpp is a compile-time
// hash of the compiler-canonical type name, so -fno-rtti builds keep working
// and the tag is stable across translation units and dynamic libraries.

#include <refrain/core/event.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <utility>

namespace refrain::core::events {

auto bus::subscribe(event_type type, listener fn, event_priority priority)
    -> subscription_id {
    const auto id{next_id_++};
    listeners_.push_back(entry{
        .id = id,
        .type = type,
        .callback = std::move(fn),
        .priority = priority,
    });
    // Keep the vector in dispatch order. During a publish the snapshot count
    // means appending is safe and the new listener misses the in-flight event;
    // sort after the outermost publish instead of shifting live entries.
    if (publish_depth_ == 0) {
        std::stable_sort(listeners_.begin(), listeners_.end(),
                         [](const entry &lhs, const entry &rhs) {
                             if (lhs.priority != rhs.priority) {
                                 return static_cast<std::int32_t>(
                                            lhs.priority) >
                                        static_cast<std::int32_t>(rhs.priority);
                             }
                             return lhs.id < rhs.id;
                         });
    }
    return id;
}

void bus::unsubscribe(subscription_id id) {
    // Unknown ids are a no-op so callers can turn unsubscribe into a blind
    // teardown step.
    for (auto it{listeners_.begin()}; it != listeners_.end(); ++it) {
        if (it->id != id) {
            continue;
        }
        if (publish_depth_ > 0) {
            // A publish is iterating this vector: erasing now would shift the
            // entries under the cursor and could skip the next listener.
            // Tombstone instead; the callback is released immediately so the
            // removed listener is never invoked again, and finish_publish()
            // compacts the vector once the outermost publish returns.
            it->alive = false;
            it->callback = nullptr;
            return;
        }
        listeners_.erase(it);
        return;
    }
}

auto bus::dispatch() -> std::size_t {
    auto executed{std::size_t{}};
    auto head{std::size_t{}};

    // deferred_ is re-read every iteration: events posted from a callback are
    // appended and drained by this same call, in post order (FIFO). Each task
    // is moved out before it runs, so a nested post that reallocates the vector
    // cannot invalidate the entry being executed. The per-dispatch budget stops
    // a listener that keeps re-posting from keeping the owner thread inside
    // dispatch() forever; the events still queued stay in deferred_ for the
    // next call.
    while (head < deferred_.size()) {
        if (max_events_per_dispatch_ != 0 &&
            executed >= max_events_per_dispatch_) {
            break;
        }
        auto task{std::move(deferred_[head])};
        ++head;
        if (task) {
            task();
            ++executed;
        }
    }
    // Drop only the drained prefix: events appended past the budget (or left
    // behind when a callback threw) must stay queued for the next dispatch.
    deferred_.erase(deferred_.begin(),
                    deferred_.begin() + static_cast<std::ptrdiff_t>(head));
    return executed;
}

void bus::set_max_events_per_dispatch(std::size_t max) noexcept {
    max_events_per_dispatch_ = max;
}

auto bus::pending() const noexcept -> std::size_t { return deferred_.size(); }

auto bus::listener_count(event_type type) const noexcept -> std::size_t {
    auto count{std::size_t{}};
    for (const auto &current : listeners_) {
        if (current.alive && current.type == type) {
            ++count;
        }
    }
    return count;
}

auto bus::subscribers() const noexcept -> std::size_t {
    auto count{std::size_t{}};
    for (const auto &current : listeners_) {
        if (current.alive) {
            ++count;
        }
    }
    return count;
}

void bus::clear() noexcept {
    if (publish_depth_ > 0) {
        // Called from a listener: tombstone every entry so the running publish
        // finishes without touching freed callbacks; the vector is compacted
        // once the outermost publish returns. Queued events are dropped now.
        for (auto &current : listeners_) {
            current.alive = false;
            current.callback = nullptr;
        }
        deferred_.clear();
        return;
    }
    listeners_.clear();
    deferred_.clear();
    // next_id_ is deliberately not reset: subscription ids stay unique for the
    // lifetime of the bus, so a stale id can never unsubscribe a later
    // listener.
}

void bus::publish_raw(event_type type, const void *event) {
    // Snapshot the listener count: subscriptions added by a callback must not
    // receive the event that is currently being published. The callback is
    // copied so a listener may unsubscribe itself without destroying the
    // std::function that is currently executing.
    const auto count{listeners_.size()};
    ++publish_depth_;
    try {
        for (std::size_t index{}; index < count && index < listeners_.size();
             ++index) {
            const auto &current{listeners_[index]};
            if (!current.alive || current.type != type) {
                continue;
            }
            const auto callback{current.callback};
            if (!callback) {
                continue; // an empty listener would throw
                          // std::bad_function_call
            }
            callback(event);
        }
    } catch (...) {
        finish_publish();
        throw;
    }
    finish_publish();
}

void bus::finish_publish() noexcept {
    if (publish_depth_ > 0) {
        --publish_depth_;
    }
    if (publish_depth_ != 0) {
        return; // a nested publish still owns the iteration
    }
    listeners_.erase(
        std::remove_if(listeners_.begin(), listeners_.end(),
                       [](const entry &current) { return !current.alive; }),
        listeners_.end());
}

void bus::post_raw(std::function<void()> fn) {
    deferred_.push_back(std::move(fn));
}

} // namespace refrain::core::events
