// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <cstdint>
#include <expected>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

namespace refrain {

/// Stable, architecture-independent error taxonomy shared by all kernel
/// primitives and (through RIDL) by service contracts. Values are part of the
/// diagnostic/API surface; append-only.
enum class error_code : std::int32_t {
    success = 0,
    unknown,
    invalid_argument,
    not_found,
    already_exists,
    permission_denied,
    io_error,
    parse_error,
    version_mismatch,
    unsupported,
    not_initialized,
    timeout,
    cancelled,
    out_of_memory,
    mod_error,
};

[[nodiscard]] auto to_string(error_code code) noexcept -> std::string_view;

/// Cold-path error payload. Errors are values (no exceptions): code + message +
/// (debug) source location of the failing check.
struct error {
    error_code code{error_code::unknown};
    std::string message{};
    std::source_location location{};
};

template <typename T> using result = std::expected<T, error>;

using status = result<void>;

[[nodiscard]] inline auto
make_error(error_code code, std::string_view message,
           std::source_location location = std::source_location::current())
    -> error {
    return error{code, std::string{message}, location};
}

/// Convenience: `return status_ok();`
[[nodiscard]] inline auto status_ok() noexcept -> status { return {}; }

} // namespace refrain
