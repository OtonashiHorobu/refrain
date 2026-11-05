// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/platform/file.hpp>
#include <refrain/platform/process.hpp>

#include <boost/ut.hpp>

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#if !defined(_WIN32)
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
};

} // namespace
