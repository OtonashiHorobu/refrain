// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Scratch directories for tests. Two test processes may run at the same time,
// so every scratch directory carries a name that is unique to its process and
// the directory below it is removed when the owning object leaves scope. The
// suites that use this helper obtain temporary paths through
// `scratch_directory` only; `temporary_path_violations` is the check that
// keeps a fixed name from coming back.

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace refrain::test_support {

/// A private directory under the system temporary directory. The name mixes a
/// clock stamp, a per-process token and a per-process counter, and creation
/// retries instead of sharing a candidate that already exists. The directory
/// and everything below it is removed on destruction, including through an
/// early return from a test.
class scratch_directory {
  public:
    scratch_directory() : path_{make_unique_directory()} {}

    ~scratch_directory() { remove(); }

    scratch_directory(const scratch_directory &) = delete;
    auto operator=(const scratch_directory &) -> scratch_directory & = delete;

    scratch_directory(scratch_directory &&other) noexcept
        : path_{std::exchange(other.path_, {})} {}

    auto operator=(scratch_directory &&other) noexcept -> scratch_directory & {
        if (this != &other) {
            remove();
            path_ = std::exchange(other.path_, {});
        }
        return *this;
    }

    [[nodiscard]] auto path() const noexcept -> const std::filesystem::path & {
        return path_;
    }

    [[nodiscard]] auto string() const -> std::string { return path_.string(); }

  private:
    void remove() noexcept {
        if (path_.empty()) {
            return;
        }
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        path_.clear();
    }

    static auto make_unique_directory() -> std::filesystem::path {
        std::error_code error;
        const auto base = std::filesystem::temp_directory_path(error);
        if (error) {
            throw std::system_error{error, "scratch directory"};
        }
        static const auto token = [] {
            std::random_device device;
            return (static_cast<std::uint64_t>(device()) << 32U) ^
                   static_cast<std::uint64_t>(device());
        }();
        static std::atomic<std::uint64_t> sequence{0};
        const auto stamp = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        for (int attempt = 0; attempt < 64; ++attempt) {
            const auto name = "refrain-test-" + std::to_string(stamp) + "-" +
                              std::to_string(token) + "-" +
                              std::to_string(sequence.fetch_add(
                                  1, std::memory_order_relaxed));
            const auto candidate = base / name;
            std::error_code created;
            if (std::filesystem::create_directories(candidate, created)) {
                return candidate;
            }
            if (created) {
                throw std::system_error{created, "scratch directory " +
                                                     candidate.string()};
            }
            // The candidate already belongs to another process or another
            // test; the next suffix is tried instead of sharing the tree.
        }
        throw std::runtime_error{
            "scratch directory: cannot create a unique directory under " +
            base.string()};
    }

    std::filesystem::path path_{};
};

/// One source line that reaches for the system temporary directory without
/// `scratch_directory`.
struct temporary_path_use {
    std::filesystem::path file{};
    std::size_t line{0};
    std::string token{};
};

/// Returns every line in `root` that builds a temporary path itself. `root` is
/// a source file or a directory searched recursively. A test suite calls this
/// on the sources it owns, so a fixed name in the system temporary directory
/// fails the run that reintroduces it.
[[nodiscard]] inline auto
temporary_path_violations(const std::filesystem::path &root)
    -> std::vector<temporary_path_use> {
    std::vector<temporary_path_use> violations;
    const auto inspect = [&violations](const std::filesystem::path &file) {
        const auto extension = file.extension();
        if (extension != ".cpp" && extension != ".hpp" && extension != ".cc" &&
            extension != ".h") {
            return;
        }
        std::ifstream stream{file};
        std::string text;
        std::size_t line = 0;
        while (std::getline(stream, text)) {
            ++line;
            const auto first = text.find_first_not_of(" \t");
            if (first == std::string::npos ||
                text.compare(first, 2, "//") == 0 ||
                text.compare(first, 1, "*") == 0 ||
                text.compare(first, 2, "/*") == 0) {
                continue;
            }
            for (const std::string_view token :
                 {"temp_directory_path", "file::temp_directory",
                  "io::temp_directory", "/tmp/", "TMPDIR"}) {
                if (text.find(token) != std::string::npos) {
                    violations.push_back(
                        temporary_path_use{.file = file,
                                           .line = line,
                                           .token = std::string{token}});
                }
            }
        }
    };
    if (std::filesystem::is_regular_file(root)) {
        inspect(root);
        return violations;
    }
    for (const auto &entry :
         std::filesystem::recursive_directory_iterator{root}) {
        if (entry.is_regular_file()) {
            inspect(entry.path());
        }
    }
    return violations;
}

} // namespace refrain::test_support
