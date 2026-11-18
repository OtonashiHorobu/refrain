// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/app/content_reload.hpp>
#include <refrain/data/content_view.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace refrain::server {

// Runtime content view handoff and publication.
//
// The composition root opens the same content layers the runtime would read
// (`--content-store` composition, `--datapack` and `--dev` directories),
// builds and validates the initial view before the tick loop starts, and
// publishes the layer list plus that view here. The in-process runtime provider
// takes the handoff when it assembles the content reload service, so the frozen
// `refrain:runtime` contract does not have to grow datapack fields.
//
// While the server runs, the runtime publishes the live snapshot through
// `publish_content_view()`. A tick consumer takes the snapshot once per tick
// and must not keep the raw view pointer across a tick boundary; the snapshot's
// shared pointer keeps the view and its registry generation alive for as long
// as the consumer holds it, so a tick-boundary swap never frees memory another
// thread is reading. The chunk-generation workers are the other reader: they
// take the snapshot once at the start of a tile task and capture that one
// generation for the whole tile, and the spawn prefill takes the boot handoff
// through `take_content_reload_inputs()` before the runtime consumes it.

/// Composition-root inputs for the runtime content view epoch.
struct content_reload_inputs {
    /// Re-openable layer list, in priority order. Empty means the server was
    /// started without any external content layer: the view is empty and a
    /// reload publishes an empty view.
    std::vector<data::content_source> sources{};
    /// The view the server started with, already loaded and validated by the
    /// composition root. Null is treated as an empty view.
    std::shared_ptr<const data::content_view> initial{};
    /// Operator identity recorded by the runtime for the first commit.
    std::string actor{"console"};
};

/// Publish the inputs for the in-process runtime provider.
void publish_content_reload_inputs(content_reload_inputs inputs);

/// Take the published inputs once; a second `run_server` call in the same
/// process does not reuse a previous server's content.
[[nodiscard]] auto take_content_reload_inputs()
    -> std::optional<content_reload_inputs>;

/// Forget the published inputs (test hygiene: the handoff is process-wide).
void clear_content_reload_inputs() noexcept;

/// Publish the live content view snapshot the tick boundary just committed.
/// Called by the runtime once per commit; readers observe the generation and
/// the view together.
void publish_content_view(app::content_view_snapshot snapshot);

/// The live snapshot published by the running server, or generation 0 with a
/// null view before the first publication.
[[nodiscard]] auto published_content_view() -> app::content_view_snapshot;

/// Forget the published snapshot (test hygiene).
void clear_content_view() noexcept;

} // namespace refrain::server
