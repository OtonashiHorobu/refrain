// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/platform/file.hpp>
#include <refrain/platform/process.hpp>

#include <boost/ut.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>

#include <aclapi.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace {
using namespace boost::ut;

namespace fs = std::filesystem;

/// Unique scratch file under the platform temp directory.
class scratch_file {
  public:
    explicit scratch_file(std::string name)
        : path_{fs::path{refrain::platform::file::temp_directory()} /
                (std::move(name) + "-pid" +
                 std::to_string(refrain::platform::process_id()))} {
        std::error_code error;
        (void)fs::remove(path_, error);
    }

    scratch_file(const scratch_file &) = delete;
    scratch_file &operator=(const scratch_file &) = delete;

    ~scratch_file() {
        std::error_code error;
        (void)fs::remove(path_, error);
    }

    [[nodiscard]] auto path() const -> const fs::path & { return path_; }

    [[nodiscard]] auto read_text() const -> std::string {
        std::ifstream in{path_, std::ios::binary};
        return std::string{std::istreambuf_iterator<char>{in},
                           std::istreambuf_iterator<char>{}};
    }

  private:
    fs::path path_;
};

constexpr std::string_view sample_text = "atomic replacement payload";

[[nodiscard]] auto as_bytes(std::string_view text) -> std::vector<std::byte> {
    const auto view =
        std::as_bytes(std::span<const char>{text.data(), text.size()});
    return std::vector<std::byte>{view.begin(), view.end()};
}

suite<"platform/file"> file_tests = [] {
    "platform/file: atomic_replace writes a new file"_test = [] {
        const scratch_file file{"refrain-platform-file-new.txt"};
        const auto bytes = as_bytes(sample_text);
        expect(static_cast<bool>(refrain::platform::file::atomic_replace(
            file.path().string(), bytes)));
        expect(fs::is_regular_file(file.path()));
        expect(file.read_text() == sample_text);
    };

    "platform/file: atomic_replace overwrites existing content"_test = [] {
        const scratch_file file{"refrain-platform-file-overwrite.txt"};
        expect(static_cast<bool>(refrain::platform::file::atomic_replace(
            file.path().string(), as_bytes("old"))));
        expect(static_cast<bool>(refrain::platform::file::atomic_replace(
            file.path().string(), as_bytes("new-and-longer-content"))));
        expect(file.read_text() == "new-and-longer-content");
    };

    "platform/file: empty path is rejected"_test = [] {
        const auto replaced = refrain::platform::file::atomic_replace(
            "", std::span<const std::byte>{});
        expect(!replaced.has_value());
        if (!replaced.has_value()) {
            expect(static_cast<int>(replaced.error().code) ==
                   static_cast<int>(refrain::error_code::invalid_argument));
        }
    };

    "platform/file: missing parent directory is io_error and leaves no temp"_test =
        [] {
            const fs::path directory =
                fs::path{refrain::platform::file::temp_directory()} /
                ("refrain-platform-file-missing-dir-pid" +
                 std::to_string(refrain::platform::process_id()));
            std::error_code error;
            (void)fs::remove_all(directory, error);
            const fs::path target = directory / "file.txt";

            const auto replaced = refrain::platform::file::atomic_replace(
                target.string(), as_bytes("data"));
            expect(!replaced.has_value());
            if (!replaced.has_value()) {
                expect(static_cast<int>(replaced.error().code) ==
                       static_cast<int>(refrain::error_code::io_error));
            }
            expect(!fs::exists(target));
        };

    "platform/file: temp_directory exists and is a directory"_test = [] {
        const auto directory = refrain::platform::file::temp_directory();
        expect(!directory.empty());
        std::error_code error;
        expect(fs::is_directory(fs::path{directory}, error));
    };

    // The temp-name sequence must be safe under concurrency. Concurrent
    // replacements of the same target must all succeed, leave exactly the
    // target file behind and end with one of the written payloads (never
    // interleaved content).
    "platform/file: concurrent atomic_replace stays consistent"_test = [] {
        const fs::path directory =
            fs::path{refrain::platform::file::temp_directory()} /
            ("refrain-platform-file-concurrent-pid" +
             std::to_string(refrain::platform::process_id()));
        std::error_code error;
        (void)fs::remove_all(directory, error);
        (void)fs::create_directories(directory, error);
        const fs::path target = directory / "shared.txt";

        constexpr int thread_count{4};
        constexpr int iterations{50};
        std::atomic<int> failures{0};
        std::vector<std::thread> writers{};
        writers.reserve(thread_count);
        for (int thread = 0; thread < thread_count; ++thread) {
            writers.emplace_back([&target, &failures, thread] {
                for (int iteration = 0; iteration < iterations; ++iteration) {
                    const std::string payload{"payload-" +
                                              std::to_string(thread) + "-" +
                                              std::to_string(iteration)};
                    if (!refrain::platform::file::atomic_replace(
                            target.string(), as_bytes(payload))) {
                        failures.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        for (auto &writer : writers) {
            writer.join();
        }

        expect(failures.load(std::memory_order_relaxed) == 0);
        std::ifstream in{target, std::ios::binary};
        const std::string text{std::istreambuf_iterator<char>{in},
                               std::istreambuf_iterator<char>{}};
        expect(text.starts_with("payload-"));

        // No leftover temp files (a duplicate name would leave one behind).
        std::size_t entries{0};
        for (const auto &entry : fs::directory_iterator{directory, error}) {
            expect(entry.path() == target);
            ++entries;
        }
        expect(entries == 1U);
        (void)fs::remove_all(directory, error);
    };

#if !defined(_WIN32)
    // Replacing an existing file must keep its permissions (a 0600
    // config/token file must not be widened by the default create mode).
    "platform/file: atomic_replace preserves existing permissions"_test = [] {
        const scratch_file file{"refrain-platform-file-perms.txt"};
        expect(static_cast<bool>(refrain::platform::file::atomic_replace(
            file.path().string(), as_bytes("secret"))));
        std::error_code error;
        fs::permissions(file.path(),
                        fs::perms::owner_read | fs::perms::owner_write,
                        fs::perm_options::replace, error);
        expect(!error);

        expect(static_cast<bool>(refrain::platform::file::atomic_replace(
            file.path().string(), as_bytes("secret-v2"))));
        const auto status = fs::status(file.path(), error);
        expect(!error);
        expect((status.permissions() & fs::perms::group_all) ==
               fs::perms::none);
        expect((status.permissions() & fs::perms::others_all) ==
               fs::perms::none);
        expect(file.read_text() == "secret-v2");
    };

    // Umask behaviour: a *new* file must obey the process umask rather than a
    // fixed mode that bypasses it (077 => expected 0600). umask() is
    // process-wide, so save it and restore it from a scope guard; Boost.UT
    // records failures without unwinding, so a plain trailing umask() call
    // would be skipped on an early return.
    "platform/file: atomic_replace honours the umask for a new file"_test = [] {
        const scratch_file file{"refrain-platform-file-umask.txt"};
        struct umask_guard {
            mode_t previous{::umask(077)};
            ~umask_guard() { (void)::umask(previous); }
        } guard{};

        expect(static_cast<bool>(refrain::platform::file::atomic_replace(
            file.path().string(), as_bytes("private"))));
        struct stat status{};
        expect(::stat(file.path().c_str(), &status) == 0);
        expect((status.st_mode & 0777) == 0600);
    };
#endif

    // The read handle performs the open and the type check in one step. These
    // tests cover the observable contract: a symlink is refused atomically by
    // the open (never a status query followed by a path open), and a FIFO is
    // rejected without blocking.
    "platform/file: read handle reads a regular file"_test = [] {
        const scratch_file file{"refrain-platform-read-regular.txt"};
        expect(static_cast<bool>(refrain::platform::file::atomic_replace(
            file.path().string(), as_bytes(sample_text))));

        auto handle{refrain::platform::file::read_only_file::open(
            file.path().string(), /*follow_symlink=*/true)};
        expect(handle.has_value());
        if (!handle) {
            return;
        }
        expect(handle->valid());
        expect(handle->size() ==
               static_cast<std::uint64_t>(sample_text.size()));

        std::vector<std::byte> bytes(sample_text.size());
        expect(handle->read(std::span<std::byte>{bytes}).has_value());
        expect(bytes == as_bytes(sample_text));
        // A read at end of file reports zero bytes.
        const auto tail{handle->read(std::span<std::byte>{bytes})};
        expect(tail.has_value() && *tail == std::size_t{0});

        const auto missing{refrain::platform::file::read_only_file::open(
            (file.path().parent_path() / "refrain-platform-read-missing.txt")
                .string(),
            /*follow_symlink=*/true)};
        expect(!missing.has_value());
        if (!missing) {
            expect(missing.error().code == refrain::error_code::io_error);
        }
        const auto empty{refrain::platform::file::read_only_file::open(
            "", /*follow_symlink=*/true)};
        expect(!empty.has_value());
        if (!empty) {
            expect(empty.error().code == refrain::error_code::invalid_argument);
        }
    };

    "platform/file: read handle refuses a symlink unless following is asked"_test =
        [] {
            const scratch_file target{"refrain-platform-read-target.txt"};
            expect(static_cast<bool>(refrain::platform::file::atomic_replace(
                target.path().string(), as_bytes("target-payload"))));
            const fs::path link{
                target.path().parent_path() /
                ("refrain-platform-read-link-pid" +
                 std::to_string(refrain::platform::process_id()) + ".txt")};
            std::error_code error;
            (void)fs::remove(link, error);
            fs::create_symlink(target.path(), link, error);
            if (error) {
                return; // no symlink support/privilege on this host
            }

            const auto refused{refrain::platform::file::read_only_file::open(
                link.string(), /*follow_symlink=*/false)};
            expect(!refused.has_value());
            if (!refused) {
                expect(refused.error().code ==
                       refrain::error_code::permission_denied);
            }

            auto allowed{refrain::platform::file::read_only_file::open(
                link.string(), /*follow_symlink=*/true)};
            expect(allowed.has_value());
            if (allowed) {
                std::vector<std::byte> bytes(64);
                const auto count{allowed->read(std::span<std::byte>{bytes})};
                expect(count.has_value());
                if (count) {
                    bytes.resize(*count);
                    expect(bytes == as_bytes("target-payload"));
                }
            }
            (void)fs::remove(link, error);
        };

#if !defined(_WIN32)
    // Opening a FIFO read-only blocks until a writer appears unless the open is
    // non-blocking. The handle must reject the FIFO instead of hanging; if this
    // call ever blocks, the test suite itself hangs, which is the failure mode
    // the non-blocking open exists to prevent.
    "platform/file: read handle rejects a FIFO without blocking"_test = [] {
        const fs::path directory{
            fs::path{refrain::platform::file::temp_directory()} /
            ("refrain-platform-read-fifo-pid" +
             std::to_string(refrain::platform::process_id()))};
        std::error_code error;
        (void)fs::remove_all(directory, error);
        fs::create_directories(directory, error);
        const fs::path fifo{directory / "pipe"};
        if (::mkfifo(fifo.c_str(), 0600) != 0) {
            (void)fs::remove_all(directory, error);
            return;
        }
        const auto rejected{refrain::platform::file::read_only_file::open(
            fifo.string(), /*follow_symlink=*/false)};
        expect(!rejected.has_value());
        if (!rejected) {
            expect(rejected.error().code == refrain::error_code::io_error);
        }
        (void)fs::remove_all(directory, error);
    };
#endif

    // The owner-private contract is enforced against the real permission
    // state: a directory that another local account can read is rejected, and
    // create_private_directory() is what makes the same directory private.
    "platform/file: owner-private check rejects a non-private directory"_test =
        [] {
            const fs::path directory{
                fs::path{refrain::platform::file::temp_directory()} /
                ("refrain-platform-private-dir-pid" +
                 std::to_string(refrain::platform::process_id()))};
            std::error_code error;
            (void)fs::remove_all(directory, error);
            fs::create_directories(directory, error);
            expect(!error);
            if (error) {
                return;
            }
            // A directory that does not exist is never owner-private.
            expect(!refrain::platform::file::is_owner_private_directory(
                (directory / "missing").string()));

        // Make the directory readable by group/other accounts. On Windows
        // the equivalent is an explicit Everyone ACE: default ACLs inherit
        // SYSTEM/Administrators (accepted like POSIX root) and may already
        // be owner-only for the creating user.
#if defined(_WIN32)
            {
                PSID everyone{nullptr};
                SID_IDENTIFIER_AUTHORITY world{SECURITY_WORLD_SID_AUTHORITY};
                expect(::AllocateAndInitializeSid(&world, 1, SECURITY_WORLD_RID,
                                                  0, 0, 0, 0, 0, 0, 0,
                                                  &everyone) != 0);
                if (everyone != nullptr) {
                    EXPLICIT_ACCESSW entry{};
                    entry.grfAccessPermissions = FILE_GENERIC_READ;
                    entry.grfAccessMode = GRANT_ACCESS;
                    entry.grfInheritance = NO_INHERITANCE;
                    entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
                    entry.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
                    entry.Trustee.ptstrName =
                        reinterpret_cast<LPWSTR>(everyone);
                    PACL acl{nullptr};
                    expect(::SetEntriesInAclW(1, &entry, nullptr, &acl) ==
                           ERROR_SUCCESS);
                    if (acl != nullptr) {
                        const auto *const path_text{directory.c_str()};
                        expect(::SetNamedSecurityInfoW(
                                   const_cast<LPWSTR>(path_text),
                                   SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                   nullptr, nullptr, acl,
                                   nullptr) == ERROR_SUCCESS);
                        (void)::LocalFree(acl);
                    }
                    ::FreeSid(everyone);
                }
            }
#else
            fs::permissions(directory,
                            fs::perms::owner_all | fs::perms::group_read |
                                fs::perms::group_exec | fs::perms::others_read |
                                fs::perms::others_exec,
                            fs::perm_options::replace, error);
            expect(!error);
#endif
            expect(!refrain::platform::file::is_owner_private_directory(
                directory.string()));

            // create_private_directory() restricts it in place (it exists
            // already), and the check then accepts it.
            expect(static_cast<bool>(
                refrain::platform::file::create_private_directory(
                    directory.string())));
            expect(refrain::platform::file::is_owner_private_directory(
                directory.string()));
            (void)fs::remove_all(directory, error);
        };
};

} // namespace
