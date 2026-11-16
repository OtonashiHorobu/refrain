// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Synthetic older-data-version RWF worlds for the migration tests.
//
// The fixtures are built from the repository's own world writer: a world is
// created at the current data version, then its recorded version is stamped
// back and the records are filled with the version-sensitive NBT shapes the
// upgrade steps rewrite. No game asset is involved: every block entity and
// player record here is synthetic.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/data/nbt.hpp>
#include <refrain/world/create.hpp>
#include <refrain/world/model.hpp>
#include <refrain/world/store.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::test_support {

namespace nbt = refrain::data::nbt;
namespace world = refrain::world;

/// `minecraft:map` item stack with the retired block-transformer component and
/// the legacy buried-treasure item name: the shape the 5014 and 5016 upgrade
/// steps rewrite.
[[nodiscard]] inline auto legacy_map_item() -> nbt::value {
    nbt::compound name{
        {"translate", nbt::make_string("filled_map.buried_treasure")}};
    nbt::compound components{
        {"minecraft:block_transformer", nbt::make_string("minecraft:identity")},
        {"minecraft:item_name", nbt::make_compound(std::move(name))},
    };
    nbt::compound item{
        {"id", nbt::make_string("minecraft:map")},
        {"count", nbt::make_int(1)},
        {"components", nbt::make_compound(std::move(components))},
    };
    return nbt::make_compound(std::move(item));
}

/// Chest block entity that holds one legacy map item.
[[nodiscard]] inline auto legacy_chest_block_entity() -> nbt::value {
    nbt::compound entity{
        {"id", nbt::make_string("minecraft:chest")},
        {"x", nbt::make_int(0)},
        {"y", nbt::make_int(64)},
        {"z", nbt::make_int(0)},
        {"Items", nbt::make_list(nbt::tag_type::compound,
                                 std::vector<nbt::value>{legacy_map_item()})},
    };
    return nbt::make_compound(std::move(entity));
}

/// Player record that carries one legacy map item and the given data version.
[[nodiscard]] inline auto legacy_player_record(std::int32_t data_version)
    -> nbt::value {
    nbt::compound player{
        {"DataVersion", nbt::make_int(data_version)},
        {"Inventory",
         nbt::make_list(nbt::tag_type::compound,
                        std::vector<nbt::value>{legacy_map_item()})},
    };
    return nbt::make_compound(std::move(player));
}

/// Stamp `data_version` into the level record and the meta header of an
/// existing world.
[[nodiscard]] inline auto stamp_world_version(std::string_view root,
                                              std::int32_t data_version)
    -> status {
    auto store{world::world_store::open(root)};
    if (!store) {
        return std::unexpected{store.error()};
    }
    auto payload{store->read_meta()};
    if (!payload) {
        return std::unexpected{payload.error()};
    }
    auto document{nbt::read(*payload)};
    if (!document) {
        return std::unexpected{document.error()};
    }
    auto *level{document->get("level")};
    if (level == nullptr) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::parse_error, "fixture level record missing")};
    }
    if (auto written{level->set("DataVersion", nbt::make_int(data_version))};
        !written) {
        return written;
    }
    if (auto written{
            document->set("data_version", nbt::make_int(data_version))};
        !written) {
        return written;
    }
    auto bytes{nbt::write(*document, "", nbt::compression::none)};
    if (!bytes) {
        return std::unexpected{bytes.error()};
    }
    return store->write_meta(*bytes);
}

/// Stamp `data_version` into the generator settings saved-data header.
[[nodiscard]] inline auto stamp_saved_data_version(std::string_view root,
                                                   std::int32_t data_version)
    -> status {
    auto store{world::world_store::open(root)};
    if (!store) {
        return std::unexpected{store.error()};
    }
    constexpr std::string_view path{
        "global/data/minecraft/world_gen_settings.dat"};
    auto bytes{store->objects().read(path)};
    if (!bytes) {
        return std::unexpected{bytes.error()};
    }
    auto document{nbt::read(*bytes)};
    if (!document) {
        return std::unexpected{document.error()};
    }
    if (auto written{document->set("DataVersion", nbt::make_int(data_version))};
        !written) {
        return written;
    }
    auto encoded{nbt::write(*document, "", nbt::compression::none)};
    if (!encoded) {
        return std::unexpected{encoded.error()};
    }
    return store->objects().write(path, *encoded, world::object_media::nbt);
}

/// Create an RWF world at `root` recorded as `data_version`.
[[nodiscard]] inline auto create_world_at_version(std::string_view root,
                                                  std::int32_t data_version,
                                                  std::int64_t seed = 7)
    -> status {
    if (auto created{world::create_world(root, seed, "Migration Fixture")};
        !created) {
        return created;
    }
    return stamp_world_version(root, data_version);
}

/// Store the legacy chest block entity at `pos`.
[[nodiscard]] inline auto
add_legacy_block_entity_chunk(std::string_view root,
                              const world::chunk_pos &pos) -> status {
    auto store{world::world_store::open(root)};
    if (!store) {
        return std::unexpected{store.error()};
    }
    auto encoded{
        nbt::write(legacy_chest_block_entity(), "", nbt::compression::none)};
    if (!encoded) {
        return std::unexpected{encoded.error()};
    }
    world::chunk value{};
    value.status = world::chunk_status::full;
    value.block_entities.push_back(std::move(*encoded));
    if (auto written{store->write_chunk(pos, value)}; !written) {
        return written;
    }
    return store->commit();
}

/// Store one player record with the legacy map item.
[[nodiscard]] inline auto add_legacy_player_record(std::string_view root,
                                                   std::string_view key,
                                                   std::int32_t data_version)
    -> status {
    auto store{world::world_store::open(root)};
    if (!store) {
        return std::unexpected{store.error()};
    }
    auto encoded{nbt::write(legacy_player_record(data_version), "",
                            nbt::compression::none)};
    if (!encoded) {
        return std::unexpected{encoded.error()};
    }
    return store->blobs().write("playerdata", key, *encoded);
}

} // namespace refrain::test_support
