// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <string_view>

namespace refrain::version {

/// Release version injected by CMake (e.g. "0.1.0-alpha").
extern const char *release_version;

/// Git branch the binary was built from (may be empty for source exports).
extern const char *branch_name;

/// Git commit hash the binary was built from (may be empty).
extern const char *commit_hash;

/// Human-readable version string, e.g. "Refrain 0.1.0-alpha [main@abc1234]".
[[nodiscard]] auto string() noexcept -> std::string_view;

} // namespace refrain::version
