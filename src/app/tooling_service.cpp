// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/app/services.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
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
#include <mutex>
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
#include <refrain/data/vanilla.hpp>
#include <refrain/data/vfs.hpp>
#include <refrain/kernel/crash.hpp>
#include <refrain/mod/discovery.hpp>
#include <refrain/mod/gamepack.hpp>
#include <refrain/mod/host.hpp>
#include <refrain/mod/manifest.hpp>
#include <refrain/mod/mod_ref.hpp>
#include <refrain/mod/rfmod.hpp>
#include <refrain/net/bot.hpp>
#include <refrain/net/bot_pool.hpp>
#include <refrain/script/wasm.hpp>
#include <refrain/server/server_properties.hpp>
#include <schema/schema.hpp>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr refrain::log::logger log_crash_reporter{"crash-reporter"};
constexpr refrain::log::logger log_schema{"schema"};
constexpr refrain::log::logger log_wasm{"wasm"};

} // namespace

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
namespace server = refrain::server;
namespace script = refrain::script;

// --- vanilla ---------------------------------------------------------------

auto run_vanilla_import(const vanilla_import_options &options) -> int {
    data::vanilla_import_options request;
    request.reports = options.reports;
    request.out = options.out;
    request.game_version = options.game_version;
    request.data_version = options.data_version;
    request.force = options.force;

    auto result = data::import_vanilla_reports(request);
    if (!result) {
        return report_error(result.error());
    }
    std::cout << "vanilla: " << result->blocks << " block(s), "
              << result->states << " state(s), " << result->presets
              << " biome preset(s), " << result->registries << " registr(ies), "
              << result->items << " item(s) -> " << result->tables_path << " ("
              << result->tables_bytes << " bytes, blake3 "
              << result->tables_digest << ")\n";
    std::cout << "  manifest: " << result->manifest_path << '\n';
    return 0;
}

auto run_protocol_bot(const protocol_bot_options &options) -> int {
    const auto separator{options.target.rfind(':')};
    if (separator == std::string::npos ||
        separator + 1 >= options.target.size()) {
        return report_error(
            refrain::make_error(refrain::error_code::invalid_argument,
                                "protocol bot: expected host:port"));
    }
    std::uint16_t port{0};
    const auto port_text{options.target.substr(separator + 1)};
    const auto parsed{std::from_chars(
        port_text.data(), port_text.data() + port_text.size(), port)};
    if (parsed.ec != std::errc{} ||
        parsed.ptr != port_text.data() + port_text.size()) {
        return report_error(
            refrain::make_error(refrain::error_code::invalid_argument,
                                "protocol bot: invalid port"));
    }
    refrain::net::bot_options bot{};
    bot.target.host = options.target.substr(0, separator);
    bot.target.port = port;
    bot.username = options.username;
    bot.timeout = std::chrono::milliseconds{options.timeout_ms};
    bot.wait_chunks = options.wait_chunks;
    bot.linger = std::chrono::milliseconds{options.linger_ms};
    bot.command = options.command;
    bot.wait_messages = options.wait_messages;
    bot.move_dx = options.move_dx;
    bot.move_dy = options.move_dy;
    bot.move_dz = options.move_dz;
    bot.move_delay = std::chrono::milliseconds{options.move_delay_ms};
    bot.command_delay = std::chrono::milliseconds{options.command_delay_ms};
    auto result{refrain::net::run_bot(bot)};
    if (!result) {
        return report_error(result.error());
    }
    fmt::print("bot: joined as '{}' after {} packet(s), {} chunk(s), {} "
               "message(s)\n",
               result->name, result->packets, result->chunks,
               result->messages.size());
    if (!result->messages.empty()) {
        fmt::print("bot: first message: {}\n", result->messages.front());
    }
    return 0;
}

namespace {

struct captured_entry {
    std::uint8_t state{0};
    std::uint32_t id{0};
    std::vector<std::byte> body{};
};

[[nodiscard]] auto read_capture(std::string_view path,
                                std::vector<captured_entry> &entries)
    -> refrain::status {
    auto bytes{refrain::core::io::read_file(
        path,
        {.max_bytes = 64ULL * 1024ULL * 1024ULL, .allow_symlink = false})};
    if (!bytes) {
        return std::unexpected{std::move(bytes.error())};
    }
    constexpr std::string_view magic{"RFCAP1\n"};
    if (bytes->size() < magic.size() ||
        !std::equal(magic.begin(), magic.end(), bytes->begin(),
                    [](char expected, std::byte actual) {
                        return static_cast<std::byte>(
                                   static_cast<unsigned char>(expected)) ==
                               actual;
                    })) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::parse_error, "not an RFCAP1 capture")};
    }
    std::size_t position{magic.size()};
    const auto read_u32 = [&](std::uint32_t &value) {
        if (position + 4 > bytes->size()) {
            return false;
        }
        value = 0;
        for (int shift{0}; shift < 32; shift += 8) {
            value |= static_cast<std::uint32_t>(
                         std::to_integer<unsigned int>(bytes->at(position)))
                     << shift;
            ++position;
        }
        return true;
    };
    while (position < bytes->size()) {
        if (position + 1 > bytes->size()) {
            break;
        }
        captured_entry entry{};
        entry.state = std::to_integer<std::uint8_t>(bytes->at(position));
        ++position;
        std::uint32_t length{0};
        if (!read_u32(entry.id) || !read_u32(length) ||
            position + length > bytes->size()) {
            return std::unexpected{refrain::make_error(
                refrain::error_code::parse_error, "truncated capture entry")};
        }
        entry.body.assign(
            bytes->begin() + static_cast<std::ptrdiff_t>(position),
            bytes->begin() + static_cast<std::ptrdiff_t>(position + length));
        position += length;
        entries.push_back(std::move(entry));
    }
    return refrain::status_ok();
}

} // namespace

auto run_protocol_diff(const protocol_diff_options &options) -> int {
    std::vector<captured_entry> left{};
    std::vector<captured_entry> right{};
    if (auto read{read_capture(options.left, left)}; !read) {
        return report_error(read.error());
    }
    if (auto read{read_capture(options.right, right)}; !read) {
        return report_error(read.error());
    }
    fmt::print("diff: left {} packet(s), right {} packet(s)\n", left.size(),
               right.size());
    const auto common{std::min(left.size(), right.size())};
    for (std::size_t index{0}; index < common; ++index) {
        if (left[index].state != right[index].state ||
            left[index].id != right[index].id) {
            fmt::print("diff: first mismatch at {}: left state={} id={}, right "
                       "state={} id={}\n",
                       index, left[index].state, left[index].id,
                       right[index].state, right[index].id);
            break;
        }
    }
    if (left.size() == right.size() && common == left.size()) {
        bool identical{true};
        for (std::size_t index{0}; index < left.size(); ++index) {
            if (left[index].state != right[index].state ||
                left[index].id != right[index].id) {
                identical = false;
                break;
            }
        }
        if (identical) {
            fmt::print("diff: packet sequence is identical\n");
        }
    }
    const auto summarize = [](const std::vector<captured_entry> &entries) {
        std::set<std::pair<std::uint8_t, std::uint32_t>> ids{};
        for (const auto &entry : entries) {
            ids.emplace(entry.state, entry.id);
        }
        return ids;
    };
    const auto left_ids{summarize(left)};
    const auto right_ids{summarize(right)};
    std::size_t only_left{0};
    std::size_t only_right{0};
    for (const auto &id : left_ids) {
        if (!right_ids.contains(id)) {
            ++only_left;
        }
    }
    for (const auto &id : right_ids) {
        if (!left_ids.contains(id)) {
            ++only_right;
        }
    }
    fmt::print("diff: unique ids left={}, right={}, only-left={}, "
               "only-right={}\n",
               left_ids.size(), right_ids.size(), only_left, only_right);
    return 0;
}

auto run_protocol_capture(const protocol_capture_options &options) -> int {
    const auto separator{options.target.rfind(':')};
    if (separator == std::string::npos ||
        separator + 1 >= options.target.size() || options.out.empty()) {
        return report_error(refrain::make_error(
            refrain::error_code::invalid_argument,
            "protocol capture: expected --host host:port --out file"));
    }
    std::uint16_t port{0};
    const auto port_text{options.target.substr(separator + 1)};
    const auto parsed{std::from_chars(
        port_text.data(), port_text.data() + port_text.size(), port)};
    if (parsed.ec != std::errc{} ||
        parsed.ptr != port_text.data() + port_text.size()) {
        return report_error(
            refrain::make_error(refrain::error_code::invalid_argument,
                                "protocol capture: invalid port"));
    }
    refrain::net::bot_options bot{};
    bot.target.host = options.target.substr(0, separator);
    bot.target.port = port;
    bot.username = options.username;
    bot.timeout = std::chrono::milliseconds{options.timeout_ms};
    bot.wait_chunks = options.wait_chunks;
    bot.linger = std::chrono::milliseconds{options.linger_ms};
    bot.capture = true;
    auto result{refrain::net::run_bot(bot)};
    if (!result) {
        return report_error(result.error());
    }
    std::vector<std::byte> file{};
    constexpr std::string_view magic{"RFCAP1\n"};
    for (const char character : magic) {
        file.push_back(
            static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
    const auto append_u32 = [&file](std::uint32_t value) {
        for (int shift{0}; shift < 32; shift += 8) {
            file.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
        }
    };
    for (const auto &packet : result->capture) {
        file.push_back(static_cast<std::byte>(packet.state));
        append_u32(packet.id);
        append_u32(static_cast<std::uint32_t>(packet.body.size()));
        file.insert(file.end(), packet.body.begin(), packet.body.end());
    }
    if (auto written{io::write_file(options.out, file)}; !written) {
        return report_error(written.error());
    }
    fmt::print("capture: {} packet(s), {} byte(s) -> {}\n",
               result->capture.size(), file.size(), options.out);
    return 0;
}

auto run_protocol_load(const protocol_load_options &options) -> int {
    const auto separator{options.target.rfind(':')};
    if (separator == std::string::npos ||
        separator + 1 >= options.target.size()) {
        return report_error(
            refrain::make_error(refrain::error_code::invalid_argument,
                                "protocol load: expected host:port"));
    }
    std::uint16_t port{0};
    const auto port_text{options.target.substr(separator + 1)};
    const auto parsed{std::from_chars(
        port_text.data(), port_text.data() + port_text.size(), port)};
    if (parsed.ec != std::errc{} ||
        parsed.ptr != port_text.data() + port_text.size()) {
        return report_error(
            refrain::make_error(refrain::error_code::invalid_argument,
                                "protocol load: invalid port"));
    }
    if (options.bots <= 0) {
        return report_error(
            refrain::make_error(refrain::error_code::invalid_argument,
                                "protocol load: bots must be positive"));
    }

    std::atomic<int> joined{0};
    std::atomic<std::uint64_t> packets{0};
    std::atomic<std::uint64_t> chunks{0};
    std::mutex errors_mutex{};
    std::mutex latency_mutex{};
    std::vector<std::int64_t> join_ms{};
    std::vector<std::string> errors{};
    std::vector<std::thread> threads{};
    threads.reserve(static_cast<std::size_t>(options.bots));
    for (int index{0}; index < options.bots; ++index) {
        threads.emplace_back([&, index] {
            refrain::log::set_thread_name("bot-" + std::to_string(index));
            refrain::net::bot_options bot{};
            bot.target.host = options.target.substr(0, separator);
            bot.target.port = port;
            bot.username = options.username_prefix + std::to_string(index);
            bot.timeout = std::chrono::milliseconds{options.timeout_ms};
            bot.linger = std::chrono::milliseconds{options.duration_ms};
            bot.walk_radius = options.walk_radius;
            // Spread the bots over a grid so the tracking radius matters.
            constexpr int grid{32};
            bot.walk_origin_x = static_cast<double>(index % grid) * 16.0;
            bot.walk_origin_z =
                static_cast<double>((index / grid) % grid) * 16.0;
            bot.walk_period = std::chrono::milliseconds{
                options.walk_period_ms > 0 ? options.walk_period_ms : 100};
            const auto started{std::chrono::steady_clock::now()};
            auto result{refrain::net::run_bot(bot)};
            if (result) {
                ++joined;
                packets.fetch_add(result->packets);
                chunks.fetch_add(result->chunks);
                const auto elapsed{
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started)};
                const std::lock_guard lock{latency_mutex};
                join_ms.push_back(static_cast<std::int64_t>(elapsed.count()) -
                                  options.duration_ms);
            } else {
                const std::lock_guard lock{errors_mutex};
                errors.push_back(result.error().message);
            }
        });
    }
    for (auto &thread : threads) {
        thread.join();
    }
    std::sort(join_ms.begin(), join_ms.end());
    const auto percentile = [&](double fraction) {
        if (join_ms.empty()) {
            return std::int64_t{0};
        }
        const auto index{static_cast<std::size_t>(
            static_cast<double>(join_ms.size() - 1) * fraction)};
        return join_ms[index];
    };
    fmt::print("load: {}/{} joined packets={} chunks={} join_ms p50={} p99={} "
               "max={}\n",
               joined.load(), options.bots, packets.load(), chunks.load(),
               percentile(0.5), percentile(0.99),
               join_ms.empty() ? std::int64_t{0} : join_ms.back());
    if (joined.load() != options.bots) {
        if (!errors.empty()) {
            fmt::print(stderr, "load: first error: {}\n", errors.front());
        }
        return 1;
    }
    return 0;
}

auto run_protocol_pool_load(const protocol_pool_load_options &options) -> int {
    const auto separator{options.target.rfind(':')};
    if (separator == std::string::npos ||
        separator + 1 >= options.target.size()) {
        return report_error(
            refrain::make_error(refrain::error_code::invalid_argument,
                                "protocol pool-load: expected host:port"));
    }
    std::uint16_t port{0};
    const auto port_text{options.target.substr(separator + 1)};
    const auto parsed{std::from_chars(
        port_text.data(), port_text.data() + port_text.size(), port)};
    if (parsed.ec != std::errc{} ||
        parsed.ptr != port_text.data() + port_text.size()) {
        return report_error(
            refrain::make_error(refrain::error_code::invalid_argument,
                                "protocol pool-load: invalid port"));
    }
    if (options.bots <= 0) {
        return report_error(
            refrain::make_error(refrain::error_code::invalid_argument,
                                "protocol pool-load: bots must be positive"));
    }
    if (options.duration_ms < 0) {
        return report_error(refrain::make_error(
            refrain::error_code::invalid_argument,
            "protocol pool-load: duration must not be negative"));
    }
    if (options.timeout_ms <= 0) {
        return report_error(refrain::make_error(
            refrain::error_code::invalid_argument,
            "protocol pool-load: timeout must be positive"));
    }
    if (options.walk_radius < 0.0) {
        return report_error(refrain::make_error(
            refrain::error_code::invalid_argument,
            "protocol pool-load: walk radius must not be negative"));
    }
    if (options.view_distance <= 0 || options.view_distance > 32) {
        return report_error(refrain::make_error(
            refrain::error_code::invalid_argument,
            "protocol pool-load: view distance must be between 1 and 32"));
    }

    refrain::net::bot_pool_options pool{};
    pool.target.host = options.target.substr(0, separator);
    pool.target.port = port;
    pool.bots = static_cast<std::size_t>(options.bots);
    pool.duration = std::chrono::milliseconds{options.duration_ms};
    pool.timeout = std::chrono::milliseconds{options.timeout_ms};
    pool.walk_radius = options.walk_radius;
    pool.walk_period = std::chrono::milliseconds{options.walk_period_ms};
    pool.username_prefix = options.username_prefix;
    pool.view_distance = static_cast<std::uint8_t>(options.view_distance);

    const auto report{refrain::net::run_bot_pool(pool)};
    fmt::print("pool-load: joined={}/{} packets={} chunks={} "
               "join_ms p50={} p99={}\n",
               report.joined, report.requested, report.packets, report.chunks,
               report.join_p50.count(), report.join_p99.count());
    if (report.joined != report.requested) {
        if (!report.errors.empty()) {
            fmt::print(stderr, "pool-load: first error: {}\n",
                       report.errors.front());
        }
        return 1;
    }
    return 0;
}

auto run_config_import_properties(const config_import_options &options) -> int {
    constexpr io::read_options read_options{
        .max_bytes = 1ULL * 1024ULL * 1024ULL, .allow_symlink = false};
    auto text{io::read_text(options.properties, read_options)};
    if (!text) {
        return report_error(text.error());
    }
    const auto mode{options.force
                        ? server::server_properties_import_mode::force
                        : server::server_properties_import_mode::merge};
    auto report{server::import_server_properties(*text, options.config_dir,
                                                 mode, options.strict)};
    if (!report) {
        return report_error(report.error());
    }
    for (const auto &key : report->unknown_keys) {
        fmt::print(stderr,
                   "config: warning: unknown server.properties key "
                   "'{}'\n",
                   key);
    }
    fmt::print("config: imported {} key(s) into {}\n", report->applied,
               options.config_dir);
    for (const auto &path : report->written_files) {
        fmt::print("config: wrote {}\n", path);
    }
    return 0;
}

auto run_vanilla_info(const vanilla_info_options &options) -> int {
    auto bytes = io::read_file(options.tables);
    if (!bytes) {
        return report_error(bytes.error());
    }
    auto tables = data::vanilla_tables::open(*bytes);
    if (!tables) {
        return report_error(tables.error());
    }
    const auto &info = tables->info();
    std::cout << options.tables << ": RVTB v" << info.version
              << " kind=" << info.kind << " data_version=" << info.data_version
              << '\n';
    std::cout << "  strings: " << info.strings << '\n';
    std::cout << "  blocks: " << info.blocks << '\n';
    std::cout << "  states: " << info.states << '\n';
    std::cout << "  properties: " << info.properties << '\n';
    std::cout << "  biome presets: " << info.presets << '\n';
    std::cout << "  registries: " << info.registries << '\n';
    std::cout << "  items: " << info.items << '\n';
    return 0;
}

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
        log_schema.error("generated files are out of date; run "
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
        log_crash_reporter.error(
            "Crashpad client is not attached (is '{}' a real "
            "crashpad_handler?)",
            handler_path);
        return 1;
    }
    std::cout << "crash: handler attached, database=" << database_path
              << "; crashing on purpose to produce a minidump\n"
              << std::flush;
    kernel::crash_reporter::trigger_for_testing();
    log_crash_reporter.error("the fault did not terminate the process");
    return 1;
#else
    log_crash_reporter.error(
        "this build does not enable REFRAIN_WITH_CRASHPAD");
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
        log_wasm.error("expected 5, got {}", *result);
        return 1;
    }
    std::cout << "wasm: OK (wasmtime module add(2,3) = " << *result << ")\n";
    return 0;
#else
    log_wasm.error("this build does not enable REFRAIN_WITH_WASM");
    return 1;
#endif
}

} // namespace refrain::app
