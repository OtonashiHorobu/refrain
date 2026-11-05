// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/io.hpp>
#include <refrain/data/builtin_image.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/path.hpp>
#include <refrain/data/vfs.hpp>

#include <boost/ut.hpp>

#include <zip.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

// Explicit using-declarations instead of `using namespace boost::ut;`: the
// test framework defines a type named `override`, which would collide with
// the virt-specifier in the pack subclass below.
using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;
namespace fs = std::filesystem;

using refrain::data::build_builtin_image;
using refrain::data::builtin_image_input;
using refrain::data::filename;
using refrain::data::is_safe_relative_path;
using refrain::data::is_valid_utf8;
using refrain::data::join_path;
using refrain::data::open_builtin_image_pack;
using refrain::data::open_dir_pack;
using refrain::data::open_zip_pack;
using refrain::data::pack;
using refrain::data::pack_file_info;
using refrain::data::pack_format;
using refrain::data::pack_source;
using refrain::data::parent_path;
using refrain::data::vfs;
using refrain::data::vfs_max_files;

[[nodiscard]] auto bytes_of(std::string_view text) -> std::vector<std::byte> {
    const auto bytes{
        std::as_bytes(std::span<const char>{text.data(), text.size()})};
    return std::vector<std::byte>{bytes.begin(), bytes.end()};
}

[[nodiscard]] auto text_of(std::span<const std::byte> bytes)
    -> std::string_view {
    return std::string_view{
        reinterpret_cast<const char *>(bytes.data()), // NOLINT
        bytes.size()};
}

[[nodiscard]] auto error_code_of(const refrain::status &value) -> int {
    return value.has_value() ? -1 : static_cast<int>(value.error().code);
}

template <typename Value>
[[nodiscard]] auto error_code_of(const refrain::result<Value> &value) -> int {
    return value.has_value() ? -1 : static_cast<int>(value.error().code);
}

[[nodiscard]] auto make_temp_directory() -> fs::path {
    static int counter{0};
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    const auto path{fs::temp_directory_path() /
                    ("refrain_vfs_test_" + std::to_string(stamp) + "_" +
                     std::to_string(counter++))};
    std::error_code error;
    fs::create_directories(path, error);
    return path;
}

/// Removes the scratch directory when the test returns (including early exits
/// on failed preconditions).
struct temp_directory {
    fs::path path{};

    explicit temp_directory(fs::path location) : path{std::move(location)} {}
    temp_directory(const temp_directory &) = delete;
    auto operator=(const temp_directory &) -> temp_directory & = delete;
    ~temp_directory() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

[[nodiscard]] auto write_scratch_file(const fs::path &root,
                                      std::string_view relative,
                                      std::string_view content) -> bool {
    const auto target{root / fs::path{std::string{relative}}};
    std::error_code error;
    fs::create_directories(target.parent_path(), error);
    if (error) {
        return false;
    }
    return refrain::core::io::write_text(target.string(), content).has_value();
}

struct archive_file {
    std::string name;
    std::string content;
    bool directory{false};
};

/// Create a synthetic zip (tests never ship real Minecraft jars/assets).
[[nodiscard]] auto make_zip(const fs::path &path,
                            const std::vector<archive_file> &files) -> bool {
    int zip_error_value{0};
    auto *archive{zip_open(path.string().c_str(), ZIP_CREATE | ZIP_TRUNCATE,
                           &zip_error_value)};
    if (archive == nullptr) {
        return false;
    }
    for (const auto &file : files) {
        if (file.directory) {
            if (zip_dir_add(archive, file.name.c_str(), ZIP_FL_ENC_UTF_8) < 0) {
                zip_discard(archive);
                return false;
            }
            continue;
        }
        // freep = 0: the source references `file.content`, which outlives
        // zip_close() because `files` is alive in the caller.
        auto *source{zip_source_buffer(archive, file.content.data(),
                                       file.content.size(), 0)};
        if (source == nullptr) {
            zip_discard(archive);
            return false;
        }
        if (zip_file_add(archive, file.name.c_str(), source,
                         ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE) < 0) {
            zip_source_free(source);
            zip_discard(archive);
            return false;
        }
    }
    return zip_close(archive) == 0;
}

/// Minimal in-test pack used to check that mount() validates arbitrary pack
/// implementations (real packs validate their own container).
class fake_pack final : public pack {
  public:
    fake_pack(std::string label, pack_format format,
              std::vector<pack_file_info> entries)
        : label_{std::move(label)}, format_{format},
          entries_{std::move(entries)} {}

    [[nodiscard]] auto format() const noexcept -> pack_format override {
        return format_;
    }
    [[nodiscard]] auto label() const noexcept -> std::string_view override {
        return label_;
    }
    [[nodiscard]] auto file_count() const noexcept -> std::size_t override {
        return entries_.size();
    }
    [[nodiscard]] auto entries() const noexcept
        -> std::span<const pack_file_info> override {
        return entries_;
    }
    [[nodiscard]] auto find(std::string_view path) const noexcept
        -> const pack_file_info * override {
        const auto match{std::find_if(entries_.begin(), entries_.end(),
                                      [path](const pack_file_info &entry) {
                                          return entry.path == path;
                                      })};
        return match == entries_.end() ? nullptr : &*match;
    }
    [[nodiscard]] auto read(std::string_view path) const
        -> refrain::result<std::vector<std::byte>> override {
        return bytes_of("fake:" + std::string{path});
    }

  private:
    std::string label_;
    pack_format format_;
    std::vector<pack_file_info> entries_;
};

const suite vfs_tests = [] {
    "vfs/path helpers join, split and normalize"_test = [] {
        expect(join_path("", "data") == "data");
        expect(join_path("data", "") == "data");
        expect(join_path("", "") == "");
        expect(join_path("data", "a.json") == "data/a.json");
        expect(join_path("data/", "a.json") == "data/a.json");
        expect(join_path("data/sub", "a/b.json") == "data/sub/a/b.json");

        expect(parent_path("data/a.json") == "data");
        expect(parent_path("a.json").empty());
        expect(parent_path("").empty());
        expect(parent_path("data/sub/a.json") == "data/sub");

        expect(filename("data/a.json") == "a.json");
        expect(filename("a.json") == "a.json");
        expect(filename("").empty());
        expect(filename("data/sub/").empty());
    };

    "vfs/path safety predicate rejects traversal"_test = [] {
        const std::array<std::string_view, 8> valid{
            "data/a.json",
            "data/file name with spaces.txt",
            "data/unicode-\u00e4\u00f6\u00fc.txt",
            "a/b/c/d",
            "pack.mcmeta",
            "assets/minecraft/lang/en_us.json",
            "data/leading-dot.file",
            "data/..hidden"};
        for (const auto path : valid) {
            expect(is_safe_relative_path(path)) << path;
        }
        const std::array<std::string_view, 12> unsafe{"",
                                                      "/etc/passwd",
                                                      "../escape",
                                                      "data/../../x",
                                                      "data/./a",
                                                      "data//a",
                                                      "data/",
                                                      "data\\a",
                                                      "data/a\x01b",
                                                      "data/a\x7f",
                                                      "..",
                                                      "."};
        for (const auto path : unsafe) {
            expect(!is_safe_relative_path(path)) << path;
        }
    };

    "vfs/UTF-8 predicate enforces RFC 3629"_test = [] {
        const std::array<std::string_view, 10> valid{
            "",
            "plain-ascii.txt",
            "data/h\xC3\xA9ll/\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E.txt",
            "\xC2\x80",         // U+0080, smallest 2-byte form
            "\xDF\xBF",         // U+07FF, largest 2-byte form
            "\xE0\xA0\x80",     // U+0800, smallest 3-byte form
            "\xEF\xBF\xBF",     // U+FFFF, largest 3-byte form
            "\xF0\x90\x80\x80", // U+10000, smallest 4-byte form
            "\xF4\x8F\xBF\xBF", // U+10FFFF, largest scalar value
            "assets/\xE2\x82\xAC.json"};
        for (const auto text : valid) {
            expect(is_valid_utf8(text)) << "expected valid UTF-8";
        }

        const std::array<std::string_view, 13> invalid{
            "\x80",             // stray continuation byte
            "\xBF",             // stray continuation byte
            "\xC0\x80",         // overlong U+0000
            "\xC1\xBF",         // overlong U+007F
            "\xE0\x80\x80",     // overlong 3-byte form
            "\xED\xA0\x80",     // UTF-16 surrogate U+D800
            "\xED\xBF\xBF",     // UTF-16 surrogate U+DFFF
            "\xF0\x80\x80\x80", // overlong 4-byte form
            "\xF4\x90\x80\x80", // above U+10FFFF
            "\xF5\x80\x80\x80", // invalid lead byte
            "\xFF",             // invalid lead byte
            "\xE2\x82",         // truncated 3-byte sequence
            "\xC3\x28"};        // invalid continuation byte
        for (const auto text : invalid) {
            expect(!is_valid_utf8(text)) << "expected invalid UTF-8";
        }
    };

    "vfs/dir + zip + memory packs merge with last-mounted priority"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto dir_root{scratch.path / "datapack"};
        expect(write_scratch_file(dir_root, "data/a.json", "dir-a"));
        expect(write_scratch_file(dir_root, "data/shared.json", "dir-shared"));
        expect(write_scratch_file(dir_root, "data/sub/x.json", "dir-x"));
        expect(write_scratch_file(dir_root, "pack.mcmeta", "{\"pack\":{}}"));

        const auto zip_path{scratch.path / "resources.zip"};
        expect(make_zip(zip_path,
                        {{.name = "data/b.json", .content = "zip-b"},
                         {.name = "data/shared.json", .content = "zip-shared"},
                         {.name = "assets/", .content = "", .directory = true},
                         {.name = "assets/minecraft/lang/en_us.json",
                          .content = "zip-lang"}}));

        std::vector<std::string> storage_paths{"data/c.json",
                                               "data/shared.json"};
        std::vector<std::vector<std::byte>> storage_bytes{
            bytes_of("mem-c"), bytes_of("mem-shared")};
        std::vector<builtin_image_input> memory_inputs{};
        for (std::size_t index{0}; index < storage_paths.size(); ++index) {
            memory_inputs.push_back(builtin_image_input{
                .path = storage_paths[index], .data = storage_bytes[index]});
        }
        const auto memory_image{build_builtin_image(memory_inputs)};
        expect(static_cast<bool>(memory_image));
        if (!memory_image) {
            return;
        }
        auto memory_pack{
            open_builtin_image_pack(*memory_image, "builtin-overlay")};
        expect(static_cast<bool>(memory_pack));
        if (!memory_pack) {
            return;
        }

        vfs stack;
        expect(!stack.is_frozen());
        expect(stack.pack_count() == 0U);
        expect(stack
                   .mount(pack_source{.path = dir_root.string(),
                                      .format = pack_format::directory,
                                      .label = "world-datapack"})
                   .has_value());
        expect(stack
                   .mount(pack_source{.path = zip_path.string(),
                                      .format = pack_format::zip,
                                      .label = "vanilla-zip"})
                   .has_value());
        expect(stack.mount(std::move(*memory_pack)).has_value());

        // Mounts are visible before freeze (mutable assembly phase); freeze
        // seals the snapshot for thread-safe reads.
        expect(stack.pack_count() == 3U);
        expect(stack.file_count() == 7U);
        stack.freeze();
        expect(stack.is_frozen());

        const auto mounted{stack.mounted()};
        expect(mounted.size() == 3U);
        if (mounted.size() == 3U) {
            expect(mounted[0].label == "world-datapack");
            expect(mounted[0].format == pack_format::directory);
            expect(mounted[0].file_count == 4U);
            expect(mounted[1].label == "vanilla-zip");
            expect(mounted[1].format == pack_format::zip);
            expect(mounted[1].file_count == 3U);
            expect(mounted[2].label == "builtin-overlay");
            expect(mounted[2].format == pack_format::memory);
            expect(mounted[2].file_count == 2U);
        }

        // Layer enumeration exposes the physical stack (needed for datapack tag
        // merging: entries hidden by a higher-priority pack must stay
        // reachable through the layer list).
        const auto layers{stack.packs()};
        expect(layers.size() == 3U);
        if (layers.size() == 3U) {
            expect(layers[0]->label() == "world-datapack");
            expect(layers[1]->label() == "vanilla-zip");
            expect(layers[2]->label() == "builtin-overlay");
            expect(layers[2] == stack.find("data/shared.json"));
        }

        // Resolution: highest-priority pack wins on a shared path.
        expect(stack.find("data/a.json") != nullptr);
        if (const auto *winner{stack.find("data/a.json")}) {
            expect(winner->label() == "world-datapack");
            expect(winner->format() == pack_format::directory);
        }
        expect(stack.find("data/b.json") != nullptr);
        if (const auto *winner{stack.find("data/b.json")}) {
            expect(winner->label() == "vanilla-zip");
        }
        expect(stack.find("data/shared.json") != nullptr);
        if (const auto *winner{stack.find("data/shared.json")}) {
            expect(winner->label() == "builtin-overlay");
            expect(winner->format() == pack_format::memory);
        }
        expect(stack.find("data/missing.json") == nullptr);

        expect(stack.exists("data/shared.json"));
        expect(!stack.exists("data/missing.json"));

        const auto dir_content{stack.read("data/a.json")};
        expect(static_cast<bool>(dir_content));
        if (dir_content) {
            expect(text_of(*dir_content) == "dir-a");
        }
        const auto zip_content{stack.read("data/b.json")};
        expect(static_cast<bool>(zip_content));
        if (zip_content) {
            expect(text_of(*zip_content) == "zip-b");
        }
        const auto memory_content{stack.read("data/c.json")};
        expect(static_cast<bool>(memory_content));
        if (memory_content) {
            expect(text_of(*memory_content) == "mem-c");
        }
        const auto overridden{stack.read("data/shared.json")};
        expect(static_cast<bool>(overridden));
        if (overridden) {
            expect(text_of(*overridden) == "mem-shared");
        }
        expect(stack.read_text("data/sub/x.json").value_or("") == "dir-x");

        const auto shared_size{stack.size("data/shared.json")};
        expect(static_cast<bool>(shared_size));
        if (shared_size) {
            expect(*shared_size == std::string_view{"mem-shared"}.size());
        }
        expect(!stack.size("data/missing.json"));
        expect(stack.size("data/missing.json").error().code ==
               refrain::error_code::not_found);
        expect(!stack.read("data/missing.json"));
        expect(stack.read("data/missing.json").error().code ==
               refrain::error_code::not_found);
        expect(!stack.read_text("data/missing.json"));

        // Merged listing is sorted, de-duplicated and reports winner sizes.
        const auto &merged{stack.list()};
        expect(merged.size() == 7U);
        expect(std::is_sorted(
            merged.begin(), merged.end(),
            [](const pack_file_info &lhs, const pack_file_info &rhs) {
                return lhs.path < rhs.path;
            }));
        if (merged.size() == 7U) {
            expect(merged[0].path == "assets/minecraft/lang/en_us.json");
            expect(merged[1].path == "data/a.json");
            expect(merged.back().path == "pack.mcmeta");
            for (const auto &entry : merged) {
                if (entry.path == "data/shared.json") {
                    expect(entry.size == std::string_view{"mem-shared"}.size());
                }
            }
        }

        const auto data_entries{stack.list_prefix("data/")};
        expect(data_entries.size() == 5U);
        const auto sub_entries{stack.list_prefix("data/sub/")};
        expect(sub_entries.size() == 1U);
        if (sub_entries.size() == 1U) {
            expect(sub_entries[0].path == "data/sub/x.json");
        }
        expect(stack.list_prefix("missing/").empty());

        const auto data_children{stack.list_directory("data")};
        expect(data_children ==
               (std::vector<std::string>{"a.json", "b.json", "c.json",
                                         "shared.json", "sub/"}));
        expect(stack.list_directory("data/") == data_children);
        expect(stack.list_directory("") ==
               (std::vector<std::string>{"assets/", "data/", "pack.mcmeta"}));
        expect(stack.list_directory("data/sub") ==
               (std::vector<std::string>{"x.json"}));
        expect(stack.list_directory("missing").empty());
        expect(stack.list_directory("/absolute").empty());
        expect(stack.list_directory("data/../data").empty());
    };

    "vfs/copies share the frozen snapshot"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{scratch.path / "data"};
        expect(write_scratch_file(root, "data/a.json", "a"));
        auto opened{open_dir_pack(root.string(), "shared")};
        expect(static_cast<bool>(opened));
        if (!opened) {
            return;
        }

        vfs original;
        expect(original.mount(std::move(*opened)).has_value());
        const vfs copy{original};
        expect(copy.file_count() == 1U);
        expect(copy.read_text("data/a.json").value_or("") == "a");

        // Freezing a copy freezes the shared snapshot...
        const_cast<vfs &>(copy).freeze();
        expect(original.is_frozen());

        // ...and a second copy sees the same mounted packs/reads.
        const vfs third{original};
        expect(third.pack_count() == 1U);
        expect(third.exists("data/a.json"));
        expect(!original
                    .mount(std::make_unique<fake_pack>(
                        "late", pack_format::directory,
                        std::vector<pack_file_info>{
                            {.path = "data/z.json", .size = 1}}))
                    .has_value());
    };

    "vfs/open errors surface as error values"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{scratch.path / "empty-root"};
        std::error_code error;
        fs::create_directories(root, error);

        expect(error_code_of(open_dir_pack("")) ==
               static_cast<int>(refrain::error_code::invalid_argument));
        expect(
            error_code_of(open_dir_pack((scratch.path / "missing").string())) ==
            static_cast<int>(refrain::error_code::not_found));
        const auto regular_file{scratch.path / "plain.txt"};
        expect(refrain::core::io::write_text(regular_file.string(), "x")
                   .has_value());
        expect(error_code_of(open_dir_pack(regular_file.string())) ==
               static_cast<int>(refrain::error_code::invalid_argument));

        expect(error_code_of(open_zip_pack("")) ==
               static_cast<int>(refrain::error_code::invalid_argument));
        expect(error_code_of(
                   open_zip_pack((scratch.path / "missing.zip").string())) ==
               static_cast<int>(refrain::error_code::io_error));
        const auto not_a_zip{scratch.path / "not-a.zip"};
        expect(refrain::core::io::write_text(not_a_zip.string(), "not a zip")
                   .has_value());
        expect(!open_zip_pack(not_a_zip.string()));

        const auto bogus_bytes{bytes_of("this is not an RFSI image")};
        expect(!open_builtin_image_pack(bogus_bytes, "bogus"));
        expect(open_builtin_image_pack(bogus_bytes, "bogus").error().code ==
               refrain::error_code::parse_error);

        // A valid RFSI image truncated by one byte is rejected, and a failed
        // mount leaves the VFS untouched.
        const std::array<builtin_image_input, 1> inputs{
            builtin_image_input{.path = "data/a.json", .data = bytes_of("{}")}};
        const auto image{build_builtin_image(inputs)};
        expect(static_cast<bool>(image));
        if (!image) {
            return;
        }
        auto truncated{*image};
        truncated.pop_back();
        vfs stack;
        expect(!open_builtin_image_pack(truncated, "truncated"));
        expect(stack.mount(pack_source{.path = "", .format = pack_format::zip})
                   .has_value() == false);
        expect(stack.pack_count() == 0U);
        expect(stack.file_count() == 0U);
    };

    "vfs/mount rejects invalid packs and post-freeze mounts"_test = [] {
        temp_directory scratch{make_temp_directory()};
        vfs stack;
        // Executable directory with only a subdirectory: zero regular files.
        const auto empty_root{scratch.path / "empty"};
        std::error_code error;
        fs::create_directories(empty_root / "data", error);
        auto empty_dir{open_dir_pack(empty_root.string())};
        expect(static_cast<bool>(empty_dir));
        if (empty_dir) {
            expect(error_code_of(stack.mount(std::move(*empty_dir))) ==
                   static_cast<int>(refrain::error_code::invalid_argument));
        }
        expect(error_code_of(stack.mount(nullptr)) ==
               static_cast<int>(refrain::error_code::invalid_argument));

        // Unsorted / duplicate / unsafe custom packs are rejected defensively.
        expect(error_code_of(stack.mount(std::make_unique<fake_pack>(
                   "unsorted", pack_format::directory,
                   std::vector<pack_file_info>{
                       {.path = "data/b.json", .size = 1},
                       {.path = "data/a.json", .size = 1}}))) ==
               static_cast<int>(refrain::error_code::invalid_argument));
        expect(error_code_of(stack.mount(std::make_unique<fake_pack>(
                   "duplicate", pack_format::directory,
                   std::vector<pack_file_info>{
                       {.path = "data/a.json", .size = 1},
                       {.path = "data/a.json", .size = 2}}))) ==
               static_cast<int>(refrain::error_code::invalid_argument));
        expect(error_code_of(stack.mount(std::make_unique<fake_pack>(
                   "unsafe", pack_format::directory,
                   std::vector<pack_file_info>{
                       {.path = "../escape.json", .size = 1}}))) ==
               static_cast<int>(refrain::error_code::invalid_argument));
        expect(stack.pack_count() == 0U);

        // A valid custom pack mounts and resolves.
        expect(stack
                   .mount(std::make_unique<fake_pack>(
                       "custom", pack_format::memory,
                       std::vector<pack_file_info>{
                           {.path = "data/z.json", .size = 3}}))
                   .has_value());
        expect(stack.file_count() == 1U);
        expect(stack.read_text("data/z.json").value_or("") ==
               "fake:data/z.json");
        stack.freeze();
        expect(error_code_of(stack.mount(std::make_unique<fake_pack>(
                   "frozen", pack_format::directory,
                   std::vector<pack_file_info>{
                       {.path = "data/y.json", .size = 1}}))) ==
               static_cast<int>(refrain::error_code::invalid_argument));
        // Memory-format pack sources need explicit bytes.
        expect(error_code_of(stack.mount(pack_source{
                   .path = "builtin.image", .format = pack_format::memory})) ==
               static_cast<int>(refrain::error_code::invalid_argument));
        expect(stack.pack_count() == 1U);
    };

    "vfs/corrupt zip and image content surfaces errors"_test = [] {
        temp_directory scratch{make_temp_directory()};

        // Zip-slip style names are rejected at open time (never extracted).
        const auto unsafe_zip{scratch.path / "unsafe.zip"};
        expect(make_zip(unsafe_zip,
                        {{.name = "../escape.txt", .content = "boom"}}));
        expect(!open_zip_pack(unsafe_zip.string()));
        expect(open_zip_pack(unsafe_zip.string()).error().code ==
               refrain::error_code::parse_error);

        // Portable-name violation: Windows device name in the archive.
        const auto device_zip{scratch.path / "device.zip"};
        expect(make_zip(device_zip, {{.name = "data/CON", .content = "boom"}}));
        expect(!open_zip_pack(device_zip.string()));

        // Empty zip: openable container, but nothing to mount.
        const auto empty_zip{scratch.path / "empty.zip"};
        expect(make_zip(empty_zip,
                        {{.name = "data/", .content = "", .directory = true}}));
        auto empty_pack{open_zip_pack(empty_zip.string())};
        expect(static_cast<bool>(empty_pack));
        if (empty_pack) {
            expect((*empty_pack)->file_count() == 0U);
            vfs stack;
            expect(error_code_of(stack.mount(std::move(*empty_pack))) ==
                   static_cast<int>(refrain::error_code::invalid_argument));
        }

        // Corrupt RFSI images surface parse errors rather than crashes.
        const std::array<builtin_image_input, 1> inputs{
            builtin_image_input{.path = "data/a.json", .data = bytes_of("{}")}};
        const auto image{build_builtin_image(inputs)};
        expect(static_cast<bool>(image));
        if (!image) {
            return;
        }
        auto broken{*image};
        broken[0] = std::byte{'X'};
        expect(!open_builtin_image_pack(broken, "broken"));
        expect(open_builtin_image_pack(broken, "broken").error().code ==
               refrain::error_code::parse_error);

        vfs stack;
        expect(stack
                   .mount(pack_source{.path = "broken.image",
                                      .format = pack_format::memory})
                   .has_value() == false);
        expect(stack.pack_count() == 0U);
    };

    "vfs/unicode and space paths are preserved verbatim"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{scratch.path / "unicode-pack"};
        expect(write_scratch_file(root, "data/file name with spaces.txt",
                                  "spaces"));
        expect(write_scratch_file(root, "data/unicode-\u00e4\u00f6\u00fc.json",
                                  "unicode"));

        auto opened{open_dir_pack(root.string())};
        expect(static_cast<bool>(opened));
        if (!opened) {
            return;
        }
        vfs stack;
        expect(stack.mount(std::move(*opened)).has_value());
        stack.freeze();

        expect(stack.exists("data/file name with spaces.txt"));
        expect(stack.read_text("data/file name with spaces.txt").value_or("") ==
               "spaces");
        expect(stack.exists("data/unicode-\u00e4\u00f6\u00fc.json"));
        expect(stack.read_text("data/unicode-\u00e4\u00f6\u00fc.json")
                   .value_or("") == "unicode");
        expect(stack.list_directory("data") ==
               (std::vector<std::string>{"file name with spaces.txt",
                                         "unicode-\u00e4\u00f6\u00fc.json"}));
        expect(!stack.exists("data/unicode-ao.json"));
    };

    "vfs/directory packs skip symlinks and surface deleted files"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{scratch.path / "symlink-pack"};
        expect(write_scratch_file(root, "data/real.json", "real"));

        std::error_code link_error;
        fs::create_symlink(root / "data/real.json", root / "data/alias.json",
                           link_error);
        const bool symlink_created{!link_error};
        std::error_code dir_link_error;
        fs::create_directory_symlink(root / "data", root / "data/link-dir",
                                     dir_link_error);

        auto opened{open_dir_pack(root.string())};
        expect(static_cast<bool>(opened));
        if (!opened) {
            return;
        }
        if (symlink_created) {
            // Symlinks are invisible to the snapshot: they could escape root.
            expect((*opened)->file_count() == 1U);
            expect((*opened)->find("data/alias.json") == nullptr);
        }
        expect((*opened)->find("data/real.json") != nullptr);

        vfs stack;
        expect(stack.mount(std::move(*opened)).has_value());
        stack.freeze();
        expect(stack.read_text("data/real.json").value_or("") == "real");

        // The pack captured the entry at open(); a deleted host file surfaces
        // as an error value instead of a crash or stale bytes.
        std::error_code remove_error;
        fs::remove(root / "data/real.json", remove_error);
        expect(!remove_error) << "failed to remove fixture file";
        const auto missing{stack.read("data/real.json")};
        expect(!missing);
        if (!missing) {
            expect(missing.error().code == refrain::error_code::io_error);
        }
    };

    "vfs/empty stack behaves like an empty merged view"_test = [] {
        vfs stack;
        stack.freeze();
        expect(stack.is_frozen());
        expect(stack.pack_count() == 0U);
        expect(stack.file_count() == 0U);
        expect(stack.mounted().empty());
        expect(stack.list().empty());
        expect(stack.list_prefix("data/").empty());
        expect(stack.list_directory("").empty());
        expect(stack.find("data/a.json") == nullptr);
        expect(!stack.exists("data/a.json"));
        expect(!stack.size("data/a.json"));
        expect(stack.size("data/a.json").error().code ==
               refrain::error_code::not_found);
        expect(!stack.read("data/a.json"));
        expect(stack.read("data/a.json").error().code ==
               refrain::error_code::not_found);
        expect(!stack.read_text("data/a.json"));
        expect(vfs_max_files >= 1024U * 1024U);
    };
};

} // namespace
