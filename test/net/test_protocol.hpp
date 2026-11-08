// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/net/protocol_codec.hpp>
#include <refrain/protocol/java777_codec.hpp>

#include <memory>

namespace refrain::test {

[[nodiscard]] inline auto make_test_protocol()
    -> std::shared_ptr<net::protocol_codec> {
    return std::make_shared<protocol::java777_codec>();
}

} // namespace refrain::test
