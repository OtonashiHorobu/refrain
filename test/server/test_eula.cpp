// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/io.hpp>
#include <refrain/server/eula.hpp>

#include "support/scratch_directory.hpp"

#include <boost/ut.hpp>

#include <filesystem>
#include <string>
#include <string_view>

namespace {

using namespace boost::ut;
using refrain::server::ensure_eula;
using refrain::server::eula_options;
using refrain::server::eula_state;
using refrain::server::format_eula_notice;

void write_text(const std::filesystem::path &path, std::string_view text) {
    const auto written = refrain::core::io::write_text(path.string(), text);
    expect(written.has_value());
}

[[nodiscard]] auto read_text(const std::filesystem::path &path) -> std::string {
    auto text = refrain::core::io::read_text(path.string());
    expect(text.has_value());
    return text ? *text : std::string{};
}

[[nodiscard]] auto path_string(const std::filesystem::path &path)
    -> std::string {
    return path.string();
}

} // namespace

namespace {

using namespace boost::ut;

suite<"server/eula"> eula_tests = [] {
    "eula/first run writes a rejected acceptance and refuses"_test = [] {
        refrain::test_support::scratch_directory scratch{};
        const auto path = scratch.path() / "eula.toml";
        const auto state = ensure_eula(eula_options{
            .path = path_string(path),
            .legacy_path = path_string(scratch.path() / "eula.txt")});
        expect(state.has_value());
        if (!state) {
            return;
        }
        expect(!state->accepted);
        expect(!state->existed);
        expect(!state->imported_legacy);
        expect(state->written);
        expect(std::filesystem::is_regular_file(path));
        expect(read_text(path).find("eula = false") != std::string::npos);
    };

    "eula/accept flag records acceptance on the first run"_test = [] {
        refrain::test_support::scratch_directory scratch{};
        const auto path = scratch.path() / "eula.toml";
        const auto state = ensure_eula(eula_options{
            .path = path_string(path), .legacy_path = {}, .accept = true});
        expect(state.has_value());
        if (!state) {
            return;
        }
        expect(state->accepted);
        expect(state->written);
        expect(read_text(path).find("eula = true") != std::string::npos);
    };

    "eula/accepted file is reused without rewriting"_test = [] {
        refrain::test_support::scratch_directory scratch{};
        const auto path = scratch.path() / "eula.toml";
        write_text(path, "eula = true\n");
        const auto state = ensure_eula(
            eula_options{.path = path_string(path), .legacy_path = {}});
        expect(state.has_value());
        if (!state) {
            return;
        }
        expect(state->accepted);
        expect(state->existed);
        expect(!state->written);
        expect(read_text(path) == "eula = true\n");
    };

    "eula/refused file is flipped only by the accept flag"_test = [] {
        refrain::test_support::scratch_directory scratch{};
        const auto path = scratch.path() / "eula.toml";
        write_text(path, "eula = false\n");
        const auto refused = ensure_eula(
            eula_options{.path = path_string(path), .legacy_path = {}});
        expect(refused.has_value());
        if (!refused) {
            return;
        }
        expect(!refused->accepted);
        expect(refused->existed);
        expect(!refused->written);
        expect(read_text(path) == "eula = false\n");

        const auto accepted = ensure_eula(eula_options{
            .path = path_string(path), .legacy_path = {}, .accept = true});
        expect(accepted.has_value());
        if (!accepted) {
            return;
        }
        expect(accepted->accepted);
        expect(accepted->written);
        expect(read_text(path).find("eula = true") != std::string::npos);
    };

    "eula/vanilla acceptance is imported once"_test = [] {
        refrain::test_support::scratch_directory scratch{};
        const auto path = scratch.path() / "eula.toml";
        const auto legacy = scratch.path() / "eula.txt";
        write_text(legacy, "#By changing the setting below to TRUE you are "
                           "indicating your agreement\neula=true\n");
        const auto imported = ensure_eula(eula_options{
            .path = path_string(path), .legacy_path = path_string(legacy)});
        expect(imported.has_value());
        if (!imported) {
            return;
        }
        expect(imported->accepted);
        expect(imported->imported_legacy);
        expect(imported->written);
        expect(read_text(path).find("eula = true") != std::string::npos);

        // The TOML decision is now authoritative: a later vanilla change does
        // not flip it back.
        write_text(legacy, "eula=false\n");
        const auto reused = ensure_eula(eula_options{
            .path = path_string(path), .legacy_path = path_string(legacy)});
        expect(reused.has_value());
        if (reused) {
            expect(reused->accepted);
            expect(!reused->imported_legacy);
        }
    };

    "eula/vanilla refusal is imported once and keeps refusing"_test = [] {
        refrain::test_support::scratch_directory scratch{};
        const auto path = scratch.path() / "eula.toml";
        const auto legacy = scratch.path() / "eula.txt";
        write_text(legacy, "eula=false\n");
        const auto state = ensure_eula(eula_options{
            .path = path_string(path), .legacy_path = path_string(legacy)});
        expect(state.has_value());
        if (!state) {
            return;
        }
        expect(!state->accepted);
        expect(state->imported_legacy);
        expect(read_text(path).find("eula = false") != std::string::npos);
    };

    "eula/vanilla property syntax is parsed permissively"_test = [] {
        refrain::test_support::scratch_directory scratch{};
        const auto legacy = scratch.path() / "eula.txt";
        write_text(legacy, "! comment\n  eula : TRUE  \n");
        const auto path = scratch.path() / "eula.toml";
        const auto state = ensure_eula(eula_options{
            .path = path_string(path), .legacy_path = path_string(legacy)});
        expect(state.has_value());
        if (state) {
            expect(state->accepted);
        }
    };

    "eula/existing refusal file is not re-imported from vanilla"_test = [] {
        refrain::test_support::scratch_directory scratch{};
        const auto path = scratch.path() / "eula.toml";
        const auto legacy = scratch.path() / "eula.txt";
        write_text(path, "eula = false\n");
        write_text(legacy, "eula=true\n");
        const auto state = ensure_eula(eula_options{
            .path = path_string(path), .legacy_path = path_string(legacy)});
        expect(state.has_value());
        if (state) {
            expect(!state->accepted);
            expect(!state->imported_legacy);
        }
    };

    "eula/non-boolean acceptance is a parse error naming the file"_test = [] {
        refrain::test_support::scratch_directory scratch{};
        const auto path = scratch.path() / "eula.toml";
        write_text(path, "eula = \"yes\"\n");
        const auto state = ensure_eula(
            eula_options{.path = path_string(path), .legacy_path = {}});
        expect(!state.has_value());
        if (!state) {
            expect(static_cast<int>(state.error().code) ==
                   static_cast<int>(refrain::error_code::parse_error));
            expect(state.error().message.find(path_string(path)) !=
                   std::string::npos);
        }
    };

    "eula/missing acceptance entry is a parse error"_test = [] {
        refrain::test_support::scratch_directory scratch{};
        const auto path = scratch.path() / "eula.toml";
        write_text(path, "level = \"world\"\n");
        const auto state = ensure_eula(
            eula_options{.path = path_string(path), .legacy_path = {}});
        expect(!state.has_value());
        if (!state) {
            expect(static_cast<int>(state.error().code) ==
                   static_cast<int>(refrain::error_code::parse_error));
            expect(state.error().message.find("eula") != std::string::npos);
        }
    };

    "eula/malformed document is a parse error"_test = [] {
        refrain::test_support::scratch_directory scratch{};
        const auto path = scratch.path() / "eula.toml";
        write_text(path, "eula = \n");
        const auto state = ensure_eula(
            eula_options{.path = path_string(path), .legacy_path = {}});
        expect(!state.has_value());
        if (!state) {
            expect(state.error().code == refrain::error_code::parse_error);
        }
    };

    "eula/empty path is invalid"_test = [] {
        const auto state =
            ensure_eula(eula_options{.path = {}, .legacy_path = {}});
        expect(!state.has_value());
        if (!state) {
            expect(state.error().code == refrain::error_code::invalid_argument);
        }
    };

    "eula/acceptance path is created below a missing parent"_test = [] {
        refrain::test_support::scratch_directory scratch{};
        const auto path = scratch.path() / "instance" / "eula.toml";
        const auto state = ensure_eula(eula_options{
            .path = path_string(path), .legacy_path = {}, .accept = true});
        expect(state.has_value());
        expect(std::filesystem::is_regular_file(path));
    };

    "eula/notice names the file, the URL and the accept command"_test = [] {
        eula_state state{};
        state.path = "eula.toml";
        state.legacy_path = "eula.txt";
        const auto notice = format_eula_notice(state);
        expect(notice.find("https://www.minecraft.net/en-us/eula") !=
               std::string::npos);
        expect(notice.find("'eula.toml'") != std::string::npos);
        expect(notice.find("refrain-server --accept-eula") !=
               std::string::npos);
        expect(notice.find("No world was created") != std::string::npos);
    };
};

} // namespace
