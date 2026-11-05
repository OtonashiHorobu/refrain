// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/checksum.hpp>
#include <refrain/world/rwf.hpp>

#include <boost/ut.hpp>

#include <fmt/format.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace boost::ut;
namespace rwf = refrain::world::rwf;
namespace world = refrain::world;
namespace fs = std::filesystem;

using rwf::blob_store;
using rwf::compression;
using rwf::region_file;
using rwf::region_options;
using world::chunk_pos;
using world::chunk_status;

class temp_directory {
  public:
    temp_directory() {
        static std::atomic<std::uint64_t> sequence{0};
        std::error_code error;
        const auto base{fs::temp_directory_path(error)};
        if (error) {
            return;
        }
        for (int attempt{0}; attempt < 16; ++attempt) {
            const auto candidate{
                base / ("refrain-rwf-" + std::to_string(sequence.fetch_add(
                                             1, std::memory_order_relaxed)))};
            if (fs::create_directories(candidate, error)) {
                path_ = candidate;
                return;
            }
            if (error && error != std::errc::file_exists) {
                return;
            }
            error.clear();
        }
    }
    temp_directory(const temp_directory &) = delete;
    auto operator=(const temp_directory &) -> temp_directory & = delete;
    ~temp_directory() {
        if (!path_.empty()) {
            std::error_code error;
            fs::remove_all(path_, error);
        }
    }

    [[nodiscard]] auto path() const -> const fs::path & { return path_; }
    [[nodiscard]] auto file(std::string_view name) const -> std::string {
        return (path_ / std::string{name}).string();
    }

  private:
    fs::path path_;
};

[[nodiscard]] auto to_bytes(std::string_view text) -> std::vector<std::byte> {
    std::vector<std::byte> out(text.size());
    std::memcpy(out.data(), text.data(), text.size());
    return out;
}

[[nodiscard]] auto read_whole(const std::string &path) -> std::string {
    std::ifstream stream{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{stream},
                       std::istreambuf_iterator<char>{}};
}

void write_whole(const std::string &path, std::string_view data) {
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    stream.write(data.data(), static_cast<std::streamsize>(data.size()));
}

void flip_byte(const std::string &path, std::size_t offset) {
    auto data{read_whole(path)};
    expect(offset < data.size());
    if (offset < data.size()) {
        data[offset] = static_cast<char>(data[offset] ^ 0x5A);
        write_whole(path, data);
    }
}

} // namespace

suite rwf_containers = [] {
    "region_roundtrip"_test = [] {
        const temp_directory directory;
        const auto path{directory.file("r.0.0.rfw")};
        auto region{region_file::create(path, {.x = 0, .z = 0})};
        expect(static_cast<bool>(region));
        expect(region->stats()->chunks == 0U);
        const auto payload{to_bytes("hello chunk")};
        const chunk_pos pos{.x = 3, .z = 7};
        expect(
            static_cast<bool>(region->write(pos, chunk_status::full, payload)));
        expect(region->contains(pos));
        auto in_memory{region->read(pos)};
        expect(static_cast<bool>(in_memory));
        expect(static_cast<bool>(*in_memory == payload));
        expect(static_cast<bool>(region->commit()));
        expect(static_cast<bool>(region->close()));

        auto reopened{region_file::load(path, {.x = 0, .z = 0})};
        expect(static_cast<bool>(reopened));
        if (!reopened) {
            return;
        }
        auto stored{reopened->read(pos)};
        expect(static_cast<bool>(stored));
        expect(static_cast<bool>(*stored == payload));
        expect(reopened->stats()->chunks == 1U);
        expect(reopened->stats()->garbage_bytes == 0U);
    };

    "region_rejects_wrong_region_and_oversize"_test = [] {
        const temp_directory directory;
        const auto path{directory.file("r.0.0.rfw")};
        auto region{region_file::create(path, {.x = 0, .z = 0})};
        expect(static_cast<bool>(region));
        const auto payload{to_bytes("x")};
        const auto outside{region->write(chunk_pos{.x = 32, .z = 0},
                                         chunk_status::full, payload)};
        expect(!outside);
        expect(outside.error().code == refrain::error_code::invalid_argument);
        const std::vector<std::byte> huge(rwf::max_record_bytes + 1,
                                          std::byte{0});
        const auto oversized{
            region->write(chunk_pos{.x = 0, .z = 0}, chunk_status::full, huge)};
        expect(!oversized);
        expect(oversized.error().code == refrain::error_code::invalid_argument);
    };

    "region_slot_mapping"_test = [] {
        expect(region_file::slot_for(chunk_pos{.x = 0, .z = 0}) == 0U);
        expect(region_file::slot_for(chunk_pos{.x = 1, .z = 0}) == 1U);
        expect(region_file::slot_for(chunk_pos{.x = 0, .z = 1}) == 32U);
        expect(region_file::slot_for(chunk_pos{.x = 31, .z = 31}) == 1023U);
        expect(region_file::slot_for(chunk_pos{.x = -1, .z = -1}) == 1023U);
        expect(region_file::slot_for(chunk_pos{.x = -32, .z = -1}) == 992U);
    };

    "region_latest_write_wins"_test = [] {
        const temp_directory directory;
        const auto path{directory.file("r.0.0.rfw")};
        auto region{region_file::create(path, {.x = 0, .z = 0})};
        expect(static_cast<bool>(region));
        const chunk_pos pos{.x = 0, .z = 0};
        for (int version{1}; version <= 3; ++version) {
            const auto payload{to_bytes("payload " + std::to_string(version))};
            expect(static_cast<bool>(
                region->write(pos, chunk_status::full, payload)));
        }
        expect(static_cast<bool>(region->commit()));
        auto reopened{region_file::load(path, {.x = 0, .z = 0})};
        expect(static_cast<bool>(reopened));
        if (!reopened) {
            return;
        }
        const auto stored{reopened->read(pos)};
        expect(static_cast<bool>(stored));
        expect(*stored == to_bytes("payload 3"));
        expect(reopened->stats()->garbage_bytes > 0U);
    };

    "region_compresses_repetitive_payload"_test = [] {
        const temp_directory directory;
        const auto path{directory.file("r.0.0.rfw")};
        auto region{region_file::create(path, {.x = 0, .z = 0})};
        expect(static_cast<bool>(region));
        const std::vector<std::byte> payload(64 * 1024, std::byte{0x41});
        const chunk_pos pos{.x = 1, .z = 1};
        expect(static_cast<bool>(
            region->write(pos, chunk_status::spawn, payload)));
        expect(region->entries()[region_file::slot_for(pos)].codec ==
               compression::zstd);
        expect(static_cast<bool>(region->commit()));
        const auto stats{region->stats()};
        expect(static_cast<bool>(stats));
        expect(stats->live_bytes < payload.size() / 4);
        const auto stored{region->read(pos)};
        expect(static_cast<bool>(stored));
        expect(static_cast<bool>(*stored == payload));
    };

    "region_per_record_zstd_levels_roundtrip"_test = [] {
        const temp_directory directory;
        const auto path{directory.file("r.0.0.rfw")};
        {
            auto created{region_file::create(path, {.x = 0, .z = 0})};
            expect(static_cast<bool>(created));
            if (!created) {
                return;
            }
            expect(static_cast<bool>(created->close()));
        }
        // Large enough that zstd shrinks it at every level, so the entry's
        // stored codec exercises the compressed-frame path for 1/3/19.
        std::string text(48 * 1024, 'a');
        for (std::size_t index{0}; index < text.size(); index += 97) {
            text[index] = static_cast<char>('A' + (index % 26U));
        }
        const auto payload{to_bytes(text)};
        const chunk_pos pos{.x = 5, .z = 9};
        for (const int level : {1, 3, 19}) {
            auto region{region_file::load(path, {.x = 0, .z = 0})};
            expect(static_cast<bool>(region)) << level;
            if (!region) {
                return;
            }
            const auto written{
                region->write(pos, chunk_status::full, payload, level)};
            expect(static_cast<bool>(written)) << level;
            if (!written) {
                return;
            }
            const auto &entry{region->entries()[region_file::slot_for(pos)]};
            expect(entry.codec == compression::zstd) << level;
            expect(entry.uncompressed == payload.size()) << level;
            expect(static_cast<bool>(region->commit())) << level;
            const auto in_memory{region->read(pos)};
            expect(static_cast<bool>(in_memory)) << level;
            if (in_memory) {
                expect(static_cast<bool>(*in_memory == payload)) << level;
            }
            expect(static_cast<bool>(region->close())) << level;

            auto reopened{region_file::load(path, {.x = 0, .z = 0})};
            expect(static_cast<bool>(reopened)) << level;
            if (!reopened) {
                return;
            }
            const auto stored{reopened->read(pos)};
            expect(static_cast<bool>(stored)) << level;
            if (stored) {
                expect(static_cast<bool>(*stored == payload)) << level;
            }
            expect(static_cast<bool>(reopened->close())) << level;
        }
    };

    "region_rejects_invalid_zstd_level"_test = [] {
        const temp_directory directory;
        const auto path{directory.file("r.0.0.rfw")};
        auto region{region_file::create(path, {.x = 0, .z = 0})};
        expect(static_cast<bool>(region));
        if (!region) {
            return;
        }
        const auto payload{to_bytes(std::string(1024, 'q'))};
        const chunk_pos pos{.x = 1, .z = 2};
        const auto before{region->stats()};
        expect(static_cast<bool>(before));
        for (const int level : {0, 20, -1}) {
            const auto rejected{
                region->write(pos, chunk_status::full, payload, level)};
            expect(!rejected) << level;
            if (!rejected) {
                expect(rejected.error().code ==
                       refrain::error_code::invalid_argument)
                    << level;
            }
        }
        const auto after{region->stats()};
        expect(static_cast<bool>(after));
        if (before && after) {
            // Rejection happens before the append: no record, no tail growth.
            expect(after->data_tail == before->data_tail);
            expect(after->chunks == before->chunks);
        }
        expect(!region->contains(pos));
    };

    "region_default_level_uses_region_options"_test = [] {
        const temp_directory directory;
        const auto default_path{directory.file("default.0.0.rfw")};
        const auto explicit_path{directory.file("explicit.0.0.rfw")};
        const auto payload{to_bytes(std::string(8192, 'm'))};
        const chunk_pos pos{.x = 4, .z = 4};
        {
            auto region{region_file::create(
                default_path, {.x = 0, .z = 0, .zstd_level = 19})};
            expect(static_cast<bool>(region));
            if (!region) {
                return;
            }
            expect(static_cast<bool>(
                region->write(pos, chunk_status::full, payload)));
            expect(static_cast<bool>(region->commit()));
            expect(static_cast<bool>(region->close()));
        }
        {
            auto region{region_file::create(explicit_path, {.x = 0, .z = 0})};
            expect(static_cast<bool>(region));
            if (!region) {
                return;
            }
            expect(static_cast<bool>(
                region->write(pos, chunk_status::full, payload, 19)));
            expect(static_cast<bool>(region->commit()));
            expect(static_cast<bool>(region->close()));
        }
        // Same input and level → byte-identical region (defaulting to the
        // region options level is not a different encoding).
        expect(read_whole(default_path) == read_whole(explicit_path));
    };

    "region_without_compression"_test = [] {
        const temp_directory directory;
        const auto path{directory.file("r.0.0.rfw")};
        auto region{region_file::create(
            path, {.x = 0, .z = 0, .codec = compression::none})};
        expect(static_cast<bool>(region));
        const std::vector<std::byte> payload(1000, std::byte{0x41});
        expect(static_cast<bool>(region->write(chunk_pos{.x = 0, .z = 0},
                                               chunk_status::full, payload)));
        expect(region->entries()[0].codec == compression::none);
        expect(static_cast<bool>(region->commit()));
        const auto stored{region->read(chunk_pos{.x = 0, .z = 0})};
        expect(static_cast<bool>(stored));
        expect(static_cast<bool>(*stored == payload));
    };

    "region_recovery_adopts_newer_slot"_test = [] {
        const temp_directory directory;
        const auto path{directory.file("r.0.0.rfw")};
        auto region{region_file::create(path, {.x = 0, .z = 0})};
        expect(static_cast<bool>(region));
        expect(static_cast<bool>(region->write(
            chunk_pos{.x = 0, .z = 0}, chunk_status::full, to_bytes("first"))));
        expect(static_cast<bool>(region->commit()));
        expect(static_cast<bool>(region->write(chunk_pos{.x = 1, .z = 1},
                                               chunk_status::full,
                                               to_bytes("second"))));
        expect(static_cast<bool>(region->commit()));
        expect(static_cast<bool>(region->close()));

        // Point the header at the older slot (generation 2) while the data
        // area and the newer slot (generation 3) are intact: recovery must
        // adopt the newer slot.
        auto bytes{read_whole(path)};
        expect(bytes.size() > rwf::data_offset);
        bytes.replace(32, 4, std::string{"\x01\x00\x00\x00", 4});
        std::array<std::byte, 4096> header{};
        std::memcpy(header.data(), bytes.data(), header.size());
        std::memset(header.data() + 36, 0, 4);
        const auto crc{refrain::data::crc32c(header)};
        std::memcpy(bytes.data() + 36, &crc, 4);
        write_whole(path, bytes);

        auto recovered{region_file::load(path, {.x = 0, .z = 0})};
        expect(static_cast<bool>(recovered));
        if (recovered) {
            expect(recovered->stats()->chunks == 2U);
            expect(static_cast<bool>(recovered->read(chunk_pos{.x = 0, .z = 0})
                                         .value_or(std::vector<std::byte>{}) ==
                                     to_bytes("first")));
            expect(static_cast<bool>(recovered->read(chunk_pos{.x = 1, .z = 1})
                                         .value_or(std::vector<std::byte>{}) ==
                                     to_bytes("second")));
        }
    };

    "region_detects_payload_corruption"_test = [] {
        const temp_directory directory;
        const auto path{directory.file("r.0.0.rfw")};
        auto region{region_file::create(path, {.x = 0, .z = 0})};
        expect(static_cast<bool>(region));
        const auto payload{to_bytes(std::string(200, 'a'))};
        expect(static_cast<bool>(region->write(chunk_pos{.x = 0, .z = 0},
                                               chunk_status::full, payload)));
        expect(static_cast<bool>(region->commit()));
        expect(static_cast<bool>(region->close()));
        flip_byte(path, rwf::data_offset + 36);
        auto reopened{region_file::load(path, {.x = 0, .z = 0})};
        expect(static_cast<bool>(reopened));
        if (reopened) {
            const auto stored{reopened->read(chunk_pos{.x = 0, .z = 0})};
            expect(!stored);
            if (!stored) {
                expect(stored.error().code == refrain::error_code::parse_error);
            }
        }
    };

    "region_rejects_short_file"_test = [] {
        const temp_directory directory;
        const auto path{directory.file("r.0.0.rfw")};
        auto region{region_file::create(path, {.x = 0, .z = 0})};
        expect(static_cast<bool>(region));
        expect(static_cast<bool>(region->close()));
        std::error_code error;
        fs::resize_file(path, rwf::data_offset - 1, error);
        expect(!error);
        const auto reopened{region_file::load(path, {.x = 0, .z = 0})};
        expect(!reopened);
    };

    "region_fsck_repairs_erased_index"_test = [] {
        const temp_directory directory;
        const auto path{directory.file("r.0.0.rfw")};
        auto region{region_file::create(path, {.x = 0, .z = 0})};
        expect(static_cast<bool>(region));
        expect(static_cast<bool>(region->write(
            chunk_pos{.x = 0, .z = 0}, chunk_status::full, to_bytes("one"))));
        expect(static_cast<bool>(region->write(
            chunk_pos{.x = 1, .z = 0}, chunk_status::full, to_bytes("two"))));
        expect(static_cast<bool>(region->commit()));
        expect(static_cast<bool>(region->close()));

        // Wipe both index slots, keeping the header and data area.
        {
            auto bytes{read_whole(path)};
            std::fill(bytes.begin() + rwf::index_a_offset,
                      bytes.begin() + rwf::data_offset, '\0');
            write_whole(path, bytes);
        }
        expect(!region_file::load(path, {.x = 0, .z = 0}));

        auto checked{rwf::check_region(path, {.x = 0, .z = 0}, false)};
        expect(static_cast<bool>(checked));
        expect(!checked->index_valid);
        expect(!checked->problems.empty());
        expect(checked->chunks == 2U);

        auto repaired{rwf::check_region(path, {.x = 0, .z = 0}, true)};
        expect(static_cast<bool>(repaired));
        expect(repaired->repaired);
        expect(repaired->header_valid);
        expect(repaired->index_valid);

        auto reopened{region_file::load(path, {.x = 0, .z = 0})};
        expect(static_cast<bool>(reopened));
        if (reopened) {
            expect(static_cast<bool>(reopened->read(chunk_pos{.x = 0, .z = 0})
                                         .value_or(std::vector<std::byte>{}) ==
                                     to_bytes("one")));
            expect(static_cast<bool>(reopened->read(chunk_pos{.x = 1, .z = 0})
                                         .value_or(std::vector<std::byte>{}) ==
                                     to_bytes("two")));
        }
    };

    "region_fsck_repairs_erased_header"_test = [] {
        const temp_directory directory;
        const auto path{directory.file("r.0.0.rfw")};
        auto region{region_file::create(path, {.x = 0, .z = 0})};
        expect(static_cast<bool>(region));
        expect(static_cast<bool>(region->write(
            chunk_pos{.x = 2, .z = 3}, chunk_status::full, to_bytes("keep"))));
        expect(static_cast<bool>(region->commit()));
        expect(static_cast<bool>(region->close()));
        {
            auto bytes{read_whole(path)};
            std::fill(bytes.begin(), bytes.begin() + 4096, '\0');
            write_whole(path, bytes);
        }
        auto checked{rwf::check_region(path, {.x = 0, .z = 0}, false)};
        expect(static_cast<bool>(checked));
        expect(!checked->header_valid);
        auto repaired{rwf::check_region(path, {.x = 0, .z = 0}, true)};
        expect(static_cast<bool>(repaired));
        expect(repaired->repaired);
        auto reopened{region_file::load(path, {.x = 0, .z = 0})};
        expect(static_cast<bool>(reopened));
        if (reopened) {
            expect(static_cast<bool>(reopened->read(chunk_pos{.x = 2, .z = 3})
                                         .value_or(std::vector<std::byte>{}) ==
                                     to_bytes("keep")));
        }
    };

    "region_compaction_drops_garbage"_test = [] {
        const temp_directory directory;
        const auto path{directory.file("r.0.0.rfw")};
        auto region{region_file::create(path, {.x = 0, .z = 0})};
        expect(static_cast<bool>(region));
        const chunk_pos pos{.x = 0, .z = 0};
        for (int version{0}; version < 6; ++version) {
            expect(static_cast<bool>(region->write(
                pos, chunk_status::full,
                to_bytes(std::string(512, static_cast<char>('a' + version))))));
        }
        expect(static_cast<bool>(region->commit()));
        const auto before{region->stats()};
        expect(static_cast<bool>(before));
        expect(before->garbage_bytes > 0U);
        auto compacted{region->compact()};
        expect(static_cast<bool>(compacted));
        if (!compacted) {
            return;
        }
        expect(compacted->garbage_bytes == 0U);
        expect(compacted->chunks == 1U);
        expect(compacted->file_bytes < before->file_bytes);
        const auto stored{region->read(pos)};
        expect(static_cast<bool>(stored));
        expect(static_cast<bool>(*stored == to_bytes(std::string(512, 'f'))));
    };

    "record_file_roundtrip_and_corruption"_test = [] {
        const temp_directory directory;
        const auto path{directory.file("meta.rfw")};
        const auto payload{to_bytes("refrain world metadata")};
        expect(static_cast<bool>(rwf::write_record_file(path, payload)));
        const auto first{read_whole(path)};
        expect(static_cast<bool>(rwf::write_record_file(path, payload)));
        expect(read_whole(path) == first);

        auto read{rwf::read_record_file(path)};
        expect(static_cast<bool>(read));
        expect(static_cast<bool>(*read == payload));

        flip_byte(path, 4096 + 20);
        const auto corrupted{rwf::read_record_file(path)};
        expect(!corrupted);
        if (!corrupted) {
            expect(corrupted.error().code == refrain::error_code::parse_error);
        }
    };

    "record_file_rejects_oversize"_test = [] {
        const temp_directory directory;
        const auto path{directory.file("meta.rfw")};
        const std::vector<std::byte> huge(rwf::max_record_bytes + 1,
                                          std::byte{0});
        expect(!rwf::write_record_file(path, huge));
        expect(!rwf::read_record_file(path));
    };

    "region_file_zstd_dictionary"_test = [] {
        const temp_directory directory;
        const auto path{directory.file("r.0.0.rfw")};
        std::string dictionary_bytes(1024, 'd');
        const auto dictionary{
            rwf::make_zstd_dictionary(to_bytes(dictionary_bytes))};
        expect(dictionary.id != 0U);
        {
            auto region{region_file::create(
                path, {.x = 0, .z = 0, .dictionary = &dictionary})};
            expect(static_cast<bool>(region));
            if (!region) {
                return;
            }
            const auto payload{to_bytes(std::string(4096, 'p'))};
            expect(static_cast<bool>(
                region->write({.x = 0, .z = 0}, chunk_status::full, payload)));
            expect(static_cast<bool>(region->commit()));
        }
        {
            auto region{region_file::load(
                path, {.x = 0, .z = 0, .dictionary = &dictionary})};
            expect(static_cast<bool>(region));
            if (!region) {
                return;
            }
            auto payload{region->read({.x = 0, .z = 0})};
            expect(static_cast<bool>(payload));
            if (payload) {
                expect(static_cast<bool>(*payload ==
                                         to_bytes(std::string(4096, 'p'))));
            }
        }
        auto without{region_file::load(path, {.x = 0, .z = 0})};
        expect(!without);
        if (!without) {
            expect(without.error().code == refrain::error_code::parse_error ||
                   without.error().code ==
                       refrain::error_code::version_mismatch);
        }
    };

    "blob_store_roundtrip"_test = [] {
        const temp_directory directory;
        blob_store blobs{directory.file("blobs")};
        const auto payload{to_bytes("player data")};
        expect(static_cast<bool>(blobs.write("playerdata", "abc", payload)));
        expect(blobs.exists("playerdata", "abc"));
        expect(!blobs.exists("playerdata", "missing"));
        auto read{blobs.read("playerdata", "abc")};
        expect(static_cast<bool>(read));
        expect(static_cast<bool>(*read == payload));
        auto keys{blobs.list("playerdata")};
        expect(static_cast<bool>(keys));
        expect(keys->size() == 1U);
        expect((*keys)[0] == "abc");
        expect(static_cast<bool>(blobs.erase("playerdata", "abc")));
        expect(!blobs.exists("playerdata", "abc"));
        expect(!blobs.erase("playerdata", "abc"));
        expect(blobs.list("playerdata")->empty());
    };

    "blob_store_group_commit_and_reopen"_test = [] {
        const temp_directory directory;
        const auto root{directory.file("blobs")};
        {
            blob_store blobs{root};
            expect(static_cast<bool>(
                blobs.write("entities", "a", to_bytes("alpha"), false)));
            expect(static_cast<bool>(
                blobs.write("entities", "b", to_bytes("beta"), false)));
            expect(static_cast<bool>(
                blobs.write("entities", "c", to_bytes("gamma"), false)));
            expect(!fs::exists(root + "/index.rfw"));
            auto pending{blobs.read("entities", "b")};
            expect(static_cast<bool>(pending));
            expect(static_cast<bool>(*pending == to_bytes("beta")));
            auto keys{blobs.list("entities")};
            expect(static_cast<bool>(keys));
            expect(keys->size() == 3U);

            expect(static_cast<bool>(blobs.commit()));
            expect(fs::exists(root + "/index.rfw"));
            std::size_t segments{0};
            for (const auto &entry :
                 fs::directory_iterator(root + "/segments")) {
                if (entry.path().extension() == ".rfseg") {
                    ++segments;
                }
            }
            expect(segments == 1U);
        }
        {
            blob_store reopened{root};
            auto read{reopened.read("entities", "a")};
            expect(static_cast<bool>(read));
            expect(static_cast<bool>(*read == to_bytes("alpha")));
            expect(static_cast<bool>(
                reopened.write("entities", "b", to_bytes("beta2"), false)));
            expect(static_cast<bool>(reopened.commit()));
            auto updated{reopened.read("entities", "b")};
            expect(static_cast<bool>(updated));
            expect(static_cast<bool>(*updated == to_bytes("beta2")));
            expect(static_cast<bool>(reopened.erase("entities", "a")));
            expect(!reopened.exists("entities", "a"));
            auto keys{reopened.list("entities")};
            expect(static_cast<bool>(keys));
            expect(keys->size() == 2U);
        }
        {
            blob_store reopened{root};
            expect(!reopened.exists("entities", "a"));
            auto read{reopened.read("entities", "c")};
            expect(static_cast<bool>(read));
            expect(static_cast<bool>(*read == to_bytes("gamma")));
        }
    };

    "blob_store_is_deterministic"_test = [] {
        const temp_directory first;
        const temp_directory second;
        const auto fill = [](const std::string &root) {
            blob_store blobs{root};
            expect(static_cast<bool>(
                blobs.write("poi", "0.0", to_bytes("zero"), false)));
            expect(static_cast<bool>(
                blobs.write("poi", "0.1", to_bytes("one"), false)));
            expect(static_cast<bool>(
                blobs.write("poi", "1.0", to_bytes("two"), false)));
            expect(static_cast<bool>(blobs.commit()));
        };
        fill(first.file("blobs"));
        fill(second.file("blobs"));
        const auto first_index{read_whole(first.file("blobs/index.rfw"))};
        const auto second_index{read_whole(second.file("blobs/index.rfw"))};
        expect(first_index == second_index);
        std::vector<std::string> first_segments;
        std::vector<std::string> second_segments;
        for (const auto &entry :
             fs::directory_iterator(first.file("blobs/segments"))) {
            first_segments.push_back(read_whole(entry.path().string()));
        }
        for (const auto &entry :
             fs::directory_iterator(second.file("blobs/segments"))) {
            second_segments.push_back(read_whole(entry.path().string()));
        }
        expect(first_segments.size() == 1U);
        expect(second_segments.size() == 1U);
        expect(first_segments == second_segments);
    };

    "blob_store_detects_segment_corruption"_test = [] {
        const temp_directory directory;
        const auto root{directory.file("blobs")};
        {
            blob_store blobs{root};
            expect(static_cast<bool>(
                blobs.write("entities", "a", to_bytes("payload"), true)));
        }
        std::string segment;
        for (const auto &entry : fs::directory_iterator(root + "/segments")) {
            if (entry.path().extension() == ".rfseg") {
                segment = entry.path().string();
            }
        }
        expect(!segment.empty());
        auto bytes{read_whole(segment)};
        expect(bytes.size() > 80U);
        bytes[80] = static_cast<char>(bytes[80] ^ 0x01);
        write_whole(segment, bytes);
        blob_store reopened{root};
        auto read{reopened.read("entities", "a")};
        expect(!read);
    };

    "object_store_paths_and_group_commit"_test = [] {
        const temp_directory directory;
        const auto root{directory.file("objects")};
        {
            world::object_store objects{root};
            expect(static_cast<bool>(objects.write(
                "global/data/minecraft/game_rules", to_bytes("rules"),
                world::object_media::nbt, false)));
            expect(static_cast<bool>(
                objects.write("players/abc/stats", to_bytes("{}"),
                              world::object_media::json, false)));
            expect(static_cast<bool>(
                objects.write("assets/icon", to_bytes("png"),
                              world::object_media::png, false)));
            expect(!fs::exists(root + "/index.rfw"));
            auto rules{objects.read("global/data/minecraft/game_rules")};
            expect(static_cast<bool>(rules));
            expect(static_cast<bool>(*rules == to_bytes("rules")));
            auto all{objects.list()};
            expect(static_cast<bool>(all));
            expect(all->size() == 3U);
            expect((*all)[0] == "assets/icon");
            auto players{objects.list("players/")};
            expect(static_cast<bool>(players));
            expect(players->size() == 1U);
            expect(static_cast<bool>(objects.commit()));
            expect(fs::exists(root + "/index.rfw"));
            expect(fs::is_directory(root + "/segments"));
        }
        {
            world::object_store reopened{root};
            auto stats{reopened.read("players/abc/stats")};
            expect(static_cast<bool>(stats));
            expect(static_cast<bool>(*stats == to_bytes("{}")));
            expect(static_cast<bool>(reopened.erase("assets/icon")));
            expect(!reopened.exists("assets/icon"));
            auto remaining{reopened.list()};
            expect(static_cast<bool>(remaining));
            expect(remaining->size() == 2U);
        }
    };

    "object_store_compaction_reclaims_garbage"_test = [] {
        const temp_directory directory;
        const auto root{directory.file("objects")};
        {
            world::object_store objects{root};
            for (int index = 0; index < 8; ++index) {
                expect(static_cast<bool>(
                    objects.write(fmt::format("global/data/object{}", index),
                                  to_bytes(std::string(256, 'x')),
                                  world::object_media::binary, false)));
            }
            expect(static_cast<bool>(objects.commit()));
            expect(static_cast<bool>(
                objects.write("global/data/object0", to_bytes("updated"),
                              world::object_media::binary, false)));
            expect(static_cast<bool>(objects.erase("global/data/object1")));
            auto before{objects.compact()};
            expect(static_cast<bool>(before));
            if (before) {
                expect(before->objects == 7U);
                expect(before->segments == 1U);
                expect(before->garbage_bytes > 0U);
            }
            auto updated{objects.read("global/data/object0")};
            expect(static_cast<bool>(updated));
            expect(static_cast<bool>(*updated == to_bytes("updated")));
            expect(!objects.exists("global/data/object1"));
        }
        world::object_store reopened{root};
        auto value{reopened.read("global/data/object7")};
        expect(static_cast<bool>(value));
        expect(static_cast<bool>(*value == to_bytes(std::string(256, 'x'))));
    };

    "object_store_large_objects_use_rfbl"_test = [] {
        const temp_directory directory;
        const auto root{directory.file("objects")};
        std::vector<std::byte> payload(5U * 1024U * 1024U);
        for (std::size_t index{0}; index < payload.size(); ++index) {
            payload[index] = static_cast<std::byte>(index % 251U);
        }
        {
            world::object_store objects{root};
            expect(static_cast<bool>(
                objects.write("datapacks/huge.zip", payload,
                              world::object_media::zip, true)));
            auto read{objects.read("datapacks/huge.zip")};
            expect(static_cast<bool>(read));
            if (read) {
                expect(read->size() == payload.size());
                expect(static_cast<bool>(*read == payload));
            }
            expect(fs::is_directory(root + "/large"));
            auto compacted{objects.compact()};
            expect(static_cast<bool>(compacted));
            auto after{objects.read("datapacks/huge.zip")};
            expect(static_cast<bool>(after));
            if (after) {
                expect(static_cast<bool>(*after == payload));
            }
        }
        world::object_store reopened{root};
        auto read{reopened.read("datapacks/huge.zip")};
        expect(static_cast<bool>(read));
        if (read) {
            expect(static_cast<bool>(*read == payload));
        }
    };

    "object_store_rejects_unsafe_paths"_test = [] {
        const temp_directory directory;
        world::object_store objects{directory.file("objects")};
        for (const std::string &path :
             {std::string{}, std::string{"/absolute"}, std::string{"a//b"},
              std::string{"a/../b"}, std::string{"a\\b"}, std::string{"a/./b"},
              std::string{"a b"}, std::string(513, 'a')}) {
            expect(!world::object_store::valid_path(path)) << path;
            expect(!objects.write(path, to_bytes("x"),
                                  world::object_media::binary));
        }
        expect(world::object_store::valid_path("global/data/minecraft/x"));
        expect(world::object_store::valid_path("players/0-1/stats"));
    };

    "blob_store_rejects_unsafe_keys"_test = [] {
        const temp_directory directory;
        blob_store blobs{directory.file("blobs")};
        const auto payload{to_bytes("x")};
        for (const std::string_view key :
             {"", "../escape", "a/b", ".hidden", "..", "with space"}) {
            expect(!blobs.write("playerdata", key, payload)) << key;
            expect(!blobs.read("playerdata", key)) << key;
        }
        expect(!blob_store::valid_key(std::string(97, 'a')));
        expect(blob_store::valid_key(std::string(96, 'a')));
        expect(!blob_store::valid_kind("bad/kind"));
        expect(!blob_store::valid_kind(".."));
        expect(blob_store::valid_key("0.-1"));
    };
};
