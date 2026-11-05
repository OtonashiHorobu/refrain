// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/nbt.hpp>
#include <refrain/world/object_store.hpp>
#include <refrain/world/saved_data.hpp>

#include <boost/ut.hpp>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using namespace boost::ut;
namespace nbt = refrain::data::nbt;
namespace world = refrain::world;
namespace fs = std::filesystem;

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
                base / ("refrain-saved-" + std::to_string(sequence.fetch_add(
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

} // namespace

suite saved_data_views = [] {
    "world_gen_settings_and_game_rules"_test = [] {
        const temp_directory directory;
        world::object_store objects{directory.file("objects")};

        nbt::value settings{nbt::make_compound()};
        expect(static_cast<bool>(
            settings.set("DataVersion", nbt::make_int(5023))));
        nbt::value settings_data{nbt::make_compound()};
        expect(static_cast<bool>(
            settings_data.set("seed", nbt::make_long(12345))));
        expect(static_cast<bool>(
            settings_data.set("generate_structures", nbt::make_byte(1))));
        expect(
            static_cast<bool>(settings.set("data", std::move(settings_data))));
        auto settings_path{
            world::saved_data_path("minecraft", "world_gen_settings.dat")};
        expect(static_cast<bool>(settings_path));
        if (settings_path) {
            expect(static_cast<bool>(
                world::write_nbt_object(objects, *settings_path, settings)));
        }
        auto parsed{world::read_world_gen_settings(objects)};
        expect(static_cast<bool>(parsed));
        if (parsed) {
            expect(parsed->data_version == 5023);
            expect(parsed->has_seed);
            expect(parsed->seed == 12345);
            expect(parsed->generate_structures);
        }

        nbt::value rules{nbt::make_compound()};
        nbt::value rules_data{nbt::make_compound()};
        expect(static_cast<bool>(
            rules_data.set("doDaylightCycle", nbt::make_string("true"))));
        expect(static_cast<bool>(
            rules_data.set("randomTickSpeed", nbt::make_string("3"))));
        expect(static_cast<bool>(rules.set("data", std::move(rules_data))));
        auto rules_path{world::saved_data_path("minecraft", "game_rules.dat")};
        expect(static_cast<bool>(rules_path));
        if (rules_path) {
            expect(static_cast<bool>(
                world::write_nbt_object(objects, *rules_path, rules)));
        }
        auto parsed_rules{world::read_game_rules(objects)};
        expect(static_cast<bool>(parsed_rules));
        if (parsed_rules) {
            expect(parsed_rules->at("doDaylightCycle") == "true");
            expect(parsed_rules->at("randomTickSpeed") == "3");
        }
        expect(static_cast<bool>(objects.commit()));
    };

    "player_json_is_byte_exact"_test = [] {
        const temp_directory directory;
        world::object_store objects{directory.file("objects")};
        const std::string json{
            R"({"minecraft:custom":{"minecraft:walk_one_cm":1}})"};
        expect(static_cast<bool>(
            objects.write("players/00000000-0000-0000-0000-000000000001/stats",
                          to_bytes(json), world::object_media::json, true)));
        auto stats{world::read_player_json(
            objects, "00000000-0000-0000-0000-000000000001", "stats")};
        expect(static_cast<bool>(stats));
        if (stats) {
            expect(*stats == json);
        }
        auto players{world::list_player_views(objects)};
        expect(static_cast<bool>(players));
        if (players) {
            expect(players->size() == 1U);
            expect((*players)[0] == "00000000-0000-0000-0000-000000000001");
        }
        expect(!world::read_player_json(
            objects, "00000000-0000-0000-0000-000000000001", "advancements"));
        expect(!world::read_player_json(objects, "../escape", "stats"));
        expect(!world::read_player_json(
            objects, "00000000-0000-0000-0000-000000000001", "other"));
    };

    "missing_and_malformed_are_errors"_test = [] {
        const temp_directory directory;
        world::object_store objects{directory.file("objects")};
        auto missing{world::read_world_gen_settings(objects)};
        expect(!missing);
        if (!missing) {
            expect(missing.error().code == refrain::error_code::not_found);
        }
        expect(static_cast<bool>(objects.write(
            "global/data/minecraft/world_gen_settings.dat", to_bytes("not nbt"),
            world::object_media::nbt, true)));
        auto malformed{world::read_world_gen_settings(objects)};
        expect(!malformed);
        if (!malformed) {
            expect(malformed.error().code == refrain::error_code::parse_error);
        }
        expect(!world::saved_data_path("", "x.dat"));
        expect(!world::saved_data_path("minecraft", "../x.dat"));
        expect(!world::saved_data_path("minecraft", "x"));
    };
};
