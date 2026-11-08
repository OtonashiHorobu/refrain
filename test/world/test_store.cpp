// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/error.hpp>
#include <refrain/core/io.hpp>
#include <refrain/data/nbt.hpp>
#include <refrain/world/store.hpp>

#include <boost/ut.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace boost::ut;
namespace nbt = refrain::data::nbt;
namespace world = refrain::world;
namespace io = refrain::core::io;
namespace fs = std::filesystem;

using nbt::tag_type;
using nbt::value;

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
                base / ("refrain-store-" + std::to_string(sequence.fetch_add(
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
    [[nodiscard]] auto sub(std::string_view name) const -> std::string {
        return (path_ / std::string{name}).string();
    }

  private:
    fs::path path_;
};

void set(value &parent, std::string key, value child) {
    expect(static_cast<bool>(parent.set(std::move(key), std::move(child))));
}

[[nodiscard]] auto make_level() -> value {
    value level{nbt::make_compound()};
    set(level, "DataVersion", nbt::make_int(5023));
    set(level, "LevelName", nbt::make_string("Store Test"));
    set(level, "Time", nbt::make_long(6000));
    value spawn{nbt::make_compound()};
    set(spawn, "pos", nbt::make_int_array({1, 2, 3}));
    set(level, "spawn", std::move(spawn));
    return level;
}

[[nodiscard]] auto make_meta(value level) -> value {
    value meta{nbt::make_compound()};
    set(meta, "format", nbt::make_string(std::string{world::meta_format}));
    set(meta, "level", std::move(level));
    return meta;
}

[[nodiscard]] auto write_meta(world::world_store &store, const value &meta)
    -> bool {
    auto bytes{nbt::write(meta, "", nbt::compression::none)};
    if (!bytes) {
        return false;
    }
    return static_cast<bool>(store.write_meta(*bytes));
}

[[nodiscard]] auto read_meta(world::world_store &store)
    -> std::optional<value> {
    auto bytes{store.read_meta()};
    if (!bytes) {
        return std::nullopt;
    }
    auto document{nbt::read(*bytes)};
    if (!document) {
        return std::nullopt;
    }
    return std::move(*document);
}

[[nodiscard]] auto meta_path(world::world_store &store) -> std::string {
    return store.root() + "/meta.rfw";
}

} // namespace

suite world_store_gameplay = [] {
    "missing_gameplay_tags_use_defaults"_test = [] {
        const temp_directory directory;
        auto store{world::world_store::create(directory.sub("world-defaults"))};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        expect(write_meta(*store, make_meta(make_level())));

        const auto level{store->level()};
        expect(static_cast<bool>(level));
        if (level) {
            expect(level->game_mode == 1);
            expect(!level->hardcore);
            expect(level->difficulty == 2);
            expect(!level->difficulty_locked);
        }

        const auto gameplay{store->read_gameplay()};
        expect(static_cast<bool>(gameplay));
        if (gameplay) {
            expect(gameplay->game_mode == 1);
            expect(!gameplay->hardcore);
            expect(gameplay->difficulty == 2);
            expect(!gameplay->difficulty_locked);
        }
    };

    "vanilla_gameplay_storage_is_projected"_test = [] {
        const temp_directory directory;
        auto store{world::world_store::create(directory.sub("world-types"))};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }

        auto level{make_level()};
        set(level, "GameType", nbt::make_int(2));
        set(level, "hardcore", nbt::make_byte(1));
        set(level, "Difficulty", nbt::make_byte(3));
        set(level, "DifficultyLocked", nbt::make_byte(1));
        expect(write_meta(*store, make_meta(std::move(level))));
        auto canonical{store->read_gameplay()};
        expect(static_cast<bool>(canonical));
        if (canonical) {
            expect(canonical->game_mode == 2);
            expect(canonical->hardcore);
            expect(canonical->difficulty == 3);
            expect(canonical->difficulty_locked);
        }

        // Byte- and int-stored variants of the same fields are accepted.
        auto narrow{make_level()};
        set(narrow, "GameType", nbt::make_byte(3));
        set(narrow, "hardcore", nbt::make_int(0));
        set(narrow, "Difficulty", nbt::make_int(1));
        set(narrow, "DifficultyLocked", nbt::make_int(0));
        expect(write_meta(*store, make_meta(std::move(narrow))));
        auto projected{store->read_gameplay()};
        expect(static_cast<bool>(projected));
        if (projected) {
            expect(projected->game_mode == 3);
            expect(!projected->hardcore);
            expect(projected->difficulty == 1);
            expect(!projected->difficulty_locked);
        }
    };

    "out_of_range_gameplay_values_keep_defaults"_test = [] {
        const temp_directory directory;
        auto store{world::world_store::create(directory.sub("world-range"))};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        auto level{make_level()};
        set(level, "GameType", nbt::make_int(9));
        set(level, "Difficulty", nbt::make_byte(-1));
        expect(write_meta(*store, make_meta(std::move(level))));
        const auto gameplay{store->read_gameplay()};
        expect(static_cast<bool>(gameplay));
        if (gameplay) {
            expect(gameplay->game_mode == 1);
            expect(gameplay->difficulty == 2);
        }
    };

    "write_gameplay_roundtrip"_test = [] {
        const temp_directory directory;
        auto store{world::world_store::create(directory.sub("world-write"))};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        expect(write_meta(*store, make_meta(make_level())));

        const world::gameplay_settings creative{
            .game_mode = 1,
            .hardcore = true,
            .difficulty = 3,
            .difficulty_locked = true,
        };
        expect(static_cast<bool>(store->write_gameplay(creative)));
        auto read{store->read_gameplay()};
        expect(static_cast<bool>(read));
        if (read) {
            expect(read->game_mode == 1);
            expect(read->hardcore);
            expect(read->difficulty == 3);
            expect(read->difficulty_locked);
        }
        // `level()` shares the projection with `read_gameplay()`.
        const auto level{store->level()};
        expect(static_cast<bool>(level));
        if (level) {
            expect(level->game_mode == 1);
            expect(level->hardcore);
            expect(level->difficulty == 3);
            expect(level->difficulty_locked);
        }

        const world::gameplay_settings adventure{
            .game_mode = 2,
            .hardcore = false,
            .difficulty = 0,
            .difficulty_locked = false,
        };
        expect(static_cast<bool>(store->write_gameplay(adventure)));
        auto reread{store->read_gameplay()};
        expect(static_cast<bool>(reread));
        if (reread) {
            expect(reread->game_mode == 2);
            expect(!reread->hardcore);
            expect(reread->difficulty == 0);
            expect(!reread->difficulty_locked);
        }
    };

    "difficulty_locked_roundtrip"_test = [] {
        const temp_directory directory;
        auto store{world::world_store::create(directory.sub("world-locked"))};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        expect(write_meta(*store, make_meta(make_level())));

        auto settings{world::gameplay_settings{}};
        settings.difficulty_locked = true;
        expect(static_cast<bool>(store->write_gameplay(settings)));
        auto locked{store->read_gameplay()};
        expect(static_cast<bool>(locked));
        if (locked) {
            expect(locked->difficulty_locked);
        }

        settings.difficulty_locked = false;
        expect(static_cast<bool>(store->write_gameplay(settings)));
        auto unlocked{store->read_gameplay()};
        expect(static_cast<bool>(unlocked));
        if (unlocked) {
            expect(!unlocked->difficulty_locked);
        }
    };

    "write_gameplay_preserves_unknown_nbt"_test = [] {
        const temp_directory directory;
        auto store{world::world_store::create(directory.sub("world-preserve"))};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }

        auto level{make_level()};
        set(level, "Player",
            nbt::make_compound(
                nbt::compound{{"Health", nbt::make_float(20.0F)},
                              {"CustomName", nbt::make_string("synthetic")}}));
        set(level, "custom_flag", nbt::make_byte(7));
        set(level, "custom_list",
            nbt::make_list(tag_type::int_,
                           {nbt::make_int(4), nbt::make_int(5)}));
        value meta{make_meta(std::move(level))};
        set(meta, "unknown_root", nbt::make_long(1234));
        value unknown_compound{nbt::make_compound()};
        set(unknown_compound, "nested", nbt::make_string("kept"));
        set(meta, "unknown_structures", std::move(unknown_compound));
        expect(write_meta(*store, meta));

        auto before{read_meta(*store)};
        expect(before.has_value());
        if (!before) {
            return;
        }
        const world::gameplay_settings settings{
            .game_mode = 3,
            .hardcore = true,
            .difficulty = 1,
            .difficulty_locked = true,
        };
        expect(static_cast<bool>(store->write_gameplay(settings)));

        auto expected{*before};
        auto *expected_level{expected.get("level")};
        expect(expected_level != nullptr);
        if (expected_level != nullptr) {
            expect(static_cast<bool>(
                expected_level->set("GameType", nbt::make_int(3))));
            expect(static_cast<bool>(
                expected_level->set("hardcore", nbt::make_byte(1))));
            expect(static_cast<bool>(
                expected_level->set("Difficulty", nbt::make_byte(1))));
            expect(static_cast<bool>(
                expected_level->set("DifficultyLocked", nbt::make_byte(1))));
        }
        const auto after{read_meta(*store)};
        expect(after.has_value());
        if (after) {
            expect(*after == expected);
        }
    };

    "write_gameplay_keeps_existing_field_width"_test = [] {
        const temp_directory directory;
        auto store{world::world_store::create(directory.sub("world-widths"))};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        auto level{make_level()};
        set(level, "GameType", nbt::make_byte(0));
        set(level, "Difficulty", nbt::make_int(2));
        expect(write_meta(*store, make_meta(std::move(level))));
        expect(static_cast<bool>(
            store->write_gameplay(world::gameplay_settings{})));
        const auto after{read_meta(*store)};
        expect(after.has_value());
        if (after) {
            const auto *after_level{after->get("level")};
            expect(after_level != nullptr);
            if (after_level != nullptr) {
                const auto *game_type{after_level->get("GameType")};
                const auto *difficulty{after_level->get("Difficulty")};
                expect(game_type != nullptr && game_type->is(tag_type::byte));
                expect(difficulty != nullptr && difficulty->is(tag_type::int_));
            }
        }
    };

    "write_gameplay_rejects_out_of_range"_test = [] {
        const temp_directory directory;
        auto store{world::world_store::create(directory.sub("world-reject"))};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        expect(write_meta(*store, make_meta(make_level())));
        const auto before{io::read_file(meta_path(*store))};
        expect(static_cast<bool>(before));
        if (!before) {
            return;
        }

        const auto unchanged = [&] {
            const auto after{io::read_file(meta_path(*store))};
            return after && *after == *before;
        };
        for (const auto mode : {-1, 4}) {
            const auto rejected{
                store->write_gameplay({.game_mode = mode, .difficulty = 2})};
            expect(!rejected);
            if (!rejected) {
                expect(rejected.error().code ==
                       refrain::error_code::invalid_argument);
            }
            expect(unchanged());
        }
        for (const auto difficulty : {-1, 4}) {
            const auto rejected{store->write_gameplay(
                {.game_mode = 1, .difficulty = difficulty})};
            expect(!rejected);
            if (!rejected) {
                expect(rejected.error().code ==
                       refrain::error_code::invalid_argument);
            }
            expect(unchanged());
        }
    };

    "missing_meta_is_an_error"_test = [] {
        const temp_directory directory;
        auto store{world::world_store::create(directory.sub("world-missing"))};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        expect(!store->read_gameplay());
        const auto written{store->write_gameplay(world::gameplay_settings{})};
        expect(!written);
        if (!written) {
            expect(written.error().code == refrain::error_code::not_found);
        }
        expect(!io::exists(meta_path(*store)));
    };
};
