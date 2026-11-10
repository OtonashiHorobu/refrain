// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/contracts/host/platform.hpp>
#include <refrain/host/platform_context.hpp>

namespace refrain::host {

// The sealed `refrain:host.platform` vtable over a platform context.
[[nodiscard]] auto platform_vtable() noexcept
    -> const refrain_host_platform_vtable &;

[[nodiscard]] inline auto make_platform_provider(platform_context &context)
    -> refrain::host::platform::provider {
    return refrain::host::platform::provider{platform_vtable(), &context};
}

} // namespace refrain::host
