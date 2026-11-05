// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/io.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/content_index.hpp>
#include <refrain/data/content_store.hpp>
#include <refrain/data/path.hpp>
#include <refrain/mod/rfmod.hpp>
#include <refrain/platform/process.hpp>

#include <boost/ut.hpp>

#include <zstd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace boost::ut;

namespace mod = refrain::mod;

namespace fs = std::filesystem;
using refrain::data::content_store;
using refrain::mod::installed_file;
using refrain::mod::packed_rfmod;
using refrain::mod::rfmod_default_max_container_bytes;
using refrain::mod::rfmod_entry;
using refrain::mod::rfmod_input;
using refrain::mod::rfmod_install_options;
using refrain::mod::rfmod_install_result;
using refrain::mod::rfmod_read_options;
using refrain::mod::rfmod_reader;
using refrain::mod::rfmod_write_options;

// ---------------------------------------------------------------------------
// scratch directories
// ---------------------------------------------------------------------------

[[nodiscard]] auto make_workspace_path(std::string_view name) -> fs::path {
    static int counter{0};
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    const auto path{fs::temp_directory_path() /
                    ("refrain_rfmod_test_" + std::to_string(stamp) + "_" +
                     std::to_string(counter++) + "_" +
                     std::to_string(refrain::platform::process_id()) + "_" +
                     std::string{name})};
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
        return refrain::core::io::write_text(target.string(), content)
            .has_value();
    }

    [[nodiscard]] auto write_bytes(std::string_view relative,
                                   std::span<const std::byte> data) const
        -> bool {
        const auto target{path(relative)};
        std::error_code error;
        fs::create_directories(target.parent_path(), error);
        if (error) {
            return false;
        }
        return refrain::core::io::write_file(target.string(), data).has_value();
    }
};

// ---------------------------------------------------------------------------
// test data helpers
// ---------------------------------------------------------------------------

[[nodiscard]] auto bytes_of(std::string_view text) -> std::vector<std::byte> {
    return std::vector<std::byte>{
        reinterpret_cast<const std::byte *>(text.data()), // NOLINT
        reinterpret_cast<const std::byte *>(text.data()) + text.size()};
}

/// Owns the file payloads and hands the writer a stable span set.
struct file_set {
    std::vector<std::pair<std::string, std::vector<std::byte>>> files{};

    auto add(std::string path, std::string_view text) -> file_set & {
        files.emplace_back(std::move(path), bytes_of(text));
        return *this;
    }

    auto add_bytes(std::string path, std::vector<std::byte> data)
        -> file_set & {
        files.emplace_back(std::move(path), std::move(data));
        return *this;
    }

    [[nodiscard]] auto inputs() const -> std::vector<rfmod_input> {
        std::vector<rfmod_input> result;
        result.reserve(files.size());
        for (const auto &[path, data] : files) {
            result.push_back(rfmod_input{
                .path = path,
                .data = std::span<const std::byte>{data.data(), data.size()}});
        }
        return result;
    }
};

[[nodiscard]] auto build(const file_set &set, rfmod_write_options options = {})
    -> std::vector<std::byte> {
    const auto inputs{set.inputs()};
    auto archive{refrain::mod::build_rfmod(std::span<const rfmod_input>{inputs},
                                           options)};
    expect(static_cast<bool>(archive)) << "build_rfmod failed";
    return archive ? std::move(*archive) : std::vector<std::byte>{};
}

/// Byte-span equality (boost.ut cannot print std::vector<std::byte>).
[[nodiscard]] auto same_bytes(std::span<const std::byte> lhs,
                              std::span<const std::byte> rhs) -> bool {
    return lhs.size() == rhs.size() &&
           std::equal(lhs.begin(), lhs.end(), rhs.begin());
}

/// Digest comparison as hex text (boost.ut has no printer for digest32).
[[nodiscard]] auto hex(const refrain::data::digest32 &digest) -> std::string {
    return refrain::data::to_hex(digest);
}

[[nodiscard]] auto open_reader(std::span<const std::byte> archive,
                               rfmod_read_options options = {})
    -> refrain::result<rfmod_reader> {
    return rfmod_reader::open(archive, options);
}

[[nodiscard]] auto manifest_text(std::string_view id = "refrain.test.rfmod",
                                 std::string_view version = "1.2.3")
    -> std::string {
    return "[mod]\nid = \"" + std::string{id} + "\"\nversion = \"" +
           std::string{version} +
           "\"\nabi = \"1\"\nside = \"both\"\n\n[meta]\ndisplay_name = "
           "\"RFMOD test mod\"\n";
}

// --- Installed content-store assertions: segments and blobs.idx ---------

[[nodiscard]] auto installed_blob_table_path(const content_store &store)
    -> fs::path {
    return fs::path{store.root()} / "content" /
           std::string{refrain::data::content_blob_table_filename};
}

[[nodiscard]] auto load_installed_blob_table(const content_store &store)
    -> std::optional<refrain::data::blob_table_view> {
    auto bytes{refrain::core::io::read_file(
        installed_blob_table_path(store).string())};
    if (!bytes) {
        return std::nullopt;
    }
    auto parsed{refrain::data::parse_blob_table(*bytes)};
    if (!parsed) {
        return std::nullopt;
    }
    return std::optional<refrain::data::blob_table_view>{std::move(*parsed)};
}

[[nodiscard]] auto find_table_entry(const refrain::data::blob_table_view &table,
                                    const refrain::data::digest32 &digest)
    -> const refrain::data::blob_table_entry * {
    const auto found{
        std::find_if(table.entries.begin(), table.entries.end(),
                     [&digest](const refrain::data::blob_table_entry &entry) {
                         return entry.digest == digest;
                     })};
    return found == table.entries.end() ? nullptr : &*found;
}

[[nodiscard]] auto
read_installed_blob(const content_store &store,
                    const refrain::data::blob_table_view &table,
                    const refrain::data::digest32 &digest)
    -> refrain::result<std::vector<std::byte>> {
    const auto *entry{find_table_entry(table, digest)};
    if (entry == nullptr) {
        return std::unexpected{
            refrain::make_error(refrain::error_code::not_found,
                                "installed blob is missing from blobs.idx")};
    }
    return store.read(refrain::data::blob_location_from_table_entry(*entry));
}

struct segment_stats {
    std::size_t count{0};
    std::uint64_t bytes{0};
};

/// Count/size the sealed `content/segments/*.rfseg` files.
[[nodiscard]] auto segment_stats_for(const fs::path &store_root)
    -> segment_stats {
    segment_stats stats;
    std::error_code error;
    fs::directory_iterator iterator{store_root / "content" / "segments", error};
    const fs::directory_iterator end{};
    while (!error && iterator != end) {
        std::error_code entry_error;
        const auto &entry{*iterator};
        if (entry.path().extension() == ".rfseg" &&
            entry.is_regular_file(entry_error) && !entry_error) {
            ++stats.count;
            const auto size{entry.file_size(entry_error)};
            if (!entry_error) {
                stats.bytes += size;
            }
        }
        iterator.increment(error);
    }
    return stats;
}

/// Count regular files below `content/blobs` (the legacy per-blob layout).
[[nodiscard]] auto count_blob_files(const fs::path &store_root) -> std::size_t {
    std::size_t count{0};
    std::error_code error;
    fs::recursive_directory_iterator iterator{
        store_root / "content" / "blobs",
        fs::directory_options::skip_permission_denied, error};
    const fs::recursive_directory_iterator end{};
    while (!error && iterator != end) {
        std::error_code entry_error;
        if (iterator->is_regular_file(entry_error) && !entry_error) {
            ++count;
        }
        iterator.increment(error);
    }
    return count;
}

// --- zstd helpers (corrupt-archive construction) ---------------------------

[[nodiscard]] auto zstd_compress(std::span<const std::byte> data)
    -> std::vector<std::byte> {
    const auto bound{ZSTD_compressBound(data.size())};
    std::vector<std::byte> compressed(bound);
    const auto written{ZSTD_compress(compressed.data(), compressed.size(),
                                     data.data(), data.size(), 3)};
    expect(ZSTD_isError(written) == 0) << "test zstd compress failed";
    compressed.resize(ZSTD_isError(written) == 0 ? written : 0);
    return compressed;
}

[[nodiscard]] auto zstd_decompress(std::span<const std::byte> data)
    -> std::vector<std::byte> {
    const auto size{ZSTD_getFrameContentSize(data.data(), data.size())};
    expect(size != ZSTD_CONTENTSIZE_ERROR) << "test: bad frame size";
    expect(size != ZSTD_CONTENTSIZE_UNKNOWN) << "test: unknown frame size";
    if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN) {
        return {};
    }
    std::vector<std::byte> tar(static_cast<std::size_t>(size));
    const auto written{
        ZSTD_decompress(tar.data(), tar.size(), data.data(), data.size())};
    expect(ZSTD_isError(written) == 0) << "test zstd decompress failed";
    tar.resize(ZSTD_isError(written) == 0 ? written : 0);
    return tar;
}

void write_octal_field(std::vector<std::byte> &tar, std::size_t offset,
                       std::size_t digits, std::uint64_t value) {
    for (std::size_t index{0}; index < digits; ++index) {
        tar[offset + index] = std::byte{'0'};
    }
    std::size_t position{digits};
    while (value > 0 && position > 0) {
        --position;
        tar[offset + position] =
            static_cast<std::byte>('0' + static_cast<char>(value & 0x7U));
        value >>= 3U;
    }
}

/// Rewrite a header checksum after mutating a field (raw tar manipulation).
void fix_checksum(std::vector<std::byte> &tar, std::size_t header_offset) {
    constexpr std::size_t checksum_offset{148};
    constexpr std::size_t checksum_size{8};
    for (std::size_t index{0}; index < checksum_size; ++index) {
        tar[header_offset + checksum_offset + index] = std::byte{' '};
    }
    std::uint64_t sum{0};
    for (std::size_t index{0}; index < 512U; ++index) {
        sum += static_cast<std::uint64_t>(
            static_cast<unsigned char>(tar[header_offset + index]));
    }
    write_octal_field(tar, header_offset + checksum_offset, 6, sum);
    tar[header_offset + checksum_offset + 6] = std::byte{0};
    tar[header_offset + checksum_offset + 7] = std::byte{' '};
}

/// Valid single-file package decompressed back to a mutable tar.
[[nodiscard]] auto single_file_tar(std::string_view name = "a.txt",
                                   std::string_view content = "hello")
    -> std::vector<std::byte> {
    file_set set;
    set.add(std::string{name}, content);
    return zstd_decompress(build(set));
}

// --- synthetic tar construction (PAX/GNU framing for reader tests) --------

constexpr std::size_t synthetic_tar_block{512};

enum class tar_flavor { posix, gnu };

/// Append one 512-byte header. `name`/`linkname` are truncated to their ustar
/// field widths, so callers can deliberately emit a short placeholder for a
/// PAX/GNU long-name override to replace.
void append_tar_header(std::vector<std::byte> &tar, std::string_view name,
                       char typeflag, std::uint64_t size,
                       std::string_view linkname = {},
                       tar_flavor flavor = tar_flavor::posix) {
    const auto offset{tar.size()};
    tar.resize(offset + synthetic_tar_block, std::byte{0});
    const auto write_field = [&tar, offset](std::size_t field,
                                            std::string_view value,
                                            std::size_t width) {
        if (!value.empty()) {
            std::memcpy(tar.data() +
                            static_cast<std::ptrdiff_t>(offset + field),
                        value.data(), std::min(value.size(), width));
        }
    };
    write_field(0, name, 100);
    write_octal_field(tar, offset + 100, 7, 0644);
    write_octal_field(tar, offset + 108, 7, 0);
    write_octal_field(tar, offset + 116, 7, 0);
    write_octal_field(tar, offset + 124, 11, size);
    write_octal_field(tar, offset + 136, 11, 0);
    write_field(157, linkname, 100);
    tar[offset + 156] = static_cast<std::byte>(typeflag);
    if (flavor == tar_flavor::gnu) {
        write_field(257, std::string_view{"ustar  \0", 8}, 8);
    } else {
        write_field(257, std::string_view{"ustar\0", 6}, 6);
        write_field(263, std::string_view{"00", 2}, 2);
    }
    fix_checksum(tar, offset);
}

/// Append a member body and its 512-byte padding.
void append_tar_payload(std::vector<std::byte> &tar,
                        std::span<const std::byte> data) {
    tar.insert(tar.end(), data.begin(), data.end());
    const auto padding{
        (synthetic_tar_block - (data.size() % synthetic_tar_block)) %
        synthetic_tar_block};
    tar.insert(tar.end(), padding, std::byte{0});
}

void append_tar_text(std::vector<std::byte> &tar, std::string_view text) {
    const auto bytes{bytes_of(text)};
    append_tar_payload(tar, bytes);
}

void append_tar_file(std::vector<std::byte> &tar, std::string_view path,
                     std::string_view content,
                     tar_flavor flavor = tar_flavor::posix) {
    append_tar_header(tar, path, '0', content.size(), {}, flavor);
    append_tar_text(tar, content);
}

/// Append an old-GNU sparse header (type 'S'): one sparse extent of
/// `stored_bytes` at offset 0, but a logical file size of `logical_bytes`.
/// Reading such an entry expands the holes, which is why the reader rejects it.
void append_gnu_sparse_header(std::vector<std::byte> &tar,
                              std::string_view name, std::uint64_t stored_bytes,
                              std::uint64_t logical_bytes) {
    append_tar_header(tar, name, 'S', stored_bytes, {}, tar_flavor::gnu);
    const auto offset{tar.size() - synthetic_tar_block};
    write_octal_field(tar, offset + 386, 12, 0);             // sp[0].offset
    write_octal_field(tar, offset + 398, 12, stored_bytes);  // sp[0].numbytes
    tar[offset + 482] = std::byte{0};                        // isextended
    write_octal_field(tar, offset + 483, 12, logical_bytes); // realsize
    fix_checksum(tar, offset);
}

/// Two zero blocks, plus optional extra zero padding.
void finish_tar(std::vector<std::byte> &tar,
                std::size_t extra_zero_blocks = 0) {
    tar.insert(tar.end(), (2U + extra_zero_blocks) * synthetic_tar_block,
               std::byte{0});
}

/// One POSIX pax extended-header record: "<len> <key>=<value>\n", where <len>
/// counts the digits of <len> itself.
[[nodiscard]] auto pax_record(std::string_view key, std::string_view value)
    -> std::string {
    const std::string body{" " + std::string{key} + "=" + std::string{value} +
                           "\n"};
    std::size_t length{body.size() + 1U};
    for (;;) {
        const auto digits{std::to_string(length).size()};
        if (body.size() + digits == length) {
            break;
        }
        length = body.size() + digits;
    }
    return std::to_string(length) + body;
}

/// Append a type 'x' pax extended header carrying one path override. The
/// following regular-file header is expected to carry a truncated name.
void append_pax_path(std::vector<std::byte> &tar, std::string_view path) {
    const auto record{pax_record("path", path)};
    append_tar_header(tar, "PaxHeaders/member", 'x', record.size());
    append_tar_text(tar, record);
}

/// Append a GNU type 'L' long-name header (the next regular header's name is
/// expected to be truncated).
void append_gnu_long_path(std::vector<std::byte> &tar, std::string_view path) {
    const std::string payload{std::string{path} + '\0'};
    append_tar_header(tar, "././@LongLink", 'L', payload.size(), {},
                      tar_flavor::gnu);
    append_tar_text(tar, payload);
}

// ---------------------------------------------------------------------------
// writer / determinism
// ---------------------------------------------------------------------------

const suite rfmod_writer_tests = [] {
    "rfmod/build is deterministic and order independent"_test = [] {
        file_set first;
        first.add("manifest.toml", manifest_text())
            .add("data/refrain_test/recipe/a.json", "{}")
            .add("assets/lang/en_us.json", R"({"a":"b"})");
        const auto one{build(first)};

        file_set shuffled;
        shuffled.add("assets/lang/en_us.json", R"({"a":"b"})")
            .add("data/refrain_test/recipe/a.json", "{}")
            .add("manifest.toml", manifest_text());
        const auto two{build(shuffled)};

        file_set again;
        again.add("manifest.toml", manifest_text())
            .add("data/refrain_test/recipe/a.json", "{}")
            .add("assets/lang/en_us.json", R"({"a":"b"})");
        const auto three{build(again)};

        expect(!one.empty());
        expect(same_bytes(one, two))
            << "shuffled input changed the archive bytes";
        expect(same_bytes(one, three)) << "two builds differ";
    };

    "rfmod/build roundtrips entries and data"_test = [] {
        file_set set;
        set.add("manifest.toml", manifest_text())
            .add("data/a/b.json", "abc")
            .add("data/c/d.json", "")
            .add_bytes("assets/blob.bin",
                       std::vector<std::byte>(700, std::byte{7}));
        auto archive{build(set)};
        auto reader{open_reader(archive)};
        expect(static_cast<bool>(reader));
        if (!reader) {
            return;
        }
        expect(reader->file_count() == 4U);
        expect(reader->entries().size() == 4U);
        // Sorted by path bytes.
        expect(reader->entries().at(0).path == "assets/blob.bin");
        expect(reader->entries().at(1).path == "data/a/b.json");
        expect(reader->entries().at(2).path == "data/c/d.json");
        expect(reader->entries().at(3).path == "manifest.toml");
        auto blob{reader->read("assets/blob.bin")};
        expect(static_cast<bool>(blob));
        if (blob) {
            expect(blob->size() == 700U);
            expect(static_cast<int>((*blob)[0]) == 7);
        }
        auto empty{reader->read("data/c/d.json")};
        expect(static_cast<bool>(empty));
        if (empty) {
            expect(empty->empty());
        }
        expect(reader->find("manifest.toml") != nullptr);
        expect(reader->find("nope.txt") == nullptr);
        expect(!reader->read("nope.txt"));
        expect(hex(reader->archive_digest()) ==
               hex(refrain::data::blake3(archive)));
        expect(reader->decompressed_size() % 512U == 0U);
    };

    "rfmod/build an empty archive roundtrips"_test = [] {
        file_set set;
        const auto archive{build(set)};
        auto reader{open_reader(archive)};
        expect(static_cast<bool>(reader));
        if (reader) {
            expect(reader->file_count() == 0U);
            expect(reader->entries().empty());
        }
    };

    "rfmod/build rejects duplicate and unsafe paths"_test = [] {
        file_set duplicates;
        duplicates.add("data/a.json", "1").add("data/a.json", "2");
        expect(!refrain::mod::build_rfmod(duplicates.inputs()));

        const std::array<std::string_view, 6> unsafe{
            "", "../evil", "/etc/passwd", "a/../b", "a\\b", "data//x.json"};
        for (const auto name : unsafe) {
            file_set set;
            set.add(std::string{name}, "x");
            const auto result{refrain::mod::build_rfmod(set.inputs())};
            expect(!result)
                << "expected unsafe path '" << name << "' to be rejected";
        }
    };

    "rfmod/build accepts unicode paths and long paths"_test = [] {
        file_set set;
        set.add("data/refrain_test/lang/español.json", "unicode")
            .add("assets/ß/ü.txt", "umlaut")
            .add(std::string("data/") + std::string(120, 'a') + "/nested.json",
                 "long");
        auto reader{open_reader(build(set))};
        expect(static_cast<bool>(reader));
        if (!reader) {
            return;
        }
        expect(reader->file_count() == 3U);
        expect(reader->find("data/refrain_test/lang/español.json") != nullptr);
        expect(reader->find("assets/ß/ü.txt") != nullptr);
    };

    "rfmod/build rejects a path that cannot fit ustar fields"_test = [] {
        file_set set;
        // > 100 + 155 bytes: needs PAX, which this writer intentionally omits.
        set.add(std::string(300, 'a') + "/x.json", "x");
        expect(!refrain::mod::build_rfmod(set.inputs()));
    };

    "rfmod/write_rfmod writes atomically and returns the digest"_test = [] {
        temp_workspace work{"write"};
        file_set set;
        set.add("manifest.toml", manifest_text());
        const auto output{work.path("mod.rfmod")};
        auto digest{refrain::mod::write_rfmod(output.string(), set.inputs())};
        expect(static_cast<bool>(digest));
        if (!digest) {
            return;
        }
        auto bytes{refrain::core::io::read_file(output.string())};
        expect(static_cast<bool>(bytes));
        if (bytes) {
            expect(hex(*digest) == hex(refrain::data::blake3(*bytes)));
        }
        auto reader{rfmod_reader::open_file(output.string())};
        expect(static_cast<bool>(reader));
        if (reader) {
            expect(reader->file_count() == 1U);
        }
    };
};

// ---------------------------------------------------------------------------
// reader strictness / corrupt corpus
// ---------------------------------------------------------------------------

const suite rfmod_reader_rejection_tests = [] {
    "rfmod/reader rejects non-zstd input"_test = [] {
        const auto garbage{bytes_of("not a zstd archive")};
        expect(!open_reader(garbage));
        const std::vector<std::byte> empty{};
        expect(!open_reader(empty));
    };

    "rfmod/reader rejects a truncated zstd stream"_test = [] {
        file_set set;
        set.add("manifest.toml", manifest_text());
        auto archive{build(set)};
        archive.resize(archive.size() / 2U);
        expect(!open_reader(archive));
    };

    "rfmod/reader rejects a truncated tar stream"_test = [] {
        auto tar{single_file_tar()};
        expect(tar.size() >= 1024U);
        tar.resize(tar.size() - 1024U); // drop both end blocks
        expect(!open_reader(zstd_compress(tar)));
    };

    "rfmod/reader rejects bad tar checksum"_test = [] {
        auto tar{single_file_tar()};
        tar[0] = std::byte{'Z'}; // mutate name, leave stale checksum
        expect(!open_reader(zstd_compress(tar)));
    };

    "rfmod/reader rejects bad magic and version"_test = [] {
        auto bad_magic{single_file_tar()};
        bad_magic[257] = std::byte{'X'};
        fix_checksum(bad_magic, 0);
        expect(!open_reader(zstd_compress(bad_magic)));

        auto bad_version{single_file_tar()};
        bad_version[263] = std::byte{'9'};
        fix_checksum(bad_version, 0);
        expect(!open_reader(zstd_compress(bad_version)));
    };

    "rfmod/reader rejects trailing garbage after the end blocks"_test = [] {
        auto tar{single_file_tar()};
        for (std::size_t index{0}; index < 32U; ++index) {
            tar.push_back(std::byte{0xAA});
        }
        expect(!open_reader(zstd_compress(tar)));

        // Garbage followed by zero padding must not hide behind the final
        // zero-block check.
        auto padded{single_file_tar()};
        for (std::size_t index{0}; index < 32U; ++index) {
            padded.push_back(std::byte{0xAA});
        }
        padded.insert(padded.end(), 1024U, std::byte{0});
        expect(!open_reader(zstd_compress(padded)));
    };

    "rfmod/reader rejects one zero block instead of two"_test = [] {
        auto tar{single_file_tar()};
        tar.resize(tar.size() - 512U); // keep only one end block
        expect(!open_reader(zstd_compress(tar)));
    };

    "rfmod/reader rejects a zero-content member without end blocks"_test = [] {
        file_set set;
        set.add("zeros.bin", std::string(4096, '\0'));
        auto tar{zstd_decompress(build(set))};
        expect(tar.size() == 512U + 4096U + 1024U);
        // Dropping both end blocks leaves the member's NUL payload at the tail:
        // the final 1024 bytes are still zero, but they are data, not a marker.
        auto no_blocks{tar};
        no_blocks.resize(no_blocks.size() - 1024U);
        const auto rejected{open_reader(zstd_compress(no_blocks))};
        expect(!rejected) << "member NUL data masked the missing end blocks";
        if (!rejected) {
            expect(static_cast<int>(rejected.error().code) ==
                   static_cast<int>(refrain::error_code::parse_error));
            expect(rejected.error().message.find("end-of-archive") !=
                   std::string::npos);
        }
        // One end block is still not the required two zero blocks.
        auto one_block{tar};
        one_block.resize(one_block.size() - 512U);
        expect(!open_reader(zstd_compress(one_block)));
        // The intact archive (two end blocks, no padding) still opens.
        expect(static_cast<bool>(open_reader(zstd_compress(tar))));
    };

    "rfmod/reader enforces the summed member payload budget"_test = [] {
        // A header whose declared size is far larger than the whole tar stream
        // must be rejected by the member budget, not by allocating/reading it.
        std::vector<std::byte> tar;
        append_tar_header(tar, "huge.bin", '0', 100000);
        finish_tar(tar);
        const auto opened{
            open_reader(zstd_compress(tar), {.max_archive_bytes = 4096})};
        expect(!opened);
        if (!opened) {
            expect(static_cast<int>(opened.error().code) ==
                   static_cast<int>(refrain::error_code::parse_error));
            expect(opened.error().message.find("budget") != std::string::npos);
        }
    };

    "rfmod/reader rejects symlinks and skips directory entries"_test = [] {
        std::vector<std::byte> symlink;
        append_tar_header(symlink, "link", '2', 0, "target");
        finish_tar(symlink);
        const auto symlink_result{open_reader(zstd_compress(symlink))};
        expect(!symlink_result);
        if (!symlink_result) {
            expect(static_cast<int>(symlink_result.error().code) ==
                   static_cast<int>(refrain::error_code::parse_error));
        }

        // Directory entries are optional metadata: skipped, not an error.
        std::vector<std::byte> with_directory;
        append_tar_header(with_directory, "data/", '5', 0);
        append_tar_file(with_directory, "data/a.txt", "abc");
        finish_tar(with_directory);
        auto directory{open_reader(zstd_compress(with_directory))};
        expect(static_cast<bool>(directory));
        if (directory) {
            expect(directory->file_count() == 1U);
            expect(directory->find("data/a.txt") != nullptr);
            expect(directory->find("data/") == nullptr);
        }
    };

    "rfmod/reader validates directory member paths"_test = [] {
        // Directory entries are skipped, but their names must still pass the
        // same path rules as regular members.
        std::vector<std::byte> evil;
        append_tar_header(evil, "../evil", '5', 0);
        append_tar_file(evil, "data/a.txt", "abc");
        finish_tar(evil);
        const auto rejected{open_reader(zstd_compress(evil))};
        expect(!rejected);
        if (!rejected) {
            expect(static_cast<int>(rejected.error().code) ==
                   static_cast<int>(refrain::error_code::parse_error));
            expect(rejected.error().message.find("../evil") !=
                   std::string::npos);
        }

        // A conventional trailing '/' directory opens and stays out of
        // entries()/payloads.
        std::vector<std::byte> with_directory;
        append_tar_header(with_directory, "dir/", '5', 0);
        append_tar_file(with_directory, "dir/a.txt", "abc");
        finish_tar(with_directory);
        const auto directory{open_reader(zstd_compress(with_directory))};
        expect(static_cast<bool>(directory));
        if (directory) {
            expect(directory->file_count() == 1U);
            expect(directory->entries().size() == 1U);
            expect(directory->entries().front().path == "dir/a.txt");
            expect(directory->find("dir/") == nullptr);
        }
    };

    "rfmod/reader rejects absolute and parent-relative paths"_test = [] {
        auto absolute{single_file_tar()};
        std::memset(absolute.data(), 0, 100);
        std::memcpy(absolute.data(), "/etc/passwd", 11);
        fix_checksum(absolute, 0);
        expect(!open_reader(zstd_compress(absolute)));

        auto parent{single_file_tar()};
        std::memset(parent.data(), 0, 100);
        std::memcpy(parent.data(), "../evil", 7);
        fix_checksum(parent, 0);
        expect(!open_reader(zstd_compress(parent)));
    };

    "rfmod/reader rejects duplicate member paths"_test = [] {
        file_set set;
        set.add("a.txt", "a").add("b.txt", "b");
        auto tar{zstd_decompress(build(set))};
        // Second header name -> "a.txt", recompute its checksum.
        const std::size_t second_header{512U + 512U}; // a.txt data "a" + pad
        std::memset(tar.data() + second_header, 0, 100);
        std::memcpy(tar.data() + second_header, "a.txt", 5);
        fix_checksum(tar, second_header);
        expect(!open_reader(zstd_compress(tar)));
    };

    "rfmod/reader rejects oversized and truncated members"_test = [] {
        auto oversized{single_file_tar()};
        write_octal_field(oversized, 124, 11, 4096); // declared size
        oversized[135] = std::byte{0};
        fix_checksum(oversized, 0);
        expect(!open_reader(zstd_compress(oversized)));

        auto truncated{single_file_tar()};
        write_octal_field(truncated, 124, 11, 4096);
        truncated[135] = std::byte{0};
        fix_checksum(truncated, 0);
        auto limited{
            open_reader(zstd_compress(truncated), {.max_file_bytes = 16})};
        expect(!limited);
    };

    "rfmod/reader enforces archive/file/count limits"_test = [] {
        file_set small;
        small.add("a.txt", "0123456789");
        const auto archive{build(small)};
        expect(!open_reader(archive, {.max_archive_bytes = 512}));
        expect(!open_reader(archive, {.max_file_bytes = 4}));

        file_set two;
        two.add("a.txt", "a").add("b.txt", "b");
        expect(!open_reader(build(two), {.max_files = 1}));
    };

    "rfmod/reader enforces the compressed container limit"_test = [] {
        // A sparse file is cheap to create but would force a full-size read if
        // the container limit were only checked after `read_file`.
        temp_workspace workspace{"container_limit"};
        const auto oversized{workspace.path("oversized.rfmod")};
        expect(workspace.write("oversized.rfmod", ""));
        std::error_code resize_error;
        fs::resize_file(oversized, rfmod_default_max_container_bytes + 1U,
                        resize_error);
        expect(!resize_error) << "cannot create the sparse container";
        if (resize_error) {
            return;
        }
        const auto rejected{rfmod_reader::open_file(oversized.string())};
        expect(!rejected);
        if (!rejected) {
            expect(static_cast<int>(rejected.error().code) ==
                   static_cast<int>(refrain::error_code::invalid_argument));
            expect(rejected.error().message.find(
                       std::to_string(rfmod_default_max_container_bytes)) !=
                   std::string::npos);
        }

        // A normal small container still opens with the default limit.
        file_set small;
        small.add("a.txt", "hello");
        const auto archive{build(small)};
        expect(workspace.write_bytes("small.rfmod", archive));
        const auto opened{
            rfmod_reader::open_file(workspace.path("small.rfmod").string())};
        expect(static_cast<bool>(opened));
    };
};

// ---------------------------------------------------------------------------
// reader: PAX/GNU extensions and libarchive type mapping
// ---------------------------------------------------------------------------

const suite rfmod_reader_libarchive_tests = [] {
    "rfmod/reader accepts a PAX long path"_test = [] {
        const std::string long_path{"data/" + std::string(260, 'a') +
                                    "/pax.json"};
        expect(long_path.size() > 255U);
        expect(long_path.size() <= 1024U);

        std::vector<std::byte> tar;
        append_pax_path(tar, long_path);
        append_tar_header(tar, std::string_view{long_path}.substr(0, 100), '0',
                          5);
        append_tar_text(tar, "hello");
        finish_tar(tar);

        auto reader{open_reader(zstd_compress(tar))};
        expect(static_cast<bool>(reader));
        if (!reader) {
            return;
        }
        expect(reader->file_count() == 1U);
        expect(reader->find(long_path) != nullptr);
        expect(reader->find(long_path.substr(0, 100)) == nullptr);
        auto payload{reader->read(long_path)};
        expect(static_cast<bool>(payload));
        if (payload) {
            expect(same_bytes(*payload, bytes_of("hello")));
        }
    };

    "rfmod/reader accepts a GNU long path"_test = [] {
        const std::string long_path{"data/" + std::string(260, 'b') +
                                    "/gnu.json"};
        expect(long_path.size() > 255U);

        std::vector<std::byte> tar;
        append_gnu_long_path(tar, long_path);
        append_tar_header(tar, std::string_view{long_path}.substr(0, 15), '0',
                          5, {}, tar_flavor::gnu);
        append_tar_text(tar, "hello");
        finish_tar(tar);

        auto reader{open_reader(zstd_compress(tar))};
        expect(static_cast<bool>(reader));
        if (!reader) {
            return;
        }
        expect(reader->file_count() == 1U);
        expect(reader->find(long_path) != nullptr);
        auto payload{reader->read(long_path)};
        expect(static_cast<bool>(payload));
        if (payload) {
            expect(same_bytes(*payload, bytes_of("hello")));
        }
    };

    "rfmod/reader verifies and installs a PAX long-path package"_test = [] {
        temp_workspace work{"pax-package"};
        const std::string long_path{"data/" + std::string(260, 'c') +
                                    "/recipe.json"};
        const std::string content{R"({"type":"x"})"};
        expect(long_path.size() > 255U);
        expect(long_path.size() <= 1024U);

        std::string manifest{
            "[mod]\nid = \"refrain.test.pax\"\nversion = \"1.0.0\"\n"
            "abi = \"1\"\nside = \"both\"\n\n[meta]\n"
            "display_name = \"PAX package\"\n\n[files]\n"};
        manifest +=
            "\"" + long_path +
            "\" = { size = " + std::to_string(content.size()) +
            ", blake3 = \"" +
            refrain::data::to_hex(refrain::data::blake3(bytes_of(content))) +
            "\", mode = 0o644 }\n";
        manifest += "[packaging]\ntool = \"refrain-tools\"\ntool_version = "
                    "\"0.1.0\"\nzstd_level = 3\ndeterministic = true\n"
                    "format_version = 1\n";

        std::vector<std::byte> tar;
        append_pax_path(tar, long_path);
        append_tar_header(tar, std::string_view{long_path}.substr(0, 100), '0',
                          content.size());
        append_tar_text(tar, content);
        append_tar_file(tar, "manifest.toml", manifest);
        finish_tar(tar);

        auto reader{open_reader(zstd_compress(tar))};
        expect(static_cast<bool>(reader));
        if (!reader) {
            return;
        }
        expect(reader->file_count() == 2U);
        expect(reader->find(long_path) != nullptr);

        const auto verified{refrain::mod::verify_rfmod(*reader)};
        expect(static_cast<bool>(verified)) << "PAX package failed verify";
        if (verified) {
            expect(verified->files.size() == 1U);
            expect(verified->files.at(0).path == long_path);
        }

        auto store{content_store::open(work.path("cas").string())};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        auto installed{refrain::mod::install_rfmod(*reader, *store)};
        expect(static_cast<bool>(installed)) << "PAX package failed install";
        if (installed) {
            expect(installed->files.size() == 2U);
            expect(installed->stored_blobs == 2U);
            auto table{load_installed_blob_table(*store)};
            expect(static_cast<bool>(table));
            if (table) {
                const auto digest{refrain::data::blake3(bytes_of(content))};
                auto stored{read_installed_blob(*store, *table, digest)};
                expect(static_cast<bool>(stored));
                if (stored) {
                    expect(same_bytes(*stored, bytes_of(content)));
                }
            }
        }
    };

    "rfmod/reader accepts trailing zero padding after the end blocks"_test =
        [] {
            std::vector<std::byte> tar;
            append_tar_file(tar, "a.txt", "hi");
            finish_tar(tar, 4); // extra zero blocks are harmless padding
            expect(static_cast<bool>(open_reader(zstd_compress(tar))));
        };

    "rfmod/reader rejects links and special files"_test = [] {
        const std::array<std::pair<char, std::string>, 5> specials{
            std::pair{'1', std::string{"hard link"}},
            std::pair{'2', std::string{"symlink"}},
            std::pair{'3', std::string{"character device"}},
            std::pair{'4', std::string{"block device"}},
            std::pair{'6', std::string{"FIFO"}}};
        for (const auto &[typeflag, label] : specials) {
            std::vector<std::byte> tar;
            append_tar_header(tar, "entry", typeflag, 0, "target");
            finish_tar(tar);
            const auto opened{open_reader(zstd_compress(tar))};
            expect(!opened) << "expected " << label << " to be rejected";
            if (!opened) {
                expect(static_cast<int>(opened.error().code) ==
                       static_cast<int>(refrain::error_code::parse_error));
            }
        }
    };

    "rfmod/reader rejects backslash and control-byte paths"_test = [] {
        std::string control{"a"};
        control.push_back('\x01');
        control.push_back('b');
        const std::array<std::string, 2> unsafe{std::string{"a\\b"}, control};
        for (const auto &path : unsafe) {
            std::vector<std::byte> tar;
            append_tar_file(tar, path, "x");
            finish_tar(tar);
            expect(!open_reader(zstd_compress(tar)))
                << "expected unsafe path to be rejected";
        }
    };

    "rfmod/reader rejects non-UTF-8 member paths"_test = [] {
        std::string invalid{"data/hell"};
        invalid.push_back(static_cast<char>(0xFF));
        invalid += ".txt";
        // Build a package-shaped archive with the same raw key in [files], so
        // the rejection happens at the member-path check, not at TOML parsing.
        std::string manifest{
            "[mod]\nid = \"refrain.test.badutf8\"\nversion = \"1.0.0\"\n"
            "abi = \"1\"\nside = \"both\"\n\n[meta]\n"
            "display_name = \"Bad UTF-8\"\n\n[files]\n"};
        manifest +=
            "\"" + invalid + "\" = { size = 1, blake3 = \"" +
            refrain::data::to_hex(refrain::data::blake3(bytes_of("x"))) +
            "\", mode = 0o644 }\n";
        manifest += "[packaging]\ntool = \"refrain-tools\"\ntool_version = "
                    "\"0.1.0\"\nzstd_level = 3\ndeterministic = true\n"
                    "format_version = 1\n";
        std::vector<std::byte> tar;
        append_tar_file(tar, invalid, "x");
        append_tar_file(tar, "manifest.toml", manifest);
        finish_tar(tar);
        const auto rejected{open_reader(zstd_compress(tar))};
        expect(!rejected);
        if (!rejected) {
            expect(static_cast<int>(rejected.error().code) ==
                   static_cast<int>(refrain::error_code::parse_error));
            expect(rejected.error().message.find("UTF-8") != std::string::npos);
        }
    };

    "rfmod/reader verifies a package with UTF-8 paths"_test = [] {
        const std::string path{"data/h\xC3\xA9ll.txt"};
        const std::string content{"unicode"};
        std::string manifest{
            "[mod]\nid = \"refrain.test.utf8\"\nversion = \"1.0.0\"\n"
            "abi = \"1\"\nside = \"both\"\n\n[meta]\n"
            "display_name = \"UTF-8 package\"\n\n[files]\n"};
        manifest +=
            "\"" + path + "\" = { size = " + std::to_string(content.size()) +
            ", blake3 = \"" +
            refrain::data::to_hex(refrain::data::blake3(bytes_of(content))) +
            "\", mode = 0o644 }\n";
        manifest += "[packaging]\ntool = \"refrain-tools\"\ntool_version = "
                    "\"0.1.0\"\nzstd_level = 3\ndeterministic = true\n"
                    "format_version = 1\n";

        std::vector<std::byte> tar;
        append_tar_file(tar, path, content);
        append_tar_file(tar, "manifest.toml", manifest);
        finish_tar(tar);

        auto reader{open_reader(zstd_compress(tar))};
        expect(static_cast<bool>(reader));
        if (!reader) {
            return;
        }
        expect(reader->find(path) != nullptr);
        const auto verified{refrain::mod::verify_rfmod(*reader)};
        expect(static_cast<bool>(verified)) << "UTF-8 package failed verify";
        if (verified) {
            expect(verified->files.size() == 1U);
            expect(verified->files.at(0).path == path);
        }
    };

    "rfmod/reader rejects GNU sparse members"_test = [] {
        // Old-GNU sparse header: 512 stored bytes but a 1,000,000-byte logical
        // file. libarchive expands the hole while reading, so the reader must
        // reject the member before trusting its logical size.
        std::vector<std::byte> tar;
        append_gnu_sparse_header(tar, "sparse.bin", 512, 1000000);
        const std::vector<std::byte> stored(512, std::byte{'x'});
        append_tar_payload(tar, stored);
        finish_tar(tar);
        const auto rejected{open_reader(zstd_compress(tar))};
        expect(!rejected);
        if (!rejected) {
            expect(static_cast<int>(rejected.error().code) ==
                   static_cast<int>(refrain::error_code::parse_error));
            expect(rejected.error().message.find("sparse") !=
                   std::string::npos);
        }
    };

    "rfmod/reader rejects a sparse member without a stored extent"_test = [] {
        // Old-GNU sparse header with an empty map: libarchive reports no
        // sparse extents, so only the declared-size vs remaining-tar-bytes
        // check catches the 64 MiB logical size (the 117 MiB-RSS attack).
        std::vector<std::byte> tar;
        append_gnu_sparse_header(tar, "sparse-empty.bin", 0,
                                 64U * 1024U * 1024U);
        finish_tar(tar);
        const auto rejected{open_reader(zstd_compress(tar))};
        expect(!rejected);
        if (!rejected) {
            expect(static_cast<int>(rejected.error().code) ==
                   static_cast<int>(refrain::error_code::parse_error));
            expect(rejected.error().message.find("remaining archive bytes") !=
                   std::string::npos);
        }
    };

    "rfmod/reader maps a malformed PAX header to parse_error"_test = [] {
        std::vector<std::byte> tar;
        append_tar_header(tar, "PaxHeaders/member", 'x', 8);
        append_tar_text(tar, "not-pax\n");
        append_tar_file(tar, "a.txt", "hi");
        finish_tar(tar);
        const auto opened{open_reader(zstd_compress(tar))};
        expect(!opened);
        if (!opened) {
            expect(static_cast<int>(opened.error().code) ==
                   static_cast<int>(refrain::error_code::parse_error));
        }
    };
};

// ---------------------------------------------------------------------------
// pack_mod_directory
// ---------------------------------------------------------------------------

const suite rfmod_pack_tests = [] {
    "rfmod/pack_mod_directory ships content only"_test = [] {
        temp_workspace work{"pack"};
        const auto mod{work.path("testmod")};
        expect(work.write("testmod/manifest.toml", manifest_text()));
        expect(work.write("testmod/README.md", "# should not ship"));
        expect(work.write("testmod/LICENSE", "MIT"));
        expect(work.write("testmod/NOTICE", "notice"));
        expect(work.write("testmod/.env", "SECRET=1"));
        expect(work.write("testmod/src/main.cpp", "int main() {}"));
        expect(work.write("testmod/data/refrain_test/recipe/a.json", "{}"));
        expect(work.write("testmod/data/.hidden", "no"));
        expect(work.write("testmod/assets/lang/en_us.json", "{}"));
        const auto script{bytes_of("wasm-bytes")};
        expect(work.write_bytes("testmod/scripts/init.wasm", script));

        const auto output{work.path("out/testmod.rfmod")};
        std::error_code error;
        fs::create_directories(output.parent_path(), error);
        auto packed{
            refrain::mod::pack_mod_directory(mod.string(), output.string())};
        expect(static_cast<bool>(packed)) << "pack_mod_directory failed";
        if (!packed) {
            return;
        }
        expect(packed->file_count ==
               6U); // manifest, LICENSE, NOTICE, data, assets, scripts
        expect(hex(packed->archive_digest) ==
               hex(refrain::data::blake3(
                   *refrain::core::io::read_file(output.string()))));

        auto reader{rfmod_reader::open_file(output.string())};
        expect(static_cast<bool>(reader));
        if (!reader) {
            return;
        }
        const std::vector<std::string> expected{
            "assets/lang/en_us.json",
            "data/refrain_test/recipe/a.json",
            "LICENSE",
            "NOTICE",
            "manifest.toml",
            "scripts/init.wasm"};
        expect(reader->file_count() == expected.size());
        for (const auto &path : expected) {
            expect(reader->find(path) != nullptr) << "missing " << path;
        }
        expect(reader->find("README.md") == nullptr);
        expect(reader->find("src/main.cpp") == nullptr);
        expect(reader->find(".env") == nullptr);
        expect(reader->find("data/.hidden") == nullptr);
        auto script_back{reader->read("scripts/init.wasm")};
        expect(static_cast<bool>(script_back));
        if (script_back) {
            expect(same_bytes(*script_back, script));
        }

        // Packaging the same tree twice is byte-identical.
        const auto second{work.path("out/testmod2.rfmod")};
        auto repacked{
            refrain::mod::pack_mod_directory(mod.string(), second.string())};
        expect(static_cast<bool>(repacked));
        if (repacked) {
            expect(hex(repacked->archive_digest) ==
                   hex(packed->archive_digest));
        }
    };

    "rfmod/pack_mod_directory rejects non-UTF-8 file names"_test = [] {
        try {
            temp_workspace work{"pack-utf8"};
            expect(work.write("mod/manifest.toml", manifest_text()));
            std::string name{"mod/data/hell"};
            name.push_back(static_cast<char>(0xFF));
            name += ".txt";
            try {
                if (!work.write(name, "x")) {
                    return; // the host cannot represent a non-UTF-8 file name
                }
            } catch (const std::filesystem::filesystem_error &) {
                return; // Windows rejects the UTF-8 -> wide conversion
            }
            const auto output{work.path("out/mod.rfmod")};
            std::error_code error;
            fs::create_directories(output.parent_path(), error);
            const auto packed{refrain::mod::pack_mod_directory(
                work.path("mod").string(), output.string())};
            expect(!packed);
            if (!packed) {
                expect(static_cast<int>(packed.error().code) ==
                       static_cast<int>(refrain::error_code::invalid_argument));
                expect(packed.error().message.find("UTF-8") !=
                       std::string::npos);
            }
        } catch (const std::exception &) {
            // NTFS names are UTF-16 and cannot hold invalid UTF-8; the host
            // cannot represent this fixture.
        }
    };

    "rfmod/pack_mod_directory validates the manifest"_test = [] {
        temp_workspace work{"pack-manifest"};
        const auto output{work.path("out/mod.rfmod")};
        std::error_code error;
        fs::create_directories(output.parent_path(), error);

        // Missing directory / missing manifest.
        expect(!refrain::mod::pack_mod_directory(work.path("absent").string(),
                                                 output.string()));
        std::error_code dir_error;
        fs::create_directories(work.path("empty"), dir_error);
        expect(!refrain::mod::pack_mod_directory(work.path("empty").string(),
                                                 output.string()));

        // Invalid manifest (not SemVer / not a mod manifest).
        expect(work.write("bad/manifest.toml",
                          "[mod]\nid = \"x\"\nversion = \"1.0\"\nabi = "
                          "\"1\"\n"));
        expect(!refrain::mod::pack_mod_directory(work.path("bad").string(),
                                                 output.string()));
        expect(work.write("bad/manifest.toml", "not toml at all"));
        expect(!refrain::mod::pack_mod_directory(work.path("bad").string(),
                                                 output.string()));
    };
};

// ---------------------------------------------------------------------------
// CAS install
// ---------------------------------------------------------------------------

const suite rfmod_install_tests = [] {
    "rfmod/install stores every packaged member in the CAS"_test = [] {
        temp_workspace work{"install"};
        expect(work.write("mod/manifest.toml", manifest_text()));
        expect(work.write("mod/data/refrain_test/recipe/a.json",
                          R"({"type":"x"})"));
        expect(work.write("mod/assets/blob.bin", "blob"));
        const auto output{work.path("out/mod.rfmod")};
        std::error_code error;
        fs::create_directories(output.parent_path(), error);
        auto packed{refrain::mod::pack_mod_directory(work.path("mod").string(),
                                                     output.string())};
        expect(static_cast<bool>(packed));
        if (!packed) {
            return;
        }
        auto reader{rfmod_reader::open_file(output.string())};
        expect(static_cast<bool>(reader));
        if (!reader) {
            return;
        }
        auto store{content_store::open(work.path("cas").string())};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        auto installed{refrain::mod::install_rfmod(*reader, *store)};
        expect(static_cast<bool>(installed)) << "install failed";
        if (!installed) {
            return;
        }
        expect(hex(installed->archive_digest) == hex(reader->archive_digest()));
        expect(installed->files.size() == reader->file_count());
        expect(installed->stored_blobs == reader->file_count());
        // Fresh CAS: every member is a new blob.
        expect(installed->new_blobs == reader->file_count());
        expect(installed->signed_package == false);
        expect(installed->trusted == false);
        expect(installed->key_id.empty());
        auto manifest_bytes{reader->read("manifest.toml")};
        expect(static_cast<bool>(manifest_bytes));
        if (manifest_bytes) {
            expect(hex(installed->manifest_digest) ==
                   hex(refrain::data::blake3(*manifest_bytes)));
        }

        // The install publishes one blob table and appends the small members to
        // content segments instead of per-file CAS blobs.
        auto table{load_installed_blob_table(*store)};
        expect(static_cast<bool>(table)) << "blobs.idx was not published";
        if (!table) {
            return;
        }
        expect(table->entries.size() == installed->files.size());
        for (const auto &file : installed->files) {
            expect(file.mode == 0644U);
            const auto *entry{find_table_entry(*table, file.digest)};
            expect(entry != nullptr)
                << "missing table record for " << file.path;
            if (entry == nullptr) {
                continue;
            }
            expect((entry->flags & refrain::data::blob_flags_in_segment) != 0U)
                << file.path << " should live in a segment";
            expect(!fs::exists(store->path_for(file.digest)))
                << "small member stored as a standalone blob: " << file.path;
            auto stored{store->read(
                refrain::data::blob_location_from_table_entry(*entry))};
            expect(static_cast<bool>(stored));
            if (stored) {
                auto archived{reader->read(file.path)};
                expect(static_cast<bool>(archived));
                if (archived) {
                    expect(same_bytes(*stored, *archived));
                }
            }
        }
        const auto segments_before{segment_stats_for(work.path("cas"))};
        expect(segments_before.count >= 1U) << "no content segment published";
        expect(segments_before.bytes > 0U);
        expect(count_blob_files(work.path("cas")) == 0U)
            << "small members must not get standalone blob files";
        expect(installed->manifest.common.id == "refrain.test.rfmod");
        expect(installed->manifest.common.version == "1.2.3");
        expect(installed->manifest.common.abi == 1U);

        // Re-install dedupes: same digests, no new blobs and no new segment
        // bytes (the published table is the dedupe set).
        auto reinstalled{refrain::mod::install_rfmod(*reader, *store)};
        expect(static_cast<bool>(reinstalled));
        if (reinstalled) {
            expect(reinstalled->new_blobs == 0U);
            expect(reinstalled->stored_blobs == installed->stored_blobs);
            expect(reinstalled->files.size() == installed->files.size());
            for (std::size_t index{0}; index < installed->files.size();
                 ++index) {
                expect(hex(reinstalled->files.at(index).digest) ==
                       hex(installed->files.at(index).digest));
                const auto *entry{find_table_entry(
                    *table, reinstalled->files.at(index).digest)};
                expect(entry != nullptr);
                if (entry != nullptr) {
                    auto stored{store->read(
                        refrain::data::blob_location_from_table_entry(*entry))};
                    expect(static_cast<bool>(stored));
                }
            }
            const auto segments_after{segment_stats_for(work.path("cas"))};
            expect(segments_after.count == segments_before.count)
                << "reinstall appended a segment";
            expect(segments_after.bytes == segments_before.bytes)
                << "reinstall appended segment bytes";
        }
    };

    "rfmod/install keeps packed members as standalone blob files"_test = [] {
        temp_workspace work{"install-packed"};
        expect(work.write("mod/manifest.toml", manifest_text()));
        const std::string big(64U * 1024U + 1U, 'z');
        expect(work.write("mod/assets/big.bin", big));
        const auto output{work.path("out/mod.rfmod")};
        std::error_code error;
        fs::create_directories(output.parent_path(), error);
        auto packed{refrain::mod::pack_mod_directory(work.path("mod").string(),
                                                     output.string())};
        expect(static_cast<bool>(packed));
        if (!packed) {
            return;
        }
        auto reader{rfmod_reader::open_file(output.string())};
        expect(static_cast<bool>(reader));
        if (!reader) {
            return;
        }
        auto store{content_store::open(work.path("cas").string())};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        auto installed{refrain::mod::install_rfmod(*reader, *store)};
        expect(static_cast<bool>(installed));
        if (!installed) {
            return;
        }
        expect(installed->files.size() == 2U);
        expect(installed->new_blobs == 2U);

        auto table{load_installed_blob_table(*store)};
        expect(static_cast<bool>(table));
        if (!table) {
            return;
        }
        const auto big_digest{refrain::data::blake3(bytes_of(big))};
        const auto *big_entry{find_table_entry(*table, big_digest)};
        expect(big_entry != nullptr);
        if (big_entry != nullptr) {
            expect((big_entry->flags & refrain::data::blob_flags_in_segment) ==
                   0U)
                << "a packed member must stay file-backed";
            expect((big_entry->flags & refrain::data::blob_flags_packed) != 0U);
            expect(big_entry->block_count == 2U);
            expect(fs::is_regular_file(store->path_for(big_digest)));
            auto stored{store->read(
                refrain::data::blob_location_from_table_entry(*big_entry))};
            expect(static_cast<bool>(stored));
            if (stored) {
                expect(same_bytes(*stored, bytes_of(big)));
            }
        }
        const auto *manifest_entry{
            find_table_entry(*table, installed->manifest_digest)};
        expect(manifest_entry != nullptr);
        if (manifest_entry != nullptr) {
            expect((manifest_entry->flags &
                    refrain::data::blob_flags_in_segment) != 0U);
            expect(!fs::exists(store->path_for(installed->manifest_digest)));
        }
    };

    "rfmod/install fails before writing when blobs.idx is damaged"_test = [] {
        temp_workspace work{"install-damaged-table"};
        expect(work.write("mod/manifest.toml", manifest_text()));
        expect(work.write("mod/data/a.json", "{}"));
        const auto output{work.path("out/mod.rfmod")};
        std::error_code error;
        fs::create_directories(output.parent_path(), error);
        auto packed{refrain::mod::pack_mod_directory(work.path("mod").string(),
                                                     output.string())};
        expect(static_cast<bool>(packed));
        if (!packed) {
            return;
        }
        auto reader{rfmod_reader::open_file(output.string())};
        expect(static_cast<bool>(reader));
        if (!reader) {
            return;
        }
        auto store{content_store::open(work.path("cas").string())};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        expect(work.write_bytes("cas/content/blobs.idx",
                                bytes_of("not a blob table")));
        const auto damaged{refrain::mod::install_rfmod(*reader, *store)};
        expect(!damaged);
        if (damaged) {
            return;
        }
        expect(static_cast<int>(damaged.error().code) ==
               static_cast<int>(refrain::error_code::parse_error));
        expect(damaged.error().message.find("blobs.idx") != std::string::npos);

        // A foreign table version is rejected the same way, still before any
        // segment/blob write.
        auto empty_table{refrain::data::build_blob_table(
            std::span<const refrain::data::blob_table_entry>{})};
        expect(static_cast<bool>(empty_table));
        if (empty_table) {
            auto foreign{*empty_table};
            foreign[4] = std::byte{2}; // RFBT version is the second u16 (LE)
            expect(work.write_bytes("cas/content/blobs.idx", foreign));
            const auto versioned{refrain::mod::install_rfmod(*reader, *store)};
            expect(!versioned);
            if (versioned) {
                return;
            }
            expect(static_cast<int>(versioned.error().code) ==
                   static_cast<int>(refrain::error_code::version_mismatch));
        }

        expect(count_blob_files(work.path("cas")) == 0U)
            << "a damaged blobs.idx must not publish blobs";
        expect(segment_stats_for(work.path("cas")).count == 0U)
            << "a damaged blobs.idx must not publish segments";
        expect(!fs::exists(work.path("cas/mods")))
            << "a damaged blobs.idx must not write a receipt";
    };

    "rfmod/install re-encodes existing resolution indexes"_test = [] {
        // A merged store already has blobs.idx + RFRX indexes bound to its
        // BLAKE3 prefix. Installing a mod grows that table, so the install must
        // re-encode every index (new prefix + shifted blob_index positions)
        // instead of leaving them fail-closed.
        temp_workspace work{"install-existing-index"};
        auto store{content_store::open(work.path("cas").string())};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        const auto existing_bytes{bytes_of("existing blob")};
        auto existing_digest{store->put(existing_bytes)};
        expect(static_cast<bool>(existing_digest));
        if (!existing_digest) {
            return;
        }

        std::vector<refrain::data::blob_table_entry> entries;
        refrain::data::blob_table_entry existing_entry{};
        existing_entry.digest = *existing_digest;
        existing_entry.logical_size = existing_bytes.size();
        existing_entry.stored_size =
            static_cast<std::uint32_t>(existing_bytes.size());
        existing_entry.refcount = 1;
        existing_entry.kind = refrain::data::blob_kind_content;
        entries.push_back(existing_entry);
        auto table_bytes{refrain::data::build_blob_table(entries)};
        expect(static_cast<bool>(table_bytes));
        if (!table_bytes) {
            return;
        }
        auto old_table{refrain::data::parse_blob_table(*table_bytes)};
        expect(static_cast<bool>(old_table));
        if (!old_table) {
            return;
        }
        const std::vector<refrain::data::resolution_input> inputs{
            refrain::data::resolution_input{
                .location = "data/existing.txt",
                .source = "fixture",
                .priority = 10,
                .blob = *existing_digest,
                .flags = 0,
            }};
        auto index_bytes{
            refrain::data::build_resolution_index(inputs, *old_table)};
        expect(static_cast<bool>(index_bytes));
        if (!index_bytes) {
            return;
        }
        expect(refrain::core::io::atomic_write_file(
                   installed_blob_table_path(*store).string(), *table_bytes)
                   .has_value());
        const auto index_path{work.path("cas/content/resolutions/fixture.idx")};
        std::error_code error;
        fs::create_directories(index_path.parent_path(), error);
        expect(!error);
        expect(refrain::core::io::atomic_write_file(index_path.string(),
                                                    *index_bytes)
                   .has_value());

        expect(work.write("mod/manifest.toml", manifest_text()));
        expect(work.write("mod/data/a.json", "{}"));
        const auto output{work.path("out/mod.rfmod")};
        fs::create_directories(output.parent_path(), error);
        auto packed{refrain::mod::pack_mod_directory(work.path("mod").string(),
                                                     output.string())};
        expect(static_cast<bool>(packed));
        if (!packed) {
            return;
        }
        auto reader{rfmod_reader::open_file(output.string())};
        expect(static_cast<bool>(reader));
        if (!reader) {
            return;
        }
        auto installed{refrain::mod::install_rfmod(*reader, *store)};
        expect(static_cast<bool>(installed));
        if (!installed) {
            return;
        }
        auto next_table{load_installed_blob_table(*store)};
        expect(static_cast<bool>(next_table));
        if (!next_table) {
            return;
        }
        expect(next_table->entries.size() == entries.size() + 2U);
        auto reindexed{refrain::core::io::read_file(index_path.string())};
        expect(static_cast<bool>(reindexed));
        if (!reindexed) {
            return;
        }
        auto parsed_index{
            refrain::data::parse_resolution_index(*reindexed, *next_table)};
        expect(static_cast<bool>(parsed_index))
            << "install left the existing RFRX bound to the old blobs.idx";
        if (!parsed_index) {
            return;
        }
        auto resolved{
            refrain::data::resolve(*parsed_index, "data/existing.txt")};
        expect(static_cast<bool>(resolved));
        if (resolved) {
            auto blob{refrain::data::resolved_blob(*resolved, *next_table)};
            expect(static_cast<bool>(blob));
            if (blob) {
                expect(hex(*blob) == hex(*existing_digest));
            }
        }
        // Reinstall still dedupes against the merged table.
        auto reinstalled{refrain::mod::install_rfmod(*reader, *store)};
        expect(static_cast<bool>(reinstalled));
        if (reinstalled) {
            expect(reinstalled->new_blobs == 0U);
        }
    };

    "rfmod/install rejects archives without a manifest"_test = [] {
        temp_workspace work{"install-nomanifest"};
        file_set set;
        set.add("data/a.json", "{}");
        auto reader{open_reader(build(set))};
        expect(static_cast<bool>(reader));
        if (!reader) {
            return;
        }
        auto store{content_store::open(work.path("cas").string())};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        const auto result{refrain::mod::install_rfmod(*reader, *store)};
        expect(!result);
        expect(static_cast<int>(result.error().code) ==
               static_cast<int>(refrain::error_code::not_found));
    };

    "rfmod/verify rejects a bare container without [files]"_test = [] {
        // build_rfmod is a pure tar.zst container: no [files]/[packaging], so
        // the package verifier must reject it instead of installing it.
        file_set set;
        set.add("manifest.toml", manifest_text()).add("data/a.json", "{}");
        auto reader{open_reader(build(set))};
        expect(static_cast<bool>(reader));
        if (!reader) {
            return;
        }
        const auto verified{refrain::mod::verify_rfmod(*reader)};
        expect(!verified);
        expect(static_cast<int>(verified.error().code) ==
               static_cast<int>(refrain::error_code::parse_error));
        expect(verified.error().message.find("[files]") != std::string::npos);
    };

    "rfmod/install rejects an inert store and an unsigned package under require_signature"_test =
        [] {
            temp_workspace work{"install-signature"};
            expect(work.write("mod/manifest.toml", manifest_text()));
            expect(work.write("mod/data/a.json", "{}"));
            const auto output{work.path("out/mod.rfmod")};
            std::error_code error;
            fs::create_directories(output.parent_path(), error);
            auto packed{refrain::mod::pack_mod_directory(
                work.path("mod").string(), output.string())};
            expect(static_cast<bool>(packed));
            if (!packed) {
                return;
            }
            auto reader{rfmod_reader::open_file(output.string())};
            expect(static_cast<bool>(reader));
            if (!reader) {
                return;
            }
            content_store inert;
            const auto inert_result{
                refrain::mod::install_rfmod(*reader, inert)};
            expect(!inert_result);
            expect(static_cast<int>(inert_result.error().code) ==
                   static_cast<int>(refrain::error_code::not_initialized));

            auto store{content_store::open(work.path("cas").string())};
            expect(static_cast<bool>(store));
            if (!store) {
                return;
            }
            const auto result{refrain::mod::install_rfmod(
                *reader, *store,
                rfmod_install_options{.require_signature = true})};
            expect(!result);
            expect(static_cast<int>(result.error().code) ==
                   static_cast<int>(refrain::error_code::not_found));
            // The rejected package published nothing.
            expect(!store->contains(
                refrain::data::blake3(*reader->read("manifest.toml"))));
        };

    "rfmod/pack -> open_file -> install end to end"_test = [] {
        temp_workspace work{"e2e"};
        expect(work.write("mod/manifest.toml", manifest_text()));
        expect(work.write("mod/data/refrain_test/recipe/a.json", "{}"));
        expect(work.write("mod/assets/lang/en_us.json", R"({"a":"b"})"));
        expect(work.write("mod/src/private.cpp", "internal"));
        const auto output{work.path("out/mod.rfmod")};
        std::error_code error;
        fs::create_directories(output.parent_path(), error);
        auto packed{refrain::mod::pack_mod_directory(work.path("mod").string(),
                                                     output.string())};
        expect(static_cast<bool>(packed));
        if (!packed) {
            return;
        }
        auto reader{rfmod_reader::open_file(output.string())};
        expect(static_cast<bool>(reader));
        if (!reader) {
            return;
        }
        expect(reader->file_count() == 3U);
        auto store{content_store::open(work.path("cas").string())};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        auto installed{refrain::mod::install_rfmod(*reader, *store)};
        expect(static_cast<bool>(installed));
        if (installed) {
            expect(installed->manifest.common.id == "refrain.test.rfmod");
            expect(installed->stored_blobs == 3U);
            expect(installed->files.size() == 3U);
        }
    };
};

} // namespace
