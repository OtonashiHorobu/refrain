// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

// Stable facade for the protocol version this build serves. Hand-written code
// outside the protocol provider should include this header instead of a
// versioned generated tree. Adding a protocol version updates only this alias
// and the version-specific provider.

#include <refrain/schema/protocol/777/configuration_clientbound.hpp>
#include <refrain/schema/protocol/777/configuration_serverbound.hpp>
#include <refrain/schema/protocol/777/handshake_serverbound.hpp>
#include <refrain/schema/protocol/777/login_clientbound.hpp>
#include <refrain/schema/protocol/777/login_serverbound.hpp>
#include <refrain/schema/protocol/777/play_clientbound.hpp>
#include <refrain/schema/protocol/777/play_serverbound.hpp>
#include <refrain/schema/protocol/777/status_clientbound.hpp>
#include <refrain/schema/protocol/777/status_serverbound.hpp>

namespace refrain::protocol {

namespace current = v777;

} // namespace refrain::protocol
