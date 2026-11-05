// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/error.hpp>

namespace refrain {

auto to_string(error_code code) noexcept -> std::string_view {
    switch (code) {
    case error_code::success:
        return "success";
    case error_code::unknown:
        return "unknown";
    case error_code::invalid_argument:
        return "invalid_argument";
    case error_code::not_found:
        return "not_found";
    case error_code::already_exists:
        return "already_exists";
    case error_code::permission_denied:
        return "permission_denied";
    case error_code::io_error:
        return "io_error";
    case error_code::parse_error:
        return "parse_error";
    case error_code::version_mismatch:
        return "version_mismatch";
    case error_code::unsupported:
        return "unsupported";
    case error_code::not_initialized:
        return "not_initialized";
    case error_code::timeout:
        return "timeout";
    case error_code::cancelled:
        return "cancelled";
    case error_code::out_of_memory:
        return "out_of_memory";
    case error_code::mod_error:
        return "mod_error";
    }
    return "unknown";
}

} // namespace refrain
