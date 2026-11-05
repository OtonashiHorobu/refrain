// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <schema/schema.hpp>

#include <schema/diagnostic.hpp>
#include <schema/emit.hpp>
#include <schema/parser.hpp>
#include <schema/sema.hpp>

#include <xxhash.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::schema {
namespace {

[[nodiscard]] auto read_text_file(const std::filesystem::path &path)
    -> result<std::string> {
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        return std::unexpected(make_error(
            error_code::io_error,
            std::format("schema: cannot open '{}'", path.generic_string())));
    }
    auto text{std::string{}};
    file.seekg(0, std::ios::end);
    const auto size{file.tellg()};
    if (size < 0) {
        return std::unexpected(
            make_error(error_code::io_error,
                       std::format("schema: cannot determine size of '{}'",
                                   path.generic_string())));
    }
    text.resize(static_cast<std::size_t>(size));
    file.seekg(0, std::ios::beg);
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file && !text.empty()) {
        return std::unexpected(make_error(
            error_code::io_error,
            std::format("schema: cannot read '{}'", path.generic_string())));
    }
    return text;
}

/// Stable display path used in diagnostics and for RSC header placement:
/// always `<root-last-component>/<path-inside-root>`. This makes generation
/// independent of whether the caller passed "schema" or an absolute path, so
/// `--check` and the drift unit test produce byte-identical output.
[[nodiscard]] auto display_path(const std::filesystem::path &root,
                                const std::filesystem::path &file)
    -> std::string {
    const auto relative{file.lexically_relative(root)};
    auto root_text{root.filename().generic_string()};
    if (root_text.empty() || root_text == ".") {
        root_text = "root";
    }
    return (std::filesystem::path{root_text} / relative).generic_string();
}

[[nodiscard]] auto dialect_for(const std::filesystem::path &path)
    -> std::optional<dialect> {
    const auto extension{path.extension().string()};
    if (extension == ".rsc") {
        return dialect::rsc;
    }
    if (extension == ".ridl") {
        return dialect::ridl;
    }
    return std::nullopt;
}

[[nodiscard]] auto discover(const std::filesystem::path &root)
    -> result<std::vector<source_file>> {
    std::error_code error;
    if (!std::filesystem::exists(root, error)) {
        return std::unexpected(
            make_error(error_code::not_found,
                       std::format("schema: root '{}' does not exist",
                                   root.generic_string())));
    }

    std::vector<std::filesystem::path> paths;
    for (std::filesystem::recursive_directory_iterator iterator{root, error},
         end;
         iterator != end; iterator.increment(error)) {
        if (error) {
            return std::unexpected(make_error(
                error_code::io_error,
                std::format("schema: cannot scan '{}': {}",
                            root.generic_string(), error.message())));
        }
        if (!iterator->is_regular_file(error)) {
            error.clear();
            continue;
        }
        if (dialect_for(iterator->path()).has_value()) {
            paths.push_back(iterator->path());
        }
    }
    std::sort(paths.begin(), paths.end());

    std::vector<source_file> sources;
    sources.reserve(paths.size());
    for (const auto &path : paths) {
        auto text{read_text_file(path)};
        if (!text.has_value()) {
            return std::unexpected(std::move(text.error()));
        }
        sources.push_back(source_file{display_path(root, path),
                                      *dialect_for(path), std::move(*text)});
    }
    return sources;
}

[[nodiscard]] auto schema_hash_of(const std::vector<source_file> &sources)
    -> result<std::string> {
    const auto state{XXH64_createState()};
    if (state == nullptr) {
        return std::unexpected(make_error(
            error_code::out_of_memory, "schema: cannot allocate hash state"));
    }
    XXH64_reset(state, 0);
    // Mix the schema format version in so the banner hash cannot be
    // mistaken for output from a different emitter format.
    XXH64_update(state, emit::schema_format_version.data(),
                 emit::schema_format_version.size());
    constexpr char separator{'\0'};
    XXH64_update(state, &separator, 1);
    for (const auto &source : sources) {
        XXH64_update(state, source.path.data(), source.path.size());
        XXH64_update(state, &separator, 1);
        XXH64_update(state, source.text.data(), source.text.size());
        XXH64_update(state, &separator, 1);
    }
    const auto digest{XXH64_digest(state)};
    XXH64_freeState(state);
    return std::format("{:016x}", digest);
}

} // namespace

auto generate_from_sources(std::vector<source_file> sources)
    -> result<generation> {
    if (sources.empty()) {
        return std::unexpected(
            make_error(error_code::invalid_argument,
                       "schema: no schema sources were provided"));
    }
    std::sort(sources.begin(), sources.end(),
              [](const source_file &lhs, const source_file &rhs) {
                  return lhs.path < rhs.path;
              });

    diagnostics sink;
    std::vector<source_unit> units;
    units.reserve(sources.size());
    for (const auto &source : sources) {
        auto unit{parse_schema(parse_request{source.kind, source.path},
                               source.text, sink)};
        if (unit.has_value()) {
            units.push_back(std::move(*unit));
        }
    }
    if (!sink.empty()) {
        auto message{sink.format()};
        if (message.empty()) {
            message = "schema: parsing failed";
        }
        return std::unexpected(
            make_error(error_code::parse_error, std::move(message)));
    }

    auto program{analyze(std::move(units), sink)};
    if (!program.has_value() || !sink.empty()) {
        auto message{sink.format()};
        if (message.empty()) {
            message = "schema: semantic analysis failed";
        }
        return std::unexpected(
            make_error(error_code::parse_error, std::move(message)));
    }

    auto hash{schema_hash_of(sources)};
    if (!hash.has_value()) {
        return std::unexpected{std::move(hash.error())};
    }
    std::vector<generated_file> files{emit_ridl(*program, *hash)};
    auto rsc_files{emit_rsc(*program, *hash)};
    files.insert(files.end(), std::make_move_iterator(rsc_files.begin()),
                 std::make_move_iterator(rsc_files.end()));
    // The reflection index covers both dialects, so it is assembled here where
    // one generation owns the complete output set (and the duplicate-path
    // check below sees it like any other emitted file).
    files.push_back(emit_reflection(*program, *hash));

    std::set<std::string> paths;
    for (const auto &file : files) {
        if (!paths.insert(file.path).second) {
            return std::unexpected(make_error(
                error_code::already_exists,
                std::format("schema: two emitters produced '{}'", file.path)));
        }
    }
    std::sort(files.begin(), files.end(),
              [](const generated_file &lhs, const generated_file &rhs) {
                  return lhs.path < rhs.path;
              });

    generation result;
    result.files = std::move(files);
    result.schema_hash = std::move(*hash);
    result.source_count = sources.size();
    return result;
}

auto generate_from_roots(std::vector<std::filesystem::path> roots)
    -> result<generation> {
    if (roots.empty()) {
        return std::unexpected(
            make_error(error_code::invalid_argument,
                       "schema: no schema roots were provided"));
    }
    std::sort(roots.begin(), roots.end());
    std::vector<source_file> sources;
    for (const auto &root : roots) {
        auto discovered{discover(root)};
        if (!discovered.has_value()) {
            return std::unexpected(std::move(discovered.error()));
        }
        sources.insert(sources.end(),
                       std::make_move_iterator(discovered->begin()),
                       std::make_move_iterator(discovered->end()));
    }
    if (sources.empty()) {
        return std::unexpected(
            make_error(error_code::not_found,
                       "schema: roots contain no '.rsc' or '.ridl' files"));
    }
    return generate_from_sources(std::move(sources));
}

/// Write `content` to `target` via a sibling temp file + rename, so an
/// interrupted generation never leaves a truncated header behind.
[[nodiscard]] auto write_file_atomic(const std::filesystem::path &target,
                                     std::string_view content) -> status {
    auto temp{target};
    temp += ".tmp";
    {
        std::ofstream stream{temp, std::ios::binary | std::ios::trunc};
        if (!stream) {
            return std::unexpected(make_error(
                error_code::io_error,
                std::format("schema: cannot create temporary file '{}'",
                            temp.generic_string())));
        }
        stream.write(content.data(),
                     static_cast<std::streamsize>(content.size()));
        stream.flush();
        if (!stream) {
            (void)std::filesystem::remove(temp);
            return std::unexpected(
                make_error(error_code::io_error,
                           std::format("schema: failed while writing '{}'",
                                       temp.generic_string())));
        }
    }
    std::error_code error;
    std::filesystem::rename(temp, target, error);
    if (error) {
        // POSIX rename replaces atomically; some platforms refuse an existing
        // target, fall back to remove + rename (generation is single-writer).
        error.clear();
        std::filesystem::remove(target, error);
        error.clear();
        std::filesystem::rename(temp, target, error);
        if (error) {
            (void)std::filesystem::remove(temp);
            return std::unexpected(make_error(
                error_code::io_error,
                std::format("schema: cannot replace '{}': {}",
                            target.generic_string(), error.message())));
        }
    }
    return status_ok();
}

auto write_files(const generation &generated,
                 const std::filesystem::path &output_root) -> status {
    for (const auto &file : generated.files) {
        const auto target{output_root / std::filesystem::path{file.path}};
        std::error_code error;
        std::filesystem::create_directories(target.parent_path(), error);
        if (error) {
            return std::unexpected(
                make_error(error_code::io_error,
                           std::format("schema: cannot create '{}': {}",
                                       target.parent_path().generic_string(),
                                       error.message())));
        }
        if (auto written{write_file_atomic(target, file.content)}; !written) {
            return written;
        }
    }
    return status_ok();
}

auto check_files(const generation &generated,
                 const std::filesystem::path &output_root)
    -> result<check_report> {
    check_report report;
    std::set<std::string> expected;
    for (const auto &file : generated.files) {
        expected.insert(file.path);
        const auto target{output_root / std::filesystem::path{file.path}};
        std::error_code error;
        if (!std::filesystem::exists(target, error)) {
            report.missing.push_back(file.path);
            continue;
        }
        auto on_disk{read_text_file(target)};
        if (!on_disk.has_value()) {
            return std::unexpected(std::move(on_disk.error()));
        }
        if (*on_disk != file.content) {
            report.changed.push_back(file.path);
        }
    }

    std::error_code error;
    if (std::filesystem::exists(output_root, error)) {
        for (std::filesystem::recursive_directory_iterator
                 iterator{output_root, error},
             end;
             iterator != end; iterator.increment(error)) {
            if (error) {
                return std::unexpected(
                    make_error(error_code::io_error,
                               std::format("schema: cannot scan '{}': {}",
                                           output_root.generic_string(),
                                           error.message())));
            }
            if (!iterator->is_regular_file(error)) {
                error.clear();
                continue;
            }
            // Report *any* unexpected file, not only `.h`/`.hpp` (a
            // future emitter may produce other extensions, and a leftover
            // `.json`/`.inl` is just as stale).
            const auto relative{std::filesystem::relative(iterator->path(),
                                                          output_root, error)};
            if (error) {
                error.clear();
                continue;
            }
            const auto key{relative.generic_string()};
            if (!expected.contains(key)) {
                report.stale.push_back(key);
            }
        }
    }
    std::sort(report.missing.begin(), report.missing.end());
    std::sort(report.changed.begin(), report.changed.end());
    std::sort(report.stale.begin(), report.stale.end());
    return report;
}

} // namespace refrain::schema
