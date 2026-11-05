// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Shared helpers for schema toolchain tests: parse+analyse in one call and a
// temporary directory guard for generation/check round-trips.

#pragma once

#include <schema/ast.hpp>
#include <schema/diagnostic.hpp>
#include <schema/parser.hpp>
#include <schema/sema.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace refrain::test {

/// One in-memory schema file.
struct schema_source {
    std::string path;
    schema::dialect dialect{schema::dialect::rsc};
    std::string text;
};

struct compile_result {
    std::optional<schema::model> program;
    std::string diagnostics;
};

inline auto compile(const std::vector<schema_source> &sources)
    -> compile_result {
    schema::diagnostics sink;
    std::vector<schema::source_unit> units;
    for (const auto &source : sources) {
        auto unit{schema::parse_schema(
            schema::parse_request{source.dialect, source.path}, source.text,
            sink)};
        if (unit.has_value()) {
            units.push_back(std::move(*unit));
        }
    }
    if (!sink.empty()) {
        return compile_result{std::nullopt, sink.format()};
    }
    auto program{schema::analyze(std::move(units), sink)};
    compile_result result;
    result.diagnostics = sink.format();
    result.program = std::move(program);
    return result;
}

class temp_directory {
  public:
    temp_directory() {
        const auto stamp{
            std::chrono::steady_clock::now().time_since_epoch().count()};
        path_ = std::filesystem::temp_directory_path() /
                ("refrain_schema_test_" + std::to_string(stamp));
        std::error_code error;
        std::filesystem::create_directories(path_, error);
    }

    temp_directory(const temp_directory &) = delete;
    temp_directory &operator=(const temp_directory &) = delete;

    ~temp_directory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] auto path() const -> const std::filesystem::path & {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

} // namespace refrain::test
