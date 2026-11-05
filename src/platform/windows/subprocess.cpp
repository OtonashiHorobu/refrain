// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/platform/subprocess.hpp>

#if defined(_WIN32)

#include <refrain/platform/file.hpp>

#include <fmt/format.h>

#include <windows.h>

#include <algorithm>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace refrain::platform {
namespace {

[[nodiscard]] auto windows_failure(std::string_view what, DWORD error)
    -> refrain::error {
    const std::error_code code{static_cast<int>(error), std::system_category()};
    return refrain::make_error(refrain::error_code::io_error,
                               fmt::format("{}: {}", what, code.message()));
}

[[nodiscard]] auto quote_argument(std::string_view argument) -> std::string {
    if (!argument.empty() &&
        argument.find_first_of(" \t\"") == std::string_view::npos) {
        return std::string{argument};
    }
    std::string quoted{"\""};
    for (const char character : argument) {
        if (character == '"') {
            quoted += "\\\"";
        } else {
            quoted += character;
        }
    }
    quoted += '"';
    return quoted;
}

} // namespace

auto run_process(const subprocess_options &options)
    -> result<subprocess_result> {
    if (options.program.empty()) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::invalid_argument, "program is empty")};
    }
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE read_pipe{INVALID_HANDLE_VALUE};
    HANDLE write_pipe{INVALID_HANDLE_VALUE};
    if (::CreatePipe(&read_pipe, &write_pipe, &attributes, 0) == 0) {
        return std::unexpected{
            windows_failure("cannot create the stdin pipe", ::GetLastError())};
    }
    ::SetHandleInformation(write_pipe, HANDLE_FLAG_INHERIT, 0);
    std::string command{quote_argument(options.program)};
    for (const auto &argument : options.arguments) {
        command += ' ';
        command += quote_argument(argument);
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = read_pipe;
    startup.hStdOutput = ::GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    const auto native_command{file::native_path(command).wstring()};
    const auto working_directory{
        options.working_directory.empty()
            ? std::wstring{}
            : file::native_path(options.working_directory).wstring()};
    std::vector<wchar_t> mutable_command{native_command.begin(),
                                         native_command.end()};
    mutable_command.push_back(L'\0');
    if (::CreateProcessW(
            nullptr, mutable_command.data(), nullptr, nullptr, TRUE, 0, nullptr,
            working_directory.empty() ? nullptr : working_directory.c_str(),
            &startup, &process) == 0) {
        const DWORD error{::GetLastError()};
        ::CloseHandle(read_pipe);
        ::CloseHandle(write_pipe);
        return std::unexpected{
            windows_failure("cannot create the child process", error)};
    }
    ::CloseHandle(read_pipe);
    std::size_t written{0};
    while (written < options.stdin_text.size()) {
        DWORD count{0};
        const auto chunk{static_cast<DWORD>(std::min<std::size_t>(
            options.stdin_text.size() - written, 1U << 20U))};
        if (::WriteFile(write_pipe, options.stdin_text.data() + written, chunk,
                        &count, nullptr) == 0) {
            break;
        }
        written += static_cast<std::size_t>(count);
    }
    ::CloseHandle(write_pipe);
    ::WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code{1};
    ::GetExitCodeProcess(process.hProcess, &exit_code);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return subprocess_result{.exit_code = static_cast<int>(exit_code)};
}

} // namespace refrain::platform

#endif // defined(_WIN32)
