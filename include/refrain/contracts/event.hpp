// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Compile-time metadata for RIDL `event` declarations.
//
// Events are in-process, transient notifications: they are not ABI structs
// (no `struct_size`), never persisted and never sent over the wire. Their
// layout is a C++ ABI contract shared by the host and in-process mods built
// from the same generated headers; `event_traits<T>::is_event` is the
// compile-time gate. The stable, human-readable identity is `id`.

#pragma once

#include <string_view>

namespace refrain::contracts {

/// Primary template: not an event. Generated service headers specialize it for
/// every RIDL `event` declaration.
template <typename Event> struct event_traits {
    static constexpr bool is_event = false;
    static constexpr std::string_view id{};
    static constexpr bool batch = false;
    static constexpr bool cancellable = false;
};

/// Compile-time gate for typed event helpers.
template <typename Event>
concept contract_event = event_traits<Event>::is_event;

} // namespace refrain::contracts
