// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/contracts/host/kernel.hpp>
#include <refrain/host/kernel_context.hpp>

namespace refrain::host {

// The sealed `refrain:host.kernel` vtable over a kernel context.
[[nodiscard]] auto kernel_vtable() noexcept
    -> const refrain_host_kernel_vtable &;

[[nodiscard]] inline auto make_kernel_provider(kernel_context &context)
    -> refrain::host::kernel::provider {
    return refrain::host::kernel::provider{kernel_vtable(), &context};
}

} // namespace refrain::host
