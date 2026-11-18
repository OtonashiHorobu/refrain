// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace refrain::script::host {

// The sandboxed guest host API: one core wasm import module named `refrain`,
// the capability that gates each import, and the bounded read-only host state
// a guest may observe.
//
// The binding is a core wasm module, not a component type: every import is a
// plain function over i32/i64 with a stable name and bounded arguments, so any
// wasm producer can link it and the runtime validates the module's import
// section against this table before instantiation. A generated guest binding
// and a component-model/WIT surface layer on top of the same names; neither
// widens what the host policy admits.
//
// The names, signatures and bounds are ABI. Adding an import bumps the minor
// version; changing a signature or lowering a bound bumps the major version. A
// guest that imports a name outside the allowlist, or one whose capability is
// not granted to that guest, is refused before any guest code runs.

/// Guest import surface version. It is independent of the host runtime ABI:
/// the imports are the stable part a guest compiles against.
inline constexpr std::uint32_t abi_major{1};
inline constexpr std::uint32_t abi_minor{0};

/// Wasm module every host import is defined under.
inline constexpr std::string_view import_module{"refrain"};

/// Append one line to the host log. Signature:
/// `(i32 level, i32 message_ptr, i32 message_bytes) -> i32`.
inline constexpr std::string_view log_import{"log"};

/// Read one sealed configuration value. Signature:
/// `(i32 key_ptr, i32 key_bytes, i32 value_ptr, i32 value_capacity) -> i32`,
/// returning the value length, or a negative status.
inline constexpr std::string_view config_get_import{"config_get"};

/// Read the host tick index. Signature: `() -> i64`.
inline constexpr std::string_view tick_import{"tick"};

/// Longest accepted guest log message.
inline constexpr std::uint32_t max_log_message_bytes{1024};
/// Longest accepted configuration key.
inline constexpr std::uint32_t max_config_key_bytes{128};
/// Longest configuration value a guest may read in one call.
inline constexpr std::uint32_t max_config_value_bytes{4096};
/// Most log lines one dispatched call may emit. It bounds the host work a
/// guest can trigger with its instruction budget: a guest that keeps logging
/// past it receives `import_quota_exceeded` instead of more log lines.
inline constexpr std::uint32_t max_log_messages_per_call{64};

/// Guest linear-memory address where the host stages the arguments of a
/// dispatched call. A guest that accepts call arguments must keep the first
/// `max_call_argument_bytes` bytes of its memory reserved; the host writes only
/// the bytes of the current call there.
inline constexpr std::uint32_t call_argument_address{0};

/// Largest argument payload of one dispatched call. The runtime refuses a
/// larger request before touching the guest.
inline constexpr std::uint32_t max_call_argument_bytes{256};

/// Status values an import returns (i32); zero is success.
inline constexpr std::int32_t import_ok{0};
/// The call was malformed: a null/out-of-bounds view, too long a message or a
/// level outside 0..4.
inline constexpr std::int32_t import_invalid_argument{-1};
/// The requested configuration key does not exist.
inline constexpr std::int32_t import_not_found{-2};
/// The output capacity is smaller than the value; nothing is written.
inline constexpr std::int32_t import_buffer_too_small{-3};
/// The guest emitted its per-call log line quota.
inline constexpr std::int32_t import_quota_exceeded{-4};
/// The host could not complete the import, for example it ran out of memory
/// while recording a log line. Guest code sees the status and keeps running.
inline constexpr std::int32_t import_internal_failure{-5};

/// Upper bound of `quote_guest_name()`'s output, excluding the trailing
/// `...` appended when the input is longer.
inline constexpr std::size_t max_quoted_guest_name_bytes{96};

/// Render an untrusted guest-supplied name (an import, export or loader id) for
/// an operator-facing message. Guest names are arbitrary bytes and may contain
/// newlines, terminal escapes or control characters, so a refusal never
/// interpolates them raw: printable ASCII passes through, every other byte
/// becomes `\xNN`, and the result is capped near
/// `max_quoted_guest_name_bytes` with `...`.
[[nodiscard]] inline auto quote_guest_name(std::string_view name)
    -> std::string {
    constexpr std::string_view hex_digits{"0123456789abcdef"};
    auto quoted{std::string{}};
    quoted.reserve(
        std::min<std::size_t>(name.size(), max_quoted_guest_name_bytes));
    for (const char character : name) {
        if (quoted.size() + 4 > max_quoted_guest_name_bytes) {
            quoted += "...";
            break;
        }
        const auto byte{static_cast<unsigned char>(character)};
        if (byte >= 0x20U && byte < 0x7FU && byte != '\\' && byte != '"' &&
            byte != '\'') {
            quoted += character;
            continue;
        }
        quoted += "\\x";
        quoted += hex_digits[byte >> 4U];
        quoted += hex_digits[byte & 0x0FU];
    }
    return quoted;
}

/// Guest callback signature: `(i32 argument_address, i32 argument_bytes) ->
/// i32`. The host invokes an exported callback by name after load; the return
/// value is the call result, and the argument bytes were staged at
/// `call_argument_address` in the guest's linear memory.
inline constexpr std::uint32_t callback_parameter_count{2};

/// One host capability. A capability is either granted to a guest or it is not;
/// there is no partial grant.
enum class capability : std::uint8_t {
    /// `log`: write one bounded line to the host log.
    log = 1U << 0,
    /// `config`: read entries of the sealed host configuration.
    config = 1U << 1,
    /// `clock`: read the host tick index.
    clock = 1U << 2,
};

/// A set of granted capabilities.
struct grant_set {
    std::uint8_t bits{0};

    constexpr grant_set() noexcept = default;
    constexpr grant_set(capability value) noexcept : bits{bit_of(value)} {}

    [[nodiscard]] constexpr auto contains(capability value) const noexcept
        -> bool {
        return (bits & bit_of(value)) != 0;
    }

    [[nodiscard]] constexpr auto empty() const noexcept -> bool {
        return bits == 0;
    }

    constexpr void grant(capability value) noexcept { bits |= bit_of(value); }

    friend constexpr auto operator==(grant_set lhs, grant_set rhs) noexcept
        -> bool = default;

  private:
    [[nodiscard]] static constexpr auto bit_of(capability value) noexcept
        -> std::uint8_t {
        return static_cast<std::uint8_t>(value);
    }
};

[[nodiscard]] constexpr auto operator|(grant_set lhs, capability rhs) noexcept
    -> grant_set {
    lhs.grant(rhs);
    return lhs;
}

[[nodiscard]] constexpr auto operator|(capability lhs, grant_set rhs) noexcept
    -> grant_set {
    return rhs | lhs;
}

[[nodiscard]] constexpr auto operator|(capability lhs, capability rhs) noexcept
    -> grant_set {
    return grant_set{lhs} | rhs;
}

/// Stable capability spelling used by the manifest `host_capabilities` list.
[[nodiscard]] auto capability_name(capability value) noexcept
    -> std::string_view;

/// Parse a capability spelling; unknown names return nullopt.
[[nodiscard]] auto parse_capability(std::string_view name) noexcept
    -> std::optional<capability>;

/// Comma-separated grant list in capability order (`clock, config, log`), used
/// in refusals so an operator sees exactly what the guest was allowed to call.
/// An empty set formats as an empty string.
[[nodiscard]] auto format_grants(grant_set grants) -> std::string;

/// Read-only host state one guest may observe. The owner seals it before the
/// guest is created and does not mutate it while the guest runs; the tick index
/// is a snapshot the owner updates only between calls, on the dispatching
/// thread.
struct environment {
    /// Sealed key/value entries a guest reads through `config_get`.
    std::map<std::string, std::string, std::less<>> config{};
    /// Monotonic server tick index the `tick` import returns.
    std::int64_t tick_index{0};
};

} // namespace refrain::script::host
