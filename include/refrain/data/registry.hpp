// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/resource_location.hpp>

#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace refrain::data {

// Generic frozen registry for the data layer.
//
// A `registry<T>` maps validated resource locations (`namespace:path`) to
// values of type `T`, and assigns each entry a stable integer id in
// registration order (0..N-1). It has two phases:
//
//  1. registration (host boot / `on_load`): `add()` may be called repeatedly;
//     entries are stored in registration order and lookups are O(1).
//  2. frozen: `freeze()` seals the snapshot. From then on `add()`/`clear()`
//     fail with `permission_denied` and the query API is safe to call from
//     multiple threads (the snapshot is immutable and all returned data is
//     owned by the snapshot).
//
// Copy semantics: `registry<T>` is a cheap value type over a shared immutable
// snapshot (`std::shared_ptr<impl>`), mirroring `refrain::data::vfs`. Copies
// view the same entries; `add()`/`freeze()`/`clear()` through one copy is
// observable through every copy. Copying a frozen registry does not duplicate
// the entries, so a frozen registry can be shared between content subsystems.
// A moved-from registry may only be destroyed or assigned.
//
// Ids are part of the data identity: entries keep their registration-order id
// for the lifetime of a snapshot, and ids of a frozen registry are part of the
// future protocol/save identity. Ids assigned before `freeze()` must not be
// persisted.
//
// Errors are values (`refrain::result`), no exceptions are thrown by the
// registry logic and no RTTI is used. The only hard boundary is allocation
// failure: `add()` translates `std::bad_alloc` (and exceptions thrown by `T`'s
// move constructor) into `out_of_memory` after rolling back the partial entry,
// so a failed `add()` leaves the registry unchanged.

/// Strongly typed registry entry id (`std::uint32_t` payload).
///
/// `invalid_value` (0xFFFF'FFFF) is the invalid sentinel; a valid registry
/// never hands it out. Comparable and hashable (see `std::hash` below).
class registry_id {
  public:
    using value_type = std::uint32_t;

    /// Sentinel value of an invalid id; also the upper bound on registry size.
    static constexpr value_type invalid_value{0xFFFF'FFFFU};

    constexpr registry_id() noexcept = default;
    explicit constexpr registry_id(value_type raw) noexcept : value_{raw} {}

    [[nodiscard]] constexpr auto value() const noexcept -> value_type {
        return value_;
    }

    [[nodiscard]] constexpr auto valid() const noexcept -> bool {
        return value_ != invalid_value;
    }

    /// `registry_id{}` (the default-constructed value) is invalid.
    [[nodiscard]] static constexpr auto invalid() noexcept -> registry_id {
        return registry_id{};
    }

    /// Decimal form of a valid id, `"invalid"` for the sentinel (diagnostics).
    [[nodiscard]] auto to_string() const -> std::string;

    friend constexpr auto operator==(registry_id, registry_id)
        -> bool = default;
    friend constexpr auto operator<=>(registry_id, registry_id) = default;

  private:
    value_type value_{invalid_value};
};

/// Canonical fingerprint encoding (BLAKE3, version 1). Internal seam shared by
/// the template registry and its tests; the stable contract is documented on
/// `registry::fingerprint()`.
namespace detail {

/// BLAKE3-256 of the canonical encoding of `type_name` + `locations` sorted by
/// canonical `namespace:path` text (order of `locations` does not matter).
/// Values are not hashed (caller responsibility). Allocates a working buffer.
[[nodiscard]] auto fingerprint_of(std::string_view type_name,
                                  std::span<const resource_location> locations)
    -> digest32;

/// Append one `dump()` prefix (`<id>\t<namespace:path>`) to `out`.
void append_dump_prefix(std::string &out, registry_id id,
                        const resource_location &location);

} // namespace detail

/// Frozen-capable registry: `resource_location` -> `T`, ids in registration
/// order, immutable after `freeze()`.
///
/// Thread safety: registration (`add`/`freeze`/`clear`) is single-threaded
/// host-boot work. Once frozen, copies of the registry may be read from any
/// thread concurrently; no query acquires a lock because the snapshot is
/// immutable.
template <typename T> class registry {
  public:
    using value_type = T;
    using location_type = resource_location;

    /// One registered entry as exposed by `entries()`.
    ///
    /// The pointers refer to the shared snapshot; they stay valid until the
    /// next `add()`/`clear()` on this registry or any copy of it (frozen
    /// snapshots are never mutated, so a frozen registry's entry refs stay
    /// valid for the snapshot's lifetime).
    struct entry_ref {
        registry_id id{};
        const location_type *location{nullptr};
        const value_type *value{nullptr};
    };

    /// Empty registry with an empty type name (see `fingerprint()`).
    registry() : impl_{std::make_shared<impl>()} {}

    /// Empty registry whose stable type name participates in `fingerprint()`.
    /// Use a persistent identifier such as `"recipe"` or
    /// `"refrain:block_registry"`, never a compiler-specific type spelling.
    explicit registry(std::string type_name) : impl_{std::make_shared<impl>()} {
        impl_->type_name = std::move(type_name);
    }

    registry(const registry &) = default;
    auto operator=(const registry &) -> registry & = default;
    registry(registry &&) noexcept = default;
    auto operator=(registry &&) noexcept -> registry & = default;
    ~registry() = default;

    /// Stable type name used by `fingerprint()` (empty when not provided).
    /// The view refers to the shared snapshot and stays valid while any copy of
    /// the registry (or a frozen snapshot view) is alive.
    [[nodiscard]] auto type_name() const noexcept -> std::string_view {
        return impl_->type_name;
    }

    /// Register `value` under `location` and return its new id.
    ///
    /// Fails with:
    ///  - `invalid_argument` for an empty location (default-constructed
    ///    `resource_location` has no identity),
    ///  - `already_exists` when `location` is already registered (the existing
    ///    entry and value are left untouched),
    ///  - `permission_denied` after `freeze()`,
    ///  - `out_of_memory` when the entry limit (`registry_id::invalid_value`
    ///    entries) is reached, or when allocating the entry fails (the partial
    ///    entry is rolled back on failure).
    [[nodiscard]] auto add(location_type location, value_type value)
        -> result<registry_id> {
        auto &state{*impl_};
        if (state.frozen) {
            return std::unexpected{
                make_error(error_code::permission_denied,
                           "registry is frozen; cannot add entry")};
        }
        if (location.empty()) {
            return std::unexpected{make_error(
                error_code::invalid_argument,
                "registry entry needs a non-empty resource location")};
        }
        if (state.index.find(location) != state.index.end()) {
            return std::unexpected{make_error(error_code::already_exists,
                                              "registry already contains '" +
                                                  location.to_string() + "'")};
        }
        if (state.values.size() >=
            static_cast<std::size_t>(registry_id::invalid_value)) {
            return std::unexpected{make_error(error_code::out_of_memory,
                                              "registry entry limit reached")};
        }

        const auto id{registry_id{
            static_cast<registry_id::value_type>(state.values.size())}};
        const auto start_size{state.values.size()};
        try {
            state.locations.reserve(start_size + 1U);
            state.values.reserve(start_size + 1U);
            state.ids.reserve(start_size + 1U);
            state.locations.push_back(std::move(location));
            state.values.push_back(std::move(value));
            state.ids.push_back(id);
            // Index insertion is last: duplicate detection above and the copy
            // of the location in the map node are the only throwing steps that
            // happen after the entry vectors grew.
            state.index.emplace(state.locations.back(), id);
        } catch (...) {
            // Roll the partially inserted entry back so a failed add() leaves
            // the registry unchanged (hard allocation/T-move boundary).
            while (state.values.size() > start_size) {
                state.values.pop_back();
            }
            while (state.locations.size() > start_size) {
                state.locations.pop_back();
            }
            while (state.ids.size() > start_size) {
                state.ids.pop_back();
            }
            return std::unexpected{make_error(
                error_code::out_of_memory,
                "registry entry allocation failed; entry was not added")};
        }
        return id;
    }

    /// Value registered under `location`, or nullptr when absent.
    [[nodiscard]] auto find(const location_type &location) const noexcept
        -> const value_type * {
        const auto &state{*impl_};
        const auto match{state.index.find(location)};
        if (match == state.index.end()) {
            return nullptr;
        }
        return &state.values[match->second.value()];
    }

    /// Value for a registry id, or nullptr for the invalid sentinel/out of
    /// range ids (ids of a different snapshot are not distinguishable, so
    /// callers must not mix snapshots).
    [[nodiscard]] auto at(registry_id id) const noexcept -> const value_type * {
        if (!id.valid()) {
            return nullptr;
        }
        const auto &state{*impl_};
        const auto index{static_cast<std::size_t>(id.value())};
        return index < state.values.size() ? &state.values[index] : nullptr;
    }

    /// True when a value is registered under `location`.
    [[nodiscard]] auto contains(const location_type &location) const noexcept
        -> bool {
        return impl_->index.find(location) != impl_->index.end();
    }

    /// Number of registered entries.
    [[nodiscard]] auto size() const noexcept -> std::size_t {
        return impl_->values.size();
    }

    [[nodiscard]] auto empty() const noexcept -> bool {
        return impl_->values.empty();
    }

    /// Ids in registration order (`0..size()-1`); empty for an empty registry.
    /// The span refers to the shared snapshot: it stays valid until the next
    /// `add()`/`clear()` on this registry or any copy (frozen snapshots are
    /// never mutated, so frozen ids stay valid for the snapshot lifetime).
    [[nodiscard]] auto ids() const noexcept -> std::span<const registry_id> {
        return impl_->ids;
    }

    /// All entries in registration order. The returned vector owns only the
    /// refs (no values are copied); each `entry_ref` points into the shared
    /// snapshot (see `entry_ref` for the lifetime rules).
    [[nodiscard]] auto entries() const -> std::vector<entry_ref> {
        const auto &state{*impl_};
        std::vector<entry_ref> result;
        result.reserve(state.values.size());
        for (std::size_t index{0}; index < state.values.size(); ++index) {
            result.push_back(entry_ref{.id = state.ids[index],
                                       .location = &state.locations[index],
                                       .value = &state.values[index]});
        }
        return result;
    }

    /// Seal the registry: reads stay valid, mutations fail with
    /// `permission_denied`. Idempotent (a second `freeze()` succeeds).
    [[nodiscard]] auto freeze() -> status {
        impl_->frozen = true;
        return status_ok();
    }

    [[nodiscard]] auto is_frozen() const noexcept -> bool {
        return impl_->frozen;
    }

    /// Drop every entry (tests/reload). Fails with `permission_denied` once
    /// frozen — a frozen snapshot is never mutated, reloading builds a new
    /// registry instead. Keeps the type name.
    [[nodiscard]] auto clear() -> status {
        auto &state{*impl_};
        if (state.frozen) {
            return std::unexpected{
                make_error(error_code::permission_denied,
                           "registry is frozen; cannot clear entries")};
        }
        std::vector<location_type>{}.swap(state.locations);
        std::vector<value_type>{}.swap(state.values);
        std::vector<registry_id>{}.swap(state.ids);
        std::unordered_map<location_type, registry_id>{}.swap(state.index);
        return status_ok();
    }

    /// Deterministic fingerprint of the registry identity.
    ///
    /// BLAKE3-256 over canonical encoding v1 (little-endian, all integers
    /// 8-byte `std::uint64_t`):
    ///
    ///     "refrain.registry.fingerprint.v1" '\0'
    ///     u64(type_name.size()) type_name bytes
    ///     u64(entry_count)
    ///     for each location in ascending canonical "namespace:path" byte
    ///     order: u64(text.size()) text bytes
    ///
    /// Consequences: the fingerprint is independent of registration order,
    /// ids and values (it identifies the location set + registry type name
    /// only); hashing content values is the caller's responsibility (e.g. by
    /// folding BLAKE3 payload digests into a separate content hash). It does
    /// not depend on frozen state, so it can be computed during assembly.
    [[nodiscard]] auto fingerprint() const -> digest32 {
        return detail::fingerprint_of(impl_->type_name, impl_->locations);
    }

    /// Stable dump for tools/diagnostics: one line per entry, registration
    /// order, `<id>\t<namespace:path>\n`. No header line; byte-identical for
    /// equal registries.
    [[nodiscard]] auto dump() const -> std::string {
        const auto &state{*impl_};
        std::string text;
        text.reserve(state.values.size() * 32U);
        for (std::size_t index{0}; index < state.values.size(); ++index) {
            detail::append_dump_prefix(text, state.ids[index],
                                       state.locations[index]);
            text.push_back('\n');
        }
        return text;
    }

    /// `dump()` with an extra caller-provided payload column:
    /// `<id>\t<namespace:path>\t<formatter(value)>\n`. The formatter must be
    /// invocable as `formatter(const T &)` and return something convertible to
    /// `std::string_view` (a `std::string` temporary is fine).
    template <typename Formatter>
    [[nodiscard]] auto dump_with(Formatter &&formatter) const -> std::string {
        static_assert(std::invocable<Formatter &, const value_type &>,
                      "dump formatter must be callable as formatter(const T&)");
        const auto &state{*impl_};
        std::string text;
        text.reserve(state.values.size() * 48U);
        for (std::size_t index{0}; index < state.values.size(); ++index) {
            detail::append_dump_prefix(text, state.ids[index],
                                       state.locations[index]);
            text.push_back('\t');
            const auto formatted{std::invoke(formatter, state.values[index])};
            text.append(std::string_view{formatted});
            text.push_back('\n');
        }
        return text;
    }

  private:
    struct impl {
        std::string type_name{};
        std::vector<location_type> locations{};
        std::vector<value_type> values{};
        std::vector<registry_id> ids{};
        std::unordered_map<location_type, registry_id> index{};
        bool frozen{false};
    };

    std::shared_ptr<impl> impl_;
};

} // namespace refrain::data

namespace std {

template <> struct hash<refrain::data::registry_id> {
    [[nodiscard]] auto
    operator()(const refrain::data::registry_id &id) const noexcept
        -> std::size_t {
        return std::hash<std::uint32_t>{}(id.value());
    }
};

} // namespace std
