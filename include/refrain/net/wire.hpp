// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/net/base.hpp>

#include <string>

namespace refrain::net {

/// A text component in its network-NBT form. The current codec supports the
/// string form, which covers chat, titles, tab list headers and disconnect
/// messages; compound components need the full NBT tree and are rejected.
struct text_component {
    std::string text{};

    friend auto operator==(const text_component &, const text_component &)
        -> bool = default;
};

/// Writes a network-NBT string tag. The text must be valid UTF-8 without NUL
/// and within the Basic Multilingual Plane, because Java's modified UTF-8
/// encodes supplementary code points as surrogate pairs.
[[nodiscard]] auto encode_text_component(writer &out,
                                         const text_component &value) -> status;

/// Reads a network-NBT string tag into `value`, or returns
/// `error_code::unsupported` for any other tag type.
[[nodiscard]] auto decode_text_component(reader &in, text_component &value)
    -> status;

} // namespace refrain::net
