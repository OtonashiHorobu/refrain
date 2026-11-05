// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/io.hpp>
#include <refrain/data/cook.hpp>
#include <refrain/data/nbt.hpp>
#include <refrain/data/rfs.hpp>
#include <refrain/data/vfs.hpp>

#include <boost/ut.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>

namespace {
using namespace boost::ut;

namespace fs = std::filesystem;
namespace data = refrain::data;
namespace io = refrain::core::io;
namespace nbt = refrain::data::nbt;

class temp_directory {
  public:
    temp_directory() {
        static std::atomic<std::uint64_t> sequence{0};
        std::error_code error;
        fs::path base = fs::temp_directory_path(error);
        if (error) {
            base = fs::path{"."};
        }
        const auto stamp = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        base /=
            "refrain_cook_test_" + std::to_string(stamp) + "_" +
            std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
        fs::create_directories(base, error);
        path_ = base;
    }
    ~temp_directory() {
        std::error_code error;
        fs::remove_all(path_, error);
    }
    temp_directory(const temp_directory &) = delete;
    auto operator=(const temp_directory &) -> temp_directory & = delete;
    temp_directory(temp_directory &&) = delete;
    auto operator=(temp_directory &&) -> temp_directory & = delete;
    [[nodiscard]] auto path() const -> const fs::path & { return path_; }

  private:
    fs::path path_{};
};

suite<"data/cook"> cook_tests = [] {
    "data/cook packs a directory into a readable RFS container"_test = [] {
        temp_directory dir;
        fs::create_directories(dir.path() / "data/ns/function");
        expect(io::write_text((dir.path() / "pack.mcmeta").string(),
                              "{\"pack\":{\"pack_format\":41}}\n")
                   .has_value());
        const std::string body{"say hello\n"};
        expect(io::write_text(
                   (dir.path() / "data/ns/function/test.mcfunction").string(),
                   body)
                   .has_value());
        expect(io::write_text((dir.path() / "data/ns/test.json").string(),
                              "{ \"value\": 1 }\n")
                   .has_value());
        nbt::compound entries{};
        entries.emplace("value",
                        nbt::value{nbt::value::storage{std::int32_t{7}}});
        auto structure{nbt::write(nbt::make_compound(std::move(entries)), "",
                                  nbt::compression::gzip)};
        expect(structure.has_value());
        if (structure) {
            fs::create_directories(dir.path() / "data/ns/structure");
            expect(io::atomic_write_file(
                       (dir.path() / "data/ns/structure/test.nbt").string(),
                       *structure)
                       .has_value());
        }

        auto cooked{data::cook_directory_rfs(dir.path().string())};
        expect(cooked.has_value());
        if (!cooked) {
            return;
        }
        expect(cooked->files == 4U);
        auto reader{data::rfs_reader::open(cooked->bytes)};
        expect(reader.has_value());
        if (!reader) {
            return;
        }
        expect(reader->contains("pack.mcmeta"));
        expect(reader->contains("data/ns/test.json"));
        auto normalized_json{reader->read("data/ns/test.json")};
        expect(normalized_json.has_value());
        if (normalized_json) {
            const std::string normalized{
                reinterpret_cast<const char *>(normalized_json->data()),
                normalized_json->size()};
            expect(normalized == "{\"value\":1}");
        }
        auto nbt_bytes{reader->read("data/ns/structure/test.nbt")};
        expect(nbt_bytes.has_value());
        if (nbt_bytes) {
            auto root{nbt::read(*nbt_bytes, nbt::compression::gzip)};
            expect(root.has_value());
            if (root) {
                const auto *compound{root->as_compound()};
                expect(compound != nullptr);
                if (compound != nullptr) {
                    const auto found{compound->find("value")};
                    expect(found != compound->end());
                    if (found != compound->end()) {
                        expect(found->second.as_int() == 7);
                    }
                }
            }
        }
        expect(reader->contains("data/ns/function/test.mcfunction"));
        const auto rfs_path{(dir.path() / "pack.rfs").string()};
        expect(io::atomic_write_file(rfs_path, cooked->bytes).has_value());
        auto pack{data::open_rfs_pack(rfs_path)};
        expect(pack.has_value());
        if (pack) {
            expect((*pack)->format() == data::pack_format::rfs);
            expect((*pack)->file_count() == 4U);
            const auto *entry{(*pack)->find("pack.mcmeta")};
            expect(entry != nullptr);
            auto payload{(*pack)->read("data/ns/function/test.mcfunction")};
            expect(payload.has_value());
        }

        auto text{reader->read("data/ns/function/test.mcfunction")};
        expect(text.has_value());
        if (text) {
            expect(std::string{reinterpret_cast<const char *>(text->data()),
                               text->size()} == body);
        }
    };

    "data/cook rejects malformed JSON"_test = [] {
        temp_directory dir;
        expect(io::write_text((dir.path() / "bad.json").string(), "{oops")
                   .has_value());
        auto cooked{data::cook_directory_rfs(dir.path().string())};
        expect(!cooked.has_value());
        if (!cooked) {
            expect(cooked.error().code == refrain::error_code::parse_error);
        }
    };

    "data/cook rejects a missing root"_test = [] {
        expect(
            !data::cook_directory_rfs("/nonexistent/refrain-cook").has_value());
    };
};

} // namespace
