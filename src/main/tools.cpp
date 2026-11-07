// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/archive/manifest.hpp>
#include <refrain/archive/zip.hpp>
#include <refrain/core/config.hpp>
#include <refrain/core/io.hpp>
#include <refrain/core/log.hpp>
#include <refrain/core/semver.hpp>
#include <refrain/core/version.hpp>
#include <refrain/data/builtin_image.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/content_index.hpp>
#include <refrain/data/content_maintenance.hpp>
#include <refrain/data/content_pack.hpp>
#include <refrain/data/content_segment.hpp>
#include <refrain/data/content_store.hpp>
#include <refrain/data/datapack.hpp>
#include <refrain/data/json.hpp>
#include <refrain/data/path.hpp>
#include <refrain/data/registry.hpp>
#include <refrain/data/store_lock.hpp>
#include <refrain/data/vfs.hpp>
#include <refrain/kernel/crash.hpp>
#include <refrain/mod/discovery.hpp>
#include <refrain/mod/gamepack.hpp>
#include <refrain/mod/host.hpp>
#include <refrain/mod/manifest.hpp>
#include <refrain/mod/mod_ref.hpp>
#include <refrain/mod/rfmod.hpp>
#include <refrain/net/ping.hpp>
#include <refrain/script/wasm.hpp>

#include <schema/schema.hpp>

#include <refrain/app/services.hpp>
#include <refrain/app/world_service.hpp>

#include <CLI/CLI.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using namespace refrain::app;

namespace app_services = refrain::app;

namespace archive = refrain::archive;
namespace config = refrain::core::config;
namespace core = refrain::core;
namespace data = refrain::data;
namespace io = refrain::core::io;
namespace log = refrain::log;
namespace mod = refrain::mod;
namespace schema = refrain::schema;
namespace script = refrain::script;

auto run_version() -> int {
    std::cout << refrain::version::string() << '\n';
    std::cout << "  release: " << refrain::version::release_version << '\n';
    std::cout << "  branch:  " << refrain::version::branch_name << '\n';
    std::cout << "  commit:  " << refrain::version::commit_hash << '\n';
    std::cout << "  build:   " << app_services::build_features() << '\n';
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    CLI::App app{"Refrain developer toolchain", "refrain-tools"};
    app.set_version_flag("--version", std::string{refrain::version::string()});

    bool verbose{false};
    app.add_flag("-v,--verbose", verbose,
                 "Enable debug logging on the console");

    auto *version_cmd =
        app.add_subcommand("version", "Print version/build info");
    version_cmd->callback([] { std::exit(run_version()); });

    std::string gamepack_path{"gamepack/gamepack.toml"};
    std::string gamepack_side{"server"};
    auto *gamepack_cmd = app.add_subcommand("gamepack", "Gamepack inspection");
    auto *dump_cmd = gamepack_cmd->add_subcommand(
        "dump", "Parse a gamepack.toml, print it and its resolved plan");
    dump_cmd->add_option("-f,--file", gamepack_path, "gamepack.toml path");
    dump_cmd->add_option("--side", gamepack_side,
                         "Host side used for the side compatibility check");

    std::string check_path{"gamepack/gamepack.toml"};
    std::string check_side{"server"};
    std::vector<std::string> required_services;
    auto *check_cmd = gamepack_cmd->add_subcommand(
        "check", "Validate a gamepack (deps, providers, load order)");
    check_cmd->add_option("-f,--file", check_path, "gamepack.toml path");
    check_cmd->add_option("--side", check_side,
                          "Host side used for the side compatibility check");
    check_cmd
        ->add_option(
            "--require", required_services,
            "Service keys the host cannot start without (repeatable, comma "
            "separated)")
        ->delimiter(',');

    auto *schema_cmd =
        app.add_subcommand("schema", "RSC/RIDL schema toolchain");
    schema_cmd->alias("idl");
    std::string schema_out{"sdk/generated"};
    bool schema_check{false};
    std::vector<std::string> schema_roots;
    auto *gen_cmd = schema_cmd->add_subcommand(
        "gen", "Generate contract/ABI headers from RSC/RIDL schemas");
    gen_cmd->add_option("-o,--out", schema_out, "Generated output root");
    gen_cmd->add_flag("--check", schema_check,
                      "Verify generated files are up to date (no writes)");
    gen_cmd->add_option("roots", schema_roots,
                        "Schema roots (default: schema sdk/idl)");

    extract_options extract{};
    auto *extract_cmd = app.add_subcommand(
        "extract", "Import data/assets from a user-local jar into the RFS "
                   "(Refrain Store; data manifest skeleton)");
    extract_cmd
        ->add_option("-j,--jar", extract.jar,
                     "Path to a user-local 26.3 jar (never committed)")
        ->required();
    extract_cmd->add_option(
        "-o,--out", extract.out,
        "Output directory for the manifest/RFS (Refrain Store)");
    extract_cmd->add_option("--manifest-out", extract.manifest_out,
                            "Manifest path (default: <out>/manifest.json)");
    extract_cmd->add_option("--filter", extract.filter,
                            "Entry filter: all, data or assets");
    extract_cmd->add_flag("--extract", extract.extract,
                          "Also extract the selected entries under --out");

    vanilla_import_options vanilla_import{};
    auto *vanilla_cmd =
        app.add_subcommand("vanilla", "Vanilla report import and inspection");
    auto *vanilla_import_cmd = vanilla_cmd->add_subcommand(
        "import", "Convert datagen reports into an RVTB table container");
    vanilla_import_cmd
        ->add_option("-r,--reports", vanilla_import.reports,
                     "Datagen --reports output directory")
        ->required();
    vanilla_import_cmd
        ->add_option("-o,--out", vanilla_import.out,
                     "Output directory (content/vanilla/<version>)")
        ->required();
    vanilla_import_cmd->add_option("--game-version",
                                   vanilla_import.game_version,
                                   "Game version recorded in the manifest");
    vanilla_import_cmd->add_option("--data-version",
                                   vanilla_import.data_version,
                                   "Data version recorded in the header");
    vanilla_import_cmd->add_flag(
        "--force", vanilla_import.force,
        "Overwrite existing tables.rvtb and MANIFEST.json");

    vanilla_info_options vanilla_info{};
    auto *vanilla_info_cmd =
        vanilla_cmd->add_subcommand("info", "Inspect an RVTB container");
    vanilla_info_cmd
        ->add_option("tables", vanilla_info.tables, "tables.rvtb path")
        ->required();

    mods_check_options mods_check{};
    auto *mods_cmd = app.add_subcommand("mods", "Mod manifest tooling");
    auto *mods_check_cmd = mods_cmd->add_subcommand(
        "check", "Validate the mods/<id>/manifest.toml tree (optionally "
                 "against a gamepack)");
    mods_check_cmd->add_option("--mods", mods_check.mods_dir,
                               "Mod tree root (default: mods)");
    mods_check_cmd->add_option("-g,--gamepack", mods_check.gamepack,
                               "Gamepack manifest to cross-check (optional)");
    mods_check_cmd->add_flag("--strict", mods_check.strict,
                             "Fail when a gamepack-declared mod has no "
                             "manifest yet");

    mods_list_options mods_list{};
    auto *mods_list_cmd = mods_cmd->add_subcommand(
        "list", "Scan instance roots (mods/<id>/manifest.toml) and print the "
                "discovered candidates (optionally verify a mods.lock)");
    mods_list_cmd->add_option("--mods", mods_list.roots,
                              "Instance root containing mods/ (repeatable)");
    mods_list_cmd->add_option(
        "-g,--gamepack", mods_list.gamepack,
        "Gamepack manifest whose builtin composition is seeded into the scan "
        "(requires --image)");
    mods_list_cmd->add_option(
        "--image", mods_list.image,
        "builtin.image loaded through the mod host to seed the builtin "
        "composition (requires --gamepack)");
    mods_list_cmd->add_option("--check-lock", mods_list.check_lock,
                              "mods.lock path to verify against the full "
                              "(builtin + user) discovery set");
    mods_list_cmd->add_flag("--strict", mods_list.strict,
                            "Reject non-mod entries under mods/ "
                            "(strict_entries)");

    mods_load_options mods_load{};
    auto *mods_load_cmd = mods_cmd->add_subcommand(
        "load", "Load a resolved gamepack's internal mods against a builtin "
                "RFSI image and dump them");
    mods_load_cmd->add_option("-g,--gamepack", mods_load.gamepack,
                              "gamepack.toml path");
    mods_load_cmd
        ->add_option("--image", mods_load.image,
                     "builtin.image path (required)")
        ->required();
    mods_load_cmd->add_option("--side", mods_load.side,
                              "Host side for the side compatibility check");

    mods_pack_options mods_pack{};
    auto *mods_pack_cmd = mods_cmd->add_subcommand(
        "pack", "Pack a mod directory into a deterministic .rfmod "
                "(optionally Ed25519-signed)");
    mods_pack_cmd
        ->add_option("mod-dir", mods_pack.mod_dir,
                     "Mod directory containing manifest.toml")
        ->required();
    mods_pack_cmd
        ->add_option("-o,--out", mods_pack.out, "Output .rfmod path (required)")
        ->required();
    mods_pack_cmd->add_option(
        "--seed-hex", mods_pack.seed_hex,
        "Ed25519 seed, 64 hex characters (requires --key-id)");
    mods_pack_cmd->add_option("--key-id", mods_pack.key_id,
                              "Signature key identifier (requires --seed-hex)");
    mods_pack_cmd->add_option("--level", mods_pack.level,
                              "zstd level in [1, 19] (default: 3)");

    mods_verify_options mods_verify{};
    auto *mods_verify_cmd = mods_cmd->add_subcommand(
        "verify", "Verify a .rfmod package (coverage, integrity, signature)");
    mods_verify_cmd
        ->add_option("archive", mods_verify.archive, "Path to the .rfmod")
        ->required();
    mods_verify_cmd->add_option(
        "--key-hex", mods_verify.key_hex,
        "Trusted Ed25519 public key, 64 hex characters (requires --key-id)");
    mods_verify_cmd->add_option("--key-id", mods_verify.key_id,
                                "Trusted key identifier (requires --key-hex)");
    mods_verify_cmd->add_flag(
        "--require-signature", mods_verify.require_signature,
        "Reject unsigned packages and signatures without a trusted key");

    mods_install_options mods_install{};
    bool no_cook{false};
    auto *mods_install_cmd = mods_cmd->add_subcommand(
        "install",
        "Verify a .rfmod, store its members in a BLAKE3 CAS and write the "
        "install receipt");
    mods_install_cmd
        ->add_option("archive", mods_install.archive, "Path to the .rfmod")
        ->required();
    mods_install_cmd
        ->add_option("--store", mods_install.store,
                     "Content store root (created on demand)")
        ->required();
    mods_install_cmd->add_option(
        "--key-hex", mods_install.key_hex,
        "Trusted Ed25519 public key, 64 hex characters (requires --key-id)");
    mods_install_cmd->add_option("--key-id", mods_install.key_id,
                                 "Trusted key identifier (requires --key-hex)");
    mods_install_cmd->add_flag(
        "--require-signature", mods_install.require_signature,
        "Reject unsigned packages and signatures without a trusted key");

    mods_installed_options mods_installed{};
    auto *mods_installed_cmd = mods_cmd->add_subcommand(
        "installed", "List per-mod install receipts under <store>/mods");
    mods_installed_cmd
        ->add_option("--store", mods_installed.store,
                     "Content store root to scan (read-only)")
        ->required();

    data_options data_opts{};
    bool skip_structures{false};
    auto *data_cmd =
        app.add_subcommand("data", "Datapack content parsing/validation");
    const auto add_data_options = [&data_opts,
                                   &skip_structures](CLI::App *command) {
        command
            ->add_option(
                "--pack", data_opts.packs,
                "Datapack directory (repeatable; priority low -> high)")
            ->required();
        command->add_option("--pack-format", data_opts.pack_format,
                            "Require this declared pack format (0 = any)");
        // CLI11 flags only set true when present: default false + negate later.
        command->add_flag("--no-structures", skip_structures,
                          "Do not decode structure templates");
    };
    auto *data_check_cmd = data_cmd->add_subcommand(
        "check", "Parse + validate datapack content (exit 1 on errors)");
    add_data_options(data_check_cmd);
    auto *data_dump_cmd = data_cmd->add_subcommand(
        "dump", "Parse and print content counts without validation");
    add_data_options(data_dump_cmd);
    data_cook_options data_cook{};
    auto *data_cook_cmd = data_cmd->add_subcommand(
        "cook", "Pack a datapack directory into a deterministic RFS container");
    data_cook_cmd->add_option("--pack", data_cook.pack, "Datapack directory")
        ->required();
    data_cook_cmd->add_option("--out", data_cook.out, "Output .rfs path")
        ->required();
    auto *data_registry_cmd = data_cmd->add_subcommand(
        "registry",
        "Build, freeze and fingerprint one registry per content kind");
    add_data_options(data_registry_cmd);
    data_registry_cmd->add_flag(
        "--list", data_opts.list,
        "Also print every entry as '<kind> <namespace:path>' (id order)");

    content_build_options content_build{};
    auto *content_cmd =
        app.add_subcommand("content", "Content-addressed store TOC indexes");
    auto *content_build_cmd = content_cmd->add_subcommand(
        "build", "Ingest a directory tree into the CAS, write a resolution "
                 "index and rebuild blobs.idx");
    content_build_cmd
        ->add_option("--root", content_build.root, "Content tree root")
        ->required();
    content_build_cmd
        ->add_option("--store", content_build.store, "Content store root")
        ->required();
    content_build_cmd
        ->add_option("--source", content_build.source,
                     "Source id recorded for every entry (e.g. mod:foo@1.0.0)")
        ->required();
    content_build_cmd->add_option("--priority", content_build.priority,
                                  "Override priority (higher wins)");
    content_build_cmd
        ->add_option("--composition", content_build.composition,
                     "Composition id (resolution index file stem)")
        ->required();
    content_build_cmd->add_option(
        "--kind", content_build.kind,
        "Blob kind: content, cook, world or builtin (default: content)");

    content_dump_options content_dump{};
    auto *content_dump_cmd = content_cmd->add_subcommand(
        "dump", "Print blobs.idx statistics and the first records");
    content_dump_cmd
        ->add_option("--store", content_dump.store, "Content store root")
        ->required();

    content_where_options content_where{};
    auto *content_where_cmd = content_cmd->add_subcommand(
        "where", "Resolve a location in a composition (winner + fallbacks)");
    content_where_cmd
        ->add_option("--store", content_where.store, "Content store root")
        ->required();
    content_where_cmd
        ->add_option("--composition", content_where.composition,
                     "Composition id of the resolution index")
        ->required();
    content_where_cmd
        ->add_option("location", content_where.location,
                     "Virtual path to resolve")
        ->required();

    content_verify_options content_verify{};
    auto *content_verify_cmd = content_cmd->add_subcommand(
        "verify", "Validate blobs.idx, every resolution index and, with "
                  "--blobs, every blob and segment");
    content_verify_cmd
        ->add_option("--store", content_verify.store, "Content store root")
        ->required();
    content_verify_cmd->add_flag(
        "--blobs", content_verify.blobs,
        "Also read every blob (raw digest / packed blocks) and verify every "
        "referenced segment");
    content_verify_cmd->add_flag(
        "--strict", content_verify.strict,
        "Treat orphan segments as a verification failure (with --blobs)");

    content_gc_options content_gc{};
    auto *content_gc_cmd = content_cmd->add_subcommand(
        "gc", "List or delete unreferenced CAS blobs and segments (default: "
              "dry run)");
    content_gc_cmd
        ->add_option("--store", content_gc.store, "Content store root")
        ->required();
    content_gc_cmd->add_flag("--dry-run", content_gc.dry_run,
                             "Only print unreferenced blob/segment statistics");
    content_gc_cmd->add_flag("--apply", content_gc.apply,
                             "Delete unreferenced blobs/segments and rebuild "
                             "blobs.idx");
    content_gc_cmd->add_flag(
        "--compact", content_gc.compact,
        "With --apply, rewrite segments that are at least 50% dead (with "
        "--dry-run, only report the plan)");
    content_gc_cmd->add_option(
        "--keep-days", content_gc.keep_days,
        "Keep unreferenced blobs/segments newer than N days (default: 0)");
    content_gc_cmd
        ->add_option("--budget-ms", content_gc.budget_ms,
                     "Wall-time budget for the deletion phase in milliseconds; "
                     "0 = unlimited (default: 0)")
        ->check(CLI::NonNegativeNumber);

    builtin_image_options builtin_image{};
    auto *builtin_cmd = app.add_subcommand(
        "builtin", "Built-in content image tooling (RFSI v1)");
    auto *builtin_image_cmd = builtin_cmd->add_subcommand(
        "image", "Build the deterministic RFSI v1 builtin image from the mod "
                 "tree (embedded by the host at build time)");
    builtin_image_cmd->add_option("--mods", builtin_image.mods_dir,
                                  "Mod tree root (default: mods)");
    builtin_image_cmd
        ->add_option("-o,--out", builtin_image.out,
                     "Output image path (required)")
        ->required();
    builtin_image_cmd->add_option("--emit-cpp", builtin_image.emit_cpp,
                                  "Emit a C++ std::array source for embedding");

    std::string crash_out{"build/crash"};
    std::string crash_handler{};
    auto *crash_cmd = app.add_subcommand("crash", "Crash reporter smoke tests");
    auto *crash_smoke_cmd = crash_cmd->add_subcommand(
        "smoke", "Attach Crashpad, crash on purpose, expect a minidump");
    crash_smoke_cmd->add_option("-o,--out", crash_out,
                                "minidump database directory");
    crash_smoke_cmd
        ->add_option("--handler", crash_handler,
                     "crashpad_handler executable path")
        ->required();
    std::string protocol_target{};
    int protocol_timeout_ms{5000};
    auto *protocol_cmd =
        app.add_subcommand("protocol", "Protocol 777 client tools");
    auto *protocol_ping_cmd = protocol_cmd->add_subcommand(
        "ping", "Status-list ping (handshake, status, pong)");
    protocol_ping_cmd
        ->add_option("target", protocol_target, "host:port (required)")
        ->required();
    protocol_ping_cmd->add_option("--timeout-ms", protocol_timeout_ms,
                                  "Whole exchange timeout in milliseconds");

    auto *wasm_cmd = app.add_subcommand("wasm", "WASM runtime smoke tests");
    auto *wasm_smoke_cmd =
        wasm_cmd->add_subcommand("smoke", "Compile and run a WAT module");

    world_import_options world_import{};
    world_export_options world_export{};
    world_fsck_options world_fsck{};
    world_compact_options world_compact{};
    world_palette_gc_options world_palette_gc{};
    world_info_options world_info{};
    world_diff_options world_diff{};
    worldgen_harness_options worldgen_harness{};
    worldgen_oracle_options worldgen_oracle{};

    auto *world_cmd = app.add_subcommand(
        "world", "RWF world storage and vanilla Anvil interop");
    auto *world_import_cmd = world_cmd->add_subcommand(
        "import", "Import a vanilla Anvil world into RWF");
    world_import_cmd->add_option("--from", world_import.from,
                                 "Anvil world directory (required)");
    world_import_cmd->add_option("--to", world_import.to,
                                 "Target RWF world directory (required)");
    world_import_cmd->add_flag("--force", world_import.force,
                               "Replace an existing target directory");
    world_import_cmd->add_option(
        "--threads", world_import.threads,
        "Worker threads (0 = auto, 1 = serial; default 0)");
    world_import_cmd->add_option(
        "--compression", world_import.compression,
        "RWF zstd profile: fast|balanced|compact (default balanced)");
    world_import_cmd->add_option("--dictionary", world_import.dictionary,
                                 "Optional zstd dictionary file");
    world_import_cmd->add_option(
        "--dictionary-scope", world_import.dictionary_scope,
        "Auto dictionary scope: dimension|world (default dimension)");
    auto *world_export_cmd = world_cmd->add_subcommand(
        "export", "Export an RWF world to the vanilla Anvil layout");
    world_export_cmd->add_option("--from", world_export.from,
                                 "RWF world directory (required)");
    world_export_cmd->add_option("--to", world_export.to,
                                 "Target Anvil directory (required)");
    world_export_cmd->add_flag("--force", world_export.force,
                               "Replace an existing target directory");
    world_export_cmd->add_option(
        "--threads", world_export.threads,
        "Worker threads (0 = auto, 1 = serial; default 0)");
    auto *world_fsck_cmd = world_cmd->add_subcommand(
        "fsck", "Validate an RWF world (indexes, records, meta)");
    world_fsck_cmd->add_option("path", world_fsck.path, "RWF world directory");
    world_fsck_cmd->add_flag("--repair", world_fsck.repair,
                             "Rebuild damaged region indexes");
    auto *world_compact_cmd = world_cmd->add_subcommand(
        "compact", "Compact the aux object store and blob store");
    world_compact_cmd->add_option("path", world_compact.path,
                                  "RWF world directory");
    auto *world_palette_gc_cmd = world_cmd->add_subcommand(
        "palette-gc",
        "Rebuild an RWF world into a fresh directory with a compacted palette "
        "(offline; source is left untouched)");
    world_palette_gc_cmd->add_option("--from", world_palette_gc.from,
                                     "Source RWF world (required)");
    world_palette_gc_cmd->add_option("--to", world_palette_gc.to,
                                     "Target RWF world (required)");
    world_palette_gc_cmd->add_flag("--force", world_palette_gc.force,
                                   "Replace an existing target directory");
    world_palette_gc_cmd->add_option(
        "--threads", world_palette_gc.threads,
        "Worker threads (0 = auto, 1 = serial; default 0)");
    auto *world_info_cmd = world_cmd->add_subcommand(
        "info", "Print world metadata and storage statistics");
    world_info_cmd->add_option("path", world_info.path, "RWF world directory");
    world_info_cmd->add_flag("--json", world_info.json,
                             "Machine readable output");
    world_info_cmd->add_flag(
        "--strict", world_info.strict,
        "Require DataVersion/spawn.pos/Time/LevelName and seed types");
    auto *world_diff_cmd = world_cmd->add_subcommand(
        "diff", "Compare two worlds by chunk semantic hash");
    world_diff_cmd->add_option("baseline", world_diff.baseline,
                               "Baseline world (Anvil or RWF)");
    world_diff_cmd->add_option("candidate", world_diff.candidate,
                               "Candidate world (Anvil or RWF)");
    world_diff_cmd->add_option("--min-match", world_diff.min_match,
                               "Required match percentage (default 100)");
    world_diff_cmd->add_flag("--json", world_diff.json,
                             "Machine readable output");

    auto *worldgen_cmd = app.add_subcommand(
        "worldgen", "World generation oracle harness (vanilla server)");
    auto *harness_cmd = worldgen_cmd->add_subcommand(
        "harness", "Compare a candidate world against an oracle world");
    harness_cmd->add_option("--candidate", worldgen_harness.candidate,
                            "Candidate world (RWF or Anvil)");
    harness_cmd->add_option("--oracle", worldgen_harness.oracle,
                            "Oracle world (Anvil or RWF)");
    harness_cmd->add_option("--report", worldgen_harness.report,
                            "Write a JSON report to this path");
    harness_cmd->add_option("--min-match", worldgen_harness.min_match,
                            "Required match percentage (default 95)");
    auto *oracle_cmd = worldgen_cmd->add_subcommand(
        "oracle",
        "Generate a world by running a user-local vanilla server jar");
    oracle_cmd->add_option("--jar", worldgen_oracle.jar,
                           "Vanilla server jar (never shipped by the repo)");
    oracle_cmd->add_option("--out", worldgen_oracle.out,
                           "Server run directory");
    oracle_cmd->add_option("--seed", worldgen_oracle.seed, "Level seed");
    oracle_cmd->add_option("--java", worldgen_oracle.java,
                           "Java executable (default: java)");

    CLI11_PARSE(app, argc, argv);

    data_opts.structures = !skip_structures;
    app_services::configure_logging(verbose);
    // Every command returns from `main`, so flush and tear the logger
    // down here instead of relying on static destruction (async log lines would
    // otherwise be dropped on the early-return paths).
    struct log_guard {
        ~log_guard() { refrain::log::shutdown(); }
    } log_guard_instance{};

    if (dump_cmd->parsed()) {
        return app_services::run_gamepack_dump(gamepack_path, gamepack_side);
    }
    if (check_cmd->parsed()) {
        return app_services::run_gamepack_check(check_path, check_side,
                                                required_services);
    }
    if (gen_cmd->parsed()) {
        if (schema_roots.empty()) {
            schema_roots = {"schema", "sdk/idl"};
        }
        return app_services::run_schema_gen(schema_out, schema_roots,
                                            schema_check);
    }
    if (extract_cmd->parsed()) {
        return app_services::run_extract(extract);
    }
    if (vanilla_import_cmd->parsed()) {
        return app_services::run_vanilla_import(vanilla_import);
    }
    if (vanilla_info_cmd->parsed()) {
        return app_services::run_vanilla_info(vanilla_info);
    }
    if (mods_check_cmd->parsed()) {
        return app_services::run_mods_check(mods_check);
    }
    if (mods_list_cmd->parsed()) {
        return app_services::run_mods_list(mods_list);
    }
    if (mods_load_cmd->parsed()) {
        return app_services::run_mods_load(mods_load);
    }
    if (mods_pack_cmd->parsed()) {
        return app_services::run_mods_pack(mods_pack);
    }
    if (mods_verify_cmd->parsed()) {
        return app_services::run_mods_verify(mods_verify);
    }
    if (mods_install_cmd->parsed()) {
        mods_install.cook = !no_cook;
        return app_services::run_mods_install(mods_install);
    }
    if (mods_installed_cmd->parsed()) {
        return app_services::run_mods_installed(mods_installed);
    }
    if (data_check_cmd->parsed()) {
        return app_services::run_data(data_opts, true);
    }
    if (data_dump_cmd->parsed()) {
        return app_services::run_data(data_opts, false);
    }
    if (data_registry_cmd->parsed()) {
        return app_services::run_data_registry(data_opts);
    }
    if (data_cook_cmd->parsed()) {
        return app_services::run_data_cook(data_cook);
    }
    if (content_build_cmd->parsed()) {
        return app_services::run_content_build(content_build);
    }
    if (content_dump_cmd->parsed()) {
        return app_services::run_content_dump(content_dump);
    }
    if (content_where_cmd->parsed()) {
        return app_services::run_content_where(content_where);
    }
    if (content_verify_cmd->parsed()) {
        return app_services::run_content_verify(content_verify);
    }
    if (content_gc_cmd->parsed()) {
        return app_services::run_content_gc(content_gc);
    }
    if (builtin_image_cmd->parsed()) {
        return app_services::run_builtin_image(builtin_image);
    }
    if (world_import_cmd->parsed()) {
        return app_services::run_world_import(world_import);
    }
    if (world_export_cmd->parsed()) {
        return app_services::run_world_export(world_export);
    }
    if (world_fsck_cmd->parsed()) {
        return app_services::run_world_fsck(world_fsck);
    }
    if (world_compact_cmd->parsed()) {
        return app_services::run_world_compact(world_compact);
    }
    if (world_palette_gc_cmd->parsed()) {
        return app_services::run_world_palette_gc(world_palette_gc);
    }
    if (world_info_cmd->parsed()) {
        return app_services::run_world_info(world_info);
    }
    if (world_diff_cmd->parsed()) {
        return app_services::run_world_diff(world_diff);
    }
    if (harness_cmd->parsed()) {
        return app_services::run_worldgen_harness(worldgen_harness);
    }
    if (oracle_cmd->parsed()) {
        return app_services::run_worldgen_oracle(worldgen_oracle);
    }
    if (wasm_smoke_cmd->parsed()) {
        return app_services::run_wasm_smoke();
    }
    if (crash_smoke_cmd->parsed()) {
        return app_services::run_crash_smoke(crash_out, crash_handler);
    }
    if (protocol_ping_cmd->parsed()) {
        const auto separator{protocol_target.rfind(':')};
        if (separator == std::string::npos || separator == 0 ||
            separator + 1 >= protocol_target.size()) {
            fmt::print(stderr, "protocol ping: expected host:port\n");
            return 1;
        }
        refrain::net::endpoint endpoint{};
        endpoint.host = protocol_target.substr(0, separator);
        std::uint16_t port{0};
        const auto port_text{protocol_target.substr(separator + 1)};
        const auto parsed{std::from_chars(
            port_text.data(), port_text.data() + port_text.size(), port)};
        if (parsed.ec != std::errc{} ||
            parsed.ptr != port_text.data() + port_text.size()) {
            fmt::print(stderr, "protocol ping: invalid port '{}'\n", port_text);
            return 1;
        }
        endpoint.port = port;
        auto result{refrain::net::ping(
            endpoint, std::chrono::milliseconds{protocol_timeout_ms})};
        if (!result) {
            fmt::print(stderr, "protocol ping: {}\n", result.error().message);
            return 1;
        }
        std::cout << "protocol: " << result->status.protocol << '\n';
        std::cout << "version: " << result->status.version_name << '\n';
        std::cout << "motd: " << result->status.description << '\n';
        std::cout << "players: " << result->status.online_players << '/'
                  << result->status.max_players << '\n';
        std::cout << "latency_ms: " << result->latency.count() << '\n';
        return 0;
    }

    std::cout << app.help();
    return 0;
}
