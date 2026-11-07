// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/net/base.hpp>

#include <cstdint>
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

/// An item stack on the wire. Only the empty stack is implemented: the codec
/// writes and accepts a zero item count and reports `error_code::unsupported`
/// for any item payload, which requires the data-component graph.
struct item_stack {
    std::uint32_t count{0};

    friend auto operator==(const item_stack &, const item_stack &)
        -> bool = default;
};

/// Writes the empty item stack encoding (`VarInt 0`).
[[nodiscard]] auto encode_item_stack(writer &out, const item_stack &value)
    -> status;

/// Reads an item count and accepts only zero; a non-empty stack is reported as
/// unsupported rather than skipped.
[[nodiscard]] auto decode_item_stack(reader &in, item_stack &value) -> status;

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
