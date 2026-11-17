// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/platform/file.hpp>

#if defined(_WIN32)

#include <refrain/core/error.hpp>

#include <windows.h>

#include <aclapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace refrain::platform::file {
namespace {

/// UTF-8 (engine convention) -> UTF-8 std::string for diagnostics.
[[nodiscard]] auto to_utf8(const std::filesystem::path &path) -> std::string {
    const std::u8string wide = path.u8string();
    return std::string{wide.begin(), wide.end()};
}

/// Build the diagnostic from an *already captured* native error: once
/// CloseHandle/DeleteFileW run, GetLastError() no longer describes the failure.
[[nodiscard]] auto io_failure(std::string_view what, std::string_view path,
                              DWORD native_error) -> error {
    std::string message{what};
    if (!path.empty()) {
        message += " '";
        message += path;
        message += "'";
    }
    message += ": ";
    message += std::system_category().message(static_cast<int>(native_error));
    return make_error(error_code::io_error, std::move(message));
}

/// Unique sibling name. The sequence is atomic because `atomic_replace` is
/// documented as concurrency-safe: a duplicated name would let two threads
/// write the same temporary file and destroy the atomicity of the rename.
///
/// Built with `std::to_chars` into a fixed buffer instead of `std::format`, so
/// the Windows implementation follows the same platform contract as POSIX:
/// name generation cannot throw for formatting reasons, and both platforms
/// produce the same shape of temporary name.
[[nodiscard]] auto temp_suffix() -> std::wstring {
    static std::atomic<std::uint64_t> sequence{0};
    const auto stamp = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto index = sequence.fetch_add(1, std::memory_order_relaxed);

    std::array<char, 80> buffer{};
    auto offset{std::size_t{0}};
    const auto append_text{[&buffer, &offset](std::string_view text) noexcept {
        for (const char character : text) {
            if (offset < buffer.size()) {
                buffer[offset++] = character;
            }
        }
    }};
    const auto append_number{[&buffer, &offset](std::uint64_t value) noexcept {
        if (offset >= buffer.size()) {
            return;
        }
        const auto result = std::to_chars(buffer.data() + offset,
                                          buffer.data() + buffer.size(), value);
        if (result.ec == std::errc{}) {
            offset = static_cast<std::size_t>(result.ptr - buffer.data());
        }
    }};
    append_text(".tmp.");
    append_number(static_cast<std::uint64_t>(::GetCurrentProcessId()));
    append_text(".");
    append_number(stamp);
    append_text(".");
    append_number(index);
    // The buffer only ever holds ASCII; widen byte for byte. 80 bytes hold any
    // 64-bit value plus the separators, so a failed conversion cannot truncate
    // a name into something another thread also generates.
    std::wstring suffix{};
    suffix.reserve(offset);
    for (std::size_t position = 0; position < offset; ++position) {
        suffix.push_back(
            static_cast<wchar_t>(static_cast<unsigned char>(buffer[position])));
    }
    // Not `noexcept`: the returned name is heap-allocated, so a hard OOM
    // reaches `atomic_replace()`'s documented hard-OOM boundary instead of
    // terminating.
    return suffix;
}

[[nodiscard]] auto write_temp_file(const std::filesystem::path &path,
                                   std::span<const std::byte> data) -> status {
    const HANDLE handle =
        ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return std::unexpected{io_failure("failed to create temporary file",
                                          "<temp>", ::GetLastError())};
    }
    std::size_t written{0};
    DWORD last_error{ERROR_SUCCESS};
    bool ok = true;
    while (written < data.size()) {
        DWORD chunk{0};
        const auto *first = data.data() + written;
        // DWORD-sized writes: a plain static_cast truncates data > 4 GiB (and a
        // size that is an exact multiple of 2^32 would loop forever).
        const auto remaining = static_cast<DWORD>(std::min<std::size_t>(
            data.size() - written, std::numeric_limits<DWORD>::max()));
        if (::WriteFile(handle, first, remaining, &chunk, nullptr) == 0) {
            last_error = ::GetLastError();
            ok = false;
            break;
        }
        if (chunk == 0) { // defensive: no progress would hang the loop
            last_error = ERROR_WRITE_FAULT;
            ok = false;
            break;
        }
        written += chunk;
    }
    if (ok && ::FlushFileBuffers(handle) == 0) {
        last_error = ::GetLastError();
        ok = false;
    }
    (void)::CloseHandle(handle);
    if (!ok) {
        return std::unexpected{io_failure(
            "failed to write/flush temporary file", "<temp>", last_error)};
    }
    return status_ok();
}

/// Current user's SID, copied into a buffer owned by the caller.
[[nodiscard]] auto current_user_sid() -> result<std::vector<BYTE>> {
    HANDLE token{INVALID_HANDLE_VALUE};
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token) == 0) {
        return std::unexpected{io_failure("failed to open the process token",
                                          {}, ::GetLastError())};
    }
    // One stack buffer holds the TOKEN_USER and its SID; align it to the
    // information structure so the cast below is valid.
    alignas(TOKEN_USER)
        std::array<std::byte, sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE>
            storage{};
    DWORD required{0};
    if (::GetTokenInformation(token, TokenUser, storage.data(),
                              static_cast<DWORD>(storage.size()),
                              &required) == 0) {
        const DWORD native_error{::GetLastError()};
        (void)::CloseHandle(token);
        return std::unexpected{
            io_failure("failed to query the process user", {}, native_error)};
    }
    (void)::CloseHandle(token);
    const auto *user{reinterpret_cast<const TOKEN_USER *>(storage.data())};
    const DWORD length{::GetLengthSid(user->User.Sid)};
    std::vector<BYTE> sid(static_cast<std::size_t>(length));
    if (::CopySid(length, sid.data(), user->User.Sid) == 0) {
        return std::unexpected{io_failure("failed to copy the process user SID",
                                          {}, ::GetLastError())};
    }
    return sid;
}

/// Principals that can bypass a directory DACL on Windows: the counterpart of
/// POSIX root, accepted by the owner-private check in addition to the owner.
[[nodiscard]] auto is_privileged_sid(PSID sid) -> bool {
    std::array<BYTE, SECURITY_MAX_SID_SIZE> buffer{};
    DWORD size{static_cast<DWORD>(buffer.size())};
    if (::CreateWellKnownSid(WinLocalSystemSid, nullptr, buffer.data(),
                             &size) != 0 &&
        ::EqualSid(sid, buffer.data()) != 0) {
        return true;
    }
    size = static_cast<DWORD>(buffer.size());
    return ::CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr,
                                buffer.data(), &size) != 0 &&
           ::EqualSid(sid, buffer.data()) != 0;
}

/// One-entry discretionary ACL: `FILE_ALL_ACCESS` for `user_sid` and nothing
/// else. Stored in 32-bit words because an ACL requires DWORD alignment.
struct private_acl {
    std::vector<std::uint32_t> storage{};

    [[nodiscard]] auto acl() noexcept -> ACL * {
        return reinterpret_cast<ACL *>(storage.data());
    }
};

[[nodiscard]] auto build_private_acl(PSID user_sid) -> result<private_acl> {
    const DWORD sid_length{::GetLengthSid(user_sid)};
    const DWORD acl_size{static_cast<DWORD>(
        sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) + sid_length)};
    private_acl result{};
    result.storage.resize(
        (static_cast<std::size_t>(acl_size) + sizeof(std::uint32_t) - 1U) /
        sizeof(std::uint32_t));
    if (::InitializeAcl(result.acl(), acl_size, ACL_REVISION) == 0) {
        return std::unexpected{io_failure("failed to initialize the private "
                                          "directory ACL",
                                          {}, ::GetLastError())};
    }
    if (::AddAccessAllowedAceEx(result.acl(), ACL_REVISION, 0, FILE_ALL_ACCESS,
                                user_sid) == 0) {
        return std::unexpected{io_failure("failed to build the private "
                                          "directory ACL",
                                          {}, ::GetLastError())};
    }
    return result;
}

} // namespace

auto executable_suffix() noexcept -> std::string_view { return ".exe"; }

auto native_path(std::string_view utf8_path) -> std::filesystem::path {
    const auto *first = reinterpret_cast<const char8_t *>(utf8_path.data());
    return std::filesystem::path{std::u8string{first, utf8_path.size()}};
}

auto native_string(std::string_view utf8_path) -> native_path_string {
    return native_path(utf8_path).wstring();
}

struct read_only_file::impl {
    HANDLE handle{INVALID_HANDLE_VALUE};
    std::uint64_t size{0};
    std::string path;

    ~impl() {
        if (handle != INVALID_HANDLE_VALUE) {
            (void)::CloseHandle(handle);
        }
    }
};

read_only_file::read_only_file() noexcept = default;
read_only_file::~read_only_file() = default;
read_only_file::read_only_file(read_only_file &&other) noexcept = default;
auto read_only_file::operator=(read_only_file &&other) noexcept
    -> read_only_file & = default;

read_only_file::read_only_file(std::unique_ptr<impl> state) noexcept
    : impl_{std::move(state)} {}

auto read_only_file::open(std::string_view utf8_path, bool follow_symlink)
    -> result<read_only_file> {
    if (utf8_path.empty()) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "cannot open an empty path")};
    }
    const std::filesystem::path location = native_path(utf8_path);
    // FILE_FLAG_OPEN_REPARSE_POINT opens the reparse point itself instead of
    // resolving it, which is the atomic counterpart of POSIX O_NOFOLLOW: the
    // attribute check below then refuses symbolic links (and junctions) that a
    // later, separate path query could no longer vouch for.
    // FILE_FLAG_BACKUP_SEMANTICS lets the open succeed for a directory so the
    // type check can report it as a non-regular file instead of an
    // access-denied error.
    DWORD flags{FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS};
    if (!follow_symlink) {
        flags |= FILE_FLAG_OPEN_REPARSE_POINT;
    }
    const HANDLE handle =
        ::CreateFileW(location.c_str(), GENERIC_READ,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      nullptr, OPEN_EXISTING, flags, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return std::unexpected{io_failure("failed to open for reading",
                                          utf8_path, ::GetLastError())};
    }
    // Pipes and devices can be opened by name; they are not regular files and
    // must be rejected before any read (a pipe read could block).
    if (::GetFileType(handle) != FILE_TYPE_DISK) {
        (void)::CloseHandle(handle);
        return std::unexpected{make_error(
            error_code::io_error,
            "cannot read a non-regular file (directory, FIFO or device) '" +
                std::string{utf8_path} + "'")};
    }
    BY_HANDLE_FILE_INFORMATION information{};
    if (::GetFileInformationByHandle(handle, &information) == 0) {
        const auto failure{io_failure("failed to query the opened file",
                                      utf8_path, ::GetLastError())};
        (void)::CloseHandle(handle);
        return std::unexpected{failure};
    }
    if (!follow_symlink &&
        (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        (void)::CloseHandle(handle);
        return std::unexpected{make_error(error_code::permission_denied,
                                          "refusing to follow symlink '" +
                                              std::string{utf8_path} + "'")};
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        (void)::CloseHandle(handle);
        return std::unexpected{make_error(
            error_code::io_error,
            "cannot read a non-regular file (directory, FIFO or device) '" +
                std::string{utf8_path} + "'")};
    }
    auto state{std::make_unique<impl>()};
    state->handle = handle;
    state->size =
        (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32U) |
        static_cast<std::uint64_t>(information.nFileSizeLow);
    state->path = std::string{utf8_path};
    return read_only_file{std::move(state)};
}

auto read_only_file::valid() const noexcept -> bool {
    return impl_ != nullptr && impl_->handle != INVALID_HANDLE_VALUE;
}

auto read_only_file::size() const noexcept -> std::uint64_t {
    return impl_ != nullptr ? impl_->size : std::uint64_t{0};
}

auto read_only_file::read(std::span<std::byte> out) -> result<std::size_t> {
    if (!valid()) {
        return std::unexpected{
            make_error(error_code::not_initialized, "file handle is closed")};
    }
    if (out.empty()) {
        return std::size_t{0};
    }
    const auto wanted = static_cast<DWORD>(
        std::min<std::size_t>(out.size(), std::numeric_limits<DWORD>::max()));
    DWORD count{0};
    if (::ReadFile(impl_->handle, out.data(), wanted, &count, nullptr) == 0) {
        return std::unexpected{
            io_failure("failed to read", impl_->path, ::GetLastError())};
    }
    return static_cast<std::size_t>(count);
}

auto atomic_replace(std::string_view path, std::span<const std::byte> data)
    -> status {
    if (path.empty()) {
        return std::unexpected{
            make_error(error_code::invalid_argument, "empty path")};
    }
    const std::filesystem::path target = native_path(path);
    if (target.empty()) {
        return std::unexpected{
            make_error(error_code::invalid_argument, "invalid UTF-8 path")};
    }
    // Atomic: two threads replacing the same path concurrently must never pick
    // the same temporary name.
    const std::filesystem::path temp{target.wstring() + temp_suffix()};

    if (auto written = write_temp_file(temp, data); !written) {
        (void)::DeleteFileW(temp.c_str());
        return written;
    }
    // Indexers, scanners and concurrent replacers can hold the target open for
    // a moment; MoveFileEx then reports a sharing violation. A short bounded
    // retry keeps the atomic-replace contract without masking permanent errors.
    for (int attempt{0}; attempt < 32; ++attempt) {
        if (::MoveFileExW(temp.c_str(), target.c_str(),
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) !=
            0) {
            return status_ok();
        }
        const DWORD native_error{::GetLastError()};
        if (native_error != ERROR_SHARING_VIOLATION &&
            native_error != ERROR_ACCESS_DENIED) {
            const auto failure{
                io_failure("failed to replace file", path, native_error)};
            (void)::DeleteFileW(temp.c_str());
            return std::unexpected{failure};
        }
        // Another writer replacing the same path only holds it for the
        // duration of its own move, so yield the CPU first and back off to a
        // real sleep once yielding stops helping.
        if (attempt < 4) {
            ::SwitchToThread();
        } else {
            ::Sleep(1);
        }
    }
    const auto failure{
        io_failure("failed to replace file", path, ::GetLastError())};
    (void)::DeleteFileW(temp.c_str());
    return std::unexpected{failure};
}

auto temp_directory() -> std::string {
    std::error_code error{};
    const auto directory = std::filesystem::temp_directory_path(error);
    if (error || directory.empty()) {
        return std::string{"."};
    }
    return to_utf8(directory);
}

auto create_private_directory(std::string_view utf8_path) -> status {
    if (utf8_path.empty()) {
        return std::unexpected{
            make_error(error_code::invalid_argument, "empty directory path")};
    }
    const std::filesystem::path path = native_path(utf8_path);

    auto user_sid{current_user_sid()};
    if (!user_sid) {
        return std::unexpected{std::move(user_sid.error())};
    }
    auto acl{build_private_acl(user_sid->data())};
    if (!acl) {
        return std::unexpected{std::move(acl.error())};
    }
    SECURITY_DESCRIPTOR descriptor{};
    if (::InitializeSecurityDescriptor(&descriptor,
                                       SECURITY_DESCRIPTOR_REVISION) == 0) {
        return std::unexpected{
            io_failure("failed to initialize the private directory security "
                       "descriptor",
                       utf8_path, ::GetLastError())};
    }
    if (::SetSecurityDescriptorDacl(&descriptor, TRUE, acl->acl(), FALSE) ==
        0) {
        return std::unexpected{
            io_failure("failed to attach the private directory ACL", utf8_path,
                       ::GetLastError())};
    }
    // Protected DACL: no inherited ACE from the parent directory is added, so a
    // parent that grants other users access cannot widen this directory.
    if (::SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED,
                                       SE_DACL_PROTECTED) == 0) {
        return std::unexpected{
            io_failure("failed to protect the private directory ACL", utf8_path,
                       ::GetLastError())};
    }
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = &descriptor;
    attributes.bInheritHandle = FALSE;

    // Create missing parents first: only the final directory carries the
    // explicit ACL and a parent that is itself owner-private would lock the
    // caller out of unrelated siblings.
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code error{};
        std::filesystem::create_directories(parent, error);
        if (error) {
            return std::unexpected{make_error(
                error_code::io_error, "failed to create private directory '" +
                                          std::string{utf8_path} +
                                          "': " + error.message())};
        }
    }
    if (::CreateDirectoryW(path.c_str(), &attributes) != 0) {
        return status_ok();
    }
    const DWORD native_error{::GetLastError()};
    if (native_error != ERROR_ALREADY_EXISTS) {
        return std::unexpected{io_failure("failed to create private directory",
                                          utf8_path, native_error)};
    }
    // The directory already exists (idempotent call): restrict it in place so a
    // directory left at the inherited permissions is tightened as well.
    const DWORD attributes_now{::GetFileAttributesW(path.c_str())};
    if (attributes_now == INVALID_FILE_ATTRIBUTES ||
        (attributes_now & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return std::unexpected{make_error(
            error_code::io_error, "failed to create private directory '" +
                                      std::string{utf8_path} +
                                      "': path exists and is not a directory")};
    }
    const DWORD status = ::SetNamedSecurityInfoW(
        const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, acl->acl(), nullptr);
    if (status != ERROR_SUCCESS) {
        return std::unexpected{io_failure(
            "failed to restrict private directory", utf8_path, status)};
    }
    return status_ok();
}

auto is_owner_private_directory(std::string_view utf8_path) -> bool {
    if (utf8_path.empty()) {
        return false;
    }
    const std::filesystem::path path = native_path(utf8_path);
    const DWORD attributes{::GetFileAttributesW(path.c_str())};
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return false;
    }
    auto user_sid{current_user_sid()};
    if (!user_sid) {
        return false;
    }
    PSID owner{nullptr};
    PACL dacl{nullptr};
    PSECURITY_DESCRIPTOR descriptor{nullptr};
    const DWORD status = ::GetNamedSecurityInfoW(
        const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr,
        &dacl, nullptr, &descriptor);
    if (status != ERROR_SUCCESS) {
        return false;
    }
    const auto release{[&descriptor]() noexcept {
        if (descriptor != nullptr) {
            (void)::LocalFree(descriptor);
        }
    }};
    // A missing DACL (null) grants everyone full access, and a directory owned
    // by another account is not this process's private directory.
    if (owner == nullptr || dacl == nullptr ||
        ::EqualSid(owner, user_sid->data()) == 0) {
        release();
        return false;
    }
    ACL_SIZE_INFORMATION information{};
    if (::GetAclInformation(dacl, &information, sizeof(information),
                            AclSizeInformation) == 0) {
        release();
        return false;
    }
    bool owner_can_access{false};
    for (DWORD index = 0; index < information.AceCount; ++index) {
        void *ace{nullptr};
        if (::GetAce(dacl, index, &ace) == 0) {
            release();
            return false;
        }
        const auto *header{static_cast<const ACE_HEADER *>(ace)};
        if (header->AceType == ACCESS_DENIED_ACE_TYPE) {
            continue; // a deny entry only narrows access
        }
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
            // An object ACE or an unknown type cannot be proven to be
            // owner-only; fail closed instead of claiming privacy.
            release();
            return false;
        }
        const auto *allowed{static_cast<const ACCESS_ALLOWED_ACE *>(ace)};
        auto *sid{reinterpret_cast<PSID>(
            const_cast<DWORD *>(&allowed->SidStart))}; // NOLINT
        if (::EqualSid(sid, user_sid->data()) != 0) {
            owner_can_access = true;
            continue;
        }
        if (is_privileged_sid(sid)) {
            continue; // SYSTEM/Administrators can bypass any DACL anyway
        }
        // Another principal has an allow entry: the directory is not private.
        release();
        return false;
    }
    release();
    // An empty DACL would deny the owner too; that is not an accessible
    // private directory.
    return owner_can_access;
}

} // namespace refrain::platform::file

#else

// POSIX implementation: see src/platform/posix/file.cpp.

#endif // _WIN32
