// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/log.hpp>

#include <refrain/platform/process.hpp>

#include <boost/ut.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
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

/// Pin an environment variable for the lifetime of the guard and restore the
/// previous state afterwards. The colour tests set `NO_COLOR` and
/// `REFRAIN_COLOR` explicitly so neither the developer's shell nor the CI
/// environment can make them order-dependent, and no test leaks an override
/// into the rest of the suite.
class environment_guard {
  public:
    environment_guard(std::string name, std::optional<std::string> value)
        : name_{std::move(name)}, saved_{current_value()} {
        apply(value);
    }

    environment_guard(const environment_guard &) = delete;
    auto operator=(const environment_guard &) -> environment_guard & = delete;

    ~environment_guard() { apply(saved_); }

  private:
    [[nodiscard]] auto current_value() const -> std::optional<std::string> {
        const char *value{std::getenv(name_.c_str())};
        if (value == nullptr) {
            return std::nullopt;
        }
        return std::string{value};
    }

    void apply(const std::optional<std::string> &value) noexcept {
        static_cast<void>(
            value.has_value()
                ? refrain::platform::set_environment_variable(name_, *value)
                : refrain::platform::unset_environment_variable(name_));
    }

    std::string name_;
    std::optional<std::string> saved_;
};

/// Remove SGR escape sequences (`ESC [ ... m`) so a coloured capture can be
/// compared with the plain capture byte for byte.
[[nodiscard]] auto strip_ansi(std::string_view text) -> std::string {
    std::string plain{};
    plain.reserve(text.size());
    std::size_t index{0};
    while (index < text.size()) {
        if (text[index] == '\x1b' && index + 1 < text.size() &&
            text[index + 1] == '[') {
            index += 2;
            while (index < text.size() &&
                   ((text[index] >= '0' && text[index] <= '9') ||
                    text[index] == ';')) {
                ++index;
            }
            if (index < text.size() && text[index] == 'm') {
                ++index;
            }
            continue;
        }
        plain.push_back(text[index]);
        ++index;
    }
    return plain;
}

/// Strip the leading `[HH:MM:SS] ` so two captures made in different seconds
/// still compare equal.
[[nodiscard]] auto after_timestamp(std::string_view line) -> std::string_view {
    const auto bracket{line.find("] ")};
    return bracket == std::string_view::npos ? line : line.substr(bracket + 2);
}

#if !defined(_WIN32)
/// Capture everything the console sink writes to stderr while logging one
/// message. stderr is a regular file, so the stream is not a terminal.
[[nodiscard]] auto capture_console(const refrain::log::config &cfg,
                                   std::string_view message) -> std::string {
    refrain::log::shutdown();
    const auto directory = scratch_directory();
    const auto err_path = directory / "console-colour.txt";
    {
        const fd_redirect capture_err{STDERR_FILENO, err_path};
        expect(static_cast<bool>(refrain::log::init(cfg)));
        refrain::log::set_thread_name("console");
        refrain::log::write(refrain::log::level::info, message);
        refrain::log::shutdown();
    }
    const auto text = read_text(err_path);
    std::error_code error;
    fs::remove_all(directory, error);
    return text;
}
#endif

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
            cfg.console_color = refrain::log::color_mode::never;
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

    // The console pattern colours the level and nothing else: the markers sit
    // between the thread name and the level, and the closing bracket still
    // follows the level exactly as before.
    "log/console pattern marks the level field for colour"_test = [] {
        const auto pattern{refrain::log::console_pattern()};
        expect(pattern == "[%H:%M:%S] [%N/%^%l%$] %v") << pattern;
        expect(pattern.find("%^%l%$") != std::string_view::npos) << pattern;
    };

    // Documented precedence: REFRAIN_COLOR overrides the configured mode,
    // NO_COLOR only takes part in the automatic choice, and an explicit
    // `always` wins over NO_COLOR.
    "log/console colour resolution follows the documented precedence"_test =
        [] {
            using refrain::log::color_mode;
            using refrain::log::console_color_enabled;

            // automatic: an interactive terminal without NO_COLOR only.
            expect(console_color_enabled(color_mode::automatic, true, false));
            expect(!console_color_enabled(color_mode::automatic, true, true));
            expect(!console_color_enabled(color_mode::automatic, false, false));
            expect(!console_color_enabled(color_mode::automatic, false, true));

            // always: explicit, so it wins over NO_COLOR and redirection.
            expect(console_color_enabled(color_mode::always, true, true));
            expect(console_color_enabled(color_mode::always, false, true));
            expect(console_color_enabled(color_mode::always, false, false));

            // never: plain even on an interactive terminal.
            expect(!console_color_enabled(color_mode::never, true, false));
            expect(!console_color_enabled(color_mode::never, true, true));
        };

#if !defined(_WIN32)
    // A forced `always` writes the level's ANSI escape; stripping the escape
    // must give back the plain line byte for byte, so the colour mode never
    // changes the text that scripts and goldens parse.
    "log/forced colour wraps the level and leaves the line unchanged"_test =
        [] {
            const environment_guard refrain_color{"REFRAIN_COLOR",
                                                  std::nullopt};
            const environment_guard no_color{"NO_COLOR", std::nullopt};

            refrain::log::config coloured{};
            coloured.console_level = refrain::log::level::info;
            coloured.console_color = refrain::log::color_mode::always;
            const auto coloured_text{capture_console(coloured, "colour probe")};

            refrain::log::config plain{};
            plain.console_level = refrain::log::level::info;
            plain.console_color = refrain::log::color_mode::never;
            const auto plain_text{capture_console(plain, "colour probe")};

            // `info` is green; the reset closes the range before the bracket.
            // The thread label and the message stay outside the escape.
            expect(coloured_text.find(
                       "[console/\x1b[32minfo\x1b[m] colour probe") !=
                   std::string::npos)
                << coloured_text;
            expect(after_timestamp(strip_ansi(coloured_text)) ==
                   after_timestamp(plain_text))
                << coloured_text << " vs " << plain_text;
        };

    // CI, piped captures and the golden files compare the bytes of stderr, so
    // the default and the explicit `never` stay plain when it is redirected.
    "log/redirected console stays escape-free"_test = [] {
        const environment_guard refrain_color{"REFRAIN_COLOR", std::nullopt};
        const environment_guard no_color{"NO_COLOR", std::nullopt};

        for (const auto mode : {refrain::log::color_mode::never,
                                refrain::log::color_mode::automatic}) {
            refrain::log::config cfg{};
            cfg.console_level = refrain::log::level::info;
            cfg.console_color = mode;
            const auto text{capture_console(cfg, "plain probe")};
            expect(text.find("\x1b[") == std::string::npos) << text;
            expect(after_timestamp(text) ==
                   std::string_view{"[console/info] plain probe\n"})
                << text;
        }
    };

    // REFRAIN_COLOR overrides the configured mode in both directions, and an
    // unknown value is ignored instead of guessed.
    "log/REFRAIN_COLOR overrides the configured colour mode"_test = [] {
        const environment_guard no_color{"NO_COLOR", std::nullopt};

        {
            const environment_guard override_mode{"REFRAIN_COLOR", "never"};
            refrain::log::config cfg{};
            cfg.console_level = refrain::log::level::info;
            cfg.console_color = refrain::log::color_mode::always;
            const auto text{capture_console(cfg, "override probe")};
            expect(text.find("\x1b[") == std::string::npos) << text;
        }
        {
            const environment_guard override_mode{"REFRAIN_COLOR", "always"};
            refrain::log::config cfg{};
            cfg.console_level = refrain::log::level::info;
            cfg.console_color = refrain::log::color_mode::never;
            const auto text{capture_console(cfg, "override probe")};
            expect(text.find("\x1b[32minfo\x1b[m") != std::string::npos)
                << text;
        }
        {
            // An unknown value falls back to the configured mode.
            const environment_guard override_mode{"REFRAIN_COLOR", "bogus"};
            refrain::log::config cfg{};
            cfg.console_level = refrain::log::level::info;
            cfg.console_color = refrain::log::color_mode::never;
            const auto text{capture_console(cfg, "override probe")};
            expect(text.find("\x1b[") == std::string::npos) << text;
        }
    };

    // NO_COLOR disables the automatic choice, but a caller that explicitly
    // asks for colour still gets it.
    "log/always keeps colour even when NO_COLOR is set"_test = [] {
        {
            const environment_guard no_color{"NO_COLOR", "1"};
            const environment_guard override_mode{"REFRAIN_COLOR", "always"};
            refrain::log::config cfg{};
            cfg.console_level = refrain::log::level::info;
            cfg.console_color = refrain::log::color_mode::automatic;
            const auto text{capture_console(cfg, "no-color probe")};
            expect(text.find("\x1b[32minfo\x1b[m") != std::string::npos)
                << text;
        }
        {
            // `auto` on a redirected stderr stays plain even when the
            // configured mode would colour.
            const environment_guard no_color{"NO_COLOR", "1"};
            const environment_guard override_mode{"REFRAIN_COLOR", "auto"};
            refrain::log::config cfg{};
            cfg.console_level = refrain::log::level::info;
            cfg.console_color = refrain::log::color_mode::always;
            const auto text{capture_console(cfg, "no-color probe")};
            expect(text.find("\x1b[") == std::string::npos) << text;
        }
    };
#endif
};

} // namespace
