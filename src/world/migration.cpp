// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/world/migration.hpp>

#include <refrain/core/error.hpp>
#include <refrain/data/nbt.hpp>
#include <refrain/world/model.hpp>
#include <refrain/world/saved_data.hpp>
#include <refrain/world/store.hpp>
#include <refrain/world/version.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::world {
namespace {

namespace nbt = data::nbt;

/// Reference schema versions of the upgrade steps. A step applies to every
/// world recorded with a data version lower than the schema version that
/// introduced it.
constexpr std::int32_t block_transformer_removed_in = 5014;
constexpr std::int32_t buried_treasure_map_renamed_in = 5016;

/// Saved-data record that carries the world generation seed.
constexpr std::string_view generator_settings_object =
    "global/data/minecraft/world_gen_settings.dat";
constexpr std::string_view saved_data_prefix = "global/data/";
constexpr std::string_view player_data_kind = "playerdata";

[[nodiscard]] auto failing(error_code code, std::string message)
    -> std::unexpected<error> {
    return std::unexpected{make_error(code, std::move(message))};
}

/// The reference item-component steps selected for a source version.
struct upgrade_steps {
    /// Schema 5014 removed the retired `minecraft:block_transformer` component.
    bool remove_block_transformer{false};
    /// Schema 5016 renamed the buried-treasure map item name.
    bool rename_buried_treasure_map{false};
    /// Number of reference steps (the version stamp is counted separately).
    std::uint32_t count{0};
};

[[nodiscard]] auto upgrade_steps_for(std::int32_t from_version)
    -> upgrade_steps {
    upgrade_steps steps{};
    if (from_version < block_transformer_removed_in) {
        steps.remove_block_transformer = true;
        ++steps.count;
    }
    if (from_version < buried_treasure_map_renamed_in) {
        steps.rename_buried_treasure_map = true;
        ++steps.count;
    }
    return steps;
}

/// Visit every compound of an NBT tree — the root, nested compounds and
/// compounds inside lists. `visit` returns true when it changed its node.
template <typename Visit>
[[nodiscard]] auto walk_compounds(nbt::value &node, Visit &visit) -> bool {
    bool changed{false};
    if (auto *compound{node.as_compound()}; compound != nullptr) {
        changed = visit(*compound);
        // Copy the child keys first: a visitor may erase a key from the
        // compound it was handed.
        std::vector<std::string> keys;
        keys.reserve(compound->size());
        for (const auto &[key, child] : *compound) {
            static_cast<void>(child);
            keys.push_back(key);
        }
        for (const auto &key : keys) {
            if (auto *child{node.get(key)}; child != nullptr) {
                changed = walk_compounds(*child, visit) || changed;
            }
        }
        return changed;
    }
    if (auto *list{node.as_list()}; list != nullptr) {
        for (auto &item : list->items) {
            changed = walk_compounds(item, visit) || changed;
        }
    }
    return changed;
}

/// Schema 5014: drop the retired block-transformer component from every item
/// stack component map.
[[nodiscard]] auto remove_block_transformer(nbt::compound &node) -> bool {
    const auto components{node.find("components")};
    if (components == node.end()) {
        return false;
    }
    auto *map{components->second.as_compound()};
    if (map == nullptr) {
        return false;
    }
    return map->erase("minecraft:block_transformer") > 0;
}

/// Schema 5016: rename the legacy buried-treasure map name. The reference fix
/// only touches `minecraft:map` stacks whose item name is a plain translation
/// component carrying the old key.
[[nodiscard]] auto rename_buried_treasure_map(nbt::compound &node) -> bool {
    const auto id{node.find("id")};
    if (id == node.end()) {
        return false;
    }
    const auto item_id{id->second.as_string()};
    if (!item_id || *item_id != "minecraft:map") {
        return false;
    }
    const auto components{node.find("components")};
    if (components == node.end()) {
        return false;
    }
    auto *map{components->second.as_compound()};
    if (map == nullptr) {
        return false;
    }
    const auto name{map->find("minecraft:item_name")};
    if (name == map->end()) {
        return false;
    }
    auto *component{name->second.as_compound()};
    // A plain translation component has exactly one field, `translate`.
    if (component == nullptr || component->size() != 1) {
        return false;
    }
    const auto translate{component->find("translate")};
    if (translate == component->end()) {
        return false;
    }
    const auto key{translate->second.as_string()};
    if (!key || *key != "filled_map.buried_treasure") {
        return false;
    }
    translate->second = nbt::make_string("item.minecraft.buried_treasure_map");
    return true;
}

/// Apply the selected reference steps to one item-bearing NBT document.
[[nodiscard]] auto apply_item_steps(nbt::value &document,
                                    const upgrade_steps &steps) -> bool {
    bool changed{false};
    if (steps.remove_block_transformer) {
        auto visit{&remove_block_transformer};
        changed = walk_compounds(document, visit) || changed;
    }
    if (steps.rename_buried_treasure_map) {
        auto visit{&rename_buried_treasure_map};
        changed = walk_compounds(document, visit) || changed;
    }
    return changed;
}

[[nodiscard]] auto encode_nbt(const nbt::value &document)
    -> result<std::vector<std::byte>> {
    return nbt::write(document, "", nbt::compression::none);
}

/// One player NBT record read into memory before the rewrite phase.
struct player_record {
    std::string key;
    nbt::value document;
};

/// One saved-data record under `global/data/`.
struct saved_data_record {
    std::string path;
    nbt::value document;
};

[[nodiscard]] auto prefix_failure(std::string_view what, const error &failure)
    -> error {
    return make_error(failure.code,
                      fmt::format("{}: {}", what, failure.message),
                      failure.location);
}

[[nodiscard]] auto require_int_version(const nbt::value &document,
                                       std::string_view what)
    -> result<std::int32_t> {
    const auto *version{document.get("DataVersion")};
    if (version == nullptr) {
        return failing(error_code::parse_error,
                       fmt::format("{} has no DataVersion", what));
    }
    const auto value{version->as_int()};
    if (!value) {
        return failing(error_code::parse_error,
                       fmt::format("{} DataVersion is not an integer", what));
    }
    return *value;
}

} // namespace

auto plan_data_migration(std::int32_t from_version, std::int32_t to_version)
    -> result<data_migration_plan> {
    if (from_version > to_version) {
        return failing(error_code::invalid_argument,
                       fmt::format("cannot migrate world data version {} down "
                                   "to {}",
                                   from_version, to_version));
    }
    if (to_version != target_data_version) {
        return failing(
            error_code::unsupported,
            fmt::format("migration target data version {} is not supported; "
                        "this build upgrades to {}",
                        to_version, target_data_version));
    }
    if (from_version < oldest_migratable_data_version) {
        return failing(
            error_code::unsupported,
            fmt::format("world data version {} is older than the supported "
                        "migration window [{}, {}]; upgrade the world in "
                        "Minecraft 26.3 before opening it here",
                        from_version, oldest_migratable_data_version,
                        target_data_version));
    }
    data_migration_plan plan{
        .from_version = from_version,
        .to_version = to_version,
    };
    if (from_version < to_version) {
        plan.steps = upgrade_steps_for(from_version).count + 1;
    }
    return plan;
}

auto migrate_world_data(std::string_view root, std::int32_t from_version,
                        std::int32_t to_version,
                        const data_migration_options &options)
    -> result<data_migration_report> {
    if (root.empty()) {
        return failing(error_code::invalid_argument, "world path is empty");
    }
    auto plan{plan_data_migration(from_version, to_version)};
    if (!plan) {
        return std::unexpected{std::move(plan.error())};
    }
    const std::string base{root};
    auto opened{world_store::open(base)};
    if (!opened) {
        return std::unexpected{std::move(opened.error())};
    }
    auto &store{*opened};
    const auto steps{upgrade_steps_for(from_version)};

    data_migration_report report{
        .from_version = from_version,
        .to_version = to_version,
        .steps = plan->steps,
        .dry_run = options.dry_run,
    };

    // --- level record ------------------------------------------------------
    auto meta_payload{store.read_meta()};
    if (!meta_payload) {
        return std::unexpected{std::move(meta_payload.error())};
    }
    auto meta_document{nbt::read(*meta_payload)};
    if (!meta_document) {
        return std::unexpected{std::move(meta_document.error())};
    }
    auto *level{meta_document->get("level")};
    if (level == nullptr || level->as_compound() == nullptr) {
        return failing(error_code::parse_error,
                       fmt::format("world '{}' level record has no 'level' "
                                   "compound",
                                   base));
    }
    const auto recorded{require_int_version(*level, "level record")};
    if (!recorded) {
        return std::unexpected{
            prefix_failure(fmt::format("world '{}'", base), recorded.error())};
    }
    if (*recorded == to_version) {
        // Already upgraded: a second open is a no-op, no record is read again.
        report.steps = 0;
        report.no_op = true;
        return report;
    }
    if (*recorded != from_version) {
        return failing(
            error_code::parse_error,
            fmt::format("world '{}' records data version {} but migration was "
                        "requested from {}",
                        base, *recorded, from_version));
    }
    if (const auto *meta_version{meta_document->get("data_version")};
        meta_version != nullptr) {
        const auto value{meta_version->as_int()};
        if (!value) {
            return failing(
                error_code::parse_error,
                fmt::format("world '{}' meta data_version is not an integer",
                            base));
        }
        if (*value != from_version && *value != to_version) {
            return failing(
                error_code::version_mismatch,
                fmt::format("world '{}' meta records data version {} while "
                            "its level record records {}",
                            base, *value, *recorded));
        }
    }

    // --- saved data --------------------------------------------------------
    auto object_paths{store.objects().list(saved_data_prefix)};
    if (!object_paths) {
        return std::unexpected{std::move(object_paths.error())};
    }
    std::vector<saved_data_record> saved_data;
    bool generator_settings_found{false};
    for (const auto &path : *object_paths) {
        if (!path.ends_with(".dat")) {
            continue;
        }
        auto bytes{store.objects().read(path)};
        if (!bytes) {
            return std::unexpected{prefix_failure(
                fmt::format("world '{}' saved data '{}'", base, path),
                bytes.error())};
        }
        auto document{nbt::read(*bytes)};
        if (!document || document->as_compound() == nullptr) {
            return failing(
                error_code::parse_error,
                fmt::format("world '{}' saved data '{}' is not a valid NBT "
                            "compound",
                            base, path));
        }
        const auto *raw_version{document->get("DataVersion")};
        if (raw_version != nullptr) {
            const auto value{raw_version->as_int()};
            if (!value) {
                return failing(
                    error_code::parse_error,
                    fmt::format("world '{}' saved data '{}' DataVersion is not "
                                "an integer",
                                base, path));
            }
            if (*value > to_version) {
                return failing(
                    error_code::version_mismatch,
                    fmt::format("world '{}' saved data '{}' records data "
                                "version {}, newer than the migration target "
                                "{}",
                                base, path, *value, to_version));
            }
            if (*value < from_version) {
                return failing(
                    error_code::version_mismatch,
                    fmt::format("world '{}' saved data '{}' records data "
                                "version {}, older than the world's {}",
                                base, path, *value, from_version));
            }
        }
        if (path == generator_settings_object) {
            generator_settings_found = true;
            const auto *data{document->get("data")};
            if (data == nullptr || data->as_compound() == nullptr) {
                return failing(
                    error_code::parse_error,
                    fmt::format("world '{}' generator settings '{}' has no "
                                "'data' compound",
                                base, path));
            }
            const auto *seed{data->get("seed")};
            if (seed == nullptr || (!seed->as_long() && !seed->as_int())) {
                return failing(
                    error_code::parse_error,
                    fmt::format("world '{}' generator settings '{}' has no "
                                "integer 'seed'",
                                base, path));
            }
        }
        saved_data.push_back(
            saved_data_record{.path = path, .document = std::move(*document)});
    }
    if (!generator_settings_found) {
        return failing(
            error_code::not_found,
            fmt::format("world '{}' has no generator settings record '{}'; "
                        "the seed cannot be migrated without it",
                        base, generator_settings_object));
    }

    // --- player records ----------------------------------------------------
    auto player_keys{store.blobs().list(player_data_kind)};
    if (!player_keys) {
        return std::unexpected{std::move(player_keys.error())};
    }
    std::vector<player_record> players;
    players.reserve(player_keys->size());
    for (const auto &key : *player_keys) {
        auto bytes{store.blobs().read(player_data_kind, key)};
        if (!bytes) {
            return std::unexpected{prefix_failure(
                fmt::format("world '{}' player record '{}'", base, key),
                bytes.error())};
        }
        auto document{nbt::read(*bytes)};
        if (!document || document->as_compound() == nullptr) {
            return failing(
                error_code::parse_error,
                fmt::format("world '{}' player record '{}' is not a valid NBT "
                            "compound",
                            base, key));
        }
        const auto version{require_int_version(
            *document, fmt::format("player record '{}'", key))};
        if (!version) {
            return std::unexpected{prefix_failure(
                fmt::format("world '{}'", base), version.error())};
        }
        if (*version > to_version) {
            return failing(
                error_code::version_mismatch,
                fmt::format("world '{}' player record '{}' records data "
                            "version {}, newer than the migration target {}",
                            base, key, *version, to_version));
        }
        if (*version < from_version) {
            return failing(
                error_code::version_mismatch,
                fmt::format("world '{}' player record '{}' records data "
                            "version {}, older than the world's {}",
                            base, key, *version, from_version));
        }
        players.push_back(
            player_record{.key = key, .document = std::move(*document)});
    }

    // --- chunks ------------------------------------------------------------
    auto positions{store.chunk_positions()};
    if (!positions) {
        return std::unexpected{std::move(positions.error())};
    }
    report.chunks_scanned = positions->size();
    for (const auto &pos : *positions) {
        auto chunk{store.read_chunk(pos)};
        if (!chunk) {
            return std::unexpected{prefix_failure(
                fmt::format("world '{}' chunk {}", base, format_chunk_pos(pos)),
                chunk.error())};
        }
        bool changed{false};
        for (std::size_t index{0}; index < chunk->block_entities.size();
             ++index) {
            auto &bytes{chunk->block_entities[index]};
            auto document{nbt::read(bytes)};
            if (!document || document->as_compound() == nullptr) {
                return failing(
                    error_code::parse_error,
                    fmt::format("world '{}' chunk {} block entity {} is not a "
                                "valid NBT compound",
                                base, format_chunk_pos(pos), index));
            }
            if (!apply_item_steps(*document, steps)) {
                continue;
            }
            auto encoded{encode_nbt(*document)};
            if (!encoded) {
                return std::unexpected{prefix_failure(
                    fmt::format("world '{}' chunk {} block entity {}", base,
                                format_chunk_pos(pos), index),
                    encoded.error())};
            }
            if (encoded->size() > max_payload_bytes) {
                return failing(
                    error_code::parse_error,
                    fmt::format("world '{}' chunk {} block entity {} grew to "
                                "{} bytes (limit {})",
                                base, format_chunk_pos(pos), index,
                                encoded->size(), max_payload_bytes));
            }
            bytes.assign(encoded->begin(), encoded->end());
            changed = true;
        }
        if (!changed) {
            continue;
        }
        ++report.chunks_migrated;
        if (!options.dry_run) {
            if (auto written{store.write_chunk(pos, *chunk)}; !written) {
                return std::unexpected{
                    prefix_failure(fmt::format("world '{}' chunk {}", base,
                                               format_chunk_pos(pos)),
                                   written.error())};
            }
        }
    }

    // --- player records: item steps and version stamp ----------------------
    report.player_records_scanned = players.size();
    for (auto &record : players) {
        bool changed{apply_item_steps(record.document, steps)};
        auto *version{record.document.get("DataVersion")};
        if (version != nullptr && version->as_int() != to_version) {
            static_cast<void>(
                record.document.set("DataVersion", nbt::make_int(to_version)));
            changed = true;
        }
        if (!changed) {
            continue;
        }
        ++report.player_records_migrated;
        if (options.dry_run) {
            continue;
        }
        auto encoded{encode_nbt(record.document)};
        if (!encoded) {
            return std::unexpected{prefix_failure(
                fmt::format("world '{}' player record '{}'", base, record.key),
                encoded.error())};
        }
        if (auto written{
                store.blobs().write(player_data_kind, record.key, *encoded)};
            !written) {
            return std::unexpected{prefix_failure(
                fmt::format("world '{}' player record '{}'", base, record.key),
                written.error())};
        }
    }

    // --- saved data: version stamp ----------------------------------------
    for (auto &record : saved_data) {
        const auto *raw{record.document.get("DataVersion")};
        const auto current{raw != nullptr ? raw->as_int()
                                          : std::optional<std::int32_t>{}};
        if (current.has_value() && *current == to_version) {
            continue;
        }
        static_cast<void>(
            record.document.set("DataVersion", nbt::make_int(to_version)));
        if (options.dry_run) {
            continue;
        }
        auto encoded{encode_nbt(record.document)};
        if (!encoded) {
            return std::unexpected{prefix_failure(
                fmt::format("world '{}' saved data '{}'", base, record.path),
                encoded.error())};
        }
        if (auto written{store.objects().write(record.path, *encoded,
                                               object_media::nbt)};
            !written) {
            return std::unexpected{prefix_failure(
                fmt::format("world '{}' saved data '{}'", base, record.path),
                written.error())};
        }
    }

    if (options.dry_run) {
        return report;
    }

    // Chunk regions and blob segments are committed before the version stamp
    // below: the stamp is the marker a later open trusts, so it is written
    // only after every upgraded record is durable.
    if (auto committed{store.commit()}; !committed) {
        return std::unexpected{
            prefix_failure(fmt::format("world '{}'", base), committed.error())};
    }

    // --- version stamp -----------------------------------------------------
    static_cast<void>(level->set("DataVersion", nbt::make_int(to_version)));
    static_cast<void>(
        meta_document->set("data_version", nbt::make_int(to_version)));
    auto stamped{encode_nbt(*meta_document)};
    if (!stamped) {
        return std::unexpected{prefix_failure(
            fmt::format("world '{}' level record", base), stamped.error())};
    }
    if (auto written{store.write_meta(*stamped)}; !written) {
        return std::unexpected{prefix_failure(
            fmt::format("world '{}' level record", base), written.error())};
    }
    return report;
}

} // namespace refrain::world
