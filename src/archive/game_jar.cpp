// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/archive/game_jar.hpp>

#include <refrain/core/io.hpp>

#include <simdjson.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace refrain::archive {

namespace {

namespace io = refrain::core::io;

/// One bundler archive version directory. A real 26.x archive has exactly one.
constexpr std::string_view bundler_prefix{"META-INF/versions/"};
constexpr std::string_view bundler_suffix{".jar"};

[[nodiscard]] auto starts_with(std::string_view value,
                               std::string_view prefix) noexcept -> bool {
    return value.size() >= prefix.size() &&
           value.substr(0, prefix.size()) == prefix;
}

[[nodiscard]] auto has_root_data(const zip_reader &reader) -> bool {
    return std::any_of(reader.entries().begin(), reader.entries().end(),
                       [](const zip_entry &entry) {
                           return !entry.directory &&
                                  starts_with(entry.name, "data/");
                       });
}

/// Parse the `id` field of a version document. A malformed or unreadable
/// document is not fatal: the bundler scan below can still find the inner
/// archive when the archive uses a different version layout.
[[nodiscard]] auto parse_version_id(std::span<const std::byte> bytes)
    -> std::string {
    if (bytes.empty()) {
        return {};
    }
    simdjson::dom::parser parser{};
    const auto text{std::string_view{
        reinterpret_cast<const char *>(bytes.data()), bytes.size()}};
    simdjson::dom::element document{};
    if (parser.parse(text).get(document) != simdjson::SUCCESS) {
        return {};
    }
    simdjson::dom::element id{};
    if (document["id"].get(id) != simdjson::SUCCESS) {
        return {};
    }
    std::string_view value{};
    if (id.get_string().get(value) != simdjson::SUCCESS) {
        return {};
    }
    return std::string{value};
}

/// Determine which archive holds the data tree.
struct archive_choice {
    /// Empty when the outer archive already holds `data/`.
    std::string inner_entry{};
    std::string version_id{};
};

[[nodiscard]] auto choose_data_archive(const zip_reader &outer,
                                       std::span<const std::byte> version_bytes)
    -> result<archive_choice> {
    archive_choice choice{};
    choice.version_id = parse_version_id(version_bytes);
    if (!choice.version_id.empty()) {
        const std::string expected{std::string{bundler_prefix} +
                                   choice.version_id + "/server-" +
                                   choice.version_id + ".jar"};
        if (outer.find(expected) != nullptr) {
            choice.inner_entry = expected;
            return choice;
        }
    }
    // Fall back to the archive layout: any versioned server jar. Sort so the
    // choice does not depend on central-directory order.
    std::vector<std::string_view> candidates{};
    for (const auto &entry : outer.entries()) {
        if (entry.directory || !starts_with(entry.name, bundler_prefix) ||
            !entry.name.ends_with(bundler_suffix) ||
            entry.name.find("/server-") == std::string_view::npos) {
            continue;
        }
        candidates.push_back(entry.name);
    }
    if (candidates.empty()) {
        return std::unexpected{make_error(
            error_code::parse_error,
            "the archive is not a Minecraft server jar: it has neither a "
            "data/ tree nor a bundler server jar")};
    }
    std::sort(candidates.begin(), candidates.end());
    choice.inner_entry = std::string{candidates.front()};
    if (choice.version_id.empty()) {
        // Derive the version from `META-INF/versions/<id>/server-<id>.jar`.
        const std::string_view path{choice.inner_entry};
        const auto after_prefix{path.substr(bundler_prefix.size())};
        const auto separator{after_prefix.find('/')};
        if (separator != std::string_view::npos) {
            choice.version_id = std::string{after_prefix.substr(0, separator)};
        }
    }
    return choice;
}

/// A private temporary directory removed when the game jar closes.
class temporary_directory {
  public:
    temporary_directory() = default;
    temporary_directory(const temporary_directory &) = delete;
    auto operator=(const temporary_directory &)
        -> temporary_directory & = delete;
    temporary_directory(temporary_directory &&other) noexcept
        : path_{std::exchange(other.path_, {})} {}
    auto operator=(temporary_directory &&other) noexcept
        -> temporary_directory & {
        if (this != &other) {
            remove();
            path_ = std::exchange(other.path_, {});
        }
        return *this;
    }
    ~temporary_directory() { remove(); }

    [[nodiscard]] static auto create() -> result<temporary_directory> {
        static std::atomic<std::uint64_t> sequence{0};
        const auto base{io::temp_directory()};
        if (base.empty()) {
            return std::unexpected{make_error(
                error_code::io_error,
                "cannot resolve the temporary directory for the bundler jar")};
        }
        std::random_device device{};
        for (int attempt{0}; attempt < 32; ++attempt) {
            const auto name{base + "/refrain-game-jar-" +
                            std::to_string(device()) + "-" +
                            std::to_string(sequence.fetch_add(
                                1, std::memory_order_relaxed))};
            std::error_code error;
            if (std::filesystem::create_directories(name, error)) {
                temporary_directory directory{};
                directory.path_ = name;
                return directory;
            }
            if (error) {
                return std::unexpected{make_error(
                    error_code::io_error,
                    "cannot create the temporary directory for the bundler "
                    "jar: " +
                        error.message())};
            }
        }
        return std::unexpected{make_error(
            error_code::io_error,
            "cannot create a unique temporary directory for the bundler "
            "jar")};
    }

    [[nodiscard]] auto path() const noexcept -> const std::string & {
        return path_;
    }

  private:
    void remove() noexcept {
        if (path_.empty()) {
            return;
        }
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        path_.clear();
    }

    std::string path_{};
};

/// Read the whole entry or return an empty vector; metadata is optional.
[[nodiscard]] auto read_optional(const zip_reader &reader,
                                 const zip_entry *entry)
    -> std::vector<std::byte> {
    if (entry == nullptr) {
        return {};
    }
    auto bytes{reader.read(*entry)};
    if (!bytes) {
        return {};
    }
    return std::move(*bytes);
}

} // namespace

struct game_jar::impl {
    zip_reader outer;
    /// Engaged when the data tree lives in the bundler's inner jar.
    std::unique_ptr<zip_reader> inner{};
    temporary_directory temporary{};
    /// Entry and reader that describe the archive's root version document.
    const zip_reader *version_reader{nullptr};
    const zip_entry *version_entry{nullptr};
    std::string version_id{};
    bool bundled{false};

    explicit impl(zip_reader archive) : outer{std::move(archive)} {}
};

game_jar::game_jar(std::unique_ptr<impl> state) noexcept
    : impl_{std::move(state)} {}

game_jar::game_jar(game_jar &&other) noexcept : impl_{std::move(other.impl_)} {}

auto game_jar::operator=(game_jar &&other) noexcept -> game_jar & {
    if (this != &other) {
        impl_ = std::move(other.impl_);
    }
    return *this;
}

game_jar::~game_jar() = default;

auto game_jar::open(std::string_view path) -> result<game_jar> {
    if (path.empty()) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "the server archive path is empty")};
    }
    auto outer{zip_reader::open(path)};
    if (!outer) {
        return std::unexpected{outer.error()};
    }
    auto state{std::make_unique<impl>(std::move(*outer))};

    const auto *version_entry{state->outer.find("version.json")};
    const auto version_bytes{read_optional(state->outer, version_entry)};
    state->version_reader = &state->outer;
    state->version_entry = version_entry;

    if (!has_root_data(state->outer)) {
        auto choice{choose_data_archive(state->outer, version_bytes)};
        if (!choice) {
            return std::unexpected{choice.error()};
        }
        const auto *inner_entry{state->outer.find(choice->inner_entry)};
        if (inner_entry == nullptr) {
            return std::unexpected{
                make_error(error_code::io_error,
                           "the bundler jar does not contain its server jar '" +
                               choice->inner_entry + "'")};
        }
        auto inner_bytes{state->outer.read(*inner_entry)};
        if (!inner_bytes) {
            return std::unexpected{inner_bytes.error()};
        }
        auto temporary{temporary_directory::create()};
        if (!temporary) {
            return std::unexpected{temporary.error()};
        }
        const auto inner_path{temporary->path() + "/server.jar"};
        if (auto written{io::write_file(inner_path, *inner_bytes)}; !written) {
            return std::unexpected{written.error()};
        }
        auto inner{zip_reader::open(inner_path)};
        if (!inner) {
            return std::unexpected{inner.error()};
        }
        state->bundled = true;
        state->version_id = std::move(choice->version_id);
        state->inner = std::make_unique<zip_reader>(std::move(*inner));
        state->temporary = std::move(*temporary);
        // Prefer the outer version document, which describes the distribution;
        // fall back to the inner one, which some archives carry instead.
        if (state->version_entry == nullptr) {
            state->version_entry = state->inner->find("version.json");
            state->version_reader = state->inner.get();
            if (state->version_id.empty() && state->version_entry != nullptr) {
                const auto bytes{
                    read_optional(*state->inner, state->version_entry)};
                state->version_id = parse_version_id(bytes);
            }
        }
    } else {
        state->version_id = parse_version_id(version_bytes);
    }
    return game_jar{std::move(state)};
}

auto game_jar::reader() const noexcept -> const zip_reader & {
    return impl_->bundled ? *impl_->inner : impl_->outer;
}

auto game_jar::bundled() const noexcept -> bool { return impl_->bundled; }

auto game_jar::archive_path() const noexcept -> std::string_view {
    return impl_->outer.archive_path();
}

auto game_jar::version_id() const noexcept -> std::string_view {
    return impl_->version_id;
}

auto game_jar::extract_version_json(std::string_view output_root)
    -> result<bool> {
    if (impl_->version_entry == nullptr || impl_->version_reader == nullptr) {
        return false;
    }
    auto status{
        impl_->version_reader->extract(*impl_->version_entry, output_root)};
    if (!status) {
        return std::unexpected{status.error()};
    }
    return true;
}

} // namespace refrain::archive
