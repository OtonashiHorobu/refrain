// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/core/io.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::core::config {

/// Kinds exposed by the literal-key introspection API (dates/times are
/// reported as `other`; manifests never use them).
enum class value_kind : std::uint8_t {
    missing = 0,
    boolean,
    integer,
    floating,
    string,
    array,
    table,
    other,
};

/// Parsed TOML document (engine settings, gamepack manifests, mod
/// manifests).
///
/// The TOML implementation type is hidden behind a pimpl so the rest of the
/// engine is not coupled to the TOML library (glaze) and error handling maps to
/// `result`.
/// Keys are dotted paths: `"gamepack.id"`, `"mods.list"`.
class document {
  public:
    document(document &&other) noexcept;
    auto operator=(document &&other) noexcept -> document &;

    document(const document &other) noexcept;
    auto operator=(const document &other) noexcept -> document &;

    ~document();

    /// Parse a TOML file.
    ///
    /// Configuration and manifests are untrusted input: the default options
    /// cap the read at 16 MiB and refuse to follow a symlink. Callers that
    /// need other limits pass explicit `read_options`.
    [[nodiscard]] static auto
    parse_file(std::string_view path,
               const io::read_options &options = io::read_options{
                   .max_bytes = 16ULL * 1024ULL * 1024ULL,
                   .allow_symlink = false}) -> result<document>;

    [[nodiscard]] static auto parse_text(std::string_view text,
                                         std::string_view origin = "<memory>")
        -> result<document>;

    [[nodiscard]] auto origin() const noexcept -> std::string_view;

    /// True when the dotted key resolves to a node.
    ///
    /// An empty key is not a valid dotted key and always reports `false`; the
    /// getters treat it as missing too, so the two APIs agree. The document
    /// root is addressed with the literal-path API instead:
    /// `contains_at({})` is true for a valid document and `keys("")` lists the
    /// root table.
    [[nodiscard]] auto contains(std::string_view key) const noexcept -> bool;

    [[nodiscard]] auto get_bool(std::string_view key, bool fallback) const
        -> bool;

    [[nodiscard]] auto get_int64(std::string_view key,
                                 std::int64_t fallback) const -> std::int64_t;

    [[nodiscard]] auto get_double(std::string_view key, double fallback) const
        -> double;

    [[nodiscard]] auto get_string(std::string_view key,
                                  std::string_view fallback) const
        -> std::string;

    /// String array getter; returns `fallback` when the key is missing or not
    /// an array of strings.
    [[nodiscard]] auto
    get_strings(std::string_view key,
                std::span<const std::string_view> fallback = {}) const
        -> std::vector<std::string>;

    /// String array getter addressed by literal key components (no dotted-path
    /// splitting). Use this for tables whose keys themselves contain dots, e.g.
    /// `[mods."refrain.sim.vanilla"]`: path = {"mods",
    /// "refrain.sim.vanilla", "requires"}.
    ///
    /// Missing nodes and wrong element types are reported as errors (unlike
    /// the dotted-path getters, which use a fallback).
    [[nodiscard]] auto
    get_strings_at(std::span<const std::string_view> path) const
        -> result<std::vector<std::string>>;

    /// Scalar string getter by literal key path (error on missing/wrong type).
    [[nodiscard]] auto
    get_string_at(std::span<const std::string_view> path) const
        -> result<std::string>;

    /// Integer getter by literal key path (error on missing/wrong type).
    /// TOML integers are stored as `int64_t`, so this is the exact width.
    [[nodiscard]] auto
    get_int64_at(std::span<const std::string_view> path) const
        -> result<std::int64_t>;

    /// Boolean getter by literal key path (error on missing/wrong type).
    [[nodiscard]] auto get_bool_at(std::span<const std::string_view> path) const
        -> result<bool>;

    /// Sorted direct child keys of the table at a literal key path. The empty
    /// path addresses the document root. Missing nodes are `not_found`; nodes
    /// that exist but are not tables are `parse_error` (the same code the
    /// other `*_at()` getters report for a type mismatch).
    [[nodiscard]] auto keys_at(std::span<const std::string_view> path) const
        -> result<std::vector<std::string>>;

    /// Node kind at a literal key path (`missing` when it does not exist).
    [[nodiscard]] auto
    kind_at(std::span<const std::string_view> path) const noexcept
        -> value_kind;

    /// True when the node at a literal key path exists.
    [[nodiscard]] auto
    contains_at(std::span<const std::string_view> path) const noexcept -> bool;

    /// Normalized TOML dump of the table at a literal key path (empty when the
    /// path does not name a table). `excluded_keys` removes direct child keys
    /// from the emitted fragment, e.g. resolver metadata such as `requires`.
    [[nodiscard]] auto
    dump_table_at(std::span<const std::string_view> path,
                  std::span<const std::string_view> excluded_keys = {}) const
        -> std::string;

    /// Child keys of a table (`""` = document root), sorted for determinism.
    [[nodiscard]] auto keys(std::string_view table_key) const
        -> std::vector<std::string>;

    /// Deterministic normalized dump (stable key order) for
    /// diagnostics/logging.
    [[nodiscard]] auto dump() const -> std::string;

  private:
    struct impl;

    explicit document(std::shared_ptr<const impl> parsed) noexcept;

    std::shared_ptr<const impl> impl_;
};

/// Read-only view of one mounted configuration namespace. Copies share an
/// immutable snapshot, so an I/O thread may keep a view across reloads.
class view {
  public:
    view() noexcept;
    view(const view &) noexcept = default;
    auto operator=(const view &) noexcept -> view & = default;
    view(view &&) noexcept = default;
    auto operator=(view &&) noexcept -> view & = default;
    ~view();

    [[nodiscard]] auto name() const noexcept -> std::string_view;

    [[nodiscard]] auto contains(std::string_view key) const noexcept -> bool;

    [[nodiscard]] auto get_bool(std::string_view key, bool fallback) const
        -> bool;
    [[nodiscard]] auto get_int64(std::string_view key,
                                 std::int64_t fallback) const -> std::int64_t;
    [[nodiscard]] auto get_double(std::string_view key, double fallback) const
        -> double;
    [[nodiscard]] auto get_string(std::string_view key,
                                  std::string_view fallback) const
        -> std::string;
    [[nodiscard]] auto
    get_strings(std::string_view key,
                std::span<const std::string_view> fallback = {}) const
        -> std::vector<std::string>;

  private:
    struct impl;
    explicit view(std::shared_ptr<const impl> state) noexcept;

    std::shared_ptr<const impl> impl_{};

    friend class store;
};

/// One namespace mounted from a TOML file, optionally layered over a defaults
/// file shipped by a provider package.
struct mount_options {
    std::string name{};
    std::string path{};
    std::string defaults_path{};
};

/// Which layer a write targets. `user` is persisted by save(); `override`
/// models CLI overrides and is never written to disk.
enum class layer : std::uint8_t { user, override };

/// Host-owned configuration store composed of one TOML file per namespace.
///
/// The store is not thread-safe. Views it returns are immutable snapshots and
/// may be read from other threads; a write rebuilds the affected snapshot.
class store {
  public:
    store();
    store(store &&) noexcept;
    auto operator=(store &&) noexcept -> store &;
    store(const store &) = delete;
    auto operator=(const store &) -> store & = delete;
    ~store();

    [[nodiscard]] static auto empty() -> store;

    /// Register a namespace. The file is read by load_all()/reload(); a
    /// missing user file is an empty layer. Duplicate names fail with
    /// `already_exists`, invalid names with `invalid_argument`.
    [[nodiscard]] auto mount(mount_options options) -> status;

    [[nodiscard]] auto load_all() -> status;

    [[nodiscard]] auto module(std::string_view name) const -> result<view>;
    [[nodiscard]] auto names() const -> std::vector<std::string>;

    [[nodiscard]] auto set_bool(std::string_view name, std::string_view key,
                                bool value, layer target = layer::user)
        -> status;
    [[nodiscard]] auto set_int64(std::string_view name, std::string_view key,
                                 std::int64_t value, layer target = layer::user)
        -> status;
    [[nodiscard]] auto set_double(std::string_view name, std::string_view key,
                                  double value, layer target = layer::user)
        -> status;
    [[nodiscard]] auto set_string(std::string_view name, std::string_view key,
                                  std::string_view value,
                                  layer target = layer::user) -> status;
    [[nodiscard]] auto set_strings(std::string_view name, std::string_view key,
                                   std::span<const std::string_view> value,
                                   layer target = layer::user) -> status;
    [[nodiscard]] auto erase(std::string_view name, std::string_view key,
                             layer target = layer::user) -> status;

    [[nodiscard]] auto reload(std::string_view name) -> status;
    [[nodiscard]] auto reload_all() -> status;
    [[nodiscard]] auto save(std::string_view name) -> status;
    [[nodiscard]] auto save_all() -> status;
    [[nodiscard]] auto dirty(std::string_view name) const -> bool;

  private:
    struct impl;

    std::unique_ptr<impl> impl_;
};

} // namespace refrain::core::config
