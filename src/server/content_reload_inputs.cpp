// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/content_reload_inputs.hpp>

#include <mutex>
#include <optional>
#include <utility>

namespace refrain::server {
namespace {

/// Startup is single-threaded up to the moment the runtime starts, and the
/// handoff is consumed before the first tick, so it needs no synchronization.
/// The state stays process-wide because the contract between the composition
/// root and the in-process provider is.
std::optional<content_reload_inputs> g_published_inputs{};

/// The live snapshot is published by the tick thread once per commit. The
/// mutex protects the rare off-tick reader (a command or a diagnostic);
/// readers copy a shared pointer and never hold the view across a tick
/// boundary.
std::mutex g_published_view_mutex{};
app::content_view_snapshot g_published_view{};

} // namespace

void publish_content_reload_inputs(content_reload_inputs inputs) {
    g_published_inputs = std::move(inputs);
}

auto take_content_reload_inputs() -> std::optional<content_reload_inputs> {
    auto taken{std::move(g_published_inputs)};
    g_published_inputs.reset();
    return taken;
}

void clear_content_reload_inputs() noexcept { g_published_inputs.reset(); }

void publish_content_view(app::content_view_snapshot snapshot) {
    const std::lock_guard lock{g_published_view_mutex};
    g_published_view = std::move(snapshot);
}

auto published_content_view() -> app::content_view_snapshot {
    const std::lock_guard lock{g_published_view_mutex};
    return g_published_view;
}

void clear_content_view() noexcept {
    const std::lock_guard lock{g_published_view_mutex};
    g_published_view = {};
}

} // namespace refrain::server
