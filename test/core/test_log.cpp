// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/log.hpp>

#include <boost/ut.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {
using namespace boost::ut;

namespace fs = std::filesystem;

#if !defined(_WIN32)
/// Redirect `target` (STDOUT_FILENO / STDERR_FILENO) into `path` until
/// `restore()` runs. The destructor restores the descriptor as well, so a
/// failed assertion can never leave the test process writing into the temp
/// file.
class fd_redirect {
  public:
    fd_redirect(int target, const fs::path &path)
        : target_{target}, saved_{::dup(target)} {
        const int file{
            ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600)};
        if (file >= 0) {
            (void)::dup2(file, target_);
            (void)::close(file);
        }
    }

    fd_redirect(const fd_redirect &) = delete;
    auto operator=(const fd_redirect &) -> fd_redirect & = delete;

    ~fd_redirect() { restore(); }

    void restore() {
        if (saved_ < 0) {
            return;
        }
        std::fflush(nullptr); // flush stdout/stderr before switching back
        (void)::dup2(saved_, target_);
        (void)::close(saved_);
        saved_ = -1;
    }

  private:
    int target_{};
    int saved_{-1};
};
#endif

[[nodiscard]] auto count_occurrences(std::string_view text,
                                     std::string_view needle) -> std::size_t {
    auto count{std::size_t{}};
    auto position{text.find(needle)};
    while (position != std::string_view::npos) {
        ++count;
        position = text.find(needle, position + needle.size());
    }
    return count;
}

[[nodiscard]] auto scratch_directory() -> fs::path {
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    auto path{fs::temp_directory_path() /
              ("refrain-log-tests-" + std::to_string(stamp))};
    std::error_code error{};
    fs::create_directories(path, error);
    return path;
}

[[nodiscard]] auto read_text(const fs::path &path) -> std::string {
    std::ifstream in{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{in},
                       std::istreambuf_iterator<char>{}};
}

[[maybe_unused]] const suite tests = [] {
    "log/level names round-trip"_test = [] {
        for (const auto *name :
             {"trace", "debug", "info", "warn", "error", "critical", "off"}) {
            const auto lvl = refrain::log::level_from_string(name);
            expect(refrain::log::to_string(lvl) == name) << name;
        }
        expect(refrain::log::level_from_string("warning") ==
               refrain::log::level::warn);
        expect(refrain::log::level_from_string("fatal") ==
               refrain::log::level::critical);
        expect(refrain::log::level_from_string("bogus") ==
               refrain::log::level::info);
    };

    "log/init is idempotent and shutdown resets state"_test = [] {
        refrain::log::shutdown();
        refrain::log::config cfg{};
        cfg.console_level = refrain::log::level::off; // silent during tests
        expect(static_cast<bool>(refrain::log::init(cfg)));
        expect(refrain::log::initialized());

        // A second init while the sink list is empty re-applies the level and
        // returns the same (disabled) state; once a logger exists init is a
        // no-op until shutdown().
        expect(static_cast<bool>(refrain::log::init(cfg)));
        expect(refrain::log::current_level() == refrain::log::level::off);

        refrain::log::write(refrain::log::level::info,
                            "message with no sink must not crash");
        refrain::log::shutdown();
        expect(!refrain::log::initialized());

        // Re-initialization after shutdown works.
        expect(static_cast<bool>(refrain::log::init(cfg)));
        refrain::log::shutdown();
    };

    "log/writing while uninitialized is a no-op"_test = [] {
        refrain::log::shutdown();
        expect(!refrain::log::initialized());
        refrain::log::write(refrain::log::level::critical, "dropped");
    };

    // The formatting wrappers must short-circuit on the level check instead of
    // formatting, locking and dispatching for disabled levels.
    "log/enabled reflects the configured sink levels"_test = [] {
        refrain::log::shutdown();
        refrain::log::config cfg{};
        cfg.console_level = refrain::log::level::warn;
        expect(static_cast<bool>(refrain::log::init(cfg)));
        expect(refrain::log::enabled(refrain::log::level::warn));
        expect(refrain::log::enabled(refrain::log::level::error));
        expect(refrain::log::enabled(refrain::log::level::critical));
        expect(!refrain::log::enabled(refrain::log::level::info));
        expect(!refrain::log::enabled(refrain::log::level::trace));
        expect(!refrain::log::enabled(refrain::log::level::off));

        refrain::log::set_level(refrain::log::level::trace);
        expect(refrain::log::enabled(refrain::log::level::trace));
        expect(refrain::log::current_level() == refrain::log::level::trace);

        refrain::log::shutdown();
        expect(!refrain::log::enabled(refrain::log::level::critical));
    };

    // An init that enables no sinks records the level and drops messages; a
    // later init is a documented no-op (sinks are created by the first
    // successful init only) and never marks itself initialized without a logger
    // in a way that would swallow a later valid init.
    "log/init without sinks is idempotent and drops messages"_test = [] {
        refrain::log::shutdown();
        refrain::log::config silent{};
        silent.console_level = refrain::log::level::off;
        expect(static_cast<bool>(refrain::log::init(silent)));
        expect(refrain::log::initialized());
        expect(!refrain::log::enabled(refrain::log::level::error));

        // A second init with a file sink is a no-op per the documented
        // "only the first successful init creates sinks" contract.
        const auto directory = scratch_directory();
        const auto log_path = directory / "late.log";
        refrain::log::config late{};
        late.console_level = refrain::log::level::off;
        late.file_path = log_path.string();
        expect(static_cast<bool>(refrain::log::init(late)));
        expect(!fs::exists(log_path));

        refrain::log::write(refrain::log::level::error, "dropped");
        refrain::log::shutdown();
        std::error_code error;
        fs::remove_all(directory, error);
    };

    // A partially failed init must roll back the sinks it already created so
    // the logger is not left half-initialized.
    "log/partial init failure rolls back and a later init succeeds"_test = [] {
        refrain::log::shutdown();
        const auto directory = scratch_directory();
        const auto regular_file = directory / "not-a-directory";
        {
            std::ofstream out{regular_file};
            out << "x";
        }

        refrain::log::config broken{};
        broken.console_level = refrain::log::level::info; // console sink first
        broken.file_path = (regular_file / "nested" / "refrain.log").string();
        const auto failed = refrain::log::init(broken);
        expect(!failed);
        if (!failed) {
            expect(failed.error().code == refrain::error_code::io_error);
        }
        expect(!refrain::log::initialized());

        // The failure left no logger behind: a clean init works afterwards.
        refrain::log::config silent{};
        silent.console_level = refrain::log::level::off;
        expect(static_cast<bool>(refrain::log::init(silent)));
        expect(refrain::log::initialized());
        expect(!refrain::log::enabled(refrain::log::level::error));
        refrain::log::shutdown();

        std::error_code error;
        fs::remove_all(directory, error);
    };

    // Console diagnostics are stderr. stdout is the tools' machine-readable
    // data channel (`dump`, `data check`, `content where`, ... are grepped by
    // CI and parsed by scripts), so a log line there corrupts the output that
    // the caller redirects to a file.
    "log/console sink writes to stderr and leaves stdout clean"_test = [] {
#if !defined(_WIN32)
        refrain::log::shutdown();
        const auto directory = scratch_directory();
        const auto out_path = directory / "console-stdout.txt";
        const auto err_path = directory / "console-stderr.txt";

        bool initialized{false};
        {
            fd_redirect capture_out{STDOUT_FILENO, out_path};
            fd_redirect capture_err{STDERR_FILENO, err_path};

            refrain::log::config cfg{};
            cfg.console_level = refrain::log::level::info;
            cfg.enable_colors = false;
            initialized = static_cast<bool>(refrain::log::init(cfg));
            refrain::log::info("console direction probe {}", 7);
            refrain::log::shutdown(); // flushes the console sink

            capture_out.restore();
            capture_err.restore();
        }

        const auto out_text = read_text(out_path);
        const auto err_text = read_text(err_path);
        expect(initialized);
        expect(out_text.find("console direction probe 7") == std::string::npos)
            << "console log leaked into stdout: " << out_text;
        expect(err_text.find("console direction probe 7") != std::string::npos)
            << "console log missing from stderr: " << err_text;

        std::error_code error;
        fs::remove_all(directory, error);
#endif
    };

    // The async logger must honour its periodic flush option, and logs must
    // reach the file.
    "log/async logger with periodic flush writes to the file sink"_test = [] {
        refrain::log::shutdown();
        const auto directory = scratch_directory();
        const auto log_path = directory / "async.log";

        refrain::log::config cfg{};
        cfg.console_level = refrain::log::level::off;
        cfg.file_path = log_path.string();
        cfg.file_level = refrain::log::level::info;
        cfg.async = true;
        cfg.flush_interval_ms = 5;
        expect(static_cast<bool>(refrain::log::init(cfg)));
        expect(refrain::log::enabled(refrain::log::level::info));
        expect(!refrain::log::enabled(refrain::log::level::debug));

        refrain::log::info("async message {}", 42);
        refrain::log::shutdown(); // flushes the async logger

        const auto text = read_text(log_path);
        expect(text.find("async message 42") != std::string::npos);
        std::error_code error;
        fs::remove_all(directory, error);
    };

    // `flush_every()` only flushes loggers registered with the spdlog registry,
    // so the `refrain` logger must be registered for the periodic flush to run.
    // Read the file *before* shutdown() (which flushes explicitly and would
    // hide a missing periodic flush).
    "log/async periodic flush reaches the file without shutdown"_test = [] {
        refrain::log::shutdown();
        const auto directory = scratch_directory();
        const auto log_path = directory / "periodic.log";

        refrain::log::config cfg{};
        cfg.console_level = refrain::log::level::off;
        cfg.file_path = log_path.string();
        cfg.file_level = refrain::log::level::info;
        cfg.async = true;
        cfg.flush_interval_ms = 5;
        expect(static_cast<bool>(refrain::log::init(cfg)));

        refrain::log::info("periodic flush message {}", 7);
        // 50ms is ten flush intervals; retry for a while so a loaded CI machine
        // cannot turn this into a timing flake (the logger is never shut down,
        // so only the periodic flusher can put the line on disk).
        auto found{false};
        const auto deadline{std::chrono::steady_clock::now() +
                            std::chrono::seconds{1}};
        while (!found && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
            found = read_text(log_path).find("periodic flush message 7") !=
                    std::string::npos;
        }

        expect(fs::exists(log_path));
        expect(found) << "periodic flush never wrote the async line";

        // Cleanup for the tests that follow: the assertion above is the one
        // that must hold without a shutdown.
        refrain::log::shutdown();
        std::error_code error;
        fs::remove_all(directory, error);
    };

    // The write hot path must not serialize every message behind the
    // init/shutdown mutex. Concurrent writers must all reach the file sink, and
    // a concurrent shutdown must be safe (no deadlock/crash):
    // write()/shutdown() are noexcept and the logger pointer is atomic.
    "log/concurrent writers reach the sink without a global lock"_test = [] {
        refrain::log::shutdown();
        const auto directory = scratch_directory();
        const auto log_path = directory / "concurrent.log";

        refrain::log::config cfg{};
        cfg.console_level = refrain::log::level::off;
        cfg.file_path = log_path.string();
        cfg.file_level = refrain::log::level::info;
        expect(static_cast<bool>(refrain::log::init(cfg)));

        constexpr int writer_count{4};
        constexpr int messages_per_writer{250};
        std::vector<std::thread> writers{};
        writers.reserve(writer_count);
        for (int writer = 0; writer < writer_count; ++writer) {
            writers.emplace_back([writer] {
                for (int message = 0; message < messages_per_writer;
                     ++message) {
                    refrain::log::info("writer {} message {}", writer, message);
                }
            });
        }
        for (auto &thread : writers) {
            thread.join();
        }
        refrain::log::shutdown();

        const auto text = read_text(log_path);
        expect(count_occurrences(text, "writer ") ==
               static_cast<std::size_t>(writer_count) * messages_per_writer);
        expect(text.find("writer 0 message 0") != std::string::npos);
        expect(text.find("writer 3 message 249") != std::string::npos);

        std::error_code error;
        fs::remove_all(directory, error);
    };

    "log/thread name appears in the file pattern"_test = [] {
        refrain::log::shutdown();
        const auto directory = scratch_directory();
        const auto log_path = directory / "thread-name.log";

        refrain::log::config cfg{};
        cfg.console_level = refrain::log::level::off;
        cfg.file_path = log_path.string();
        cfg.file_level = refrain::log::level::info;
        expect(static_cast<bool>(refrain::log::init(cfg)));

        refrain::log::set_thread_name("worker-0");
        constexpr refrain::log::logger probe{"log-test"};
        probe.info("thread name probe {}", 7);
        refrain::log::info("untagged probe");
        refrain::log::shutdown();

        const auto text = read_text(log_path);
        expect(text.find("[worker-0/info] [log-test]: thread name probe 7") !=
               std::string::npos)
            << text;
        expect(text.find("[worker-0/info] untagged probe") != std::string::npos)
            << text;
        std::error_code error;
        fs::remove_all(directory, error);
    };

    // The async logger formats on spdlog's backend thread. The registry is
    // keyed by the producer's TID (copied into async_msg), so `%N` must still
    // print the producer name instead of the backend's.
    "log/async thread name keeps the producer name"_test = [] {
        refrain::log::shutdown();
        const auto directory = scratch_directory();
        const auto log_path = directory / "async-thread-name.log";

        refrain::log::config cfg{};
        cfg.console_level = refrain::log::level::off;
        cfg.file_path = log_path.string();
        cfg.file_level = refrain::log::level::info;
        cfg.async = true;
        cfg.flush_interval_ms = 5;
        expect(static_cast<bool>(refrain::log::init(cfg)));

        refrain::log::set_thread_name("worker-7");
        refrain::log::info("async thread name probe");
        refrain::log::shutdown();

        const auto text = read_text(log_path);
        expect(text.find("[worker-7/info] async thread name probe") !=
               std::string::npos)
            << text;
        std::error_code error;
        fs::remove_all(directory, error);
    };

    // Unnamed threads must keep the line machine-readable instead of printing
    // an empty bracket pair.
    "log/unnamed thread falls back to the numeric tid"_test = [] {
        refrain::log::shutdown();
        const auto directory = scratch_directory();
        const auto log_path = directory / "unnamed-thread.log";

        refrain::log::config cfg{};
        cfg.console_level = refrain::log::level::off;
        cfg.file_path = log_path.string();
        cfg.file_level = refrain::log::level::info;
        expect(static_cast<bool>(refrain::log::init(cfg)));

        refrain::log::set_thread_name({});
        refrain::log::info("unnamed thread probe");
        refrain::log::shutdown();

        const auto text = read_text(log_path);
        expect(text.find("[t") != std::string::npos) << text;
        expect(text.find("/info] unnamed thread probe") != std::string::npos)
            << text;
        std::error_code error;
        fs::remove_all(directory, error);
    };
};

} // namespace
