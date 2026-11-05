// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/app/services.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <refrain/archive/manifest.hpp>
#include <refrain/archive/zip.hpp>
#include <refrain/core/config.hpp>
#include <refrain/core/io.hpp>
#include <refrain/core/log.hpp>
#include <refrain/core/semver.hpp>
#include <refrain/core/version.hpp>
#include <refrain/data/builtin_image.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/content_index.hpp>
#include <refrain/data/content_maintenance.hpp>
#include <refrain/data/content_pack.hpp>
#include <refrain/data/content_segment.hpp>
#include <refrain/data/content_store.hpp>
#include <refrain/data/datapack.hpp>
#include <refrain/data/json.hpp>
#include <refrain/data/path.hpp>
#include <refrain/data/registry.hpp>
#include <refrain/data/store_lock.hpp>
#include <refrain/data/vfs.hpp>
#include <refrain/kernel/crash.hpp>
#include <refrain/mod/discovery.hpp>
#include <refrain/mod/gamepack.hpp>
#include <refrain/mod/host.hpp>
#include <refrain/mod/manifest.hpp>
#include <refrain/mod/mod_ref.hpp>
#include <refrain/mod/rfmod.hpp>
#include <refrain/script/wasm.hpp>
#include <schema/schema.hpp>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace refrain::app {

namespace archive = refrain::archive;
namespace config = refrain::core::config;
namespace core = refrain::core;
namespace data = refrain::data;
namespace io = refrain::core::io;
namespace kernel = refrain::kernel;
namespace log = refrain::log;
namespace mod = refrain::mod;
namespace schema = refrain::schema;
namespace script = refrain::script;

// --- extract ---------------------------------------------------------------

[[nodiscard]] auto parse_filter(std::string_view name)
    -> refrain::result<archive::extract_filter> {
    if (name == "all" || name == "data" || name == "assets") {
        return archive::filter_from_string(name);
    }
    return std::unexpected{refrain::make_error(
        refrain::error_code::invalid_argument,
        "extract: --filter must be one of all, data, assets")};
}

auto run_extract(const extract_options &options) -> int {
    auto filter{parse_filter(options.filter)};
    if (!filter) {
        return report_error(filter.error());
    }

    auto reader{archive::zip_reader::open(options.jar)};
    if (!reader) {
        return report_error(reader.error());
    }

    auto manifest{archive::build_manifest(*reader, *filter)};
    if (!manifest) {
        return report_error(manifest.error());
    }
    manifest->generator = std::string{refrain::version::release_version};

    const auto manifest_path{options.manifest_out.empty()
                                 ? data::join_path(options.out, "manifest.json")
                                 : options.manifest_out};
    auto directories{io::create_directories(options.out)};
    if (!directories) {
        return report_error(directories.error());
    }
    auto json{archive::manifest_to_json(*manifest)};
    if (!json) {
        return report_error(json.error());
    }
    auto written{io::atomic_write_text(manifest_path, *json + '\n')};
    if (!written) {
        return report_error(written.error());
    }

    std::cout << "extract: " << manifest->total_files << " file(s), "
              << manifest->total_bytes << " byte(s) from " << manifest->source
              << " (filter=" << archive::to_string(*filter) << ") -> "
              << manifest_path << '\n';
    for (const auto &category : manifest->categories) {
        const auto count{static_cast<std::size_t>(
            std::count_if(manifest->entries.begin(), manifest->entries.end(),
                          [&category](const archive::manifest_entry &entry) {
                              return entry.category == category;
                          }))};
        std::cout << "  " << category << ": " << count << '\n';
    }

    if (!options.extract) {
        std::cout << "  manifest only (pass --extract to write the selected "
                     "entries under "
                  << options.out << ")\n";
        return 0;
    }

    std::size_t extracted{0};
    for (const auto &entry : manifest->entries) {
        const auto *zip_entry{reader->find(entry.path)};
        if (zip_entry == nullptr) {
            return report_error(refrain::make_error(
                refrain::error_code::not_found,
                "extract: entry disappeared from the archive: " + entry.path));
        }
        auto status{reader->extract(*zip_entry, options.out)};
        if (!status) {
            return report_error(status.error());
        }
        ++extracted;
    }
    // A 26.3 jar root has no pack.mcmeta; its data format lives in version.json
    // (`pack_version.data_major`). Keep that metadata next to the extracted
    // data/ tree so `data check --pack-format` can gate a jar-root pack.
    if (*filter == archive::extract_filter::data) {
        if (const auto *version{reader->find("version.json")};
            version != nullptr) {
            auto status{reader->extract(*version, options.out)};
            if (!status) {
                return report_error(status.error());
            }
            std::cout << "extract: wrote format metadata version.json\n";
        }
    }
    std::cout << "extract: wrote " << extracted << " file(s) under "
              << options.out << '\n';
    return 0;
}

// --- schema ----------------------------------------------------------------

auto run_schema_gen(const std::string &output_root,
                    const std::vector<std::string> &roots, bool check) -> int {
    std::vector<std::filesystem::path> schema_roots;
    schema_roots.reserve(roots.size());
    for (const auto &root : roots) {
        schema_roots.emplace_back(root);
    }

    auto generated{schema::generate_from_roots(std::move(schema_roots))};
    if (!generated) {
        return report_error(generated.error());
    }

    const std::string summary{std::to_string(generated->files.size()) +
                              " files from " +
                              std::to_string(generated->source_count) +
                              " schemas, hash " + generated->schema_hash};

    if (check) {
        auto report{schema::check_files(*generated, output_root)};
        if (!report) {
            return report_error(report.error());
        }
        if (report->ok()) {
            std::cout << "schema: OK (" << summary << ")\n";
            return 0;
        }
        for (const auto &path : report->missing) {
            std::cout << "  missing: " << path << '\n';
        }
        for (const auto &path : report->changed) {
            std::cout << "  changed: " << path << '\n';
        }
        for (const auto &path : report->stale) {
            std::cout << "  stale:   " << path << '\n';
        }
        log::error("schema: generated files are out of date; run "
                   "'refrain-tools schema gen' and commit the result");
        // Same stderr fallback as report_error().
        if (!log::enabled(log::level::error)) {
            std::cerr << "error: schema: generated files are out of date; run "
                         "'refrain-tools schema gen' and commit the result\n";
        }
        return 1;
    }

    auto written{schema::write_files(*generated, output_root)};
    if (!written) {
        return report_error(written.error());
    }
    std::cout << "schema: wrote " << summary << " -> " << output_root << '\n';
    return 0;
}

// --- crash ----------------------------------------------------------------

auto run_crash_smoke(const std::string &database_path,
                     const std::string &handler_path) -> int {
#if REFRAIN_WITH_CRASHPAD
    kernel::crash_config config{};
    config.product = "Refrain";
    config.version = std::string{refrain::version::release_version};
    config.handler_path = handler_path;
    config.database_path = database_path;
    config.fallback_marker = false;
    config.start_handler = true;

    auto initialized{kernel::crash_reporter::initialize(config)};
    if (!initialized) {
        return report_error(initialized.error());
    }
    if (!kernel::crash_reporter::enabled()) {
        refrain::log::error(
            "crash: Crashpad client is not attached (is '{}' a real "
            "crashpad_handler?)",
            handler_path);
        return 1;
    }
    std::cout << "crash: handler attached, database=" << database_path
              << "; crashing on purpose to produce a minidump\n"
              << std::flush;
    kernel::crash_reporter::trigger_for_testing();
    refrain::log::error("crash: the fault did not terminate the process");
    return 1;
#else
    refrain::log::error(
        "crash: this build does not enable REFRAIN_WITH_CRASHPAD");
    static_cast<void>(database_path);
    static_cast<void>(handler_path);
    return 1;
#endif
}

// --- wasm ------------------------------------------------------------------

auto run_wasm_smoke() -> int {
#if REFRAIN_WITH_WASM
    auto runtime{script::wasm::runtime::create()};
    if (!runtime) {
        return report_error(runtime.error());
    }
    constexpr std::string_view wat{R"((module
  (func (export "add") (param i32 i32) (result i32)
    local.get 0
    local.get 1
    i32.add)))"};
    auto module{runtime->compile_wat(wat)};
    if (!module) {
        return report_error(module.error());
    }
    const std::array<std::int32_t, 2> args{2, 3};
    auto result{runtime->call_i32(*module, "add", args)};
    if (!result) {
        return report_error(result.error());
    }
    if (*result != 5) {
        log::error("wasm smoke: expected 5, got {}", *result);
        return 1;
    }
    std::cout << "wasm: OK (wasmtime module add(2,3) = " << *result << ")\n";
    return 0;
#else
    log::error("wasm: this build does not enable REFRAIN_WITH_WASM");
    return 1;
#endif
}

} // namespace refrain::app
