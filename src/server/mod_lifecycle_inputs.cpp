// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/mod_lifecycle_inputs.hpp>

#include <optional>
#include <utility>

namespace refrain::server {
namespace {

/// Startup is single-threaded up to the moment the runtime starts, so the
/// handoff needs no synchronization; the runtime takes it before its first
/// tick. The state stays process-wide because the contract between the
/// composition root and the in-process provider is.
std::optional<mod_lifecycle_inputs> g_published_inputs{};

} // namespace

void publish_mod_lifecycle_inputs(mod_lifecycle_inputs inputs) {
    g_published_inputs = std::move(inputs);
}

auto take_mod_lifecycle_inputs() -> std::optional<mod_lifecycle_inputs> {
    auto taken{std::move(g_published_inputs)};
    g_published_inputs.reset();
    return taken;
}

void clear_mod_lifecycle_inputs() noexcept { g_published_inputs.reset(); }

} // namespace refrain::server
