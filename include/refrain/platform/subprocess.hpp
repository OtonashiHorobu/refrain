// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace refrain::platform {

// Child-process execution for tooling (the worldgen oracle runs a user-supplied
// vanilla server jar; CI never ships or downloads one). Output is inherited
// (the child writes to our stdout/stderr); stdin is fed from `stdin_text` and
// closed. OS APIs stay in src/platform/{posix,windows}.

struct subprocess_options {
    /// Executable name (PATH lookup) or path.
    std::string program{};
    std::vector<std::string> arguments{};
    /// Empty keeps the parent's working directory.
    std::string working_directory{};
    /// Written to the child's stdin before it is closed (e.g. "stop\n").
    std::string stdin_text{};
};

struct subprocess_result {
    /// Process exit status; a signal-terminated child reports 128 + signal.
    int exit_code{0};
};

[[nodiscard]] auto run_process(const subprocess_options &options)
    -> result<subprocess_result>;

} // namespace refrain::platform
