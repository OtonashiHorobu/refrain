// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/world/store.hpp>

#include <refrain/core/io.hpp>
#include <refrain/data/nbt.hpp>
#include <refrain/data/path.hpp>
#include <refrain/world/chunk_codec.hpp>
#include <refrain/world/maintenance.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace refrain::world {
namespace {

using data::join_path;

/// Upper bound for the serialized world palette table (`RFPT`). The reader has
/// the same limit; a table that exceeded it could be written but never opened.
constexpr std::size_t max_palette_bytes{4ULL << 20U};

[[nodiscard]] auto failing(error_code code, std::string_view message)
    -> std::unexpected<error> {
    return std::unexpected{make_error(code, message)};
}

/// Inclusive bounds of the vanilla enumerations projected into `level_info`.
constexpr std::int32_t max_game_mode{3};
constexpr std::int32_t max_difficulty{3};

[[nodiscard]] auto valid_enumeration(std::int32_t value, std::int32_t upper)
    -> bool {
    return value >= 0 && value <= upper;
}

/// Read an integer-valued `level` tag. Vanilla stores enumeration fields as
/// either TAG_Int or TAG_Byte depending on the field and the world version, so
/// both are accepted; every other tag type is ignored.
[[nodiscard]] auto stored_integer(const data::nbt::value &level,
                                  std::string_view key)
    -> std::optional<std::int32_t> {
    const auto *field{level.get(key)};
    if (field == nullptr) {
        return std::nullopt;
    }
    if (const auto value{field->as_int()}) {
        return *value;
    }
    if (const auto value{field->as_byte()}) {
        return static_cast<std::int32_t>(*value);
    }
    return std::nullopt;
}

/// Fill the gameplay fields of `info` from a `level` compound. Missing,
/// mistyped or out-of-range tags keep the field defaults so that opening an
/// older or foreign world never fails on them.
void project_gameplay(const data::nbt::value &level, level_info &info) {
    if (const auto value{stored_integer(level, "GameType")};
        value.has_value() && valid_enumeration(*value, max_game_mode)) {
        info.game_mode = *value;
    }
    if (const auto value{stored_integer(level, "hardcore")};
        value.has_value()) {
        info.hardcore = *value != 0;
    }
    if (const auto value{stored_integer(level, "Difficulty")};
        value.has_value() && valid_enumeration(*value, max_difficulty)) {
        info.difficulty = *value;
    }
    if (const auto value{stored_integer(level, "DifficultyLocked")};
        value.has_value()) {
        info.difficulty_locked = *value != 0;
    }
}

/// Replace one gameplay tag of a `level` compound. A field already stored as
/// TAG_Byte or TAG_Int keeps that width; otherwise `canonical` (the vanilla
/// type) is used.
[[nodiscard]] auto
set_gameplay_integer(data::nbt::value &level, std::string_view key,
                     std::int32_t value, data::nbt::tag_type canonical)
    -> status {
    auto kind{canonical};
    if (const auto *existing{level.get(key)}; existing != nullptr) {
        if (existing->is(data::nbt::tag_type::byte) ||
            existing->is(data::nbt::tag_type::int_)) {
            kind = existing->type();
        }
    }
    if (kind == data::nbt::tag_type::byte) {
        return level.set(std::string{key},
                         data::nbt::make_byte(static_cast<std::int8_t>(value)));
    }
    return level.set(std::string{key}, data::nbt::make_int(value));
}

[[nodiscard]] auto valid_dimension_component(std::string_view value) -> bool {
    return !value.empty() && value != "." && value != ".." &&
           value.find('\0') == std::string_view::npos &&
           value.find('/') == std::string_view::npos &&
           value.find('\\') == std::string_view::npos;
}

[[nodiscard]] auto region_path(const std::string &root, std::int32_t x,
                               std::int32_t z) -> std::string {
    return fmt::format("{}/regions/{}.{}.rfw", root, x, z);
}

[[nodiscard]] auto parse_region_name(std::string_view name)
    -> std::optional<std::pair<std::int32_t, std::int32_t>> {
    constexpr std::string_view suffix{".rfw"};
    if (name.size() <= suffix.size() ||
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return std::nullopt;
    }
    auto stem{name.substr(0, name.size() - suffix.size())};
    const auto dot{stem.find('.')};
    if (dot == std::string_view::npos) {
        return std::nullopt;
    }
    const auto parse =
        [](std::string_view text) -> std::optional<std::int32_t> {
        if (text.empty()) {
            return std::nullopt;
        }
        std::int32_t value{0};
        const auto parsed{
            std::from_chars(text.data(), text.data() + text.size(), value)};
        if (parsed.ec != std::errc{} ||
            parsed.ptr != text.data() + text.size()) {
            return std::nullopt;
        }
        return value;
    };
    const auto x{parse(stem.substr(0, dot))};
    const auto z{parse(stem.substr(dot + 1))};
    if (!x || !z) {
        return std::nullopt;
    }
    // Keep `region * 32 + local` inside int32 (chunk coordinates are int32).
    constexpr std::int32_t max_region_coordinate{67108863};
    if (*x < -max_region_coordinate - 1 || *x > max_region_coordinate ||
        *z < -max_region_coordinate - 1 || *z > max_region_coordinate) {
        return std::nullopt;
    }
    return std::pair{*x, *z};
}

[[nodiscard]] auto scan_tree(const std::string &directory, std::size_t depth,
                             std::size_t &files, std::uint64_t &bytes)
    -> status {
    if (!core::io::is_directory(directory)) {
        return status_ok();
    }
    auto entries{core::io::list_directory(directory)};
    if (!entries) {
        return std::unexpected{entries.error()};
    }
    for (const auto &name : *entries) {
        const auto child{join_path(directory, name)};
        if (core::io::is_symlink(child)) {
            continue;
        }
        if (core::io::is_directory(child)) {
            if (depth >= 8) {
                continue;
            }
            if (auto status{scan_tree(child, depth + 1, files, bytes)};
                !status) {
                return status;
            }
            continue;
        }
        const auto size{core::io::file_size(child)};
        if (!size) {
            return std::unexpected{size.error()};
        }
        ++files;
        bytes += *size;
    }
    return status_ok();
}

} // namespace

auto position_for_slot(std::int32_t region_x, std::int32_t region_z,
                       std::uint32_t slot, std::int32_t region_edge)
    -> chunk_pos {
    return chunk_pos{.x = region_x * region_edge +
                          static_cast<std::int32_t>(
                              slot % static_cast<std::uint32_t>(region_edge)),
                     .z = region_z * region_edge +
                          static_cast<std::int32_t>(
                              slot / static_cast<std::uint32_t>(region_edge))};
}

[[nodiscard]] auto check_dimension(const std::string &root,
                                   const std::string &label, bool repair,
                                   world_check &report,
                                   const rwf::zstd_dictionary *dictionary,
                                   const palette_table *palette) -> status {
    const auto regions{join_path(root, "regions")};
    if (core::io::is_directory(regions)) {
        auto names{core::io::list_directory(regions)};
        if (!names) {
            return std::unexpected{names.error()};
        }
        for (const auto &name : *names) {
            const auto coordinates{parse_region_name(name)};
            if (!coordinates) {
                continue;
            }
            ++report.regions;
            const auto path{join_path(regions, name)};
            const rwf::region_options options{.x = coordinates->first,
                                              .z = coordinates->second,
                                              .dictionary = dictionary};
            auto region_check{rwf::check_region(path, options, repair)};
            if (!region_check) {
                ++report.corrupt;
                report.problems.push_back(fmt::format(
                    "{}/{}: {}", label, name, region_check.error().message));
                continue;
            }
            report.chunks += region_check->chunks;
            report.garbage_bytes += region_check->garbage_bytes;
            report.repaired = report.repaired || region_check->repaired;
            // Problems that `--repair` already fixed (rebuilt index, dropped
            // damaged records) are not reported as remaining corruption.
            if (!region_check->repaired) {
                for (const auto &problem : region_check->problems) {
                    ++report.corrupt;
                    report.problems.push_back(
                        fmt::format("{}/{}: {}", label, name, problem));
                }
            }
            auto region{rwf::region_file::load(path, options)};
            if (!region) {
                ++report.corrupt;
                report.problems.push_back(fmt::format("{}/{}: {}", label, name,
                                                      region.error().message));
                continue;
            }
            const auto entries{region->entries()};
            for (std::uint32_t slot{0}; slot < entries.size(); ++slot) {
                if (entries[slot].empty()) {
                    continue;
                }
                const auto pos{position_for_slot(coordinates->first,
                                                 coordinates->second, slot,
                                                 rwf::region_edge)};
                auto payload{region->read(pos)};
                if (!payload) {
                    ++report.corrupt;
                    report.problems.push_back(fmt::format(
                        "{}/{} chunk {}: {}", label, name,
                        format_chunk_pos(pos), payload.error().message));
                    continue;
                }
                if (auto decoded{decode_chunk(*payload, palette)}; !decoded) {
                    ++report.corrupt;
                    report.problems.push_back(fmt::format(
                        "{}/{} chunk {}: {}", label, name,
                        format_chunk_pos(pos), decoded.error().message));
                }
            }
        }
    }
    rwf::blob_store blobs{join_path(root, "blobs")};
    for (const auto *kind : {"entities", "poi", "playerdata"}) {
        auto keys{blobs.list(kind)};
        if (!keys) {
            ++report.corrupt;
            report.problems.push_back(fmt::format("{}/blobs/{}: {}", label,
                                                  kind, keys.error().message));
            continue;
        }
        for (const auto &key : *keys) {
            auto payload{blobs.read(kind, key)};
            if (!payload) {
                ++report.corrupt;
                report.problems.push_back(fmt::format("{}/blobs/{}/{}: {}",
                                                      label, kind, key,
                                                      payload.error().message));
            }
        }
    }
    object_store objects{join_path(root, "objects")};
    auto paths{objects.list()};
    if (!paths) {
        ++report.corrupt;
        report.problems.push_back(
            fmt::format("{}/objects: {}", label, paths.error().message));
        return status_ok();
    }
    for (const auto &path : *paths) {
        auto payload{objects.read(path)};
        if (!payload) {
            ++report.corrupt;
            report.problems.push_back(fmt::format(
                "{}/objects/{}: {}", label, path, payload.error().message));
        }
    }
    return status_ok();
}

struct world_store::impl {
    /// Maximum simultaneously open region handles. Bounds descriptors and the
    /// 32 KiB per-region index cache for world-wide scans and long-running
    /// servers; callers only use the returned pointer before the next open.
    static constexpr std::size_t max_open_regions{64};

    std::string root;
    std::string overworld_root;
    int zstd_level{rwf::default_zstd_level};
    std::unique_ptr<rwf::blob_store> blobs;
    std::unique_ptr<object_store> objects;
    std::shared_ptr<const rwf::zstd_dictionary> dictionary;
    std::map<std::pair<std::string, std::string>,
             std::shared_ptr<const rwf::zstd_dictionary>>
        dimension_dictionaries;
    /// Dictionaries displaced by `install_*`. Open region handles keep a raw
    /// pointer into these (rwf.hpp `region_options`), so the store must retain
    /// them until the last region handle is destroyed.
    std::vector<std::shared_ptr<const rwf::zstd_dictionary>>
        retired_dictionaries;
    palette_table palette;
    bool palette_dirty{false};

    [[nodiscard]] static auto read_dictionary(const std::string &path)
        -> result<std::shared_ptr<const rwf::zstd_dictionary>> {
        auto bytes{rwf::read_record_file(path, rwf::max_record_bytes)};
        if (!bytes) {
            return std::unexpected{bytes.error()};
        }
        return std::make_shared<const rwf::zstd_dictionary>(
            rwf::make_zstd_dictionary(*bytes));
    }

    [[nodiscard]] auto load_palette() -> status {
        if (!objects->exists("palette")) {
            return status_ok();
        }
        auto bytes{objects->read("palette", max_object_bytes)};
        if (!bytes) {
            return std::unexpected{bytes.error()};
        }
        auto parsed{palette_table::parse(*bytes)};
        if (!parsed) {
            return std::unexpected{parsed.error()};
        }
        palette = std::move(*parsed);
        return status_ok();
    }

    [[nodiscard]] auto load_dictionary() -> status {
        const auto path{join_path(root, "dictionaries/chunk.zdict")};
        if (!core::io::exists(path)) {
            return status_ok();
        }
        auto loaded{read_dictionary(path)};
        if (!loaded) {
            return std::unexpected{loaded.error()};
        }
        dictionary = std::move(*loaded);
        return status_ok();
    }

    /// Load every `dimensions/<ns>/<dim>/dictionaries/chunk.zdict` so a
    /// corrupt or unreadable dictionary fails `open` instead of surfacing as
    /// a misleading "region requires dictionary" error later.
    [[nodiscard]] auto load_dimension_dictionaries() -> status {
        const auto dimensions_root{join_path(root, "dimensions")};
        if (!core::io::is_directory(dimensions_root)) {
            return status_ok();
        }
        auto namespaces{core::io::list_directory(dimensions_root)};
        if (!namespaces) {
            return std::unexpected{namespaces.error()};
        }
        for (const auto &namespace_name : *namespaces) {
            const auto namespace_root{
                join_path(dimensions_root, namespace_name)};
            if (core::io::is_symlink(namespace_root) ||
                !core::io::is_directory(namespace_root)) {
                continue;
            }
            auto names{core::io::list_directory(namespace_root)};
            if (!names) {
                return std::unexpected{names.error()};
            }
            for (const auto &name : *names) {
                const auto dimension_root{join_path(namespace_root, name)};
                if (core::io::is_symlink(dimension_root) ||
                    !core::io::is_directory(dimension_root)) {
                    continue;
                }
                const auto path{
                    join_path(dimension_root, "dictionaries/chunk.zdict")};
                if (!core::io::exists(path)) {
                    continue;
                }
                auto loaded{read_dictionary(path)};
                if (!loaded) {
                    return std::unexpected{loaded.error()};
                }
                dimension_dictionaries[{namespace_name, name}] =
                    std::move(*loaded);
            }
        }
        return status_ok();
    }

    [[nodiscard]] auto effective_dictionary(std::string_view namespace_name,
                                            std::string_view dimension) const
        -> const rwf::zstd_dictionary * {
        const auto found{dimension_dictionaries.find(
            {std::string{namespace_name}, std::string{dimension}})};
        if (found != dimension_dictionaries.end() && found->second != nullptr) {
            return found->second.get();
        }
        return dictionary.get();
    }

    std::map<std::pair<std::int32_t, std::int32_t>, rwf::region_file> regions;

    [[nodiscard]] auto region_for(std::int32_t x, std::int32_t z, bool create)
        -> result<rwf::region_file *> {
        const auto key{std::pair{x, z}};
        if (const auto found{regions.find(key)}; found != regions.end()) {
            return &found->second;
        }
        const auto path{region_path(overworld_root, x, z)};
        if (!create && !core::io::exists(path)) {
            return std::unexpected{make_error(
                error_code::not_found,
                fmt::format("region ({}, {}) does not exist", x, z))};
        }
        if (create) {
            // The regions directory is created with the world, but a world
            // that was moved, copied without its empty directories or
            // restored from a partial backup can lack it. Recreating it here
            // keeps that from turning every save into an io-writer error
            // while the process keeps pretending its world is writable.
            if (auto created{core::io::create_directories(
                    join_path(overworld_root, "regions"))};
                !created) {
                return std::unexpected{created.error()};
            }
        }
        auto opened{rwf::region_file::load_or_create(
            path,
            {.x = x,
             .z = z,
             .codec = rwf::compression::zstd,
             .zstd_level = zstd_level,
             .dictionary = effective_dictionary("minecraft", "overworld")})};
        if (!opened) {
            return std::unexpected{opened.error()};
        }
        // Bound open handles: a full-world scan (`stats`, `chunk_positions`,
        // export) walks every region, and a long-running server would otherwise
        // leak one descriptor per visited region. Callers use the returned
        // pointer immediately; eviction happens only on the next open.
        while (regions.size() >= max_open_regions) {
            auto victim{regions.begin()};
            if (auto closed{victim->second.close()}; !closed) {
                return std::unexpected{closed.error()};
            }
            regions.erase(victim);
        }
        const auto [position, inserted] =
            regions.emplace(key, std::move(*opened));
        static_cast<void>(inserted);
        return &position->second;
    }

    /// Encode and append one chunk. An empty `level` keeps the level the
    /// region handle was opened with (the pre-adaptive call path); a value
    /// overrides it for this record only.
    [[nodiscard]] auto write_chunk(const chunk_pos &pos, const chunk &value,
                                   std::optional<int> level) -> status {
        const auto before{palette.size()};
        auto payload{encode_chunk(value, &palette)};
        if (!payload) {
            return std::unexpected{payload.error()};
        }
        if (palette.size() != before) {
            palette_dirty = true;
        }
        auto region{region_for(pos.x >> 5, pos.z >> 5, true)};
        if (!region) {
            return std::unexpected{region.error()};
        }
        if (level.has_value()) {
            return (*region)->write(pos, value.status, *payload, *level);
        }
        return (*region)->write(pos, value.status, *payload);
    }

    [[nodiscard]] auto commit_all() -> status {
        // Palette first: chunk records in every dimension reference table ids,
        // so a crash after committing a region but before persisting the table
        // would leave chunks that cannot be decoded (and cannot be repaired).
        // Writing an as-yet-unused palette entry is harmless.
        bool palette_written{false};
        if (objects != nullptr && palette_dirty) {
            auto bytes{palette.serialize()};
            if (bytes.size() > max_palette_bytes) {
                return failing(
                    error_code::invalid_argument,
                    fmt::format("world palette is {} bytes (limit {}); run "
                                "`world palette-gc` to compact it",
                                bytes.size(), max_palette_bytes));
            }
            if (auto written{objects->write("palette", bytes,
                                            object_media::binary, false)};
                !written) {
                return written;
            }
            palette_written = true;
        }
        if (objects != nullptr) {
            if (auto committed{objects->commit()}; !committed) {
                return committed;
            }
            if (palette_written) {
                palette_dirty = false;
            }
        }
        for (auto &[key, region] : regions) {
            static_cast<void>(key);
            if (auto status{region.commit()}; !status) {
                return status;
            }
        }
        if (blobs != nullptr) {
            if (auto committed{blobs->commit()}; !committed) {
                return committed;
            }
        }
        if (objects != nullptr) {
            // Publish objects written by callers after the palette commit.
            return objects->commit();
        }
        return status_ok();
    }
};

world_store::world_store() noexcept = default;
world_store::~world_store() = default;
world_store::world_store(world_store &&other) noexcept = default;
auto world_store::operator=(world_store &&other) noexcept
    -> world_store & = default;
world_store::world_store(std::unique_ptr<impl> state)
    : impl_{std::move(state)} {}

auto world_store::create(std::string_view root) -> result<world_store> {
    if (root.empty()) {
        return std::unexpected{
            make_error(error_code::invalid_argument, "world path is empty")};
    }
    const std::string base{root};
    if (core::io::exists(base)) {
        if (!core::io::is_directory(base)) {
            return std::unexpected{make_error(
                error_code::already_exists,
                fmt::format("'{}' exists and is not a directory", base))};
        }
        auto entries{core::io::list_directory(base)};
        if (!entries) {
            return std::unexpected{entries.error()};
        }
        if (!entries->empty()) {
            return std::unexpected{
                make_error(error_code::already_exists,
                           fmt::format("'{}' is not empty", base))};
        }
    }
    if (auto status{core::io::create_directories(
            join_path(base, "dimensions/minecraft/overworld/regions"))};
        !status) {
        return std::unexpected{status.error()};
    }
    auto state{std::make_unique<impl>()};
    state->root = base;
    state->overworld_root = join_path(base, "dimensions/minecraft/overworld");
    state->blobs = std::make_unique<rwf::blob_store>(
        join_path(state->overworld_root, "blobs"));
    state->objects = std::make_unique<object_store>(join_path(base, "objects"));
    if (auto loaded{state->load_dictionary()}; !loaded) {
        return std::unexpected{loaded.error()};
    }
    if (auto loaded{state->load_dimension_dictionaries()}; !loaded) {
        return std::unexpected{loaded.error()};
    }
    if (auto loaded{state->load_palette()}; !loaded) {
        return std::unexpected{loaded.error()};
    }
    return world_store{std::move(state)};
}

auto world_store::open(std::string_view root) -> result<world_store> {
    if (root.empty()) {
        return std::unexpected{
            make_error(error_code::invalid_argument, "world path is empty")};
    }
    const std::string base{root};
    // Finish or roll back an interrupted quiesced palette swap before any
    // region handle is opened. A damaged/ambiguous marker fails closed with
    // parse_error/io_error instead of opening a half-swapped world.
    if (auto recovered{recover_palette_maintenance(base)}; !recovered) {
        return std::unexpected{recovered.error()};
    }
    if (!core::io::is_directory(base)) {
        return std::unexpected{
            make_error(error_code::not_found,
                       fmt::format("world '{}' is not a directory", base))};
    }
    auto state{std::make_unique<impl>()};
    state->root = base;
    state->overworld_root = join_path(base, "dimensions/minecraft/overworld");
    state->blobs = std::make_unique<rwf::blob_store>(
        join_path(state->overworld_root, "blobs"));
    state->objects = std::make_unique<object_store>(join_path(base, "objects"));
    if (auto loaded{state->load_dictionary()}; !loaded) {
        return std::unexpected{loaded.error()};
    }
    if (auto loaded{state->load_dimension_dictionaries()}; !loaded) {
        return std::unexpected{loaded.error()};
    }
    if (auto loaded{state->load_palette()}; !loaded) {
        return std::unexpected{loaded.error()};
    }
    return world_store{std::move(state)};
}

auto world_store::valid() const noexcept -> bool { return impl_ != nullptr; }

auto world_store::root() const -> const std::string & {
    static const std::string empty;
    return impl_ != nullptr ? impl_->root : empty;
}

auto world_store::read_meta() -> result<std::vector<std::byte>> {
    if (!valid()) {
        return std::unexpected{
            make_error(error_code::not_initialized, "world store is closed")};
    }
    return rwf::read_record_file(join_path(impl_->root, "meta.rfw"));
}

auto world_store::write_meta(std::span<const std::byte> payload) -> status {
    if (!valid()) {
        return failing(error_code::not_initialized, "world store is closed");
    }
    return rwf::write_record_file(join_path(impl_->root, "meta.rfw"), payload);
}

auto world_store::level() -> result<level_info> {
    auto payload{read_meta()};
    if (!payload) {
        return std::unexpected{payload.error()};
    }
    const auto document{data::nbt::read(*payload)};
    if (!document) {
        return std::unexpected{document.error()};
    }
    const auto *level{document->get("level")};
    if (level == nullptr || level->as_compound() == nullptr) {
        return std::unexpected{make_error(error_code::parse_error,
                                          "meta.rfw has no 'level' compound")};
    }
    level_info info{};
    if (const auto *version{level->get("DataVersion")}; version != nullptr) {
        if (const auto value{version->as_int()}) {
            info.data_version = *value;
        }
    }
    // 26.3 moved world generation settings into a saved-data file:
    // `data/minecraft/world_gen_settings.dat` (`{DataVersion, data:
    // {seed,...}}`).
    const auto apply_settings = [&info](const data::nbt::value &settings) {
        if (const auto *settings_data{settings.get("data")};
            settings_data != nullptr &&
            settings_data->as_compound() != nullptr) {
            if (const auto *seed{settings_data->get("seed")}; seed != nullptr) {
                if (const auto value{seed->as_long()}) {
                    info.seed = *value;
                } else if (const auto narrowed{seed->as_int()}) {
                    info.seed = *narrowed;
                }
            }
        }
    };
    const auto settings_object{"global/data/minecraft/world_gen_settings.dat"};
    if (impl_->objects->exists(settings_object)) {
        auto bytes{impl_->objects->read(settings_object)};
        if (bytes) {
            if (auto settings_document{data::nbt::read(*bytes)}) {
                apply_settings(*settings_document);
            }
        }
    }
    if (const auto *spawn{level->get("spawn")};
        spawn != nullptr && spawn->as_compound() != nullptr) {
        if (const auto *position{spawn->get("pos")}; position != nullptr) {
            if (const auto values{position->as_int_array()};
                values && values->size() >= 3) {
                info.spawn_x = (*values)[0];
                info.spawn_y = (*values)[1];
                info.spawn_z = (*values)[2];
            }
        }
    }
    if (const auto *time{level->get("Time")}; time != nullptr) {
        if (const auto value{time->as_long()}) {
            info.time = *value;
        } else if (const auto narrowed{time->as_int()}) {
            info.time = *narrowed;
        }
    }
    if (const auto *name{level->get("LevelName")}; name != nullptr) {
        if (const auto text{name->as_string()}) {
            info.level_name = std::string{*text};
        }
    }
    project_gameplay(*level, info);
    return info;
}

auto world_store::has_chunks() -> result<bool> {
    if (!valid()) {
        return std::unexpected{
            make_error(error_code::not_initialized, "world store is closed")};
    }
    const auto directory{join_path(impl_->overworld_root, "regions")};
    if (!core::io::is_directory(directory)) {
        return false;
    }
    auto names{core::io::list_directory(directory)};
    if (!names) {
        return std::unexpected{names.error()};
    }
    for (const auto &name : *names) {
        const auto coordinates{parse_region_name(name)};
        if (!coordinates) {
            continue;
        }
        auto region{
            impl_->region_for(coordinates->first, coordinates->second, false)};
        if (!region) {
            return std::unexpected{region.error()};
        }
        const auto entries{(*region)->entries()};
        const auto live{std::find_if(
            entries.begin(), entries.end(),
            [](const rwf::region_entry &entry) { return !entry.empty(); })};
        if (live != entries.end()) {
            return true;
        }
    }
    return false;
}

auto world_store::project_level() -> result<level_projection> {
    auto record{level()};
    if (!record) {
        return std::unexpected{record.error()};
    }
    auto chunks{has_chunks()};
    if (!chunks) {
        return std::unexpected{chunks.error()};
    }
    return level_projection{.level = std::move(*record), .has_chunks = *chunks};
}

auto world_store::read_gameplay() -> result<gameplay_settings> {
    auto info{level()};
    if (!info) {
        return std::unexpected{info.error()};
    }
    return gameplay_settings{
        .game_mode = info->game_mode,
        .hardcore = info->hardcore,
        .difficulty = info->difficulty,
        .difficulty_locked = info->difficulty_locked,
    };
}

auto world_store::write_gameplay(const gameplay_settings &settings) -> status {
    if (!valid()) {
        return failing(error_code::not_initialized, "world store is closed");
    }
    if (!valid_enumeration(settings.game_mode, max_game_mode)) {
        return failing(error_code::invalid_argument,
                       fmt::format("game mode {} is outside [0, {}]",
                                   settings.game_mode, max_game_mode));
    }
    if (!valid_enumeration(settings.difficulty, max_difficulty)) {
        return failing(error_code::invalid_argument,
                       fmt::format("difficulty {} is outside [0, {}]",
                                   settings.difficulty, max_difficulty));
    }
    const auto path{join_path(impl_->root, "meta.rfw")};
    if (!core::io::exists(path)) {
        return failing(error_code::not_found, "meta.rfw is missing");
    }
    auto payload{rwf::read_record_file(path)};
    if (!payload) {
        return std::unexpected{payload.error()};
    }
    auto document{data::nbt::read(*payload)};
    if (!document) {
        return std::unexpected{document.error()};
    }
    auto *level{document->get("level")};
    if (level == nullptr || level->as_compound() == nullptr) {
        return failing(error_code::parse_error,
                       "meta.rfw has no 'level' compound");
    }
    // Vanilla stores GameType as an int and the other three as bytes; an
    // existing byte/int width is kept so unrelated tags are not retyped.
    if (auto written{set_gameplay_integer(
            *level, "GameType", settings.game_mode, data::nbt::tag_type::int_)};
        !written) {
        return written;
    }
    if (auto written{set_gameplay_integer(*level, "hardcore",
                                          settings.hardcore ? 1 : 0,
                                          data::nbt::tag_type::byte)};
        !written) {
        return written;
    }
    if (auto written{set_gameplay_integer(*level, "Difficulty",
                                          settings.difficulty,
                                          data::nbt::tag_type::byte)};
        !written) {
        return written;
    }
    if (auto written{set_gameplay_integer(*level, "DifficultyLocked",
                                          settings.difficulty_locked ? 1 : 0,
                                          data::nbt::tag_type::byte)};
        !written) {
        return written;
    }
    auto bytes{data::nbt::write(*document, "", data::nbt::compression::none)};
    if (!bytes) {
        return std::unexpected{bytes.error()};
    }
    return rwf::write_record_file(path, *bytes);
}

auto world_store::validate_level() -> result<level_info> {
    auto payload{read_meta()};
    if (!payload) {
        return std::unexpected{payload.error()};
    }
    const auto document{data::nbt::read(*payload)};
    if (!document) {
        return std::unexpected{document.error()};
    }
    const auto *level_node{document->get("level")};
    if (level_node == nullptr || level_node->as_compound() == nullptr) {
        return std::unexpected{make_error(error_code::parse_error,
                                          "meta.rfw has no 'level' compound")};
    }
    const auto require = [](const data::nbt::value *field,
                            std::string_view name,
                            std::string_view expected) -> status {
        if (field == nullptr) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           fmt::format("meta.level.{} is missing (expected {})",
                                       name, expected))};
        }
        return status_ok();
    };
    const auto *version{level_node->get("DataVersion")};
    if (auto present{require(version, "DataVersion", "int")}; !present) {
        return std::unexpected{present.error()};
    }
    if (!version->as_int()) {
        return std::unexpected{make_error(
            error_code::parse_error, "meta.level.DataVersion must be an int")};
    }
    const auto *spawn{level_node->get("spawn")};
    if (auto present{require(spawn, "spawn", "compound")}; !present) {
        return std::unexpected{present.error()};
    }
    if (spawn->as_compound() == nullptr) {
        return std::unexpected{make_error(
            error_code::parse_error, "meta.level.spawn must be a compound")};
    }
    const auto *position{spawn->get("pos")};
    if (auto present{require(position, "spawn.pos", "int_array[3]")};
        !present) {
        return std::unexpected{present.error()};
    }
    const auto values{position->as_int_array()};
    if (!values || values->size() < 3U) {
        return std::unexpected{make_error(
            error_code::parse_error,
            "meta.level.spawn.pos must be an int array with at least 3 "
            "entries")};
    }
    const auto *time{level_node->get("Time")};
    if (auto present{require(time, "Time", "long or int")}; !present) {
        return std::unexpected{present.error()};
    }
    if (!time->as_long() && !time->as_int()) {
        return std::unexpected{make_error(
            error_code::parse_error, "meta.level.Time must be a long or int")};
    }
    const auto *name{level_node->get("LevelName")};
    if (auto present{require(name, "LevelName", "string")}; !present) {
        return std::unexpected{present.error()};
    }
    if (!name->as_string()) {
        return std::unexpected{make_error(
            error_code::parse_error, "meta.level.LevelName must be a string")};
    }

    const auto settings_object{"global/data/minecraft/world_gen_settings.dat"};
    if (impl_->objects->exists(settings_object)) {
        auto bytes{impl_->objects->read(settings_object)};
        if (!bytes) {
            return std::unexpected{bytes.error()};
        }
        auto settings{data::nbt::read(*bytes)};
        if (!settings) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "world_gen_settings.dat is not valid NBT")};
        }
        const auto *settings_data{settings->get("data")};
        if (settings_data == nullptr ||
            settings_data->as_compound() == nullptr) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "world_gen_settings.dat has no 'data' compound")};
        }
        const auto *seed{settings_data->get("seed")};
        if (seed == nullptr || (!seed->as_long() && !seed->as_int())) {
            return std::unexpected{make_error(
                error_code::parse_error,
                "world_gen_settings.dat data.seed must be a long or int")};
        }
    }
    return level();
}

auto world_store::palette_usage_stats() -> result<palette_usage> {
    auto positions{chunk_positions()};
    if (!positions) {
        return std::unexpected{positions.error()};
    }
    std::set<std::uint32_t> referenced{};
    for (const auto &pos : *positions) {
        auto chunk{read_chunk(pos)};
        if (!chunk) {
            return std::unexpected{chunk.error()};
        }
        const auto mark = [this, &referenced](std::string_view key) {
            if (const auto *id{impl_->palette.find(key)}; id != nullptr) {
                referenced.insert(*id);
            }
        };
        for (const auto &section : chunk->sections) {
            for (const auto &entry : section.palette) {
                mark(entry.key);
            }
            for (const auto &biome : section.biome_palette) {
                mark(biome);
            }
        }
    }
    return palette_usage{
        .table_entries = impl_->palette.size(),
        .referenced_entries = referenced.size(),
        .chunks = positions->size(),
    };
}

auto world_store::read_chunk(const chunk_pos &pos) -> result<chunk> {
    if (!valid()) {
        return std::unexpected{
            make_error(error_code::not_initialized, "world store is closed")};
    }
    auto region{impl_->region_for(pos.x >> 5, pos.z >> 5, false)};
    if (!region) {
        return std::unexpected{region.error()};
    }
    auto payload{(*region)->read(pos)};
    if (!payload) {
        return std::unexpected{payload.error()};
    }
    return decode_chunk(*payload, &impl_->palette);
}

auto world_store::write_chunk(const chunk_pos &pos, const chunk &value)
    -> status {
    if (!valid()) {
        return failing(error_code::not_initialized, "world store is closed");
    }
    return impl_->write_chunk(pos, value, std::nullopt);
}

auto world_store::write_chunk(const chunk_pos &pos, const chunk &value,
                              int zstd_level) -> status {
    if (!valid()) {
        return failing(error_code::not_initialized, "world store is closed");
    }
    if (!rwf::is_valid_zstd_level(zstd_level)) {
        return failing(error_code::invalid_argument,
                       fmt::format("zstd level {} is outside [{}, {}]",
                                   zstd_level, rwf::min_zstd_level,
                                   rwf::max_zstd_level));
    }
    return impl_->write_chunk(pos, value, zstd_level);
}

auto world_store::contains(const chunk_pos &pos) -> bool {
    if (!valid()) {
        return false;
    }
    auto region{impl_->region_for(pos.x >> 5, pos.z >> 5, false)};
    return region && (*region)->contains(pos);
}

auto world_store::chunk_positions() -> result<std::vector<chunk_pos>> {
    if (!valid()) {
        return std::unexpected{
            make_error(error_code::not_initialized, "world store is closed")};
    }
    std::vector<chunk_pos> positions;
    const auto directory{join_path(impl_->overworld_root, "regions")};
    if (!core::io::is_directory(directory)) {
        return positions;
    }
    auto names{core::io::list_directory(directory)};
    if (!names) {
        return std::unexpected{names.error()};
    }
    for (const auto &name : *names) {
        const auto coordinates{parse_region_name(name)};
        if (!coordinates) {
            continue;
        }
        auto region{
            impl_->region_for(coordinates->first, coordinates->second, false)};
        if (!region) {
            return std::unexpected{region.error()};
        }
        const auto entries{(*region)->entries()};
        for (std::uint32_t slot{0}; slot < entries.size(); ++slot) {
            if (entries[slot].empty()) {
                continue;
            }
            positions.push_back(position_for_slot(coordinates->first,
                                                  coordinates->second, slot,
                                                  rwf::region_edge));
        }
    }
    std::sort(positions.begin(), positions.end());
    return positions;
}

auto world_store::commit() -> status {
    if (!valid()) {
        return failing(error_code::not_initialized, "world store is closed");
    }
    return impl_->commit_all();
}

auto world_store::blobs() -> rwf::blob_store & { return *impl_->blobs; }

auto world_store::set_zstd_level(int level) -> status {
    if (!valid()) {
        return std::unexpected{
            make_error(error_code::not_initialized, "world store is closed")};
    }
    if (level < rwf::min_zstd_level || level > rwf::max_zstd_level) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       fmt::format("zstd level {} is outside [{}, {}]", level,
                                   rwf::min_zstd_level, rwf::max_zstd_level))};
    }
    impl_->zstd_level = level;
    return status_ok();
}

auto world_store::zstd_level() const noexcept -> int {
    return impl_ != nullptr ? impl_->zstd_level : rwf::default_zstd_level;
}

auto world_store::compact_regions(std::uint64_t min_bytes, unsigned min_percent)
    -> result<std::size_t> {
    if (!valid()) {
        return std::unexpected{
            make_error(error_code::not_initialized, "world store is closed")};
    }
    const auto should_compact = [min_bytes,
                                 min_percent](const rwf::region_stats &stats) {
        if (stats.garbage_bytes < min_bytes) {
            return false;
        }
        return stats.file_bytes == 0 ||
               stats.garbage_bytes * 100U >=
                   stats.file_bytes * static_cast<std::uint64_t>(min_percent);
    };

    std::set<std::string> open_paths{};
    std::size_t compacted{0};
    for (auto &[key, region] : impl_->regions) {
        open_paths.insert(
            region_path(impl_->overworld_root, key.first, key.second));
        auto stats{region.stats()};
        if (!stats) {
            return std::unexpected{stats.error()};
        }
        if (!should_compact(*stats)) {
            continue;
        }
        auto rewritten{region.compact()};
        if (!rewritten) {
            return std::unexpected{rewritten.error()};
        }
        ++compacted;
    }

    // Closed handles (dimensions other than the currently open overworld
    // regions) are compacted by scanning `dimensions/<ns>/<dim>/regions`.
    const auto dimensions_root{join_path(impl_->root, "dimensions")};
    if (core::io::is_directory(dimensions_root)) {
        auto namespaces{core::io::list_directory(dimensions_root)};
        if (!namespaces) {
            return std::unexpected{namespaces.error()};
        }
        for (const auto &namespace_name : *namespaces) {
            const auto namespace_root{
                join_path(dimensions_root, namespace_name)};
            if (core::io::is_symlink(namespace_root) ||
                !core::io::is_directory(namespace_root)) {
                continue;
            }
            auto names{core::io::list_directory(namespace_root)};
            if (!names) {
                return std::unexpected{names.error()};
            }
            for (const auto &name : *names) {
                const auto regions_dir{
                    join_path(namespace_root, name + "/regions")};
                if (!core::io::is_directory(regions_dir)) {
                    continue;
                }
                auto files{core::io::list_directory(regions_dir)};
                if (!files) {
                    return std::unexpected{files.error()};
                }
                for (const auto &file : *files) {
                    const auto slot{parse_region_name(file)};
                    if (!slot) {
                        continue;
                    }
                    const auto path{join_path(regions_dir, file)};
                    if (open_paths.contains(path)) {
                        continue;
                    }
                    auto region{rwf::region_file::load_or_create(
                        path, {.x = slot->first,
                               .z = slot->second,
                               .codec = rwf::compression::zstd,
                               .dictionary = impl_->effective_dictionary(
                                   namespace_name, name)})};
                    if (!region) {
                        return std::unexpected{region.error()};
                    }
                    auto stats{region->stats()};
                    if (!stats) {
                        return std::unexpected{stats.error()};
                    }
                    if (!should_compact(*stats)) {
                        if (auto closed{region->close()}; !closed) {
                            return std::unexpected{closed.error()};
                        }
                        continue;
                    }
                    auto rewritten{region->compact()};
                    if (!rewritten) {
                        return std::unexpected{rewritten.error()};
                    }
                    ++compacted;
                    if (auto closed{region->close()}; !closed) {
                        return std::unexpected{closed.error()};
                    }
                }
            }
        }
    }
    return compacted;
}

auto world_store::compact() -> result<world_compact> {
    if (!valid()) {
        return std::unexpected{
            make_error(error_code::not_initialized, "world store is closed")};
    }
    auto objects{impl_->objects->compact()};
    if (!objects) {
        return std::unexpected{objects.error()};
    }
    auto blobs{impl_->blobs->compact()};
    if (!blobs) {
        return std::unexpected{blobs.error()};
    }
    world_compact report{.objects = *objects, .blobs = *blobs};
    // Every non-overworld dimension has its own blobs/objects store; compact
    // them too instead of silently leaving their dead segments behind.
    const auto dimensions_root{join_path(impl_->root, "dimensions")};
    if (core::io::is_directory(dimensions_root)) {
        auto namespaces{core::io::list_directory(dimensions_root)};
        if (!namespaces) {
            return std::unexpected{namespaces.error()};
        }
        for (const auto &namespace_name : *namespaces) {
            auto names{core::io::list_directory(
                join_path(dimensions_root, namespace_name))};
            if (!names) {
                return std::unexpected{names.error()};
            }
            for (const auto &name : *names) {
                if (namespace_name == "minecraft" && name == "overworld") {
                    continue;
                }
                const auto dimension_root{
                    join_path(dimensions_root, namespace_name + "/" + name)};
                const auto objects_dir{join_path(dimension_root, "objects")};
                if (core::io::is_directory(objects_dir)) {
                    object_store dimension_objects{objects_dir};
                    auto compacted{dimension_objects.compact()};
                    if (!compacted) {
                        return std::unexpected{compacted.error()};
                    }
                    report.objects.objects += compacted->objects;
                    report.objects.segments += compacted->segments;
                    report.objects.live_bytes += compacted->live_bytes;
                    report.objects.garbage_bytes += compacted->garbage_bytes;
                }
                const auto blobs_dir{join_path(dimension_root, "blobs")};
                if (core::io::is_directory(blobs_dir)) {
                    rwf::blob_store dimension_blobs{blobs_dir};
                    auto compacted{dimension_blobs.compact()};
                    if (!compacted) {
                        return std::unexpected{compacted.error()};
                    }
                    report.blobs.objects += compacted->objects;
                    report.blobs.segments += compacted->segments;
                    report.blobs.live_bytes += compacted->live_bytes;
                    report.blobs.garbage_bytes += compacted->garbage_bytes;
                }
            }
        }
    }
    return report;
}

auto world_store::objects() -> object_store & { return *impl_->objects; }

auto world_store::install_dictionary(std::span<const std::byte> bytes)
    -> status {
    if (!valid()) {
        return failing(error_code::not_initialized, "world store is closed");
    }
    auto dictionary{rwf::make_zstd_dictionary(bytes)};
    if (auto created{core::io::create_directories(
            join_path(impl_->root, "dictionaries"))};
        !created) {
        return created;
    }
    if (auto written{rwf::write_record_file(
            join_path(impl_->root, "dictionaries/chunk.zdict"), bytes,
            rwf::compression::zstd, 3, true)};
        !written) {
        return written;
    }
    if (impl_->dictionary != nullptr) {
        impl_->retired_dictionaries.push_back(impl_->dictionary);
    }
    impl_->dictionary =
        std::make_shared<const rwf::zstd_dictionary>(std::move(dictionary));
    return status_ok();
}

auto world_store::install_dimension_dictionary(std::string_view namespace_name,
                                               std::string_view dimension,
                                               std::span<const std::byte> bytes)
    -> status {
    if (!valid()) {
        return failing(error_code::not_initialized, "world store is closed");
    }
    if (!valid_dimension_component(namespace_name) ||
        !valid_dimension_component(dimension)) {
        return failing(error_code::invalid_argument,
                       "dimension name is not a single path component");
    }
    const auto directory{fmt::format("{}/dimensions/{}/{}/dictionaries",
                                     impl_->root, namespace_name, dimension)};
    if (auto created{core::io::create_directories(directory)}; !created) {
        return created;
    }
    if (auto written{
            rwf::write_record_file(fmt::format("{}/chunk.zdict", directory),
                                   bytes, rwf::compression::zstd, 3, true)};
        !written) {
        return written;
    }
    auto dictionary{rwf::make_zstd_dictionary(bytes)};
    auto &slot{impl_->dimension_dictionaries[{std::string{namespace_name},
                                              std::string{dimension}}]};
    if (slot != nullptr) {
        impl_->retired_dictionaries.push_back(std::move(slot));
    }
    slot = std::make_shared<const rwf::zstd_dictionary>(std::move(dictionary));
    return status_ok();
}

auto world_store::dictionary() const -> const rwf::zstd_dictionary * {
    return impl_ != nullptr ? impl_->dictionary.get() : nullptr;
}

auto world_store::dictionary_for(std::string_view namespace_name,
                                 std::string_view dimension) const
    -> const rwf::zstd_dictionary * {
    if (impl_ == nullptr) {
        return nullptr;
    }
    return impl_->effective_dictionary(namespace_name, dimension);
}

auto world_store::install_palette(palette_table table) -> status {
    if (!valid()) {
        return failing(error_code::not_initialized, "world store is closed");
    }
    if (table.serialize().size() > max_palette_bytes) {
        return failing(error_code::invalid_argument,
                       fmt::format("world palette exceeds the {} byte limit; "
                                   "cannot install it",
                                   max_palette_bytes));
    }
    impl_->palette = std::move(table);
    impl_->palette_dirty = true;
    return status_ok();
}

auto world_store::palette() const -> const palette_table & {
    static const palette_table empty;
    return impl_ != nullptr ? impl_->palette : empty;
}

auto world_store::stats() -> result<world_stats> {
    if (!valid()) {
        return std::unexpected{
            make_error(error_code::not_initialized, "world store is closed")};
    }
    world_stats report{};
    const auto dimensions_root{join_path(impl_->root, "dimensions")};
    if (core::io::is_directory(dimensions_root)) {
        auto namespaces{core::io::list_directory(dimensions_root)};
        if (!namespaces) {
            return std::unexpected{namespaces.error()};
        }
        for (const auto &namespace_name : *namespaces) {
            const auto namespace_root{
                join_path(dimensions_root, namespace_name)};
            if (core::io::is_symlink(namespace_root) ||
                !core::io::is_directory(namespace_root)) {
                continue;
            }
            auto names{core::io::list_directory(namespace_root)};
            if (!names) {
                return std::unexpected{names.error()};
            }
            for (const auto &name : *names) {
                const auto dimension_root{join_path(namespace_root, name)};
                if (core::io::is_symlink(dimension_root) ||
                    !core::io::is_directory(dimension_root)) {
                    continue;
                }
                ++report.dimensions;
                const auto regions_dir{join_path(dimension_root, "regions")};
                if (core::io::is_directory(regions_dir)) {
                    auto region_names{core::io::list_directory(regions_dir)};
                    if (!region_names) {
                        return std::unexpected{region_names.error()};
                    }
                    for (const auto &region_name : *region_names) {
                        const auto coordinates{parse_region_name(region_name)};
                        if (!coordinates) {
                            continue;
                        }
                        auto region{rwf::region_file::load(
                            join_path(regions_dir, region_name),
                            {.x = coordinates->first,
                             .z = coordinates->second,
                             .dictionary =
                                 dictionary_for(namespace_name, name)})};
                        if (!region) {
                            return std::unexpected{region.error()};
                        }
                        auto region_stats{region->stats()};
                        if (!region_stats) {
                            return std::unexpected{region_stats.error()};
                        }
                        ++report.regions;
                        report.chunks += region_stats->chunks;
                    }
                }
                const auto blobs_dir{join_path(dimension_root, "blobs")};
                if (core::io::is_directory(blobs_dir)) {
                    rwf::blob_store dimension_blobs{blobs_dir};
                    for (const auto *kind : {"entities", "poi", "playerdata"}) {
                        auto keys{dimension_blobs.list(kind)};
                        if (!keys) {
                            return std::unexpected{keys.error()};
                        }
                        if (std::string_view{kind} == "entities") {
                            report.entity_chunks += keys->size();
                        } else if (std::string_view{kind} == "poi") {
                            report.poi_chunks += keys->size();
                        } else {
                            report.playerdata += keys->size();
                        }
                    }
                }
            }
        }
    }
    auto object_paths{impl_->objects->list()};
    if (!object_paths) {
        return std::unexpected{object_paths.error()};
    }
    report.objects = object_paths->size();
    const auto count_objects =
        [this](std::string_view prefix) -> result<std::size_t> {
        auto paths{impl_->objects->list(prefix)};
        if (!paths) {
            return std::unexpected{paths.error()};
        }
        return paths->size();
    };
    auto datapacks{count_objects("datapacks/")};
    if (!datapacks) {
        return std::unexpected{datapacks.error()};
    }
    report.datapack_files = *datapacks;
    std::size_t aux_files{0};
    for (const auto *prefix : {"global/data/", "overworld/data/", "assets/"}) {
        auto count{count_objects(prefix)};
        if (!count) {
            return std::unexpected{count.error()};
        }
        aux_files += *count;
    }
    report.aux_files = aux_files;
    std::size_t total_files{0};
    std::uint64_t total_bytes{0};
    if (auto status{scan_tree(impl_->root, 0, total_files, total_bytes)};
        !status) {
        return std::unexpected{status.error()};
    }
    report.bytes = total_bytes;
    return report;
}
auto world_store::check(bool repair) -> result<world_check> {
    if (!valid()) {
        return std::unexpected{
            make_error(error_code::not_initialized, "world store is closed")};
    }
    world_check report{};
    const auto directory{join_path(impl_->overworld_root, "regions")};
    if (core::io::is_directory(directory)) {
        auto names{core::io::list_directory(directory)};
        if (!names) {
            return std::unexpected{names.error()};
        }
        for (const auto &name : *names) {
            const auto coordinates{parse_region_name(name)};
            if (!coordinates) {
                continue;
            }
            ++report.regions;
            const auto path{region_path(impl_->overworld_root,
                                        coordinates->first,
                                        coordinates->second)};
            const rwf::region_options options{
                .x = coordinates->first,
                .z = coordinates->second,
                .dictionary = dictionary_for("minecraft", "overworld")};
            auto region_check{rwf::check_region(path, options, repair)};
            if (!region_check) {
                ++report.corrupt;
                report.problems.push_back(
                    fmt::format("{}: {}", name, region_check.error().message));
                continue;
            }
            report.chunks += region_check->chunks;
            report.garbage_bytes += region_check->garbage_bytes;
            if (region_check->repaired) {
                report.repaired = true;
            }
            if (!region_check->repaired && !region_check->problems.empty()) {
                ++report.corrupt;
                for (const auto &problem : region_check->problems) {
                    report.problems.push_back(
                        fmt::format("{}: {}", name, problem));
                }
            }
            auto region{rwf::region_file::load(path, options)};
            if (!region) {
                ++report.corrupt;
                report.problems.push_back(
                    fmt::format("{}: {}", name, region.error().message));
                continue;
            }
            const auto entries{region->entries()};
            for (std::uint32_t slot{0}; slot < entries.size(); ++slot) {
                if (entries[slot].empty()) {
                    continue;
                }
                const auto pos{position_for_slot(coordinates->first,
                                                 coordinates->second, slot,
                                                 rwf::region_edge)};
                auto payload{region->read(pos)};
                if (!payload) {
                    ++report.corrupt;
                    report.problems.push_back(fmt::format(
                        "{}: chunk {}: {}", name, format_chunk_pos(pos),
                        payload.error().message));
                    continue;
                }
                if (auto decoded{decode_chunk(*payload, &impl_->palette)};
                    !decoded) {
                    ++report.corrupt;
                    report.problems.push_back(fmt::format(
                        "{}: chunk {}: {}", name, format_chunk_pos(pos),
                        decoded.error().message));
                }
            }
        }
    }
    report.dimensions = 1;
    const auto dimensions_root{join_path(impl_->root, "dimensions")};
    if (core::io::is_directory(dimensions_root)) {
        auto namespaces{core::io::list_directory(dimensions_root)};
        if (!namespaces) {
            return std::unexpected{namespaces.error()};
        }
        for (const auto &namespace_name : *namespaces) {
            auto names{core::io::list_directory(
                join_path(dimensions_root, namespace_name))};
            if (!names) {
                return std::unexpected{names.error()};
            }
            for (const auto &name : *names) {
                if (namespace_name == "minecraft" && name == "overworld") {
                    continue;
                }
                const auto label{fmt::format("{}:{}", namespace_name, name)};
                if (auto checked{check_dimension(
                        join_path(dimensions_root, namespace_name + "/" + name),
                        label, repair, report,
                        dictionary_for(namespace_name, name), &impl_->palette)};
                    !checked) {
                    return std::unexpected{checked.error()};
                }
                ++report.dimensions;
            }
        }
    }
    const auto meta_path{join_path(impl_->root, "meta.rfw")};
    if (!core::io::exists(meta_path)) {
        ++report.corrupt;
        report.problems.push_back("meta.rfw is missing");
    } else {
        auto payload{rwf::read_record_file(meta_path)};
        if (!payload) {
            ++report.corrupt;
            report.problems.push_back(
                fmt::format("meta.rfw: {}", payload.error().message));
        } else if (const auto document{data::nbt::read(*payload)}; !document) {
            ++report.corrupt;
            report.problems.push_back(
                fmt::format("meta.rfw: {}", document.error().message));
        }
    }
    auto object_paths{impl_->objects->list()};
    if (!object_paths) {
        ++report.corrupt;
        report.problems.push_back(
            fmt::format("objects: {}", object_paths.error().message));
    } else {
        for (const auto &path : *object_paths) {
            auto payload{impl_->objects->read(path)};
            if (!payload) {
                ++report.corrupt;
                report.problems.push_back(fmt::format("objects/{}: {}", path,
                                                      payload.error().message));
            }
        }
    }
    for (const auto *kind : {"entities", "poi", "playerdata"}) {
        auto keys{impl_->blobs->list(kind)};
        if (!keys) {
            ++report.corrupt;
            report.problems.push_back(
                fmt::format("blobs/{}: {}", kind, keys.error().message));
            continue;
        }
        for (const auto &key : *keys) {
            auto payload{impl_->blobs->read(kind, key)};
            if (!payload) {
                ++report.corrupt;
                report.problems.push_back(fmt::format(
                    "blobs/{}/{}: {}", kind, key, payload.error().message));
            }
        }
    }
    return report;
}

} // namespace refrain::world
