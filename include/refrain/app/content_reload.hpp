// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/app/mod_management.hpp>
#include <refrain/core/error.hpp>
#include <refrain/data/content_view.hpp>

#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::app {

// Content view epoch: replace the datapack content a running server reads
// without restarting it.
//
// The service owns one live content view (a `data::content_view`, i.e. the
// frozen pack stack plus the validated recipe/loot/advancement/worldgen
// documents). A reload request rebuilds the view on a background thread from
// the same re-openable layer list the server started with; the new view is
// loaded and validated before it is published. `commit()` - called once per
// tick at the tick boundary - publishes a finished build atomically by
// swapping the shared snapshot. It never waits for the build, so a slow pack
// never blocks a tick.
//
// The previous view stays alive while any reader still holds its snapshot:
// readers take a `content_view_snapshot` once per tick and never keep a raw
// `content_view` pointer across a tick boundary. The last snapshot that refers
// to the old view releases it in its destructor.
//
// A failed build, or a build whose block/item/entity registry data differs from
// the live view, is rolled back: the live view is unchanged and the error names
// the layer, path or registry file that needs attention. Registry changes are
// refused with an outcome that says the change needs a restart, because the
// running server froze its registries at startup and existing chunks keep the
// generation they were written with.

/// One shared view and the epoch it was published as. Generation 0 means the
/// server started without any content view.
struct content_view_snapshot {
    std::uint64_t generation{0};
    std::shared_ptr<const data::content_view> view{};
};

/// One tick-boundary commit result. Only one of `published`, `pending`,
/// `failed` and `restart_required` is true on a commit that did something.
struct content_reload_commit_result {
    /// A validated view was swapped in as the new live generation.
    bool published{false};
    /// A build is still running; the live view stays as it is.
    bool pending{false};
    /// The build failed validation (or the builder failed); the live view
    /// stays.
    bool failed{false};
    /// The build succeeded but changed registry data, which needs a restart.
    bool restart_required{false};
    /// Operator-facing lines, one event per line.
    std::vector<std::string> lines{};
    /// The error that caused a failed or restart-required commit.
    std::string error{};
};

/// Configuration of the content view epoch.
struct content_reload_options {
    /// The hot gate (`[lifecycle] hot_reload_data` in `mods_admin.toml`). A
    /// disabled service refuses every request instead of reloading quietly.
    bool enabled{false};
    /// File the gate was read from (diagnostics). Empty means the built-in
    /// default.
    std::string gate_source{};
};

class content_reload {
  public:
    /// Builds one candidate view. The service calls it on a background thread;
    /// it must capture everything it needs (the layer list and build options)
    /// and must not touch the live view.
    using builder =
        std::function<result<std::shared_ptr<const data::content_view>>()>;

    /// `initial` is the view the server started with (generation 0 when there
    /// was none). `build` must not be empty.
    content_reload(content_view_snapshot initial,
                   content_reload_options options, builder build);
    content_reload(const content_reload &) = delete;
    auto operator=(const content_reload &) -> content_reload & = delete;
    content_reload(content_reload &&) = delete;
    auto operator=(content_reload &&) -> content_reload & = delete;
    ~content_reload();

    /// Start one background rebuild. One build may be in flight at a time; the
    /// returned lines describe the published generation and when the candidate
    /// will be published.
    ///
    /// Errors: `permission_denied` when the hot gate is disabled,
    /// `already_exists` when a build is already in flight.
    [[nodiscard]] auto request() -> result<std::vector<std::string>>;

    /// Publish a finished build at a tick boundary. Never waits for the build:
    /// a still-running build reports `pending` and is retried on a later tick.
    /// A failed build is dropped and the live view is unchanged.
    [[nodiscard]] auto commit() -> content_reload_commit_result;

    /// The current live view. Callers take this once per tick and must not keep
    /// it across a tick boundary; the snapshot keeps the view alive for as long
    /// as the caller holds it.
    [[nodiscard]] auto snapshot() const -> content_view_snapshot;

    [[nodiscard]] auto pending() const -> bool;

    [[nodiscard]] auto generation() const -> std::uint64_t;

    [[nodiscard]] auto options() const noexcept
        -> const content_reload_options & {
        return options_;
    }

  private:
    mutable std::mutex mutex_{};
    content_view_snapshot live_{};
    content_reload_options options_{};
    builder build_{};
    std::future<result<std::shared_ptr<const data::content_view>>>
        build_future_{};
};

/// Build the hot content-reload port the shared command service
/// (`app::mod_management`) calls. The command layer owns parsing, permission
/// checks, the gate and the audit record; the returned port queues the rebuild
/// and reports the generation the new view publishes as.
[[nodiscard]] auto make_content_reload_port(content_reload &service)
    -> content_reload_port;

} // namespace refrain::app
