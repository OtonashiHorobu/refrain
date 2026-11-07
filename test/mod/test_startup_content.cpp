// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/config.hpp>
#include <refrain/core/io.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/content_index.hpp>
#include <refrain/data/content_maintenance.hpp>
#include <refrain/data/content_segment.hpp>
#include <refrain/data/content_store.hpp>
#include <refrain/data/json.hpp>
#include <refrain/data/path.hpp>
#include <refrain/data/signature.hpp>
#include <refrain/mod/discovery.hpp>
#include <refrain/mod/mod_ref.hpp>
#include <refrain/mod/rfmod.hpp>
#include <refrain/mod/startup_content.hpp>
#include <refrain/platform/process.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fmt/format.h>

namespace {

using namespace boost::ut;

namespace mod = refrain::mod;

namespace fs = std::filesystem;
namespace io = refrain::core::io;

using refrain::error_code;
using refrain::data::batch;
using refrain::data::blob_digest;
using refrain::data::blob_flags_packed;
using refrain::data::blob_kind_content;
using refrain::data::blob_metadata;
using refrain::data::blob_table_entry;
using refrain::data::blob_table_view;
using refrain::data::build_blob_table;
using refrain::data::content_blob_table_path;
using refrain::data::content_resolution_path;
using refrain::data::content_store;
using refrain::data::digest32;
using refrain::data::parse_blob_table;
using refrain::data::parse_resolution_index;
using refrain::data::resolution_index_view;
using refrain::data::resolve;
using refrain::data::resolved_blob;
using refrain::data::to_hex;
using refrain::mod::receipt_file;
using refrain::mod::rfmod_signing_options;
using refrain::mod::rfmod_trusted_key;
using refrain::mod::rfmod_write_options;
using refrain::mod::run_startup_content;
using refrain::mod::startup_content_options;

using gc_mode = startup_content_options::gc_mode;

constexpr std::string_view fixed_seed_hex{
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"};
constexpr std::string_view test_key_id{"startup-test-key"};
constexpr std::string_view test_manifest{
    "[mod]\n"
    "id = \"refrain.test.startup\"\n"
    "version = \"1.0.0\"\n"
    "abi = \"1\"\n"
    "side = \"both\"\n"
    "\n"
    "[meta]\n"
    "display_name = \"Startup content test\"\n"};

// ---------------------------------------------------------------------------
// scratch directories / bytes
// ---------------------------------------------------------------------------

[[nodiscard]] auto make_workspace_path(std::string_view name) -> fs::path {
    static int counter{0};
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    const auto path{fs::temp_directory_path() /
                    fmt::format("refrain_startup_content_test_{}_{}_{}_{}",
                                stamp, counter++,
                                refrain::platform::process_id(), name)};
    std::error_code error;
    fs::create_directories(path, error);
    return path;
}

struct temp_workspace {
    fs::path root{};

    explicit temp_workspace(std::string_view name)
        : root{make_workspace_path(name)} {}
    temp_workspace(const temp_workspace &) = delete;
    auto operator=(const temp_workspace &) -> temp_workspace & = delete;
    ~temp_workspace() {
        std::error_code error;
        fs::remove_all(root, error);
    }

    [[nodiscard]] auto path(std::string_view relative) const -> fs::path {
        return root / fs::path{std::string{relative}};
    }

    [[nodiscard]] auto write(std::string_view relative,
                             std::string_view content) const -> bool {
        const auto target{path(relative)};
        std::error_code error;
        fs::create_directories(target.parent_path(), error);
        if (error) {
            return false;
        }
        return io::write_text(target.string(), content).has_value();
    }
};

[[nodiscard]] auto bytes_of(std::string_view text) -> std::vector<std::byte> {
    const auto bytes{
        std::as_bytes(std::span<const char>{text.data(), text.size()})};
    return std::vector<std::byte>{bytes.begin(), bytes.end()};
}

[[nodiscard]] auto read_bytes(const fs::path &path)
    -> std::optional<std::vector<std::byte>> {
    auto bytes{io::read_file(path.string())};
    if (!bytes) {
        return std::nullopt;
    }
    return std::optional<std::vector<std::byte>>{std::move(*bytes)};
}

[[nodiscard]] auto same_bytes(std::span<const std::byte> lhs,
                              std::span<const std::byte> rhs) -> bool {
    return lhs.size() == rhs.size() &&
           std::equal(lhs.begin(), lhs.end(), rhs.begin());
}

[[nodiscard]] auto copy_package(const fs::path &source, const fs::path &target)
    -> bool {
    std::error_code error;
    fs::create_directories(target.parent_path(), error);
    if (error) {
        return false;
    }
    return fs::copy_file(source, target, fs::copy_options::overwrite_existing,
                         error) &&
           !error;
}

[[nodiscard]] auto file_count(const fs::path &directory,
                              std::string_view extension) -> std::size_t {
    std::error_code error;
    std::size_t count{0};
    fs::directory_iterator iterator{directory, error};
    const fs::directory_iterator end{};
    while (!error && iterator != end) {
        std::error_code entry_error;
        if (iterator->path().extension() == extension &&
            iterator->is_regular_file(entry_error) && !entry_error) {
            ++count;
        }
        iterator.increment(error);
    }
    return count;
}

/// Sorted BLAKE3 fingerprints of the regular files in `directory` (byte-level
/// idempotency assertions; directory order is unspecified).
[[nodiscard]] auto directory_fingerprint(const fs::path &directory,
                                         std::string_view extension)
    -> std::vector<std::string> {
    std::vector<std::string> digests;
    std::error_code error;
    fs::directory_iterator iterator{directory, error};
    const fs::directory_iterator end{};
    while (!error && iterator != end) {
        std::error_code entry_error;
        if (iterator->path().extension() == extension &&
            iterator->is_regular_file(entry_error) && !entry_error) {
            auto bytes{read_bytes(iterator->path())};
            if (bytes) {
                digests.push_back(to_hex(refrain::data::blake3(*bytes)));
            }
        }
        iterator.increment(error);
    }
    std::sort(digests.begin(), digests.end());
    return digests;
}

// ---------------------------------------------------------------------------
// content store helpers (mirror the maintenance test's manual publication)
// ---------------------------------------------------------------------------

/// RFBT record a batch publisher would build from one `blob_metadata`.
[[nodiscard]] auto entry_from(const blob_metadata &meta,
                              std::uint32_t refcount = 1) -> blob_table_entry {
    std::uint32_t block_count{0};
    if ((meta.flags & blob_flags_packed) != 0U) {
        block_count =
            static_cast<std::uint32_t>((meta.logical_size + 65535U) / 65536U);
    }
    return blob_table_entry{
        .digest = meta.digest,
        .logical_size = meta.logical_size,
        .flags = meta.flags,
        .refcount = refcount,
        .kind = blob_kind_content,
        .block_count = block_count,
        .stored_size = meta.stored_size,
        .segment_id = meta.segment_id,
        .segment_offset = meta.segment_offset,
    };
}

[[nodiscard]] auto entries_from(const std::vector<blob_metadata> &blobs)
    -> std::vector<blob_table_entry> {
    std::vector<blob_table_entry> records;
    records.reserve(blobs.size());
    for (const auto &meta : blobs) {
        records.push_back(entry_from(meta));
    }
    return records;
}

[[nodiscard]] auto write_table(std::string_view root,
                               std::span<const blob_table_entry> records)
    -> bool {
    auto bytes{build_blob_table(records)};
    if (!bytes) {
        return false;
    }
    const auto path{content_blob_table_path(root)};
    const auto directory{refrain::data::parent_path(path)};
    if (!directory.empty()) {
        auto created{io::create_directories(std::string{directory})};
        if (!created) {
            return false;
        }
    }
    return static_cast<bool>(io::atomic_write_file(path, *bytes));
}

/// Run one batch and return its new blob records (empty on failure).
[[nodiscard]] auto stage(std::string_view root,
                         std::span<const std::vector<std::byte>> payloads)
    -> std::vector<blob_metadata> {
    auto staged{batch::begin(root, nullptr)};
    if (!staged) {
        return {};
    }
    for (const auto &payload : payloads) {
        if (!staged->put(payload)) {
            return {};
        }
    }
    if (!staged->commit()) {
        return {};
    }
    const auto blobs{staged->new_blobs()};
    return {blobs.begin(), blobs.end()};
}

[[nodiscard]] auto read_table(std::string_view root)
    -> std::optional<blob_table_view> {
    auto bytes{io::read_file(content_blob_table_path(root))};
    if (!bytes) {
        return std::nullopt;
    }
    auto parsed{parse_blob_table(*bytes)};
    if (!parsed) {
        return std::nullopt;
    }
    return std::optional<blob_table_view>{std::move(*parsed)};
}

[[nodiscard]] auto read_index(std::string_view root,
                              std::string_view composition,
                              const blob_table_view &table)
    -> std::optional<resolution_index_view> {
    auto bytes{io::read_file(content_resolution_path(root, composition))};
    if (!bytes) {
        return std::nullopt;
    }
    auto parsed{parse_resolution_index(*bytes, table)};
    if (!parsed) {
        return std::nullopt;
    }
    return std::optional<resolution_index_view>{std::move(*parsed)};
}

// ---------------------------------------------------------------------------
// receipts
// ---------------------------------------------------------------------------

/// Deterministic `refrain.mod-receipt/1` document (the same shape
/// `install_rfmod` writes).
[[nodiscard]] auto
format_receipt(std::string_view id, std::string_view version, std::uint32_t abi,
               const digest32 &archive_blake3, const digest32 &manifest_blake3,
               const std::vector<receipt_file> &files,
               std::string_view origin = "user") -> std::string {
    using json_value = refrain::data::json::value;
    json_value::array_type list;
    list.reserve(files.size());
    for (const auto &file : files) {
        json_value::object_type entry;
        entry.emplace("path", json_value{file.path});
        entry.emplace("size", json_value{static_cast<std::int64_t>(file.size)});
        entry.emplace("blake3", json_value{to_hex(file.blake3)});
        entry.emplace("mode", json_value{std::uint32_t{0644}});
        list.emplace_back(std::move(entry));
    }
    json_value::object_type receipt;
    receipt.emplace("schema", json_value{"refrain.mod-receipt/1"});
    receipt.emplace("id", json_value{id});
    receipt.emplace("version", json_value{version});
    receipt.emplace("abi", json_value{static_cast<std::int64_t>(abi)});
    receipt.emplace("archive_blake3", json_value{to_hex(archive_blake3)});
    receipt.emplace("manifest_blake3", json_value{to_hex(manifest_blake3)});
    receipt.emplace("signed", json_value{false});
    receipt.emplace("trusted", json_value{false});
    receipt.emplace("key_id", json_value{std::string{}});
    receipt.emplace("origin", json_value{origin});
    receipt.emplace("files", json_value{std::move(list)});
    return json_value{std::move(receipt)}.dump();
}

[[nodiscard]] auto write_receipt(const fs::path &store_root,
                                 std::string_view id, std::string_view version,
                                 std::string_view json) -> bool {
    const auto path{store_root / "mods" / fs::path{std::string{id}} /
                    (std::string{version} + ".json")};
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) {
        return false;
    }
    return io::write_text(path.string(), json).has_value();
}

[[nodiscard]] auto receipt_path_for(const fs::path &store_root,
                                    std::string_view id,
                                    std::string_view version) -> fs::path {
    return store_root / "mods" / fs::path{std::string{id}} /
           (std::string{version} + ".json");
}

/// Two-file receipt over the batch blobs `alpha`/`beta`.
[[nodiscard]] auto
write_two_file_receipt(const fs::path &store_root,
                       const std::vector<blob_metadata> &blobs) -> bool {
    expect(blobs.size() == 2U);
    if (blobs.size() != 2U) {
        return false;
    }
    const std::vector<receipt_file> files{
        receipt_file{
            .path = "data/a.txt", .size = 5, .blake3 = blobs[0].digest},
        receipt_file{
            .path = "data/b.txt", .size = 4, .blake3 = blobs[1].digest}};
    const auto archive{refrain::data::blake3(bytes_of("archive"))};
    const auto manifest{refrain::data::blake3(bytes_of("manifest"))};
    return write_receipt(store_root, "refrain.test.index", "1.0.0",
                         format_receipt("refrain.test.index", "1.0.0", 1,
                                        archive, manifest, files));
}

[[nodiscard]] auto make_options(std::string store_root,
                                std::string composition = "test-gp")
    -> startup_content_options {
    startup_content_options options{};
    options.store_root = std::move(store_root);
    options.composition = std::move(composition);
    options.auto_install = false;
    options.gc = gc_mode::off;
    return options;
}

// ---------------------------------------------------------------------------
// .rfmod fixtures (signed/unsigned package through pack_mod_directory)
// ---------------------------------------------------------------------------

struct packed_fixture {
    std::string archive_path{};
    digest32 archive_digest{};
    refrain::data::ed25519_public_key public_key{};
};

[[nodiscard]] auto pack_fixture(const temp_workspace &work,
                                std::string_view output_name,
                                bool signed_package)
    -> std::optional<packed_fixture> {
    expect(work.write("src/manifest.toml", test_manifest));
    expect(work.write("src/LICENSE", "MIT\n"));
    expect(work.write("src/data/test/a.json", "{}"));
    expect(work.write("src/assets/x.txt", "x"));

    rfmod_write_options write_options{};
    packed_fixture fixture;
    if (signed_package) {
        auto seed{refrain::data::parse_seed_hex(fixed_seed_hex)};
        expect(static_cast<bool>(seed));
        if (!seed) {
            return std::nullopt;
        }
        auto keypair{refrain::data::keypair_from_seed(*seed)};
        expect(static_cast<bool>(keypair));
        if (!keypair) {
            return std::nullopt;
        }
        write_options.signing =
            rfmod_signing_options{.secret_key = keypair->secret_key,
                                  .key_id = std::string{test_key_id}};
        fixture.public_key = keypair->public_key;
    }
    const auto output{work.path(output_name)};
    auto packed{refrain::mod::pack_mod_directory(
        work.path("src").string(), output.string(), write_options)};
    expect(static_cast<bool>(packed)) << "pack_mod_directory failed";
    if (!packed) {
        return std::nullopt;
    }
    fixture.archive_path = packed->archive_path;
    fixture.archive_digest = packed->archive_digest;
    return fixture;
}

} // namespace

const suite startup_content_tests = [] {
    "startup content: pins use mods.lock baseline and gamepack overrides"_test =
        [] {
            refrain::mod::mod_lock lock{};
            lock.mods = {
                refrain::mod::locked_mod{.id = "mod.a", .version = "1.0.0"},
                refrain::mod::locked_mod{.id = "mod.b", .version = "2.0.0"},
            };
            const std::vector<refrain::mod::mod_ref> overrides{
                refrain::mod::mod_ref{.id = "mod.b", .version = "3.0.0"},
                refrain::mod::mod_ref{.id = "mod.c", .version = "1.0.0"},
            };
            const auto merged{
                refrain::mod::merge_pinned_versions(&lock, overrides)};
            expect(merged.size() == 3U);
            expect(merged.at("mod.a") == "1.0.0");
            expect(merged.at("mod.b") == "3.0.0");
            expect(merged.at("mod.c") == "1.0.0");

            // auto_scan disabled / missing lock: the gamepack list is the sole
            // source; lock-only ids must not leak into the pin map.
            const auto without_lock{
                refrain::mod::merge_pinned_versions(nullptr, overrides)};
            expect(without_lock.size() == 2U);
            expect(!without_lock.contains("mod.a"));
            expect(without_lock.at("mod.b") == "3.0.0");
        };

    "startup content: empty store/inbox reports zero and publishes nothing"_test =
        [] {
            temp_workspace work{"empty"};
            const auto store_root{(work.path("store")).string()};
            std::vector<std::string> lines;
            auto options{make_options(store_root)};
            options.gc = gc_mode::dry_run;
            options.log = [&lines](std::string_view line) {
                lines.emplace_back(line);
            };

            auto report{run_startup_content(options)};
            expect(static_cast<bool>(report));
            if (!report) {
                return;
            }
            expect(report->scanned == 0U);
            expect(report->installed == 0U);
            expect(report->skipped == 0U);
            expect(report->failed == 0U);
            expect(report->receipt_files == 0U);
            expect(report->published_entries == 0U);
            expect(!report->index_published);
            expect(report->gc_deleted_blobs == 0U);
            expect(report->gc_deleted_segments == 0U);
            expect(report->lock_acquired);
            expect(!report->read_only);
            expect(!fs::exists(
                work.path("store/content/resolutions/test-gp.idx")));
            // The lock is released on every path (including success).
            expect(!fs::exists(work.path("store/content/.lock")));
            expect(!lines.empty());
        };

    "startup content: receipts publish a bound resolution index"_test = [] {
        temp_workspace work{"index"};
        const auto store_root{(work.path("store")).string()};
        const std::vector<std::vector<std::byte>> payloads{bytes_of("alpha"),
                                                           bytes_of("beta")};
        const auto blobs{stage(store_root, payloads)};
        expect(blobs.size() == 2U);
        if (blobs.size() != 2U) {
            return;
        }
        expect(write_table(store_root, entries_from(blobs)));
        expect(write_two_file_receipt(work.path("store"), blobs));

        std::vector<std::string> lines;
        auto options{make_options(store_root)};
        options.log = [&lines](std::string_view line) {
            lines.emplace_back(line);
        };
        auto report{run_startup_content(options)};
        expect(static_cast<bool>(report));
        if (!report) {
            return;
        }
        expect(report->index_published);
        expect(report->published_entries == 2U);
        expect(report->receipt_files == 2U);
        expect(report->scanned == 0U);
        expect(report->installed == 0U);
        const auto layer_line{std::find(
            lines.begin(), lines.end(),
            "startup-content: layer: refrain.test.index@1.0.0 priority=1000")};
        expect(layer_line != lines.end());

        auto table{read_table(store_root)};
        expect(table.has_value());
        if (!table) {
            return;
        }
        auto index{read_index(store_root, "test-gp", *table)};
        expect(index.has_value());
        if (!index) {
            return;
        }
        auto resolved{resolve(*index, "data/a.txt")};
        expect(static_cast<bool>(resolved));
        if (!resolved) {
            return;
        }
        expect(resolved->winner != nullptr);
        if (resolved->winner == nullptr) {
            return;
        }
        expect(resolved->winner->source == "refrain.test.index");
        expect(resolved->winner->priority == 1000);
        auto digest{resolved_blob(*resolved, *table)};
        expect(static_cast<bool>(digest));
        if (digest) {
            expect(*digest == blobs[0].digest);
        }
    };

    "startup content: layer order decides a shared location across mods"_test =
        [] {
            temp_workspace work{"license"};
            const auto store_root{(work.path("store")).string()};
            const std::vector<std::vector<std::byte>> payloads{
                bytes_of("license-alpha"), bytes_of("license-beta")};
            const auto blobs{stage(store_root, payloads)};
            expect(blobs.size() == 2U);
            if (blobs.size() != 2U) {
                return;
            }
            expect(write_table(store_root, entries_from(blobs)));
            const auto archive{refrain::data::blake3(bytes_of("archive"))};
            const auto manifest{refrain::data::blake3(bytes_of("manifest"))};
            // Two mods ship a different LICENSE. Each layer gets its own
            // priority from the layer order, so both entries are legal and the
            // list order (beta first) decides the winner, not the source id.
            expect(write_receipt(
                work.path("store"), "mod.alpha", "1.0.0",
                format_receipt("mod.alpha", "1.0.0", 1, archive, manifest,
                               {receipt_file{.path = "LICENSE",
                                             .size = static_cast<std::uint64_t>(
                                                 payloads[0].size()),
                                             .blake3 = blobs[0].digest}})));
            expect(write_receipt(
                work.path("store"), "mod.beta", "1.0.0",
                format_receipt("mod.beta", "1.0.0", 1, archive, manifest,
                               {receipt_file{.path = "LICENSE",
                                             .size = static_cast<std::uint64_t>(
                                                 payloads[1].size()),
                                             .blake3 = blobs[1].digest}})));

            std::vector<std::string> lines;
            auto options{make_options(store_root)};
            options.layer_order = {"mod.beta", "mod.alpha"};
            options.log = [&lines](std::string_view line) {
                lines.emplace_back(line);
            };
            auto report{run_startup_content(options)};
            expect(static_cast<bool>(report));
            if (!report) {
                return;
            }
            expect(report->index_published);
            expect(report->published_entries == 2U);
            expect(report->failed == 0U);
            expect(
                std::find(
                    lines.begin(), lines.end(),
                    "startup-content: layer: mod.beta@1.0.0 priority=1000") !=
                lines.end());
            expect(
                std::find(
                    lines.begin(), lines.end(),
                    "startup-content: layer: mod.alpha@1.0.0 priority=999") !=
                lines.end());

            auto table{read_table(store_root)};
            expect(table.has_value());
            if (!table) {
                return;
            }
            auto index{read_index(store_root, "test-gp", *table)};
            expect(index.has_value());
            if (!index) {
                return;
            }
            auto resolved{resolve(*index, "LICENSE")};
            expect(static_cast<bool>(resolved));
            if (!resolved) {
                return;
            }
            expect(resolved->winner != nullptr);
            if (resolved->winner != nullptr) {
                expect(resolved->winner->source == "mod.beta");
                expect(resolved->winner->priority == 1000);
                auto digest{resolved_blob(*resolved, *table)};
                expect(static_cast<bool>(digest));
                if (digest) {
                    expect(*digest == blobs[1].digest);
                }
            }
            expect(resolved->fallback.size() == 1U);
            if (resolved->fallback.size() == 1U) {
                expect(resolved->fallback[0]->source == "mod.alpha");
                expect(resolved->fallback[0]->priority == 999);
                auto digest{blob_digest(*resolved->fallback[0], *table)};
                expect(static_cast<bool>(digest));
                if (digest) {
                    expect(*digest == blobs[0].digest);
                }
            }
        };

    "startup content: gamepack pin selects the exact installed version"_test =
        [] {
            temp_workspace work{"pinned"};
            const auto store_root{(work.path("store")).string()};
            const std::vector<std::vector<std::byte>> payloads{
                bytes_of("alpha-v1"), bytes_of("alpha-v2")};
            const auto blobs{stage(store_root, payloads)};
            expect(blobs.size() == 2U);
            if (blobs.size() != 2U) {
                return;
            }
            expect(write_table(store_root, entries_from(blobs)));
            const auto archive{refrain::data::blake3(bytes_of("archive"))};
            const auto manifest{refrain::data::blake3(bytes_of("manifest"))};
            for (std::size_t index{0}; index < payloads.size(); ++index) {
                const auto version{fmt::format("{}.0.0", index + 1)};
                expect(write_receipt(
                    work.path("store"), "mod.alpha", version,
                    format_receipt(
                        "mod.alpha", version, 1, archive, manifest,
                        {receipt_file{.path = "data/a.txt",
                                      .size = static_cast<std::uint64_t>(
                                          payloads[index].size()),
                                      .blake3 = blobs[index].digest}})));
            }

            // The gamepack pin overrides the default highest-SemVer choice:
            // 2.0.0 is installed, but the declaration pins 1.0.0.
            auto options{make_options(store_root)};
            options.pinned_versions = {{"mod.alpha", "1.0.0"},
                                       {"refrain.content.vanilla", "1.0.0"}};
            // Builtin ids declared by the gamepack have no receipt; the pin is
            // ignored instead of failing the boot.
            auto report{run_startup_content(options)};
            expect(static_cast<bool>(report));
            if (!report) {
                return;
            }
            expect(report->index_published);
            auto table{read_table(store_root)};
            expect(table.has_value());
            if (!table) {
                return;
            }
            auto index{read_index(store_root, "test-gp", *table)};
            expect(index.has_value());
            if (!index) {
                return;
            }
            auto resolved{resolve(*index, "data/a.txt")};
            expect(static_cast<bool>(resolved));
            if (!resolved) {
                return;
            }
            expect(resolved->winner != nullptr);
            if (resolved->winner == nullptr) {
                return;
            }
            expect(resolved->winner->source == "mod.alpha");
            auto digest{resolved_blob(*resolved, *table)};
            expect(static_cast<bool>(digest));
            if (digest) {
                expect(*digest == blobs[0].digest);
            }
            // There is exactly one layer: the pinned version. 2.0.0 is not a
            // fallback candidate and does not become a second priority.
            expect(resolved->fallback.empty());

            // Without a pin the same store selects the highest SemVer.
            options.pinned_versions.clear();
            auto unpinned{run_startup_content(options)};
            expect(static_cast<bool>(unpinned));
            if (!unpinned) {
                return;
            }
            auto restored{read_table(store_root)};
            expect(restored.has_value());
            if (!restored) {
                return;
            }
            auto index2{read_index(store_root, "test-gp", *restored)};
            expect(index2.has_value());
            if (!index2) {
                return;
            }
            auto winner{resolve(*index2, "data/a.txt")};
            expect(static_cast<bool>(winner));
            if (!winner || winner->winner == nullptr) {
                return;
            }
            auto winner_digest{resolved_blob(*winner, *restored)};
            expect(static_cast<bool>(winner_digest));
            if (winner_digest) {
                expect(*winner_digest == blobs[1].digest);
            }
        };

    "startup content: origin outranks SemVer without an explicit pin"_test =
        [] {
            temp_workspace work{"origin"};
            const auto store_root{(work.path("store")).string()};
            const std::vector<std::vector<std::byte>> payloads{
                bytes_of("user-v1"), bytes_of("first-party-v2")};
            const auto blobs{stage(store_root, payloads)};
            expect(blobs.size() == 2U);
            if (blobs.size() != 2U) {
                return;
            }
            expect(write_table(store_root, entries_from(blobs)));
            const auto archive{refrain::data::blake3(bytes_of("archive"))};
            const auto manifest{refrain::data::blake3(bytes_of("manifest"))};
            const auto write_version = [&](std::string_view version,
                                           std::size_t blob,
                                           std::string_view origin) {
                return write_receipt(
                    work.path("store"), "mod.alpha", version,
                    format_receipt(
                        "mod.alpha", version, 1, archive, manifest,
                        {receipt_file{.path = "data/a.txt",
                                      .size = static_cast<std::uint64_t>(
                                          payloads[blob].size()),
                                      .blake3 = blobs[blob].digest}},
                        origin));
            };
            expect(write_version("1.0.0", 0, "user"));
            expect(write_version("2.0.0", 1, "first_party"));

            std::vector<std::string> lines;
            auto options{make_options(store_root)};
            options.log = [&lines](std::string_view line) {
                lines.emplace_back(line);
            };
            auto report{run_startup_content(options)};
            expect(static_cast<bool>(report));
            if (!report) {
                return;
            }
            expect(std::find(lines.begin(), lines.end(),
                             "startup-content: index: mod.alpha multiple "
                             "versions, using 1.0.0 origin=user") !=
                   lines.end());
            auto table{read_table(store_root)};
            expect(table.has_value());
            if (!table) {
                return;
            }
            auto index{read_index(store_root, "test-gp", *table)};
            expect(index.has_value());
            if (!index) {
                return;
            }
            auto resolved{resolve(*index, "data/a.txt")};
            expect(static_cast<bool>(resolved));
            if (!resolved || resolved->winner == nullptr) {
                return;
            }
            auto digest{resolved_blob(*resolved, *table)};
            expect(static_cast<bool>(digest));
            if (digest) {
                expect(*digest == blobs[0].digest);
            }

            // An explicit pin still wins over the origin tier.
            options.pinned_versions = {{"mod.alpha", "2.0.0"}};
            auto pinned{run_startup_content(options)};
            expect(static_cast<bool>(pinned));
            if (!pinned) {
                return;
            }
            auto table2{read_table(store_root)};
            expect(table2.has_value());
            if (!table2) {
                return;
            }
            auto index2{read_index(store_root, "test-gp", *table2)};
            expect(index2.has_value());
            if (!index2) {
                return;
            }
            auto winner{resolve(*index2, "data/a.txt")};
            expect(static_cast<bool>(winner));
            if (!winner || winner->winner == nullptr) {
                return;
            }
            auto winner_digest{resolved_blob(*winner, *table2)};
            expect(static_cast<bool>(winner_digest));
            if (winner_digest) {
                expect(*winner_digest == blobs[1].digest);
            }
        };

    "startup content: unlisted layers append in origin order"_test = [] {
        temp_workspace work{"origin-order"};
        const auto store_root{(work.path("store")).string()};
        const std::vector<std::vector<std::byte>> payloads{bytes_of("first"),
                                                           bytes_of("user")};
        const auto blobs{stage(store_root, payloads)};
        expect(blobs.size() == 2U);
        if (blobs.size() != 2U) {
            return;
        }
        expect(write_table(store_root, entries_from(blobs)));
        const auto archive{refrain::data::blake3(bytes_of("archive"))};
        const auto manifest{refrain::data::blake3(bytes_of("manifest"))};
        // `mod.first` sorts before `mod.user` by id, but origin must decide the
        // append order: the user layer gets the higher priority.
        expect(write_receipt(
            work.path("store"), "mod.first", "1.0.0",
            format_receipt("mod.first", "1.0.0", 1, archive, manifest,
                           {receipt_file{.path = "data/first.txt",
                                         .size = static_cast<std::uint64_t>(
                                             payloads[0].size()),
                                         .blake3 = blobs[0].digest}},
                           "first_party")));
        expect(write_receipt(
            work.path("store"), "mod.user", "1.0.0",
            format_receipt("mod.user", "1.0.0", 1, archive, manifest,
                           {receipt_file{.path = "data/user.txt",
                                         .size = static_cast<std::uint64_t>(
                                             payloads[1].size()),
                                         .blake3 = blobs[1].digest}},
                           "user")));

        std::vector<std::string> lines;
        auto options{make_options(store_root)};
        options.log = [&lines](std::string_view line) {
            lines.emplace_back(line);
        };
        auto report{run_startup_content(options)};
        expect(static_cast<bool>(report));
        if (!report) {
            return;
        }
        expect(std::find(lines.begin(), lines.end(),
                         "startup-content: layer: mod.user@1.0.0 "
                         "priority=1000") != lines.end());
        expect(std::find(lines.begin(), lines.end(),
                         "startup-content: layer: mod.first@1.0.0 "
                         "priority=999") != lines.end());
    };

    "startup content: a pinned version that is not installed fails closed"_test =
        [] {
            temp_workspace work{"pinned-missing"};
            const auto store_root{(work.path("store")).string()};
            const std::vector<std::vector<std::byte>> payloads{
                bytes_of("only")};
            const auto blobs{stage(store_root, payloads)};
            expect(blobs.size() == 1U);
            if (blobs.size() != 1U) {
                return;
            }
            expect(write_table(store_root, entries_from(blobs)));
            const auto archive{refrain::data::blake3(bytes_of("archive"))};
            const auto manifest{refrain::data::blake3(bytes_of("manifest"))};
            expect(write_receipt(
                work.path("store"), "mod.alpha", "1.0.0",
                format_receipt("mod.alpha", "1.0.0", 1, archive, manifest,
                               {receipt_file{.path = "data/a.txt",
                                             .size = static_cast<std::uint64_t>(
                                                 payloads[0].size()),
                                             .blake3 = blobs[0].digest}})));

            auto options{make_options(store_root)};
            options.pinned_versions = {{"mod.alpha", "9.0.0"}};
            auto report{run_startup_content(options)};
            expect(!report);
            if (report) {
                return;
            }
            expect(report.error().code == error_code::not_found);
            expect(report.error().message.find("mod.alpha@9.0.0") !=
                   std::string::npos);
            expect(!fs::exists(
                work.path("store/content/resolutions/test-gp.idx")));
        };

    "startup content: layer order and base priority are validated"_test = [] {
        temp_workspace work{"invalid-layers"};
        const auto store_root{(work.path("store")).string()};

        auto options{make_options(store_root)};
        options.layer_order = {"mod.a", "mod.a"};
        auto duplicate{run_startup_content(options)};
        expect(!duplicate);
        if (duplicate) {
            return;
        }
        expect(duplicate.error().code == error_code::invalid_argument);
        expect(duplicate.error().message.find("duplicate") !=
               std::string::npos);
        expect(duplicate.error().message.find("mod.a") != std::string::npos);

        options.layer_order.clear();
        options.base_priority = 2147483648U;
        auto range{run_startup_content(options)};
        expect(!range);
        if (range) {
            return;
        }
        expect(range.error().code == error_code::invalid_argument);
        expect(range.error().message.find("base_priority") !=
               std::string::npos);

        const std::vector<std::vector<std::byte>> payloads{bytes_of("alpha"),
                                                           bytes_of("beta")};
        const auto blobs{stage(store_root, payloads)};
        expect(blobs.size() == 2U);
        if (blobs.size() != 2U) {
            return;
        }
        expect(write_table(store_root, entries_from(blobs)));
        const auto archive{refrain::data::blake3(bytes_of("archive"))};
        const auto manifest{refrain::data::blake3(bytes_of("manifest"))};
        expect(write_receipt(
            work.path("store"), "mod.a", "1.0.0",
            format_receipt("mod.a", "1.0.0", 1, archive, manifest,
                           {receipt_file{.path = "data/a.txt",
                                         .size = static_cast<std::uint64_t>(
                                             payloads[0].size()),
                                         .blake3 = blobs[0].digest}})));
        expect(write_receipt(
            work.path("store"), "mod.b", "1.0.0",
            format_receipt("mod.b", "1.0.0", 1, archive, manifest,
                           {receipt_file{.path = "data/b.txt",
                                         .size = static_cast<std::uint64_t>(
                                             payloads[1].size()),
                                         .blake3 = blobs[1].digest}})));
        // Two selected layers do not fit a base priority of 1.
        options.base_priority = 1;
        auto too_small{run_startup_content(options)};
        expect(!too_small);
        if (!too_small) {
            expect(too_small.error().code == error_code::invalid_argument);
            expect(too_small.error().message.find("base_priority") !=
                   std::string::npos);
        }
        expect(!fs::exists(work.path("store/content/resolutions/test-gp.idx")));
    };

    "startup content: layer-order anomalies are aggregated into one line"_test =
        [] {
            temp_workspace work{"layer-warnings"};
            const auto store_root{(work.path("store")).string()};
            const std::vector<std::vector<std::byte>> payloads{
                bytes_of("alpha"), bytes_of("beta")};
            const auto blobs{stage(store_root, payloads)};
            expect(blobs.size() == 2U);
            if (blobs.size() != 2U) {
                return;
            }
            expect(write_table(store_root, entries_from(blobs)));
            const auto archive{refrain::data::blake3(bytes_of("archive"))};
            const auto manifest{refrain::data::blake3(bytes_of("manifest"))};
            expect(write_receipt(
                work.path("store"), "mod.alpha", "1.0.0",
                format_receipt("mod.alpha", "1.0.0", 1, archive, manifest,
                               {receipt_file{.path = "data/a.txt",
                                             .size = static_cast<std::uint64_t>(
                                                 payloads[0].size()),
                                             .blake3 = blobs[0].digest}})));
            expect(write_receipt(
                work.path("store"), "mod.beta", "1.0.0",
                format_receipt("mod.beta", "1.0.0", 1, archive, manifest,
                               {receipt_file{.path = "data/b.txt",
                                             .size = static_cast<std::uint64_t>(
                                                 payloads[1].size()),
                                             .blake3 = blobs[1].digest}})));

            std::vector<std::string> lines;
            auto options{make_options(store_root)};
            // Mirrors the server: the gamepack declares builtin ids that are
            // not content receipts. They are ignored, not warned per id.
            options.layer_order = {
                "refrain.content.vanilla", "refrain.data.vanilla",
                "refrain.world.vanilla",   "refrain.sim.vanilla",
                "refrain.storage.vanilla", "refrain.proto.vanilla",
                "refrain.session.vanilla", "mod.beta"};
            options.log = [&lines](std::string_view line) {
                lines.emplace_back(line);
            };
            auto report{run_startup_content(options)};
            expect(static_cast<bool>(report));
            if (!report) {
                return;
            }
            expect(report->index_published);
            expect(report->published_entries == 2U);

            const auto count_containing = [&lines](std::string_view needle) {
                return std::count_if(lines.begin(), lines.end(),
                                     [needle](const std::string &line) {
                                         return line.find(needle) !=
                                                std::string::npos;
                                     });
            };
            expect(count_containing("declared id(s) missing") == 1);
            expect(count_containing("appended:") == 1);
            const auto missing_line{std::find_if(
                lines.begin(), lines.end(), [](const std::string &line) {
                    return line.find("declared id(s) missing") !=
                           std::string::npos;
                })};
            expect(missing_line != lines.end());
            if (missing_line != lines.end()) {
                expect(missing_line->find("7 declared id(s)") !=
                       std::string::npos);
                expect(missing_line->find("and 2 more") != std::string::npos);
            }
            expect(
                std::find(
                    lines.begin(), lines.end(),
                    "startup-content: layer: mod.beta@1.0.0 priority=1000") !=
                lines.end());
            expect(std::find(lines.begin(), lines.end(),
                             "startup-content: layer: "
                             "mod.alpha@1.0.0 priority=999") != lines.end());

            auto table{read_table(store_root)};
            expect(table.has_value());
            if (!table) {
                return;
            }
            auto index{read_index(store_root, "test-gp", *table)};
            expect(index.has_value());
            if (!index) {
                return;
            }
            auto beta{resolve(*index, "data/b.txt")};
            auto alpha{resolve(*index, "data/a.txt")};
            expect(static_cast<bool>(beta));
            expect(static_cast<bool>(alpha));
            if (beta && beta->winner != nullptr) {
                expect(beta->winner->source == "mod.beta");
                expect(beta->winner->priority == 1000);
            }
            if (alpha && alpha->winner != nullptr) {
                expect(alpha->winner->source == "mod.alpha");
                expect(alpha->winner->priority == 999);
            }
        };

    "startup content: a corrupt receipt fails before publish/GC"_test = [] {
        temp_workspace work{"corrupt"};
        const auto store_root{(work.path("store")).string()};
        const std::vector<std::vector<std::byte>> payloads{bytes_of("alpha"),
                                                           bytes_of("beta")};
        const auto blobs{stage(store_root, payloads)};
        expect(blobs.size() == 2U);
        if (blobs.size() != 2U) {
            return;
        }
        expect(write_table(store_root, entries_from(blobs)));
        expect(write_two_file_receipt(work.path("store"), blobs));

        auto options{make_options(store_root)};
        options.gc = gc_mode::apply;
        options.gc_keep_days = 0;
        auto published{run_startup_content(options)};
        expect(static_cast<bool>(published));
        if (!published) {
            return;
        }
        expect(published->index_published);

        const auto index_path{
            work.path("store/content/resolutions/test-gp.idx")};
        const auto table_path{work.path("store/content/blobs.idx")};
        const auto index_before{read_bytes(index_path)};
        const auto table_before{read_bytes(table_path)};
        expect(index_before.has_value());
        expect(table_before.has_value());
        if (!index_before || !table_before) {
            return;
        }

        const auto receipt{receipt_path_for(work.path("store"),
                                            "refrain.test.index", "1.0.0")};
        expect(io::write_text(receipt.string(), "{").has_value());

        auto failed{run_startup_content(options)};
        expect(!failed);
        if (failed) {
            return;
        }
        auto slashed = [](std::string text) {
            std::replace(text.begin(), text.end(), '\\', '/');
            return text;
        };
        expect(
            slashed(failed.error().message).find(slashed(receipt.string())) !=
            std::string::npos);
        const auto index_after{read_bytes(index_path)};
        const auto table_after{read_bytes(table_path)};
        expect(index_after.has_value());
        expect(table_after.has_value());
        if (index_after && table_after) {
            expect(same_bytes(*index_before, *index_after));
            expect(same_bytes(*table_before, *table_after));
        }
    };

    "startup content: store lock times out, then reclaims a stale owner"_test =
        [] {
            temp_workspace work{"lock"};
            const auto store_root{(work.path("store")).string()};
            const auto lock_directory{work.path("store/content/.lock")};
            const auto owner{lock_directory / "owner"};
            std::error_code error;
            fs::create_directories(lock_directory, error);
            expect(!error);
            // A live owner pid: the lock is respected until the timeout.
            expect(io::write_text(owner.string(),
                                  fmt::format("pid={}\nstarted=0\n",
                                              refrain::platform::process_id()))
                       .has_value());

            std::vector<std::string> lines;
            auto options{make_options(store_root)};
            options.lock_timeout_ms = 100;
            options.log = [&lines](std::string_view line) {
                lines.emplace_back(line);
            };
            auto blocked{run_startup_content(options)};
            expect(!blocked);
            if (blocked) {
                return;
            }
            expect(blocked.error().code == error_code::io_error);
            expect(blocked.error().message.find(".lock") != std::string::npos);

            // Removing the holder lets the next run acquire the lock.
            fs::remove_all(lock_directory, error);
            expect(!error);
            auto acquired{run_startup_content(options)};
            expect(static_cast<bool>(acquired));
            if (acquired) {
                expect(acquired->lock_acquired);
            }
            expect(!fs::exists(lock_directory));

            // A dead owner pid is reclaimed with a warning.
            fs::create_directories(lock_directory, error);
            expect(!error);
            expect(io::write_text(owner.string(), "pid=4294967295\nstarted=0\n")
                       .has_value());
            lines.clear();
            options.lock_timeout_ms = 1000;
            auto reclaimed{run_startup_content(options)};
            expect(static_cast<bool>(reclaimed));
            if (reclaimed) {
                expect(reclaimed->lock_acquired);
            }
            expect(!fs::exists(lock_directory));
            const auto warned{std::any_of(
                lines.begin(), lines.end(), [](const std::string &line) {
                    return line.find("stale lock") != std::string::npos;
                })};
            expect(warned);

            // An owner record older than one hour is stale even when the pid
            // cannot be probed (portable mtime fallback).
            fs::create_directories(lock_directory, error);
            expect(!error);
            fs::last_write_time(lock_directory,
                                fs::file_time_type::clock::now() -
                                    std::chrono::hours{2},
                                error);
            if (error) {
                return; // directory timestamps are not settable everywhere
            }
            lines.clear();
            auto aged{run_startup_content(options)};
            expect(static_cast<bool>(aged));
            if (aged) {
                expect(aged->lock_acquired);
            }
            const auto aged_warning{std::any_of(
                lines.begin(), lines.end(), [](const std::string &line) {
                    return line.find("older than") != std::string::npos;
                })};
            expect(aged_warning);
        };

    "startup content: gc apply keeps receipt roots and deletes the rest"_test =
        [] {
            temp_workspace work{"gc"};
            const auto store_root{(work.path("store")).string()};
            auto store{content_store::open(store_root)};
            expect(static_cast<bool>(store));
            if (!store) {
                return;
            }
            const auto kept{store->put(bytes_of("keep-me"))};
            const auto dropped{store->put(bytes_of("delete-me"))};
            expect(static_cast<bool>(kept));
            expect(static_cast<bool>(dropped));
            if (!kept || !dropped) {
                return;
            }
            const std::vector<blob_table_entry> records{
                blob_table_entry{.digest = *kept,
                                 .logical_size = 7,
                                 .flags = 0,
                                 .refcount = 1,
                                 .kind = blob_kind_content,
                                 .block_count = 0,
                                 .stored_size = 7,
                                 .segment_id = 0,
                                 .segment_offset = 0},
                blob_table_entry{.digest = *dropped,
                                 .logical_size = 9,
                                 .flags = 0,
                                 .refcount = 1,
                                 .kind = blob_kind_content,
                                 .block_count = 0,
                                 .stored_size = 9,
                                 .segment_id = 0,
                                 .segment_offset = 0}};
            expect(write_table(store_root, records));
            const std::vector<receipt_file> files{receipt_file{
                .path = "data/kept.txt", .size = 7, .blake3 = *kept}};
            const auto archive{refrain::data::blake3(bytes_of("archive"))};
            const auto manifest{refrain::data::blake3(bytes_of("manifest"))};
            expect(write_receipt(work.path("store"), "refrain.test.gc", "1.0.0",
                                 format_receipt("refrain.test.gc", "1.0.0", 1,
                                                archive, manifest, files)));

            auto options{make_options(store_root)};
            // Freshly written blobs must not be protected by the keep window.
            options.gc_keep_days = 0;
            options.gc = gc_mode::off;
            auto off{run_startup_content(options)};
            expect(static_cast<bool>(off));
            if (off) {
                expect(off->gc_deleted_blobs == 0U);
                expect(off->gc_planned_blobs == 0U);
            }
            expect(fs::exists(store->path_for(*dropped)));

            options.gc = gc_mode::dry_run;
            auto dry{run_startup_content(options)};
            expect(static_cast<bool>(dry));
            if (dry) {
                expect(dry->gc_deleted_blobs == 0U);
                expect(dry->gc_planned_blobs == 1U);
            }
            expect(fs::exists(store->path_for(*dropped)));

            options.gc = gc_mode::apply;
            auto applied{run_startup_content(options)};
            expect(static_cast<bool>(applied));
            if (applied) {
                expect(applied->gc_deleted_blobs == 1U);
                expect(applied->gc_planned_blobs == 1U);
            }
            expect(fs::exists(store->path_for(*kept)));
            expect(!fs::exists(store->path_for(*dropped)));
            auto table{read_table(store_root)};
            expect(table.has_value());
            if (table) {
                const auto contains = [&table](const digest32 &digest) {
                    return std::any_of(
                        table->entries.begin(), table->entries.end(),
                        [&digest](const blob_table_entry &entry) {
                            return entry.digest == digest;
                        });
                };
                expect(contains(*kept));
                expect(!contains(*dropped));
            }
        };

    "startup content: inbox package installs, publishes and is idempotent"_test =
        [] {
            temp_workspace work{"install"};
            auto fixture{pack_fixture(work, "a.rfmod", true)};
            expect(fixture.has_value());
            if (!fixture) {
                return;
            }
            const auto store_root{(work.path("store")).string()};
            const auto inbox{work.path("store/mods/inbox")};
            expect(copy_package(work.path("a.rfmod"), inbox / "a.rfmod"));

            std::vector<std::string> lines;
            auto options{make_options(store_root)};
            options.auto_install = true;
            options.require_signature = true;
            options.trusted_keys = {
                rfmod_trusted_key{.key_id = std::string{test_key_id},
                                  .public_key = fixture->public_key}};
            options.gc = gc_mode::apply;
            options.gc_keep_days = 0;
            options.log = [&lines](std::string_view line) {
                lines.emplace_back(line);
            };

            auto report{run_startup_content(options)};
            expect(static_cast<bool>(report));
            if (!report) {
                return;
            }
            expect(report->scanned == 1U);
            expect(report->installed == 1U);
            expect(report->skipped == 0U);
            expect(report->failed == 0U);
            expect(report->index_published);
            // manifest.toml + LICENSE + data/test/a.json + assets/x.txt in
            // the receipt; the package manifest is metadata and is not
            // published as a VFS location.
            expect(report->receipt_files == 4U);
            expect(report->published_entries == 3U);
            expect(fs::exists(
                work.path("store/mods/refrain.test.startup/1.0.0.json")));
            expect(
                fs::exists(work.path("store/content/resolutions/test-gp.idx")));
            expect(fs::exists(work.path("store/content/blobs.idx")));
            expect(file_count(work.path("store/content/segments"), ".rfseg") >=
                   1U);
            // keep_installed: the package moved out of the inbox root.
            expect(!fs::exists(inbox / "a.rfmod"));
            expect(fs::exists(inbox / "installed" / "a.rfmod"));

            const auto segment_count{
                file_count(work.path("store/content/segments"), ".rfseg")};
            const auto receipt_before{
                read_bytes(work.path("store/mods/refrain.test.startup/"
                                     "1.0.0.json"))};
            const auto segments_before{directory_fingerprint(
                work.path("store/content/segments"), ".rfseg")};
            // Second run: the same archive is offered again and skipped; the
            // receipt and segment bytes must not change.
            expect(copy_package(work.path("a.rfmod"), inbox / "a.rfmod"));
            auto again{run_startup_content(options)};
            expect(static_cast<bool>(again));
            if (again) {
                expect(again->scanned == 1U);
                expect(again->installed == 0U);
                expect(again->skipped == 1U);
                expect(again->failed == 0U);
            }
            expect(file_count(work.path("store/content/segments"), ".rfseg") ==
                   segment_count);
            expect(directory_fingerprint(work.path("store/content/segments"),
                                         ".rfseg") == segments_before);
            const auto receipt_after{
                read_bytes(work.path("store/mods/refrain.test.startup/"
                                     "1.0.0.json"))};
            expect(receipt_before.has_value());
            expect(receipt_after.has_value());
            if (receipt_before && receipt_after) {
                expect(same_bytes(*receipt_before, *receipt_after));
            }
        };

    "startup content: an untrusted package is rejected and moved aside"_test =
        [] {
            temp_workspace work{"reject"};
            auto fixture{pack_fixture(work, "bad.rfmod", false)};
            expect(fixture.has_value());
            if (!fixture) {
                return;
            }
            const auto store_root{(work.path("store")).string()};
            const auto inbox{work.path("store/mods/inbox")};
            expect(copy_package(work.path("bad.rfmod"), inbox / "bad.rfmod"));

            auto options{make_options(store_root)};
            options.auto_install = true;
            options.require_signature = true;
            auto report{run_startup_content(options)};
            expect(static_cast<bool>(report));
            if (!report) {
                return;
            }
            expect(report->scanned == 1U);
            expect(report->installed == 0U);
            expect(report->failed == 1U);
            expect(!report->index_published);
            expect(report->receipt_files == 0U);
            expect(!fs::exists(inbox / "bad.rfmod"));
            expect(fs::exists(inbox / "rejected" / "bad.rfmod"));
            expect(!fs::exists(
                work.path("store/mods/refrain.test.startup/1.0.0.json")));
        };
};
