// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::core::events {

/// Process-wide type tag without RTTI (`-fno-rtti` builds included).
///
/// The tag is a 64-bit FNV-1a hash of the compiler-canonical name of `Event`,
/// *not* the address of a per-type static. An address tag is only unique
/// within one link image: a `dlopen`ed mod that instantiates
/// `type_of<Event>()` itself can end up with a different address than the host
/// (especially with `-fvisibility=hidden`), silently breaking subscriber
/// matching across the dynamic-library boundary. Hashing the canonical type
/// name (the name the compiler prints in the function signature) yields the
/// same tag in every translation unit and every loaded image built with the
/// same compiler and ABI. Aliases of the same type share the tag because the
/// compiler prints the canonical type; a type that is *declared differently*
/// in two images (an ODR violation) is a different tag, which is the safe
/// failure mode.
///
/// 64 bits make an accidental collision negligible for the number of event
/// types a process carries; the tag is a dispatch key, not a security
/// boundary.
using event_type = std::uint64_t;

using subscription_id = std::uint64_t;

/// Dispatch order: higher priority first; equal priorities keep registration
/// order (stable). Default subscriptions use `normal`.
enum class event_priority : std::int32_t {
    low = 0,
    normal = 100,
    high = 200,
    monitor = 1000,
};

namespace detail {

/// Constexpr FNV-1a (64-bit): stable across compilers/standard libraries
/// because it is spelled out here instead of relying on `std::hash`, whose
/// value is implementation-defined and not stable across builds.
[[nodiscard]] constexpr auto fnv1a_64(std::string_view text) noexcept
    -> std::uint64_t {
    std::uint64_t hash{14695981039346656037ULL};
    for (const char character : text) {
        hash ^= static_cast<std::uint64_t>(
            static_cast<unsigned char>(character)); // NOLINT
        hash *= 1099511628211ULL;
    }
    return hash;
}

} // namespace detail

/// Stable compile-time tag for an event type. `consteval` guarantees the hash
/// is baked into every image at build time (there is no runtime registry to
/// initialize and no address identity involved).
template <typename Event>
[[nodiscard]] consteval auto type_of() noexcept -> event_type {
#if defined(_MSC_VER) && !defined(__clang__)
    return detail::fnv1a_64(__FUNCSIG__);
#else
    return detail::fnv1a_64(__PRETTY_FUNCTION__);
#endif
}

/// Deterministic, ordered, single-threaded event bus used between kernel,
/// providers and mods.
///
/// Dispatch contract:
///  - listeners are invoked synchronously in subscription order,
///  - `post` queues an event for a `dispatch()` call,
///  - the bus itself is not thread-safe: publish/post/dispatch from the owner
///    thread only; workers hand messages back through queues.
///
/// Mutation during a publish (`subscribe`/`unsubscribe`/`clear` from inside a
/// listener) is supported:
///  - a listener removed by an earlier callback in the same publish is not
///    invoked (removal is deferred so the iteration cannot skip the listener
///    that follows it).
///  - a listener subscribed during a publish does not receive the event that
///    is currently being published,
///  - `post()` from a listener is queued and dispatched by the *same*
///    `dispatch()` call (in post order), bounded by the per-dispatch event
///    budget below.
///
/// The bus owns callbacks that capture `this` (deferred tasks); copying or
/// moving it would duplicate listeners and leave closures pointing at the
/// source object, so both operations are deleted.
class bus {
  public:
    using listener = std::function<void(const void *event)>;

    bus() = default;
    bus(const bus &) = delete;
    auto operator=(const bus &) -> bus & = delete;
    bus(bus &&) = delete;
    auto operator=(bus &&) -> bus & = delete;
    ~bus() = default;

    [[nodiscard]] auto
    subscribe(event_type type, listener fn,
              event_priority priority = event_priority::normal)
        -> subscription_id;

    template <typename Event, typename Fn>
    [[nodiscard]] auto subscribe(Fn &&fn) -> subscription_id {
        return subscribe<Event>(event_priority::normal, std::forward<Fn>(fn));
    }

    template <typename Event, typename Fn>
    [[nodiscard]] auto subscribe(event_priority priority, Fn &&fn)
        -> subscription_id {
        return subscribe(
            type_of<Event>(),
            [callback = std::forward<Fn>(fn)](const void *payload) {
                callback(*static_cast<const Event *>(payload));
            },
            priority);
    }

    void unsubscribe(subscription_id id);

    template <typename Event> void publish(const Event &event) {
        publish_raw(type_of<Event>(), &event);
    }

    /// Queue an event for dispatch: the next `dispatch()` call publishes it,
    /// or the current one when called from a listener.
    template <typename Event> void post(Event event) {
        post_raw([this, stored = std::move(event)]() mutable {
            publish_raw(type_of<Event>(), &stored);
        });
    }

    /// Publish up to `max_events_per_dispatch` queued events, in post order.
    /// Events posted from a listener are appended and also dispatched in this
    /// call, so the budget bounds a listener that keeps re-posting the same
    /// event; whatever is left stays queued for the next `dispatch()`.
    /// Returns the number of events published.
    auto dispatch() -> std::size_t;

    /// Upper bound on the events one `dispatch()` call publishes. Defaults to
    /// 1'000'000 (far above a real tick's volume, small enough that a
    /// self-posting listener cannot wedge the owner thread forever); 0 means
    /// "no limit". Tests set small values to observe the budget.
    void set_max_events_per_dispatch(std::size_t max) noexcept;

    [[nodiscard]] auto pending() const noexcept -> std::size_t;

    [[nodiscard]] auto listener_count(event_type type) const noexcept
        -> std::size_t;

    [[nodiscard]] auto subscribers() const noexcept -> std::size_t;

    void clear() noexcept;

  private:
    struct entry {
        subscription_id id{0};
        event_type type{0};
        listener callback{};
        event_priority priority{event_priority::normal};
        bool alive{true};
    };

    void publish_raw(event_type type, const void *event);

    void post_raw(std::function<void()> fn);

    /// Drop tombstones left by removals during a publish and reset the publish
    /// depth. Called at the end of every publish (including the throwing path).
    void finish_publish() noexcept;

    std::vector<entry> listeners_{};
    std::vector<std::function<void()>> deferred_{};
    subscription_id next_id_{1};
    std::size_t publish_depth_{0};
    std::size_t max_events_per_dispatch_{1'000'000};
};

} // namespace refrain::core::events
