// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/contracts/host/core.hpp>
#include <refrain/host/context.hpp>

namespace refrain::host {

// The sealed `refrain:host.core` vtable over a host context. The table is
// stateless and process-lifetime; the context is passed as `self`.
[[nodiscard]] auto core_vtable() noexcept -> const refrain_host_core_vtable &;

[[nodiscard]] inline auto make_core_provider(context &host_context)
    -> core::provider {
    return core::provider{core_vtable(), &host_context};
}

} // namespace refrain::host
