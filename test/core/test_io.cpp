// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/io.hpp>

#include <boost/ut.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h> // mkfifo
#endif

namespace {
using namespace boost::ut;

namespace fs = std::filesystem;

/// Self-cleaning unique scratch directory under the system temp dir.
class temp_directory {
  public:
    temp_directory() {
        static std::atomic<std::uint64_t> sequence{0};
        std::error_code error;
        fs::path base = fs::temp_directory_path(error);
        if (error) {
            base = fs::path{"."};
        }
        const auto stamp = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        base /=
            "refrain_io_test_" + std::to_string(stamp) + "_" +
            std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
        fs::create_directories(base, error);
        path_ = base;
    }

    ~temp_directory() {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    temp_directory(const temp_directory &) = delete;
    auto operator=(const temp_directory &) = delete;
    temp_directory(temp_directory &&) = delete;
    auto operator=(temp_directory &&) = delete;

    [[nodiscard]] auto path() const -> const fs::path & { return path_; }

  private:
    fs::path path_{};
};

[[nodiscard]] auto as_unsigned(std::byte value) -> unsigned {
    return std::to_integer<unsigned>(value);
}

suite<"core/io"> io_tests = [] {
    "io/text roundtrip"_test = [] {
        temp_directory dir;
        const std::string path = (dir.path() / "hello.txt").string();
        const std::string content{"hello refrain\nsecond line\n"};

        expect(refrain::core::io::write_text(path, content).has_value());

        const auto text = refrain::core::io::read_text(path);
        expect(text.has_value());
        if (text.has_value()) {
            expect(*text == content);
        }
        expect(refrain::core::io::exists(path));
        expect(refrain::core::io::is_regular_file(path));
        expect(!refrain::core::io::is_directory(path));

        const auto size = refrain::core::io::file_size(path);
        expect(size.has_value());
        if (size.has_value()) {
            expect(*size == static_cast<std::uint64_t>(content.size()));
        }
    };

    "io/binary roundtrip preserves zero bytes"_test = [] {
        temp_directory dir;
        const std::string path = (dir.path() / "blob.bin").string();
        const std::vector<std::byte> bytes{
            std::byte{0x00}, std::byte{0x01}, std::byte{0x7F}, std::byte{0x80},
            std::byte{0xFF}, std::byte{0x00}, std::byte{0x42}};

        expect(refrain::core::io::write_file(path, bytes).has_value());

        const auto read = refrain::core::io::read_file(path);
        expect(read.has_value());
        if (read.has_value()) {
            expect(read->size() == bytes.size());
            if (read->size() == bytes.size()) {
                for (std::size_t index = 0; index < bytes.size(); ++index) {
                    expect(as_unsigned((*read)[index]) ==
                           as_unsigned(bytes[index]));
                }
            }
        }
    };

    "io/atomic write replaces an existing file and cleans temp files"_test =
        [] {
            temp_directory dir;
            const std::string path = (dir.path() / "config.json").string();

            expect(
                refrain::core::io::write_text(path, "old content").has_value());
            expect(refrain::core::io::atomic_write_text(path, "new content")
                       .has_value());

            const auto text = refrain::core::io::read_text(path);
            expect(text.has_value());
            if (text.has_value()) {
                expect(*text == "new content");
            }

            const auto entries =
                refrain::core::io::list_directory(dir.path().string());
            expect(entries.has_value());
            if (entries.has_value()) {
                expect(entries->size() == 1U);
                if (entries->size() == 1U) {
                    expect((*entries)[0] == "config.json");
                }
            }
        };

    "io/atomic binary write creates a new file"_test = [] {
        temp_directory dir;
        const std::string path = (dir.path() / "new.bin").string();
        const std::vector<std::byte> bytes{std::byte{0xDE}, std::byte{0xAD},
                                           std::byte{0xBE}, std::byte{0xEF}};

        expect(refrain::core::io::atomic_write_file(path, bytes).has_value());

        const auto read = refrain::core::io::read_file(path);
        expect(read.has_value());
        if (read.has_value()) {
            expect(read->size() == bytes.size());
            if (read->size() == bytes.size()) {
                for (std::size_t index = 0; index < bytes.size(); ++index) {
                    expect(as_unsigned((*read)[index]) ==
                           as_unsigned(bytes[index]));
                }
            }
        }
    };

    "io/missing file reports io_error"_test = [] {
        temp_directory dir;
        const std::string path = (dir.path() / "missing.bin").string();

        const auto bytes = refrain::core::io::read_file(path);
        expect(!bytes.has_value());
        if (!bytes.has_value()) {
            expect(static_cast<int>(bytes.error().code) ==
                   static_cast<int>(refrain::error_code::io_error));
        }

        expect(!refrain::core::io::read_text(path).has_value());
        expect(!refrain::core::io::file_size(path).has_value());
        expect(!refrain::core::io::exists(path));
        expect(!refrain::core::io::is_regular_file(path));
    };

    "io/list_directory returns sorted entry names"_test = [] {
        temp_directory dir;
        for (const std::string name : {"beta.txt", "alpha.txt", "gamma.txt"}) {
            expect(
                refrain::core::io::write_text((dir.path() / name).string(), "x")
                    .has_value());
        }
        expect(refrain::core::io::create_directories(
                   (dir.path() / "delta").string())
                   .has_value());

        const auto names =
            refrain::core::io::list_directory(dir.path().string());
        expect(names.has_value());
        if (names.has_value()) {
            const std::vector<std::string> expected{"alpha.txt", "beta.txt",
                                                    "delta", "gamma.txt"};
            expect(names->size() == expected.size());
            if (names->size() == expected.size()) {
                for (std::size_t index = 0; index < expected.size(); ++index) {
                    expect((*names)[index] == expected[index]);
                }
            }
        }
    };

    "io/create_directories is idempotent and current/temp paths exist"_test =
        [] {
            temp_directory dir;
            const std::string nested = (dir.path() / "a" / "b" / "c").string();

            expect(refrain::core::io::create_directories(nested).has_value());
            expect(refrain::core::io::is_directory(nested));
            expect(refrain::core::io::create_directories(nested).has_value());

            const auto current = refrain::core::io::current_path();
            expect(current.has_value());
            if (current.has_value()) {
                expect(!current->empty());
            }

            const std::string temp = refrain::core::io::temp_directory();
            expect(!temp.empty());
            expect(refrain::core::io::is_directory(temp));
        };

    "io/write into a missing parent directory reports io_error"_test = [] {
        temp_directory dir;
        const std::string path = (dir.path() / "missing" / "file.txt").string();

        const auto written = refrain::core::io::write_text(path, "data");
        expect(!written.has_value());
        if (!written.has_value()) {
            expect(static_cast<int>(written.error().code) ==
                   static_cast<int>(refrain::error_code::io_error));
        }
    };

    // Path strings are UTF-8 on every platform. On a POSIX host this byte
    // sequence is the native filename; on Windows the implementation converts
    // from/to UTF-8 instead of the active ANSI code page.
    "io/utf-8 paths roundtrip through every helper"_test = [] {
        temp_directory dir;
        const std::string name{"unicode-\u00e4\u00f6\u00fc-\u4e2d\u6587.txt"};
        const std::string path = (dir.path() / name).string();
        const std::string content{"utf8 payload"};

        expect(refrain::core::io::write_text(path, content).has_value());
        expect(refrain::core::io::exists(path));
        expect(refrain::core::io::is_regular_file(path));

        const auto text = refrain::core::io::read_text(path);
        expect(text.has_value());
        if (text.has_value()) {
            expect(*text == content);
        }
        const auto size = refrain::core::io::file_size(path);
        expect(size.has_value());
        if (size.has_value()) {
            expect(*size == static_cast<std::uint64_t>(content.size()));
        }

        // list_directory returns the same UTF-8 name (not a transcoded one).
        const auto names =
            refrain::core::io::list_directory(dir.path().string());
        expect(names.has_value());
        if (names.has_value()) {
            expect(names->size() == std::size_t{1});
            if (names->size() == std::size_t{1}) {
                expect((*names)[0] == name);
            }
        }

        // Atomic replacement goes through the platform layer with the same
        // UTF-8 path.
        expect(
            refrain::core::io::atomic_write_text(path, "replaced").has_value());
        const auto replaced = refrain::core::io::read_text(path);
        expect(replaced.has_value());
        if (replaced.has_value()) {
            expect(*replaced == "replaced");
        }
    };

    // A failing stream operation must not report a stale/bogus OS cause
    // (e.g. ": Success"). Missing-parent open failures are the common case
    // where errno is not guaranteed to belong to the failing operation.
    "io/stream failures omit bogus os detail"_test = [] {
        temp_directory dir;
        const std::string missing_parent =
            (dir.path() / "no-such-dir" / "file.txt").string();
        const auto written =
            refrain::core::io::write_text(missing_parent, "data");
        expect(!written.has_value());
        if (!written.has_value()) {
            expect(written.error().code == refrain::error_code::io_error);
            expect(written.error().message.find("Success") ==
                   std::string::npos);
        }

        const auto read_missing =
            refrain::core::io::read_file((dir.path() / "nope.bin").string());
        expect(!read_missing.has_value());
        if (!read_missing.has_value()) {
            expect(read_missing.error().code == refrain::error_code::io_error);
            expect(read_missing.error().message.find("Success") ==
                   std::string::npos);
        }
    };

    // file_size() reports exact byte counts and surfaces non-regular-file
    // errors instead of returning a bogus value.
    "io/file_size reports exact sizes and rejects directories"_test = [] {
        temp_directory dir;
        const std::vector<std::byte> data(100'000U, std::byte{0x5a});
        const std::string path = (dir.path() / "blob.bin").string();
        expect(refrain::core::io::write_file(path, data).has_value());

        const auto size = refrain::core::io::file_size(path);
        expect(size.has_value());
        if (size.has_value()) {
            expect(*size == static_cast<std::uint64_t>(data.size()));
        }

        const auto directory_size =
            refrain::core::io::file_size(dir.path().string());
        expect(!directory_size.has_value());
        if (!directory_size.has_value()) {
            expect(directory_size.error().code ==
                   refrain::error_code::io_error);
        }
    };

    // Untrusted manifests must not be able to make the reader allocate their
    // whole declared size: the limit is checked before any reservation, and
    // the read loop enforces it again against growth.
    "io/read_file rejects files over max_bytes"_test = [] {
        temp_directory dir;
        const std::string path = (dir.path() / "big.bin").string();
        const std::vector<std::byte> data(4096U, std::byte{0x41});
        expect(refrain::core::io::write_file(path, data).has_value());

        const auto over = refrain::core::io::read_file(
            path, {.max_bytes = 1024U, .allow_symlink = true});
        expect(!over.has_value());
        if (!over.has_value()) {
            expect(over.error().code == refrain::error_code::invalid_argument);
        }
        expect(!refrain::core::io::read_text(path, {.max_bytes = 1024U})
                    .has_value());

        // The exact size still fits, and the no-options overload keeps its
        // permissive default.
        const auto exact = refrain::core::io::read_file(
            path, {.max_bytes = static_cast<std::uint64_t>(data.size()),
                   .allow_symlink = true});
        expect(exact.has_value());
        if (exact.has_value()) {
            expect(exact->size() == data.size());
        }
        expect(refrain::core::io::read_file(path).has_value());

        // A zero limit accepts only an empty file.
        expect(
            !refrain::core::io::read_file(path, {.max_bytes = 0U}).has_value());
        const std::string empty = (dir.path() / "empty.bin").string();
        expect(refrain::core::io::write_text(empty, "").has_value());
        const auto zero =
            refrain::core::io::read_file(empty, {.max_bytes = 0U});
        expect(zero.has_value());
        if (zero.has_value()) {
            expect(zero->empty());
        }
    };

    // A symlinked manifest must not be able to redirect a read outside the
    // instance root when the caller opts out of following links.
    "io/read_file rejects symlinks unless allowed"_test = [] {
        temp_directory dir;
        const std::string target = (dir.path() / "target.txt").string();
        expect(refrain::core::io::write_text(target, "payload").has_value());
        const std::string link = (dir.path() / "link.txt").string();
        std::error_code link_error;
        fs::create_symlink(target, link, link_error);
        if (link_error) {
            return; // no symlink support/privilege on this host
        }

        const auto refused =
            refrain::core::io::read_file(link, {.allow_symlink = false});
        expect(!refused.has_value());
        if (!refused.has_value()) {
            expect(refused.error().code ==
                   refrain::error_code::permission_denied);
        }
        const auto allowed =
            refrain::core::io::read_text(link, {.allow_symlink = true});
        expect(allowed.has_value());
        if (allowed.has_value()) {
            expect(*allowed == "payload");
        }
    };

    // Directories and FIFOs have no readable content. The FIFO case is the
    // important one: the status check must reject it *before* opening, because
    // opening a FIFO without a writer blocks forever.
    "io/read_file rejects non-regular files"_test = [] {
        temp_directory dir;
        const auto directory =
            refrain::core::io::read_file(dir.path().string());
        expect(!directory.has_value());
        if (!directory.has_value()) {
            expect(directory.error().code == refrain::error_code::io_error);
        }
#if !defined(_WIN32)
        const std::string fifo = (dir.path() / "pipe").string();
        if (::mkfifo(fifo.c_str(), 0600) == 0) {
            const auto pipe{refrain::core::io::read_file(
                fifo, {.max_bytes = 1024U * 1024U, .allow_symlink = false})};
            expect(!pipe.has_value());
            if (!pipe.has_value()) {
                expect(pipe.error().code == refrain::error_code::io_error);
            }
        }
#endif
    };

    "io/status_at reports target kind and errors"_test = [] {
        temp_directory dir;
        const std::string file = (dir.path() / "file.txt").string();
        const std::string subdir = (dir.path() / "sub").string();
        expect(refrain::core::io::write_text(file, "x").has_value());
        expect(refrain::core::io::create_directories(subdir).has_value());

        const auto file_status{refrain::core::io::status_at(file)};
        expect(file_status.has_value());
        if (file_status) {
            expect(file_status->exists);
            expect(file_status->regular);
            expect(!file_status->directory);
            expect(!file_status->symlink);
        }
        const auto dir_status{refrain::core::io::status_at(subdir)};
        expect(dir_status.has_value());
        if (dir_status) {
            expect(dir_status->exists);
            expect(dir_status->directory);
        }
        const auto missing{
            refrain::core::io::status_at((dir.path() / "missing").string())};
        expect(missing.has_value());
        if (missing) {
            expect(!missing->exists);
        }

        // A symlink is reported on the path itself while the target fields
        // describe the destination; permission to create one is platform
        // dependent, so skip when it fails.
        std::error_code error;
        const std::string link = (dir.path() / "link.txt").string();
        fs::create_symlink(file, link, error);
        if (!error) {
            const auto link_status{refrain::core::io::status_at(link)};
            expect(link_status.has_value());
            if (link_status) {
                expect(link_status->symlink);
                expect(link_status->exists);
                expect(link_status->regular);
            }
        }
    };
};

} // namespace
