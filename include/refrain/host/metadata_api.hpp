// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/contracts/host/metadata.hpp>

namespace refrain::host {

// The sealed `refrain:host.metadata` vtable over the generated metadata
// tables. It is stateless; `self` is ignored and may be null.
[[nodiscard]] auto metadata_vtable() noexcept
    -> const refrain_host_metadata_vtable &;

[[nodiscard]] inline auto make_metadata_provider()
    -> refrain::host::metadata::provider {
    return refrain::host::metadata::provider{metadata_vtable(), nullptr};
}

} // namespace refrain::host
