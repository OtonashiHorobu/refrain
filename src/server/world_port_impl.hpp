// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Internal seam between the port selection and the contract implementation.

#pragma once

#include <refrain/server/world_port.hpp>

#include <memory>

namespace refrain::server::detail {

/// Build the ABI-backed port. Defined next to the implementation so the
/// contract class stays private to its translation unit.
[[nodiscard]] auto
make_contract_world_port(const server_service_handles &services)
    -> std::unique_ptr<world_port>;

} // namespace refrain::server::detail
