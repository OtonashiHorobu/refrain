// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Opt-in light oracle: compute the light of a vanilla-generated chunk with this
// engine and compare it cell by cell with the light the reference server stored
// in the same chunk.
//
// `REFRAIN_VANILLA_LIGHT_WORLD` points at a vanilla 26.3 Anvil world whose
// chunks were generated and saved by the reference server. The test imports it
// into an RWF world under the system temporary directory, keeps the reference
// light arrays, lets the engine recompute the light of the same columns and
// reports the number of differing nibbles. It skips itself when the variable is
// unset, so CI never needs the game.
//
// The comparison is exact: a generated column stores the light of the blocks
// it was saved with, so a full recompute must reproduce both layers cell for
// cell. A reference world made with commands can keep stale light of removed
// sources; regenerate it from a plain generation run for this test:
//
//   python3 tools/worldgen/oracle/generate.py --plain --seed 12345 --output
//       /tmp/light-oracle
//   REFRAIN_VANILLA_LIGHT_WORLD=/tmp/light-oracle/world ./build/Release/bin/
//       refrain-tests "world/light-oracle"
//
// `REFRAIN_VANILLA_LIGHT_CHUNK` pins the compared column as `x,z`; without it
// the test picks the fully generated column with the most stored light. A
// column whose saved light predates a later block write (a generated chunk
// can carry such stale light) does not compare equal; pin a settled column
// for the gate run.

#include <refrain/core/io.hpp>
#include <refrain/data/vanilla.hpp>
#include <refrain/sim/world.hpp>
#include <refrain/world/anvil.hpp>
#include <refrain/world/store.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace data = refrain::data;
namespace sim = refrain::sim;
namespace world = refrain::world;

constexpr std::size_t light_bytes{2048};

[[nodiscard]] auto scratch_directory() -> std::filesystem::path {
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    const auto path{std::filesystem::temp_directory_path() /
                    ("refrain_light_oracle_" + std::to_string(stamp))};
    std::error_code error{};
    std::filesystem::create_directories(path, error);
    return path;
}

struct scratch {
    std::filesystem::path directory{scratch_directory()};
    scratch() = default;
    scratch(const scratch &) = delete;
    auto operator=(const scratch &) -> scratch & = delete;
    scratch(scratch &&) = delete;
    auto operator=(scratch &&) -> scratch & = delete;
    ~scratch() {
        std::error_code error{};
        std::filesystem::remove_all(directory, error);
    }
};

[[nodiscard]] auto nibble(std::span<const std::byte> data, std::size_t index)
    -> std::uint8_t {
    const auto packed{static_cast<std::uint8_t>(data[index >> 1U])};
    return static_cast<std::uint8_t>((packed >> ((index & 1U) * 4U)) & 0x0FU);
}

/// Reference light of one column: section Y -> (block, sky) arrays. A layer
/// whose array is empty was not stored by the reference; the reference's
/// storage compacts homogeneous layers away.
using light_map =
    std::map<std::int32_t,
             std::pair<std::vector<std::byte>, std::vector<std::byte>>>;

[[nodiscard]] auto reference_light(world::world_store &store,
                                   const world::chunk_pos &pos)
    -> std::optional<light_map> {
    auto chunk{store.read_chunk(pos)};
    if (!chunk) {
        return std::nullopt;
    }
    light_map light{};
    for (const auto &layer : chunk->light_sections) {
        light.emplace(static_cast<std::int32_t>(layer.y),
                      std::pair{layer.block_light, layer.sky_light});
    }
    return light;
}

/// Light table of the tracked content snapshot. The oracle recomputes the
/// reference light from the same content a release ships, so a wrong table is
/// reported as a light mismatch rather than hiding in a fixture.
[[nodiscard]] auto tracked_light_table()
    -> std::optional<sim::block_light_table> {
    const std::string path{std::string{REFRAIN_SOURCE_DIR} +
                           "/content/vanilla/26.3/tables.rvtb"};
    auto bytes = refrain::core::io::read_file(path);
    if (!bytes) {
        std::fprintf(stderr, "light-oracle: %s\n",
                     bytes.error().message.c_str());
        return std::nullopt;
    }
    auto tables = data::vanilla_tables::open(*bytes);
    if (!tables) {
        std::fprintf(stderr, "light-oracle: %s\n",
                     tables.error().message.c_str());
        return std::nullopt;
    }
    return sim::block_light_table::from_tables(*tables);
}

struct mismatch_counts {
    /// Nibbles where the engine is brighter than the reference.
    std::uint64_t brighter{0};
    /// Nibbles where the engine is darker than the reference.
    std::uint64_t darker{0};
    std::uint64_t compared{0};
    /// First mismatching cells and levels, for the failure message.
    struct cell {
        std::int32_t x{0};
        std::int32_t y{0};
        std::int32_t z{0};
        std::uint8_t actual{0};
        std::uint8_t expected{0};
    };
    std::array<cell, 4> first{};
    std::size_t first_count{0};

    void note(std::int32_t x, std::int32_t y, std::int32_t z,
              std::uint8_t actual, std::uint8_t expected) {
        if (first_count < first.size()) {
            first[first_count++] = cell{x, y, z, actual, expected};
        }
    }
};

} // namespace

suite<"world/light-oracle"> light_oracle_tests = [] {
    "computed light matches the reference for a generated chunk"_test = [] {
        const char *source = std::getenv("REFRAIN_VANILLA_LIGHT_WORLD");
        if (source == nullptr) {
            return;
        }
        scratch temp{};
        const auto converted{(temp.directory / "rwf").string()};
        auto imported{world::anvil::import_world(source, converted, true, 1)};
        expect(imported.has_value())
            << (imported ? "" : imported.error().message);
        if (!imported) {
            return;
        }
        std::optional<world::chunk_pos> centre{};
        std::optional<light_map> reference{};
        {
            auto store{world::world_store::open(converted)};
            expect(store.has_value());
            if (!store) {
                return;
            }
            auto positions{store->chunk_positions()};
            expect(positions.has_value());
            if (!positions) {
                return;
            }
            // An explicitly pinned column wins over the heuristic below. It
            // is how a fixed reference chunk is compared reproducibly.
            if (const char *pinned = std::getenv("REFRAIN_VANILLA_LIGHT_CHUNK");
                pinned != nullptr) {
                std::int32_t x = 0;
                std::int32_t z = 0;
                if (std::sscanf(pinned, "%d,%d", &x, &z) == 2) {
                    centre = world::chunk_pos{x, z};
                }
            }
            // Without a pinned column, compare the reference chunk with a
            // complete 3x3 neighbourhood and the most stored light layers:
            // light propagation needs one column of context in every
            // direction, and partially generated columns carry no light.
            std::size_t best_values{0};
            if (!centre.has_value()) {
                for (const auto &candidate : *positions) {
                    bool complete{true};
                    for (auto dz{-1}; dz <= 1 && complete; ++dz) {
                        for (auto dx{-1}; dx <= 1 && complete; ++dx) {
                            auto neighbour{store->read_chunk(world::chunk_pos{
                                candidate.x + dx, candidate.z + dz})};
                            complete = neighbour.has_value();
                        }
                    }
                    if (!complete) {
                        continue;
                    }
                    auto light{reference_light(*store, candidate)};
                    if (!light) {
                        continue;
                    }
                    std::size_t values{0};
                    for (const auto &[section_y, arrays] : *light) {
                        static_cast<void>(section_y);
                        values += arrays.first.size() + arrays.second.size();
                    }
                    if (values > best_values) {
                        best_values = values;
                        centre = candidate;
                    }
                }
            }
            expect(centre.has_value())
                << "no chunk with a full 3x3 neighbourhood";
            if (!centre) {
                return;
            }
            std::printf("light-oracle: reference chunk (%d, %d) with %zu light "
                        "bytes\n",
                        centre->x, centre->z, best_values);
            reference = reference_light(*store, *centre);
            expect(reference.has_value());
            if (!reference) {
                return;
            }
        }

        auto runtime{sim::world::open(converted)};
        expect(runtime.has_value());
        if (!runtime) {
            return;
        }
        auto light_table{tracked_light_table()};
        expect(light_table.has_value());
        expect(light_table.has_value() && !light_table->empty());
        if (!light_table.has_value() || light_table->empty()) {
            return;
        }
        runtime->set_block_light_table(std::move(*light_table));
        for (auto dz{-1}; dz <= 1; ++dz) {
            for (auto dx{-1}; dx <= 1; ++dx) {
                auto loaded{runtime->ensure_chunk(
                    world::chunk_pos{centre->x + dx, centre->z + dz})};
                expect(loaded.has_value())
                    << (loaded ? "" : loaded.error().message);
                if (!loaded) {
                    return;
                }
            }
        }
        const auto *column{runtime->find_chunk(*centre)};
        expect(column != nullptr);
        if (column == nullptr) {
            return;
        }
        // The reference stores a light layer only when it is not homogeneous:
        // an absent sky layer is full above the highest stored sky layer and
        // dark below it; an absent block-light layer is always dark.
        std::int32_t top_sky_y{0};
        bool have_sky{false};
        for (const auto &[section_y, arrays] : *reference) {
            if (arrays.second.size() == light_bytes) {
                top_sky_y = std::max(top_sky_y, section_y);
                have_sky = true;
            }
        }
        mismatch_counts block_counts{};
        mismatch_counts sky_counts{};
        // Padding sections carry no block positions; the reference leaves
        // their light implicit and stores no arrays for them.
        const auto lowest_block_section{column->y_pos};
        const auto highest_block_section{
            column->sections.empty()
                ? column->y_pos
                : static_cast<std::int32_t>(column->sections.back().y)};
        for (const auto &layer : column->storage.light_sections) {
            const auto section_y{static_cast<std::int32_t>(layer.y)};
            if (section_y < lowest_block_section ||
                section_y > highest_block_section) {
                continue;
            }
            const auto found{reference->find(section_y)};
            const std::array<const std::vector<std::byte> *, 2> arrays{
                found == reference->end() ? nullptr : &found->second.first,
                found == reference->end() ? nullptr : &found->second.second,
            };
            const std::array<const std::vector<std::byte> *, 2> computed{
                &layer.block_light, &layer.sky_light};
            for (std::size_t which{0}; which < 2; ++which) {
                const auto &actual{*computed[which]};
                if (actual.size() != light_bytes) {
                    continue;
                }
                const auto *wanted{arrays[which]};
                const bool present{wanted != nullptr &&
                                   wanted->size() == light_bytes};
                auto &counts{which == 0 ? block_counts : sky_counts};
                counts.compared += 4096;
                for (std::size_t index{0}; index < 4096; ++index) {
                    const auto actual_level{nibble(actual, index)};
                    std::uint8_t expected_level{0};
                    if (present) {
                        expected_level = nibble(*wanted, index);
                    } else if (which == 1) {
                        expected_level =
                            have_sky && section_y > top_sky_y ? 15 : 0;
                    }
                    if (actual_level == expected_level) {
                        continue;
                    }
                    if (actual_level > expected_level) {
                        ++counts.brighter;
                    } else {
                        ++counts.darker;
                    }
                    const auto local_y{static_cast<std::int32_t>(index >> 8U)};
                    const auto local_z{
                        static_cast<std::int32_t>((index >> 4U) & 0xFU)};
                    const auto local_x{static_cast<std::int32_t>(index & 0xFU)};
                    counts.note(
                        centre->x * 16 + local_x, section_y * 16 + local_y,
                        centre->z * 16 + local_z, actual_level, expected_level);
                }
            }
        }
        const auto print_first = [&runtime](const char *name,
                                            const mismatch_counts &counts) {
            for (std::size_t i{0}; i < counts.first_count; ++i) {
                const auto &cell{counts.first[i]};
                const auto id{runtime->block_at(sim::block_pos{
                    cell.x, static_cast<std::int16_t>(cell.y), cell.z})};
                std::printf(
                    "light-oracle: %s mismatch at (%d, %d, %d) actual=%u "
                    "expected=%u block=%s\n",
                    name, cell.x, cell.y, cell.z,
                    static_cast<unsigned>(cell.actual),
                    static_cast<unsigned>(cell.expected),
                    std::string{runtime->registry().key(id)}.c_str());
            }
        };
        print_first("block", block_counts);
        print_first("sky", sky_counts);
        std::printf(
            "light-oracle: block light compared %llu cells, brighter %llu, "
            "darker %llu; sky light compared %llu cells, brighter %llu, "
            "darker %llu\n",
            static_cast<unsigned long long>(block_counts.compared),
            static_cast<unsigned long long>(block_counts.brighter),
            static_cast<unsigned long long>(block_counts.darker),
            static_cast<unsigned long long>(sky_counts.compared),
            static_cast<unsigned long long>(sky_counts.brighter),
            static_cast<unsigned long long>(sky_counts.darker));
        // A generated reference column stores the light its blocks imply, so
        // the recompute must match both layers cell for cell: a difference is
        // a wrong source table, transmission rule or shape occlusion.
        expect(block_counts.brighter == 0)
            << "computed block light is brighter than the reference";
        expect(block_counts.darker == 0)
            << "computed block light is darker than the reference";
        expect(sky_counts.brighter == 0)
            << "computed sky light is brighter than the reference";
        expect(sky_counts.darker == 0)
            << "computed sky light is darker than the reference";
    };
};
