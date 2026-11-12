// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/world/create.hpp>

#include <refrain/core/io.hpp>
#include <refrain/data/nbt.hpp>
#include <refrain/world/anvil.hpp>
#include <refrain/world/rwf.hpp>
#include <refrain/world/store.hpp>

#include <fmt/format.h>

#include <string>

namespace refrain::world {

namespace {

constexpr std::int32_t initial_spawn_y = 64;

[[nodiscard]] auto join_path(std::string_view root, std::string_view leaf)
    -> std::string {
    return fmt::format("{}/{}", root, leaf);
}

void set_child(data::nbt::value &parent, std::string key,
               data::nbt::value child) {
    static_cast<void>(parent.set(std::move(key), std::move(child)));
}

/// The `level` compound `world_store::validate_level` requires: a data
/// version, a spawn position, a game time and a level name. The generator
/// settings object carries the seed separately.
[[nodiscard]] auto level_document(std::int64_t seed, std::string_view name)
    -> data::nbt::value {
    auto level{data::nbt::make_compound()};
    set_child(level, "DataVersion",
              data::nbt::make_int(anvil::supported_data_version));
    set_child(level, "LevelName", data::nbt::make_string(std::string{name}));
    set_child(level, "RandomSeed", data::nbt::make_long(seed));
    set_child(level, "Time", data::nbt::make_long(0));
    set_child(level, "DayTime", data::nbt::make_long(0));
    auto spawn{data::nbt::make_compound()};
    set_child(
        spawn, "pos",
        data::nbt::make_int_array(data::nbt::int_array{0, initial_spawn_y, 0}));
    set_child(level, "spawn", std::move(spawn));
    return level;
}

[[nodiscard]] auto meta_document(const data::nbt::value &level)
    -> data::nbt::value {
    auto meta{data::nbt::make_compound()};
    set_child(meta, "format", data::nbt::make_string(std::string{meta_format}));
    set_child(meta, "data_version",
              data::nbt::make_int(anvil::supported_data_version));
    set_child(meta, "imported_from", data::nbt::make_string("new"));
    set_child(meta, "gamepack_id", data::nbt::make_string(""));
    set_child(meta, "mods_lock_fingerprint", data::nbt::make_string(""));
    set_child(meta, "level", level);
    return meta;
}

/// `global/data/minecraft/world_gen_settings.dat`: the seed the generator
/// reads back. Vanilla keeps it under `data.seed`.
[[nodiscard]] auto generator_settings(std::int64_t seed) -> data::nbt::value {
    auto data{data::nbt::make_compound()};
    set_child(data, "seed", data::nbt::make_long(seed));
    auto root{data::nbt::make_compound()};
    set_child(root, "data", std::move(data));
    return root;
}

} // namespace

auto create_world(std::string_view root, std::int64_t seed,
                  std::string_view level_name) -> status {
    if (root.empty()) {
        return std::unexpected{
            make_error(error_code::invalid_argument, "world path is empty")};
    }
    const std::string base{root};
    if (core::io::exists(join_path(base, "meta.rfw"))) {
        return std::unexpected{
            make_error(error_code::already_exists,
                       fmt::format("world '{}' already exists", base))};
    }
    for (const auto *directory : {"", "dimensions/minecraft/overworld/regions",
                                  "dimensions/minecraft/overworld/blobs",
                                  "objects", "global/data/minecraft"}) {
        const auto path{directory[0] == '\0' ? base
                                             : join_path(base, directory)};
        if (auto created{core::io::create_directories(path)}; !created) {
            return created;
        }
    }

    const auto level{level_document(seed, level_name)};
    auto meta_bytes{data::nbt::write(meta_document(level), "",
                                     data::nbt::compression::none)};
    if (!meta_bytes) {
        return std::unexpected{meta_bytes.error()};
    }
    // `meta.rfw` is an RWF record file, not raw NBT: the store reads it back
    // through the same record reader as every other metadata payload.
    if (auto written{
            rwf::write_record_file(join_path(base, "meta.rfw"), *meta_bytes)};
        !written) {
        return written;
    }
    // The generator seed lives in the object store, so the store has to be
    // open before it can be written.
    auto store{world_store::open(base)};
    if (!store) {
        return std::unexpected{store.error()};
    }
    auto settings_bytes{data::nbt::write(generator_settings(seed), "",
                                         data::nbt::compression::none)};
    if (!settings_bytes) {
        return std::unexpected{settings_bytes.error()};
    }
    return store->objects().write(
        "global/data/minecraft/world_gen_settings.dat", *settings_bytes,
        object_media::nbt);
}

} // namespace refrain::world
