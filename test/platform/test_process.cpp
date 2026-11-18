// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/platform/file.hpp>
#include <refrain/platform/process.hpp>

#include "support/scratch_directory.hpp"

#include <boost/ut.hpp>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if !defined(_WIN32)
// White-box POSIX test of the platform layer itself (see the allow-list entry
// in tools/platform_check.py): observing "died by signal" requires spawning a
// real child process and inspecting waitpid()'s status, and observing terminal
// detection requires replacing the stderr descriptor.
#include <fcntl.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#else
// White-box Windows test of the liveness probe: the access-denied arm needs a
// real process object with a restricted DACL, and the terminal probe replaces
// the stderr descriptor. The same allow-list entry covers both arms.
#include <windows.h>

#include <aclapi.h>
#include <sddl.h>

#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#endif

namespace {
using namespace boost::ut;

namespace fs = std::filesystem;

auto read_text(const fs::path &path) -> std::string {
    std::ifstream in{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{in},
                       std::istreambuf_iterator<char>{}};
}

#if defined(_WIN32)
/// Deny `PROCESS_QUERY_LIMITED_INFORMATION` on `process` to the current user.
///
/// A normal token then gets `ERROR_ACCESS_DENIED` from `OpenProcess`, the same
/// answer a process of another account gives; a token that holds
/// SeDebugPrivilege bypasses the DACL, which the caller detects by opening the
/// child again.
[[nodiscard]] auto deny_query_access(HANDLE process) -> bool {
    static_assert(PROCESS_QUERY_LIMITED_INFORMATION == 0x1000,
                  "the SDDL access mask below is PROCESS_QUERY_LIMITED_"
                  "INFORMATION");
    HANDLE token{nullptr};
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token) == 0) {
        return false;
    }
    DWORD size{0};
    static_cast<void>(
        ::GetTokenInformation(token, TokenUser, nullptr, 0, &size));
    // The TOKEN_USER record starts with a pointer, so the buffer needs pointer
    // alignment (a byte vector would not guarantee it).
    std::vector<std::uint64_t> buffer((size + sizeof(std::uint64_t) - 1U) /
                                      sizeof(std::uint64_t));
    const bool read_user{size > 0 &&
                         ::GetTokenInformation(token, TokenUser, buffer.data(),
                                               size, &size) != 0};
    ::CloseHandle(token);
    if (!read_user) {
        return false;
    }
    const auto *user{reinterpret_cast<const TOKEN_USER *>(buffer.data())};
    LPWSTR sid_text{nullptr};
    if (::ConvertSidToStringSidW(user->User.Sid, &sid_text) == 0) {
        return false;
    }
    const std::wstring sddl{std::wstring{L"D:(D;;0x1000;;;"} + sid_text + L")"};
    ::LocalFree(sid_text);
    PSECURITY_DESCRIPTOR descriptor{nullptr};
    if (::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr) == 0) {
        return false;
    }
    BOOL present{FALSE};
    PACL dacl{nullptr};
    BOOL defaulted{FALSE};
    const bool has_dacl{::GetSecurityDescriptorDacl(descriptor, &present, &dacl,
                                                    &defaulted) != 0 &&
                        present == TRUE};
    const DWORD set_error{::SetSecurityInfo(process, SE_KERNEL_OBJECT,
                                            DACL_SECURITY_INFORMATION, nullptr,
                                            nullptr, dacl, nullptr)};
    const bool applied{has_dacl &&
                       set_error == static_cast<DWORD>(ERROR_SUCCESS)};
    ::LocalFree(descriptor);
    return applied;
}
#endif

suite<"platform/process"> process_tests = [] {
    "platform/process: process_id is non-zero"_test = [] {
        expect(refrain::platform::process_id() > 0U);
    };

    "platform/process: the running process is alive and an unused id is dead"_test =
        [] {
            expect(refrain::platform::process_alive(
                refrain::platform::process_id()));
            // A valid pid value far outside either platform's allocator: no
            // process owns it, and the early range checks do not apply.
            expect(
                !refrain::platform::process_alive(std::uint64_t{0x7FFFFFF0U}));
        };

#if defined(_WIN32)
    "platform/process: an inaccessible live process counts as alive"_test = [] {
        // Spawn a child long enough to be probed, then deny this user
        // PROCESS_QUERY_LIMITED_INFORMATION on its process object. A normal
        // token then gets ERROR_ACCESS_DENIED from OpenProcess, and the probe
        // must report the child alive, like POSIX EPERM.
        std::wstring command{L"cmd.exe /c ping -n 30 127.0.0.1 > nul"};
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION child{};
        if (::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                             &child) == 0) {
            expect(true) << "SKIPPED: cannot spawn the liveness probe child";
            return;
        }
        const bool denied{deny_query_access(child.hProcess)};
        const HANDLE reopened{::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                            FALSE, child.dwProcessId)};
        const DWORD open_error{reopened == nullptr ? ::GetLastError() : 0U};
        if (reopened != nullptr) {
            // The token holds SeDebugPrivilege (or the DACL change did not take
            // effect), so the access-denied arm does not apply.
            ::CloseHandle(reopened);
        } else {
            expect(denied) << "the process DACL could not be restricted";
            if (denied) {
                expect(open_error == ERROR_ACCESS_DENIED);
                expect(refrain::platform::process_alive(child.dwProcessId))
                    << "an access-denied live process must count as alive";
            }
        }
        static_cast<void>(::TerminateProcess(child.hProcess, 0));
        static_cast<void>(::WaitForSingleObject(child.hProcess, INFINITE));
        ::CloseHandle(child.hThread);
        ::CloseHandle(child.hProcess);
    };
#endif

    "platform/process: executable_path names the binary being run"_test = [] {
        const auto path = refrain::platform::executable_path();
        if (!path.has_value()) {
            // Only Linux and Windows resolve the image path; a POSIX host
            // without /proc reports unsupported and the test says so.
            expect(static_cast<int>(path.error().code) ==
                   static_cast<int>(refrain::error_code::unsupported));
            return;
        }
        expect(path->starts_with('/') || path->find(':') != std::string::npos);
        std::error_code error;
        expect(fs::is_regular_file(fs::path{*path}, error));
        expect(!error);
    };

    "platform/process: null callback is invalid_argument"_test = [] {
        const auto installed =
            refrain::platform::install_fault_handler(nullptr, nullptr);
        expect(!installed.has_value());
        if (!installed.has_value()) {
            expect(static_cast<int>(installed.error().code) ==
                   static_cast<int>(refrain::error_code::invalid_argument));
        }
    };

    "platform/process: install/restore fault handler is idempotent"_test = [] {
        auto callback = [](const refrain::platform::fault_info &,
                           void *) noexcept {};
        const auto installed =
            refrain::platform::install_fault_handler(callback, nullptr);
        // Windows: unsupported (Crashpad is the supported path there).
        if (!installed.has_value()) {
            expect(static_cast<int>(installed.error().code) ==
                   static_cast<int>(refrain::error_code::unsupported));
            expect(true) << "SKIPPED: platform fault fallback unsupported "
                            "(Windows uses Crashpad)";
            return;
        }
        // Re-install replaces the callback without failing.
        expect(static_cast<bool>(
            refrain::platform::install_fault_handler(callback, nullptr)));
        refrain::platform::restore_fault_handlers();
        refrain::platform::restore_fault_handlers(); // idempotent
        expect(true); // sanity: both restore calls returned without crashing
    };

    "platform/process: fault marker is async-safe preformatted text"_test = [] {
        auto callback = [](const refrain::platform::fault_info &,
                           void *) noexcept {};
        const auto installed =
            refrain::platform::install_fault_handler(callback, nullptr);
        if (!installed.has_value()) {
            expect(true) << "SKIPPED: platform fault fallback unsupported "
                            "(Windows uses Crashpad)";
            return;
        }

        const refrain::test_support::scratch_directory scratch;
        const fs::path directory = scratch.path();
        const fs::path marker = directory / "crash_marker.txt";
        for (const auto kind : {refrain::platform::fault_kind::segmentation,
                                refrain::platform::fault_kind::abort}) {
            const auto native = refrain::platform::native_signal_for(kind);
            if (native == 0) {
                continue;
            }
            const bool written = refrain::platform::write_fault_marker(
                marker.string().c_str(), {kind, native});
            expect(written);
        }
        refrain::platform::restore_fault_handlers();

        const std::string text = read_text(marker);
        expect(!text.empty());
        expect(text.find("refrain crash pid=") != std::string::npos);
    };

#if !defined(_WIN32) && !defined(REFRAIN_SANITIZE_BUILD)
    // The fallback fault handler must die by signal instead of calling _exit
    // while the fault signal is still blocked: _exit(128 + sig) makes the child
    // exit *normally* and never produce a core dump. Dying by signal keeps the
    // OS core-dump and Crashpad paths working.
    // (Skipped under ASan/UBSan/TSan: the sanitizer runtimes intercept the
    // deliberately undefined null dereference.)
    "platform/process: fallback fault handler dies by signal, not _exit"_test =
        [] {
            auto callback = [](const refrain::platform::fault_info &,
                               void *) noexcept {};
            const auto installed =
                refrain::platform::install_fault_handler(callback, nullptr);
            expect(static_cast<bool>(installed));
            if (!installed) {
                return;
            }

            const pid_t child = ::fork();
            expect(child >= 0);
            if (child == 0) {
                // Child: suppress core-file litter but keep the default
                // disposition so death-by-signal is observable via waitpid().
                const struct rlimit no_core{0, 0};
                (void)::setrlimit(RLIMIT_CORE, &no_core);
                refrain::platform::trigger_segmentation_fault();
                ::_exit(0); // unreachable when the handler is correct
            }

            int status{0};
            expect(::waitpid(child, &status, 0) == child);
            expect(WIFSIGNALED(status));
            if (WIFSIGNALED(status)) {
                expect(WTERMSIG(status) == SIGSEGV);
            }
            refrain::platform::restore_fault_handlers();
        };
#endif

#if !defined(_WIN32)
    // The console log sink colours only an interactive terminal; this probe is
    // what keeps a redirected stderr (CI, pipes, files) plain.
    "platform/process: stderr is not a terminal when redirected"_test = [] {
        const refrain::test_support::scratch_directory scratch;
        const auto path{scratch.path() / "stderr.txt"};
        auto terminal{true};
        {
            const int saved{::dup(STDERR_FILENO)};
            expect(saved >= 0);
            const int file{
                ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600)};
            expect(file >= 0);
            if (file >= 0) {
                (void)::dup2(file, STDERR_FILENO);
                (void)::close(file);
            }
            terminal = refrain::platform::stderr_is_terminal();
            std::fflush(nullptr);
            if (saved >= 0) {
                (void)::dup2(saved, STDERR_FILENO);
                (void)::close(saved);
            }
        }
        expect(!terminal);
    };

#if defined(__linux__)
    // A redirected stream alone would also pass if the probe always returned
    // false; a pseudo-terminal proves the positive case.
    "platform/process: stderr is a terminal on a pseudo-terminal"_test = [] {
        const int master{::posix_openpt(O_RDWR | O_NOCTTY)};
        if (master < 0) {
            return; // no pseudo-terminal support in this environment
        }
        if (::grantpt(master) != 0 || ::unlockpt(master) != 0) {
            (void)::close(master);
            return;
        }
        const char *slave_name{::ptsname(master)};
        if (slave_name == nullptr) {
            (void)::close(master);
            return;
        }
        const int slave{::open(slave_name, O_RDWR | O_NOCTTY)};
        if (slave < 0) {
            (void)::close(master);
            return;
        }
        const int saved{::dup(STDERR_FILENO)};
        (void)::dup2(slave, STDERR_FILENO);
        const auto terminal{refrain::platform::stderr_is_terminal()};
        std::fflush(nullptr);
        if (saved >= 0) {
            (void)::dup2(saved, STDERR_FILENO);
            (void)::close(saved);
        }
        (void)::close(slave);
        (void)::close(master);
        expect(terminal);
    };
#endif
#endif

#if defined(_WIN32)
    // The console log sink colours only an interactive terminal; this probe is
    // what keeps a redirected stderr (CI, pipes, files) plain.
    "platform/process: stderr is not a terminal when redirected"_test = [] {
        const refrain::test_support::scratch_directory scratch;
        const auto path{scratch.path() / "stderr.txt"};
        const int saved{::_dup(::_fileno(stderr))};
        expect(saved >= 0);
        const int file{::_wopen(path.c_str(),
                                _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY,
                                _S_IREAD | _S_IWRITE)};
        expect(file >= 0);
        if (file >= 0) {
            (void)::_dup2(file, ::_fileno(stderr));
            (void)::_close(file);
        }
        const auto terminal{refrain::platform::stderr_is_terminal()};
        if (saved >= 0) {
            (void)::_dup2(saved, ::_fileno(stderr));
            (void)::_close(saved);
        }
        expect(!terminal);
    };
#endif
};

} // namespace
