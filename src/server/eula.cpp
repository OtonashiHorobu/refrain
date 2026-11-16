// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/eula.hpp>

#include <refrain/core/config.hpp>
#include <refrain/core/io.hpp>

#include <fmt/format.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace refrain::server {

namespace {

namespace io = refrain::core::io;

/// Untrusted small documents: an acceptance file is one line, so the read and
/// line limits stay far below the configuration defaults.
constexpr std::uint64_t max_eula_bytes = 64ULL * 1024ULL;

[[nodiscard]] auto trim(std::string_view text) noexcept -> std::string_view {
    const auto first{text.find_first_not_of(" \t\r\n")};
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last{text.find_last_not_of(" \t\r\n")};
    return text.substr(first, last - first + 1U);
}

[[nodiscard]] auto ascii_lower(std::string_view text) -> std::string {
    std::string folded{text};
    for (auto &character : folded) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return folded;
}

/// Read the `eula` boolean of the TOML acceptance file.
[[nodiscard]] auto read_acceptance(std::string_view path) -> result<bool> {
    auto document{refrain::core::config::document::parse_file(
        path,
        io::read_options{.max_bytes = max_eula_bytes, .allow_symlink = false})};
    if (!document) {
        return std::unexpected{document.error()};
    }
    const std::array<std::string_view, 1> key{"eula"};
    auto accepted{document->get_bool_at(key)};
    if (!accepted) {
        return std::unexpected{make_error(
            error_code::parse_error,
            fmt::format("acceptance file '{}' has no boolean 'eula' entry; it "
                        "must contain 'eula = false' or 'eula = true'",
                        path))};
    }
    return *accepted;
}

/// Read the decision recorded in a vanilla `eula.txt`. The file is a Java
/// properties document; only the `eula` key matters and anything unreadable
/// means "not accepted", which is what the vanilla server decides too.
[[nodiscard]] auto read_legacy_acceptance(std::string_view path) -> bool {
    auto text{io::read_text(path, io::read_options{.max_bytes = max_eula_bytes,
                                                   .allow_symlink = false})};
    if (!text) {
        return false;
    }
    std::size_t start{0};
    while (start <= text->size()) {
        const auto end{text->find('\n', start)};
        const auto stop{end == std::string::npos ? text->size() : end};
        const auto line{
            trim(std::string_view{*text}.substr(start, stop - start))};
        if (!line.empty() && line.front() != '#' && line.front() != '!') {
            const auto separator{line.find_first_of("=:")};
            if (separator != std::string_view::npos &&
                trim(line.substr(0, separator)) == "eula") {
                return ascii_lower(trim(line.substr(separator + 1U))) == "true";
            }
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1U;
    }
    return false;
}

/// Deterministic acceptance document. The prose explains what the flag means
/// and points at the official agreement instead of reproducing it.
[[nodiscard]] auto acceptance_text(bool accepted) -> std::string {
    return fmt::format(
        "# Refrain records your acceptance of the Minecraft End User License\n"
        "# Agreement here. Read the agreement at {} before setting this to\n"
        "# true. Refrain is not affiliated with, approved by or associated\n"
        "# with Mojang or Microsoft; setting 'eula = true' confirms that you\n"
        "# accept the agreement.\n"
        "eula = {}\n",
        eula_url, accepted ? "true" : "false");
}

/// Write the acceptance file, creating its parent directory if needed.
[[nodiscard]] auto write_acceptance(std::string_view path, bool accepted)
    -> status {
    auto parent{io::create_directories(io::parent_directory(path))};
    if (!parent) {
        return parent;
    }
    return io::atomic_write_text(path, acceptance_text(accepted));
}

/// True when `path` names an existing regular file.
[[nodiscard]] auto regular_file(std::string_view path) -> result<bool> {
    auto status{io::status_at(path)};
    if (!status) {
        return std::unexpected{status.error()};
    }
    return status->exists && status->regular;
}

} // namespace

auto ensure_eula(const eula_options &options) -> result<eula_state> {
    if (options.path.empty()) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "the acceptance file path is empty")};
    }
    eula_state state{.path = options.path, .legacy_path = options.legacy_path};

    auto exists{regular_file(options.path)};
    if (!exists) {
        return std::unexpected{exists.error()};
    }
    state.existed = *exists;

    if (state.existed) {
        auto accepted{read_acceptance(options.path)};
        if (!accepted) {
            return std::unexpected{accepted.error()};
        }
        state.accepted = *accepted;
        if (!state.accepted && options.accept) {
            if (auto written{write_acceptance(options.path, true)}; !written) {
                return std::unexpected{written.error()};
            }
            state.accepted = true;
            state.written = true;
        }
        return state;
    }

    // The TOML file does not exist yet: a vanilla acceptance is imported once,
    // so an existing installation keeps the decision it already recorded.
    bool accepted{false};
    if (options.accept) {
        accepted = true;
    } else if (!options.legacy_path.empty()) {
        auto legacy{regular_file(options.legacy_path)};
        if (!legacy) {
            return std::unexpected{legacy.error()};
        }
        if (*legacy) {
            accepted = read_legacy_acceptance(options.legacy_path);
            state.imported_legacy = true;
        }
    }
    if (auto written{write_acceptance(options.path, accepted)}; !written) {
        return std::unexpected{written.error()};
    }
    state.accepted = accepted;
    state.written = true;
    return state;
}

auto format_eula_notice(const eula_state &state) -> std::string {
    std::string notice{
        "Refrain needs your acceptance of the Minecraft End User License "
        "Agreement before it can start.\n"};
    notice += fmt::format("Read the agreement at {}.\n", eula_url);
    notice += fmt::format("Acceptance file: '{}'.\n", state.path);
    if (state.imported_legacy) {
        notice +=
            fmt::format("The file was created from '{}' with eula = false.\n",
                        state.legacy_path);
    } else if (state.existed) {
        notice += "The file already records eula = false.\n";
    } else {
        notice += "The file was created with eula = false.\n";
    }
    notice += "To accept the agreement and start the server, run:\n";
    notice += "  refrain-server --accept-eula\n";
    notice += "No world was created and no port was opened.\n";
    return notice;
}

} // namespace refrain::server
