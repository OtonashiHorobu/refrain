// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/io.hpp>
#include <refrain/data/json.hpp>
#include <refrain/data/nbt.hpp>
#include <refrain/platform/process.hpp>
#include <refrain/world/anvil.hpp>
#include <refrain/world/maintenance.hpp>
#include <refrain/world/store.hpp>

#include <boost/ut.hpp>

#include <fmt/format.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace boost::ut;

namespace fs = std::filesystem;
namespace io = refrain::core::io;
namespace nbt = refrain::data::nbt;
namespace world = refrain::world;

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
                base /
                fmt::format("refrain-maintenance-{}",
                            sequence.fetch_add(1, std::memory_order_relaxed))};
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
    [[nodiscard]] auto sub(std::string_view name) const -> std::string {
        return (path_ / std::string{name}).string();
    }

  private:
    fs::path path_;
};

[[nodiscard]] auto normalized_path(std::string_view path) -> std::string {
    return fs::absolute(fs::path{std::string{path}})
        .lexically_normal()
        .string();
}

[[nodiscard]] auto make_chunk() -> world::chunk {
    world::chunk chunk{};
    chunk.format_version = world::chunk_format_version;
    chunk.status = world::chunk_status::full;
    chunk.y_pos = 0;
    world::section_record section{};
    section.y = 0;
    section.kind = world::section_kind::single_value;
    section.bits_per_entry = 0;
    section.block_count = static_cast<std::uint16_t>(world::section_volume);
    section.biome_kind = world::section_kind::single_value;
    section.biome_bits_per_entry = 0;
    section.biome_palette.push_back("minecraft:plains");
    section.palette.push_back(world::palette_entry{
        .key = "minecraft:stone",
        .count = static_cast<std::uint16_t>(world::section_volume),
        .state = 0,
    });
    chunk.sections.push_back(std::move(section));
    return chunk;
}

/// Build a minimal RWF world with one chunk and the `meta.rfw` level compound
/// that the Anvil round-trip needs. `with_unused` appends a palette entry no
/// chunk references (the GC target).
[[nodiscard]] auto create_world(const std::string &root, bool with_unused)
    -> bool {
    auto store{world::world_store::create(root)};
    if (!store) {
        return false;
    }
    if (!store->write_chunk(world::chunk_pos{0, 0}, make_chunk())) {
        return false;
    }
    nbt::value level{nbt::make_compound()};
    if (!level.set("DataVersion", nbt::make_int(5023))) {
        return false;
    }
    nbt::value meta{nbt::make_compound()};
    if (!meta.set("level", std::move(level))) {
        return false;
    }
    auto meta_bytes{nbt::write(meta, "", nbt::compression::none)};
    if (!meta_bytes || !store->write_meta(*meta_bytes)) {
        return false;
    }
    if (with_unused) {
        auto palette{store->palette()};
        static_cast<void>(
            palette.intern("minecraft:refrain_unused_palette_entry"));
        if (!store->install_palette(std::move(palette))) {
            return false;
        }
    }
    return static_cast<bool>(store->commit());
}

[[nodiscard]] auto copy_world(const std::string &from, const std::string &to)
    -> bool {
    std::error_code error;
    fs::copy(fs::path{from}, fs::path{to}, fs::copy_options::recursive, error);
    return !error;
}

void write_marker(const std::string &root, std::string_view phase,
                  const std::string &temp, const std::string &backup) {
    const auto world_root{normalized_path(root)};
    refrain::data::json::value::object_type object;
    object.emplace("backup", refrain::data::json::value{backup});
    object.emplace("phase", refrain::data::json::value{std::string{phase}});
    object.emplace("pid", refrain::data::json::value{static_cast<std::int64_t>(
                              refrain::platform::process_id())});
    object.emplace("schema", refrain::data::json::value{
                                 std::string{"refrain.palette-maintenance/1"}});
    object.emplace("temp", refrain::data::json::value{temp});
    object.emplace("world", refrain::data::json::value{world_root});
    const auto marker{world::palette_maintenance_marker_path(world_root)};
    expect(static_cast<bool>(io::write_text(
        marker,
        fmt::format(
            "{}\n",
            refrain::data::json::value{std::move(object)}.dump_pretty(2)))));
}

[[nodiscard]] auto marker_exists(const std::string &root) -> bool {
    return io::exists(world::palette_maintenance_marker_path(root));
}

[[nodiscard]] auto same_digests(const std::string &left,
                                const std::string &right) -> bool {
    auto before{world::anvil::scan_world(left)};
    auto after{world::anvil::scan_world(right)};
    if (!before || !after || before->size() != after->size()) {
        return false;
    }
    for (std::size_t index{0}; index < before->size(); ++index) {
        if ((*before)[index].dimension != (*after)[index].dimension ||
            (*before)[index].pos != (*after)[index].pos ||
            (*before)[index].hash != (*after)[index].hash) {
            return false;
        }
    }
    return true;
}

[[maybe_unused]] const suite tests = [] {
    "world/maintenance rebuilds the palette in place"_test = [] {
        const temp_directory directory;
        const auto root{directory.sub("world")};
        expect(create_world(root, true));
        const auto before_root{directory.sub("before")};
        expect(copy_world(root, before_root));
        const auto before_digest{world::anvil::scan_world(root)};
        expect(static_cast<bool>(before_digest));

        std::size_t palette_before{0};
        {
            auto store{world::world_store::open(root)};
            expect(static_cast<bool>(store));
            if (!store) {
                return;
            }
            palette_before = store->palette().size();
            const auto usage{store->palette_usage_stats()};
            expect(static_cast<bool>(usage));
            if (usage) {
                expect(usage->table_entries == palette_before);
                expect(usage->referenced_entries < usage->table_entries);
            }
        }

        auto report{world::rebuild_palette_in_place(
            root, {.threads = 1, .zstd_level = 3})};
        expect(report.has_value()) << (report ? "" : report.error().message);
        if (!report) {
            return;
        }
        expect(report->entries_before == palette_before);
        expect(report->entries_after < report->entries_before);
        expect(report->chunks == 1U);
        expect(report->regions == 1U);
        expect(!report->recovered);
        expect(!marker_exists(root));
        expect(!io::exists(world::palette_maintenance_temp_path(root, 0)));
        expect(!io::exists(world::palette_maintenance_backup_path(root, 0)));

        expect(same_digests(before_root, root));
        const auto after_digest{world::anvil::scan_world(root)};
        expect(static_cast<bool>(after_digest));
        if (before_digest && after_digest) {
            expect(before_digest->size() == after_digest->size());
            if (before_digest->size() == after_digest->size()) {
                for (std::size_t index{0}; index < before_digest->size();
                     ++index) {
                    expect((*before_digest)[index].hash ==
                           (*after_digest)[index].hash);
                }
            }
        }

        auto store{world::world_store::open(root)};
        expect(static_cast<bool>(store));
        if (store) {
            expect(store->palette().size() == report->entries_after);
            auto chunk{store->read_chunk({0, 0})};
            expect(static_cast<bool>(chunk));
            if (chunk) {
                expect(chunk->sections.size() == 1U);
                if (!chunk->sections.empty()) {
                    expect(chunk->sections[0].palette.size() == 1U);
                    expect(chunk->sections[0].palette[0].key ==
                           "minecraft:stone");
                    expect(chunk->sections[0].block_count ==
                           world::section_volume);
                    expect(chunk->sections[0].biome_palette.size() == 1U);
                    expect(chunk->sections[0].biome_palette[0] ==
                           "minecraft:plains");
                }
            }
        }

        // Idempotent: a second rebuild has nothing left to reclaim.
        auto second{world::rebuild_palette_in_place(
            root, {.threads = 1, .zstd_level = 3})};
        expect(second.has_value()) << (second ? "" : second.error().message);
        if (second) {
            expect(second->entries_before == second->entries_after);
            expect(second->entries_before == report->entries_after);
            expect(!second->recovered);
        }
    };

    "world/maintenance never overwrites pre-existing scratch paths"_test = [] {
        const temp_directory directory;
        const auto root{directory.sub("world")};
        expect(create_world(root, true));
        const auto temp_zero{world::palette_maintenance_temp_path(root, 0)};
        const auto backup_zero{world::palette_maintenance_backup_path(root, 0)};
        expect(static_cast<bool>(io::write_text(temp_zero, "keep-temp")));
        expect(static_cast<bool>(io::write_text(backup_zero, "keep-backup")));

        auto report{world::rebuild_palette_in_place(root, {})};
        expect(report.has_value()) << (report ? "" : report.error().message);
        auto temp_text{io::read_text(temp_zero)};
        auto backup_text{io::read_text(backup_zero)};
        expect(temp_text.has_value() && *temp_text == "keep-temp");
        expect(backup_text.has_value() && *backup_text == "keep-backup");
        expect(!io::exists(world::palette_maintenance_temp_path(root, 1)));
        expect(!io::exists(world::palette_maintenance_backup_path(root, 1)));
    };

    "world/maintenance corrupt marker fails closed"_test = [] {
        const temp_directory directory;
        const auto root{directory.sub("world")};
        expect(create_world(root, true));
        const auto marker{world::palette_maintenance_marker_path(root)};
        expect(static_cast<bool>(io::write_text(marker, "{not json")));

        auto opened{world::world_store::open(root)};
        expect(!opened);
        if (!opened) {
            expect(opened.error().code == refrain::error_code::parse_error);
        }
        auto report{world::rebuild_palette_in_place(root, {})};
        expect(!report);
        if (!report) {
            expect(report.error().code == refrain::error_code::parse_error);
        }
        expect(io::exists(marker));
        expect(static_cast<bool>(world::world_store::open(root)) == false);
    };

    "world/maintenance rejects foreign marker paths"_test = [] {
        const temp_directory directory;
        const auto root{directory.sub("world")};
        expect(create_world(root, true));
        const auto foreign{directory.sub("foreign")};
        expect(static_cast<bool>(io::create_directories(foreign)));
        expect(static_cast<bool>(
            io::write_text((fs::path{foreign} / "keep.txt").string(), "keep")));

        const auto marker{world::palette_maintenance_marker_path(root)};
        refrain::data::json::value::object_type object;
        object.emplace("backup",
                       refrain::data::json::value{
                           world::palette_maintenance_backup_path(root, 0)});
        object.emplace("phase",
                       refrain::data::json::value{std::string{"swapping"}});
        object.emplace("pid",
                       refrain::data::json::value{static_cast<std::int64_t>(
                           refrain::platform::process_id())});
        object.emplace("schema", refrain::data::json::value{std::string{
                                     "refrain.palette-maintenance/1"}});
        object.emplace("temp", refrain::data::json::value{foreign});
        object.emplace("world",
                       refrain::data::json::value{normalized_path(root)});
        expect(static_cast<bool>(io::write_text(
            marker,
            fmt::format("{}\n", refrain::data::json::value{std::move(object)}
                                    .dump_pretty(2)))));

        auto opened{world::world_store::open(root)};
        expect(!opened);
        if (!opened) {
            expect(opened.error().code == refrain::error_code::parse_error);
        }
        expect(io::exists((fs::path{foreign} / "keep.txt").string()));
        expect(io::exists(marker));
    };

    "world/maintenance recovers planned and rebuilding crashes"_test = [] {
        const temp_directory directory;

        const auto planned{directory.sub("planned")};
        expect(create_world(planned, true));
        write_marker(planned, "planned",
                     world::palette_maintenance_temp_path(planned, 0),
                     world::palette_maintenance_backup_path(planned, 0));
        auto recovered{world::recover_palette_maintenance(planned)};
        expect(recovered.has_value() && *recovered);
        expect(!marker_exists(planned));
        expect(static_cast<bool>(world::world_store::open(planned)));
        auto again{world::recover_palette_maintenance(planned)};
        expect(again.has_value() && !*again);

        const auto rebuilding{directory.sub("rebuilding")};
        expect(create_world(rebuilding, true));
        const auto partial{world::palette_maintenance_temp_path(rebuilding, 0)};
        expect(static_cast<bool>(io::create_directories(partial)));
        expect(static_cast<bool>(io::write_text(
            (fs::path{partial} / "partial.bin").string(), "partial")));
        write_marker(rebuilding, "rebuilding", partial,
                     world::palette_maintenance_backup_path(rebuilding, 0));
        auto rebuilding_recovered{
            world::recover_palette_maintenance(rebuilding)};
        expect(rebuilding_recovered.has_value() && *rebuilding_recovered);
        expect(!io::exists(partial));
        expect(!marker_exists(rebuilding));
        expect(static_cast<bool>(world::world_store::open(rebuilding)));
    };

    "world/maintenance rolls forward a complete swapping temp"_test = [] {
        const temp_directory directory;
        const auto root{directory.sub("world")};
        expect(create_world(root, true));
        const auto before{world::anvil::scan_world(root)};
        expect(static_cast<bool>(before));
        const auto temp{world::palette_maintenance_temp_path(root, 0)};
        const auto backup{world::palette_maintenance_backup_path(root, 0)};
        expect(copy_world(root, temp));
        write_marker(root, "swapping", temp, backup);

        auto recovered{world::recover_palette_maintenance(root)};
        expect(recovered.has_value() && *recovered);
        expect(!io::exists(temp));
        expect(!io::exists(backup));
        expect(!marker_exists(root));
        auto store{world::world_store::open(root)};
        expect(static_cast<bool>(store));
        const auto after{world::anvil::scan_world(root)};
        expect(static_cast<bool>(after));
        if (before && after) {
            expect(before->size() == after->size());
            if (before->size() == after->size()) {
                for (std::size_t index{0}; index < before->size(); ++index) {
                    expect((*before)[index].hash == (*after)[index].hash);
                }
            }
        }
    };

    "world/maintenance recovery handles the remaining swap states"_test = [] {
        const temp_directory directory;

        // R,B,!T: the temp was promoted; only the backup and marker remain.
        const auto promoted{directory.sub("promoted")};
        expect(create_world(promoted, true));
        const auto promoted_backup{
            world::palette_maintenance_backup_path(promoted, 0)};
        expect(copy_world(promoted, promoted_backup));
        write_marker(promoted, "swapping",
                     world::palette_maintenance_temp_path(promoted, 0),
                     promoted_backup);
        auto promoted_recovered{world::recover_palette_maintenance(promoted)};
        expect(promoted_recovered.has_value() && *promoted_recovered);
        expect(!io::exists(promoted_backup));
        expect(!marker_exists(promoted));
        expect(static_cast<bool>(world::world_store::open(promoted)));

        // R,!B,!T: the swap completed and only the marker is left.
        const auto finished{directory.sub("finished")};
        expect(create_world(finished, true));
        write_marker(finished, "swapping",
                     world::palette_maintenance_temp_path(finished, 0),
                     world::palette_maintenance_backup_path(finished, 0));
        auto finished_recovered{world::recover_palette_maintenance(finished)};
        expect(finished_recovered.has_value() && *finished_recovered);
        expect(!marker_exists(finished));
        expect(static_cast<bool>(world::world_store::open(finished)));

        // R,!B,T with an incomplete temp: discard the temp, keep the source.
        const auto incomplete{directory.sub("incomplete")};
        expect(create_world(incomplete, true));
        const auto incomplete_temp{
            world::palette_maintenance_temp_path(incomplete, 0)};
        expect(static_cast<bool>(io::create_directories(incomplete_temp)));
        write_marker(incomplete, "swapping", incomplete_temp,
                     world::palette_maintenance_backup_path(incomplete, 0));
        auto incomplete_recovered{
            world::recover_palette_maintenance(incomplete)};
        expect(incomplete_recovered.has_value() && *incomplete_recovered);
        expect(!io::exists(incomplete_temp));
        expect(!marker_exists(incomplete));
        expect(static_cast<bool>(world::world_store::open(incomplete)));

        // !R,B,!T: the temp was lost; restore the backup.
        const auto restore{directory.sub("restore")};
        expect(create_world(restore, true));
        const auto restore_backup{
            world::palette_maintenance_backup_path(restore, 0)};
        std::error_code error;
        fs::rename(fs::path{restore}, fs::path{restore_backup}, error);
        expect(!error);
        write_marker(restore, "swapping",
                     world::palette_maintenance_temp_path(restore, 0),
                     restore_backup);
        auto restore_recovered{world::recover_palette_maintenance(restore)};
        expect(restore_recovered.has_value() && *restore_recovered);
        expect(io::exists(restore));
        expect(!io::exists(restore_backup));
        expect(!marker_exists(restore));
        expect(static_cast<bool>(world::world_store::open(restore)));

        // !R,!B,T: the only recoverable copy is the complete temp.
        const auto only_temp{directory.sub("only-temp")};
        expect(create_world(only_temp, true));
        const auto only_temp_path{
            world::palette_maintenance_temp_path(only_temp, 0)};
        fs::rename(fs::path{only_temp}, fs::path{only_temp_path}, error);
        expect(!error);
        write_marker(only_temp, "swapping", only_temp_path,
                     world::palette_maintenance_backup_path(only_temp, 0));
        auto only_recovered{world::recover_palette_maintenance(only_temp)};
        expect(only_recovered.has_value() && *only_recovered);
        expect(io::exists(only_temp));
        expect(!io::exists(only_temp_path));
        expect(!marker_exists(only_temp));
        expect(static_cast<bool>(world::world_store::open(only_temp)));
    };

    "world/store open recovers an interrupted root-to-backup swap"_test = [] {
        const temp_directory directory;
        const auto root{directory.sub("world")};
        expect(create_world(root, true));
        const auto temp{world::palette_maintenance_temp_path(root, 0)};
        const auto backup{world::palette_maintenance_backup_path(root, 0)};
        expect(copy_world(root, temp));
        std::error_code error;
        fs::rename(fs::path{root}, fs::path{backup}, error);
        expect(!error);
        expect(!io::exists(root));
        write_marker(root, "swapping", temp, backup);

        auto store{world::world_store::open(root)};
        expect(store.has_value()) << (store ? "" : store.error().message);
        expect(io::exists(root));
        expect(!io::exists(backup));
        expect(!io::exists(temp));
        expect(!marker_exists(root));
    };

    "world/maintenance ambiguous swap fails closed"_test = [] {
        const temp_directory directory;
        const auto root{directory.sub("world")};
        expect(create_world(root, true));
        const auto temp{world::palette_maintenance_temp_path(root, 0)};
        const auto backup{world::palette_maintenance_backup_path(root, 0)};
        expect(copy_world(root, temp));
        expect(copy_world(root, backup));
        write_marker(root, "swapping", temp, backup);

        auto opened{world::world_store::open(root)};
        expect(!opened);
        if (!opened) {
            expect(opened.error().code == refrain::error_code::io_error);
        }
        expect(io::exists(root));
        expect(io::exists(backup));
        expect(io::exists(temp));
        expect(marker_exists(root));
    };

    "world/maintenance reports a prior crash recovery"_test = [] {
        const temp_directory directory;
        const auto root{directory.sub("world")};
        expect(create_world(root, true));
        write_marker(root, "planned",
                     world::palette_maintenance_temp_path(root, 0),
                     world::palette_maintenance_backup_path(root, 0));
        auto report{world::rebuild_palette_in_place(root, {})};
        expect(report.has_value()) << (report ? "" : report.error().message);
        if (report) {
            expect(report->recovered);
            expect(report->entries_after < report->entries_before);
        }
        expect(!marker_exists(root));
    };

    "world/maintenance read-only parent fails without damage"_test = [] {
#if defined(__unix__) || defined(__APPLE__)
        const temp_directory directory;
        const auto parent{fs::path{directory.sub("readonly")}};
        std::error_code error;
        expect(fs::create_directories(parent, error));
        const auto root{(parent / "world").string()};
        expect(create_world(root, true));
        fs::permissions(parent, fs::perms::owner_read | fs::perms::owner_exec,
                        fs::perm_options::replace, error);
        expect(!error);
        // A privileged process (root/CAP_DAC_OVERRIDE) can still create files
        // in a read-only directory; probe instead of including <unistd.h>.
        const auto probe{(parent / "probe").string()};
        if (io::write_text(probe, "probe")) {
            std::error_code ignored;
            fs::remove(fs::path{probe}, ignored);
            fs::permissions(parent, fs::perms::owner_all, fs::perm_options::add,
                            error);
            expect(!error);
            return;
        }
        auto report{world::rebuild_palette_in_place(root, {})};
        expect(!report);
        if (!report) {
            expect(report.error().code ==
                       refrain::error_code::permission_denied ||
                   report.error().code == refrain::error_code::io_error);
        }
        fs::permissions(parent, fs::perms::owner_all, fs::perm_options::add,
                        error);
        expect(!error);
        expect(static_cast<bool>(world::world_store::open(root)));
        expect(!marker_exists(root));
#endif
    };
};

} // namespace
