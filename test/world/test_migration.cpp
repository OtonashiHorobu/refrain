// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Data version migration of RWF worlds.
//
// The migration is a pure record transformation plus a version stamp: these
// tests pin the supported window, the fail-closed cases, the exact NBT the
// item-component steps rewrite, the no-op on an upgraded world and the rule
// that a failed migration never stamps the new version.

#include <refrain/core/error.hpp>
#include <refrain/core/io.hpp>
#include <refrain/data/nbt.hpp>
#include <refrain/world/migration.hpp>
#include <refrain/world/saved_data.hpp>
#include <refrain/world/store.hpp>
#include <refrain/world/version.hpp>

#include <boost/ut.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "support/migration_world.hpp"
#include "support/scratch_directory.hpp"

namespace {

using namespace boost::ut;

namespace nbt = refrain::data::nbt;
namespace support = refrain::test_support;
namespace test_support = refrain::test_support;
namespace world = refrain::world;

constexpr std::int32_t fixture_version = 5013;

[[nodiscard]] auto world_path(const test_support::scratch_directory &directory)
    -> std::string {
    return (directory.path() / "world").string();
}

[[nodiscard]] auto recorded_version(const std::string &root) -> std::int32_t {
    auto version{world::read_world_data_version(root)};
    expect(static_cast<bool>(version));
    return version ? *version : 0;
}

[[nodiscard]] auto saved_data_version(const std::string &root) -> std::int32_t {
    auto store{world::world_store::open(root)};
    expect(static_cast<bool>(store));
    if (!store) {
        return 0;
    }
    auto settings{world::read_world_gen_settings(store->objects())};
    expect(static_cast<bool>(settings));
    return settings ? settings->data_version : 0;
}

/// First item stack of a container-like NBT document.
[[nodiscard]] auto first_item(const nbt::value &document)
    -> const nbt::value * {
    const auto *items{document.get("Items")};
    if (items == nullptr) {
        items = document.get("Inventory");
    }
    if (items == nullptr) {
        return nullptr;
    }
    const auto *list{items->as_list()};
    if (list == nullptr || list->items.empty()) {
        return nullptr;
    }
    return &list->items.front();
}

[[nodiscard]] auto item_components(const nbt::value &document)
    -> const nbt::compound * {
    const auto *item{first_item(document)};
    if (item == nullptr) {
        return nullptr;
    }
    const auto *components{item->get("components")};
    return components != nullptr ? components->as_compound() : nullptr;
}

[[nodiscard]] auto encoded_block_entity(const std::string &root,
                                        const world::chunk_pos &pos)
    -> std::vector<std::byte> {
    auto store{world::world_store::open(root)};
    expect(static_cast<bool>(store));
    if (!store) {
        return {};
    }
    auto chunk{store->read_chunk(pos)};
    expect(static_cast<bool>(chunk));
    if (!chunk || chunk->block_entities.empty()) {
        return {};
    }
    return chunk->block_entities.front();
}

const suite migration_tests = [] {
    "migration/plan covers the reference steps and the target stamp"_test = [] {
        auto earliest{world::plan_data_migration(5013, 5023)};
        expect(static_cast<bool>(earliest));
        if (earliest) {
            expect(earliest->steps == 3U); // 5014 step, 5016 step, stamp
        }
        auto middle{world::plan_data_migration(5014, 5023)};
        expect(static_cast<bool>(middle));
        if (middle) {
            expect(middle->steps == 2U); // 5016 step, stamp
        }
        auto recent{world::plan_data_migration(5016, 5023)};
        expect(static_cast<bool>(recent));
        if (recent) {
            expect(recent->steps == 1U); // stamp only
        }
        auto current{world::plan_data_migration(5023, 5023)};
        expect(static_cast<bool>(current));
        if (current) {
            expect(current->steps == 0U);
        }
    };

    "migration/plan refuses versions outside the supported window"_test = [] {
        auto too_old{world::plan_data_migration(5012, 5023)};
        expect(!too_old);
        if (!too_old) {
            expect(too_old.error().code == refrain::error_code::unsupported);
            expect(too_old.error().message.find("5012") != std::string::npos);
            expect(too_old.error().message.find("5013") != std::string::npos);
            expect(too_old.error().message.find("5023") != std::string::npos);
        }
        auto newer{world::plan_data_migration(5024, 5023)};
        expect(!newer);
        if (!newer) {
            expect(newer.error().code == refrain::error_code::invalid_argument);
        }
        auto foreign_target{world::plan_data_migration(5013, 5022)};
        expect(!foreign_target);
        if (!foreign_target) {
            expect(foreign_target.error().code ==
                   refrain::error_code::unsupported);
        }
    };

    "migration/upgrades level, saved data, chunk NBT and player records"_test =
        [] {
            test_support::scratch_directory directory{};
            const auto root{world_path(directory)};
            expect(static_cast<bool>(
                support::create_world_at_version(root, fixture_version)));
            // The generator settings record was written without a version
            // header by the world creator; the upgrade adds the target.
            expect(recorded_version(root) == fixture_version);
            expect(saved_data_version(root) == 0);

            const world::chunk_pos pos{0, 0};
            expect(static_cast<bool>(
                support::add_legacy_block_entity_chunk(root, pos)));
            constexpr std::string_view player_key{
                "00000000-0000-0000-0000-000000000001"};
            expect(static_cast<bool>(support::add_legacy_player_record(
                root, player_key, fixture_version)));

            auto report{world::migrate_world_data(root, fixture_version, 5023)};
            expect(static_cast<bool>(report));
            if (!report) {
                return;
            }
            expect(report->steps == 3U);
            expect(report->chunks_scanned == 1U);
            expect(report->chunks_migrated == 1U);
            expect(report->player_records_scanned == 1U);
            expect(report->player_records_migrated == 1U);
            expect(!report->no_op);
            expect(!report->dry_run);

            expect(recorded_version(root) == 5023);
            expect(saved_data_version(root) == 5023);

            // Chunk block entity: the retired component is gone and the item
            // name carries the renamed translation key.
            auto store{world::world_store::open(root)};
            expect(static_cast<bool>(store));
            if (!store) {
                return;
            }
            auto chunk{store->read_chunk(pos)};
            expect(static_cast<bool>(chunk));
            if (!chunk || chunk->block_entities.empty()) {
                return;
            }
            auto entity{nbt::read(chunk->block_entities.front())};
            expect(static_cast<bool>(entity));
            if (!entity) {
                return;
            }
            const auto *components{item_components(*entity)};
            expect(components != nullptr);
            if (components == nullptr) {
                return;
            }
            expect(!components->contains("minecraft:block_transformer"));
            const auto *name{
                components->contains("minecraft:item_name")
                    ? components->at("minecraft:item_name").as_compound()
                    : nullptr};
            expect(name != nullptr);
            if (name != nullptr) {
                const auto translate{name->contains("translate")
                                         ? name->at("translate").as_string()
                                         : std::nullopt};
                expect(translate.has_value());
                if (translate) {
                    expect(*translate == "item.minecraft.buried_treasure_map");
                }
            }

            // Player record: same item rewrite plus the version stamp.
            auto player{store->blobs().read("playerdata", player_key)};
            expect(static_cast<bool>(player));
            if (!player) {
                return;
            }
            auto player_document{nbt::read(*player)};
            expect(static_cast<bool>(player_document));
            if (!player_document) {
                return;
            }
            const auto *player_version{player_document->get("DataVersion")};
            expect(player_version != nullptr);
            if (player_version != nullptr) {
                expect(player_version->as_int() == 5023);
            }
            const auto *player_components{item_components(*player_document)};
            expect(player_components != nullptr);
            if (player_components != nullptr) {
                expect(!player_components->contains(
                    "minecraft:block_transformer"));
            }
        };

    "migration/second run over an upgraded world is a no-op"_test = [] {
        test_support::scratch_directory directory{};
        const auto root{world_path(directory)};
        expect(static_cast<bool>(
            support::create_world_at_version(root, fixture_version)));
        const world::chunk_pos pos{0, 0};
        expect(static_cast<bool>(
            support::add_legacy_block_entity_chunk(root, pos)));
        expect(static_cast<bool>(
            support::stamp_saved_data_version(root, fixture_version)));

        expect(static_cast<bool>(
            world::migrate_world_data(root, fixture_version, 5023)));
        const auto entity_after_first{encoded_block_entity(root, pos)};
        const auto meta_after_first{
            refrain::core::io::read_file(root + "/meta.rfw")};
        expect(static_cast<bool>(meta_after_first));

        auto second{world::migrate_world_data(root, fixture_version, 5023)};
        expect(static_cast<bool>(second));
        if (second) {
            expect(second->no_op);
            expect(second->steps == 0U);
            expect(second->chunks_scanned == 0U);
            expect(second->chunks_migrated == 0U);
        }
        expect(encoded_block_entity(root, pos) == entity_after_first);
        auto meta_second{refrain::core::io::read_file(root + "/meta.rfw")};
        expect(static_cast<bool>(meta_second));
        if (meta_after_first && meta_second) {
            expect(*meta_second == *meta_after_first);
        }
    };

    "migration/dry run counts the work and writes nothing"_test = [] {
        test_support::scratch_directory directory{};
        const auto root{world_path(directory)};
        expect(static_cast<bool>(
            support::create_world_at_version(root, fixture_version)));
        const world::chunk_pos pos{0, 0};
        expect(static_cast<bool>(
            support::add_legacy_block_entity_chunk(root, pos)));
        const auto entity_before{encoded_block_entity(root, pos)};
        const auto meta_before{
            refrain::core::io::read_file(root + "/meta.rfw")};
        expect(saved_data_version(root) == 0);

        world::data_migration_options options{.dry_run = true};
        auto report{
            world::migrate_world_data(root, fixture_version, 5023, options)};
        expect(static_cast<bool>(report));
        if (report) {
            expect(report->dry_run);
            expect(report->chunks_scanned == 1U);
            expect(report->chunks_migrated == 1U);
            expect(report->steps == 3U);
        }
        expect(recorded_version(root) == fixture_version);
        expect(saved_data_version(root) == 0);
        expect(encoded_block_entity(root, pos) == entity_before);
        auto meta_after{refrain::core::io::read_file(root + "/meta.rfw")};
        expect(static_cast<bool>(meta_after));
        if (meta_before && meta_after) {
            expect(*meta_after == *meta_before);
        }
    };

    "migration/refuses an out-of-window world without touching it"_test = [] {
        test_support::scratch_directory directory{};
        const auto root{world_path(directory)};
        expect(static_cast<bool>(support::create_world_at_version(root, 5012)));
        const world::chunk_pos pos{0, 0};
        expect(static_cast<bool>(
            support::add_legacy_block_entity_chunk(root, pos)));
        const auto entity_before{encoded_block_entity(root, pos)};
        const auto meta_before{
            refrain::core::io::read_file(root + "/meta.rfw")};

        auto report{world::migrate_world_data(root, 5012, 5023)};
        expect(!report);
        if (!report) {
            expect(report.error().code == refrain::error_code::unsupported);
            expect(report.error().message.find("5012") != std::string::npos);
            expect(report.error().message.find("5013") != std::string::npos);
        }
        expect(recorded_version(root) == 5012);
        expect(encoded_block_entity(root, pos) == entity_before);
        auto meta_after{refrain::core::io::read_file(root + "/meta.rfw")};
        expect(static_cast<bool>(meta_after));
        if (meta_before && meta_after) {
            expect(*meta_after == *meta_before);
        }
    };

    "migration/refuses a request that disagrees with the recorded version"_test =
        [] {
            test_support::scratch_directory directory{};
            const auto root{world_path(directory)};
            expect(static_cast<bool>(
                support::create_world_at_version(root, 5014)));
            const auto meta_before{
                refrain::core::io::read_file(root + "/meta.rfw")};

            auto report{world::migrate_world_data(root, 5013, 5023)};
            expect(!report);
            if (!report) {
                expect(report.error().code == refrain::error_code::parse_error);
                expect(report.error().message.find(
                           "records data version 5014") != std::string::npos);
            }
            expect(recorded_version(root) == 5014);
            auto meta_after{refrain::core::io::read_file(root + "/meta.rfw")};
            expect(static_cast<bool>(meta_after));
            if (meta_before && meta_after) {
                expect(*meta_after == *meta_before);
            }
        };

    "migration/fails closed on a corrupt chunk without stamping"_test = [] {
        test_support::scratch_directory directory{};
        const auto root{world_path(directory)};
        expect(static_cast<bool>(
            support::create_world_at_version(root, fixture_version)));
        // A block entity payload that is not NBT: the store accepts it (the
        // field is opaque), the migration must refuse it and name the chunk.
        auto store{world::world_store::open(root)};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        world::chunk value{};
        value.status = world::chunk_status::full;
        value.block_entities.push_back(
            std::vector<std::byte>{std::byte{0xFF}, std::byte{0xFF}});
        expect(store->write_chunk(world::chunk_pos{0, 0}, value).has_value());
        expect(store->commit().has_value());

        auto report{world::migrate_world_data(root, fixture_version, 5023)};
        expect(!report);
        if (!report) {
            const auto message{report.error().message};
            expect(message.find("(0, 0)") != std::string::npos);
        }
        expect(recorded_version(root) == fixture_version);
    };
};

} // namespace
