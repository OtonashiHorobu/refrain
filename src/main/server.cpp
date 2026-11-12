// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Dedicated server composition root: CLI -> logging -> gamepack
// load/validate/resolve -> content/mods -> optional headless runtime. The
// runtime (`--world`) owns the fixed 20 Hz tick loop, the plain console queue
// and the watchdog; sim/protocol providers plug into it.
// `--dump-gamepack` exposes the resolved composition for CI and the
// dual-provider smoke test.

#include <refrain/app/composition.hpp>
#include <refrain/app/host_context.hpp>
#include <refrain/app/services.hpp>
#include <refrain/builtin/image.hpp>
#include <refrain/contracts/abi.hpp>
#include <refrain/contracts/services/runtime.hpp>
#include <refrain/core/config.hpp>
#include <refrain/core/io.hpp>
#include <refrain/core/log.hpp>
#include <refrain/core/version.hpp>
#include <refrain/data/builtin_image.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/content_maintenance.hpp>
#include <refrain/data/content_pack.hpp>
#include <refrain/data/datapack.hpp>
#include <refrain/data/path.hpp>
#include <refrain/data/signature.hpp>
#include <refrain/data/vfs.hpp>
#include <refrain/host/core_api.hpp>
#include <refrain/host/kernel_api.hpp>
#include <refrain/host/metadata_api.hpp>
#include <refrain/host/platform_api.hpp>
#include <refrain/kernel/crash.hpp>
#include <refrain/mod/discovery.hpp>
#include <refrain/mod/gamepack.hpp>
#include <refrain/mod/host.hpp>
#include <refrain/mod/startup_content.hpp>
#include <refrain/server/server_properties.hpp>
#include <refrain/world/create.hpp>

#include <CLI/CLI.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr refrain::log::logger log_cli{"cli"};
constexpr refrain::log::logger log_config{"config"};
constexpr refrain::log::logger log_content{"content"};
constexpr refrain::log::logger log_crash_reporter{"crash-reporter"};
constexpr refrain::log::logger log_discovery{"discovery"};
constexpr refrain::log::logger log_host{"host"};
constexpr refrain::log::logger log_world{"world"};
constexpr refrain::log::logger log_mods{"mods"};

} // namespace

namespace {

using refrain::app::report_error;

/// Report a startup error and return the process exit code. Logging/crash
/// reporter teardown is owned by the RAII guard in main(), so every return path
/// (including this one) runs it exactly once, in the right order.
[[nodiscard]] auto report_fatal(const refrain::error &error) -> int {
    return report_error(error);
}

/// Default `--content-composition` suffix: only `[A-Za-z0-9._-]` survive,
/// every other byte becomes '_' (P2c2; the value is used as an RFRX file
/// stem).
[[nodiscard]] auto sanitize_component(std::string_view text) -> std::string {
    std::string sanitized;
    sanitized.reserve(text.size());
    for (const char character : text) {
        const auto byte{static_cast<unsigned char>(character)};
        const bool safe{(byte >= 'A' && byte <= 'Z') ||
                        (byte >= 'a' && byte <= 'z') ||
                        (byte >= '0' && byte <= '9') || character == '.' ||
                        character == '_' || character == '-'};
        sanitized.push_back(safe ? character : '_');
    }
    if (sanitized.empty()) {
        return "unknown";
    }
    return sanitized;
}

/// `--content-gc` text -> policy (CLI11 already restricts the value; this
/// keeps the composition root fail-closed if the two ever diverge).
[[nodiscard]] auto parse_gc_mode(std::string_view text)
    -> std::optional<refrain::mod::startup_content_options::gc_mode> {
    using gc_mode = refrain::mod::startup_content_options::gc_mode;
    if (text == "off") {
        return gc_mode::off;
    }
    if (text == "dry-run") {
        return gc_mode::dry_run;
    }
    if (text == "apply") {
        return gc_mode::apply;
    }
    return std::nullopt;
}

/// Stable diagnostic name for the generated runtime error enum.
[[nodiscard]] auto
runtime_error_name(refrain::services::runtime::runtime_error error)
    -> std::string_view {
    using runtime_error = refrain::services::runtime::runtime_error;
    switch (error) {
    case runtime_error::invalid_config:
        return "invalid_config";
    case runtime_error::io_error:
        return "io_error";
    case runtime_error::internal:
        return "internal";
    }
    return "unknown";
}

/// `--content-trusted-key <id>:<hex>` (same key bytes/format as
/// `mods install --key-id/--key-hex`). The id is everything before the last
/// ':' so key ids may themselves contain ':'.
[[nodiscard]] auto
parse_content_trusted_keys(const std::vector<std::string> &specs)
    -> refrain::result<std::vector<refrain::mod::rfmod_trusted_key>> {
    std::vector<refrain::mod::rfmod_trusted_key> keys;
    keys.reserve(specs.size());
    for (const auto &spec : specs) {
        const auto separator{spec.rfind(':')};
        if (separator == std::string::npos || separator == 0U ||
            separator + 1U >= spec.size()) {
            return std::unexpected{refrain::make_error(
                refrain::error_code::invalid_argument,
                fmt::format("refrain-server: --content-trusted-key '{}' must "
                            "be <key-id>:<64-hex-public-key>",
                            spec))};
        }
        auto public_key{refrain::data::parse_public_key_hex(
            std::string_view{spec}.substr(separator + 1U))};
        if (!public_key) {
            return std::unexpected{refrain::make_error(
                refrain::error_code::invalid_argument,
                fmt::format("refrain-server: --content-trusted-key '{}': {}",
                            spec, public_key.error().message))};
        }
        keys.push_back(refrain::mod::rfmod_trusted_key{
            .key_id = spec.substr(0, separator), .public_key = *public_key});
    }
    return keys;
}

/// Default `mods.lock` location: next to the gamepack it belongs to. Reuse
/// the shared virtual-path helpers instead of hand-splitting separators in the
/// composition root.
[[nodiscard]] auto default_mods_lock_path(std::string_view gamepack_path)
    -> std::string {
    return refrain::data::join_path(refrain::data::parent_path(gamepack_path),
                                    "mods.lock");
}

} // namespace

int main(int argc, char **argv) {
    CLI::App app{"Refrain dedicated server (server-first)", "refrain-server"};
    app.set_version_flag("--version", std::string{refrain::version::string()});

    std::string gamepack{"gamepack/gamepack.toml"};
    app.add_option("-g,--gamepack", gamepack, "Path to gamepack.toml");
    std::string config_dir{"config"};
    app.add_option("--config-dir", config_dir,
                   "Directory with per-namespace TOML configuration files "
                   "(disabled when empty)");
    std::string log_level{"info"};
    app.add_option("--log-level", log_level, "Console log level");
    // Crash reporting is opt-in: without --crash-dir the server never creates a
    // database directory (keeps the working tree/CI smoke runs clean). When
    // given, a crash directory is created (0700) and the fallback marker is
    // enabled; an empty handler_path is expected (Crashpad is not bundled).
    std::string crash_dir;
    app.add_option("--crash-dir", crash_dir,
                   "Directory for the local crash database; enables crash "
                   "reporting (default: disabled)");
    bool dump_gamepack{false};
    app.add_flag("--dump-gamepack", dump_gamepack,
                 "Print the resolved gamepack/provider graph and exit");
    std::int64_t seed_value{0};
    bool seed_given{false};
    auto *seed_option = app.add_option("--seed", seed_value,
                                       "World seed for a newly created world");
    seed_option->take_last();
    std::string world_path;
    app.add_option("--world", world_path,
                   "RWF world directory (overrides world.path; enables the "
                   "tick loop)");
    std::string import_properties_path;
    app.add_option("--import-properties", import_properties_path,
                   "Import a Java server.properties file as a fallback layer");
    std::int64_t run_ticks{0};
    app.add_option("--ticks", run_ticks,
                   "Run N ticks then stop (0 = until 'stop'/EOF)");
    std::string command_file;
    app.add_option("--cmd-file", command_file,
                   "Batch console commands, one per line (tick 0)");
    std::string script_file;
    app.add_option("--script", script_file,
                   "Tick-indexed replay script ('<tick> <command>' per line)");
    std::uint64_t hash_every{0};
    app.add_option("--hash-every", hash_every,
                   "Log the state hash every N ticks (0 disables)");
    int world_zstd_level{3};
    app.add_option(
        "--world-zstd-level", world_zstd_level,
        "Default zstd level for regions opened by the runtime (1..19)");
    int zstd_near_level{1};
    app.add_option("--zstd-near-level", zstd_near_level,
                   "Adaptive zstd level within the near radius (default: 1)");
    int zstd_mid_level{3};
    app.add_option("--zstd-mid-level", zstd_mid_level,
                   "Adaptive zstd level within the mid radius (default: 3)");
    int zstd_far_level{6};
    app.add_option("--zstd-far-level", zstd_far_level,
                   "Adaptive zstd level beyond the mid radius (default: 6)");
    std::int32_t zstd_near_radius{2};
    app.add_option("--zstd-near-radius", zstd_near_radius,
                   "Near chunk radius for the adaptive policy (default: 2)");
    std::int32_t zstd_mid_radius{8};
    app.add_option("--zstd-mid-radius", zstd_mid_radius,
                   "Mid chunk radius for the adaptive policy (default: 8)");
    std::uint32_t watchdog_timeout_ms{5000};
    app.add_option("--watchdog-timeout-ms", watchdog_timeout_ms,
                   "Warn when a tick is stalled longer than N ms (0 disables)");
    std::uint64_t autosave_interval_ticks{6000};
    app.add_option("--autosave-interval-ticks", autosave_interval_ticks,
                   "Flush dirty chunks every N ticks (0 disables)");
    std::uint64_t compact_interval_ticks{0};
    app.add_option("--compact-interval-ticks", compact_interval_ticks,
                   "Try runtime region compaction every N ticks (0 disables)");
    std::uint64_t store_compact_interval_ticks{0};
    app.add_option("--store-compact-interval-ticks",
                   store_compact_interval_ticks,
                   "Compact aux object/blob stores every N ticks (0 disables)");
    std::uint64_t compact_min_bytes{1ULL * 1024ULL * 1024ULL};
    app.add_option("--compact-min-bytes", compact_min_bytes,
                   "Minimum region bytes before runtime compaction "
                   "(default: 1 MiB)")
        ->check(CLI::NonNegativeNumber);
    std::uint32_t compact_garbage_percent{40};
    app.add_option("--compact-garbage-percent", compact_garbage_percent,
                   "Compact a region when garbage reaches this percent");
    std::uint64_t content_gc_interval_ticks{0};
    app.add_option(
        "--content-gc-interval-ticks", content_gc_interval_ticks,
        "Run content store mark-sweep GC every N ticks (0 disables)");
    std::vector<std::string> required_services;
    app.add_option(
           "--require", required_services,
           "Service keys this host cannot start without (repeatable, comma "
           "separated)")
        ->delimiter(',');
    // `auto_scan` / `mods.lock` runtime wiring. `--mods` is an instance
    // root (`<dir>/mods/<id>/manifest.toml`), scanned as origin=user.
    std::vector<std::string> mods_roots;
    auto *mods_option =
        app.add_option("--mods", mods_roots,
                       "Instance root containing mods/<id>/manifest.toml "
                       "(repeatable; scanned with origin=user)");
    std::string mods_lock;
    auto *mods_lock_option =
        app.add_option("--mods-lock", mods_lock,
                       "Path to mods.lock (default: <gamepack.toml "
                       "directory>/mods.lock)");
    bool force_auto_scan{false};
    app.add_flag("--auto-scan", force_auto_scan, "Force mod auto-discovery on");
    bool no_auto_scan{false};
    app.add_flag("--no-auto-scan", no_auto_scan,
                 "Force mod auto-discovery off (ignore gamepack auto_scan)");
    bool update_lock{false};
    auto *update_lock_option =
        app.add_flag("--update-lock", update_lock,
                     "Rewrite an existing mods.lock when the composition "
                     "drifted");

    // Startup content bootstrap. Every
    // option is inert unless --content-store is given, so the default startup
    // sequence is unchanged.
    std::string content_store;
    app.add_option("--content-store", content_store,
                   "Content store root; enables the startup content "
                   "bootstrap (scan inbox, install, publish index, GC)");
    std::string content_inbox;
    auto *content_inbox_option =
        app.add_option("--content-inbox", content_inbox,
                       "RFMOD inbox directory (default: "
                       "<store>/mods/inbox)");
    std::string content_composition;
    auto *content_composition_option =
        app.add_option("--content-composition", content_composition,
                       "Resolution composition to publish (default: "
                       "gamepack-<gamepack id>)");
    bool force_auto_install{false};
    auto *auto_install_option =
        app.add_flag("--auto-install", force_auto_install,
                     "Install inbox packages (default: follow gamepack "
                     "auto_scan)");
    bool no_auto_install{false};
    auto *no_auto_install_option =
        app.add_flag("--no-auto-install", no_auto_install,
                     "Never install inbox packages (scan/report only)");
    std::string content_gc{"apply"};
    auto *content_gc_option =
        app.add_option("--content-gc", content_gc,
                       "Startup GC policy: off, dry-run or apply")
            ->check(CLI::IsMember({"off", "dry-run", "apply"}));
    int gc_keep_days{7};
    auto *gc_keep_days_option =
        app.add_option("--gc-keep-days", gc_keep_days,
                       "Keep unreferenced content newer than N days");
    std::uint64_t gc_budget_ms{2000};
    auto *gc_budget_ms_option =
        app.add_option("--gc-budget-ms", gc_budget_ms,
                       "Wall-time budget for the startup GC deletion phase in "
                       "milliseconds; 0 = unlimited (default: 2000)")
            ->check(CLI::NonNegativeNumber);
    int lock_timeout_ms{5000};
    auto *lock_timeout_option =
        app.add_option("--lock-timeout-ms", lock_timeout_ms,
                       "Store lock timeout in milliseconds");
    bool content_require_signature{false};
    auto *content_require_signature_option =
        app.add_flag("--content-require-signature", content_require_signature,
                     "Require a trusted signature for startup installs");
    std::vector<std::string> content_trusted_keys;
    auto *content_trusted_key_option =
        app.add_option("--content-trusted-key", content_trusted_keys,
                       "Trusted signing key <id>:<hex> (repeatable)");

    // Runtime content mount: external datapack directories layered on top
    // of the published content composition. Repeatable; later entries have
    // higher priority (data::vfs semantics). Independent of --content-store.
    std::vector<std::string> datapack_dirs;
    std::vector<std::string> dev_dirs;
    app.add_option("--dev", dev_dirs,
                   "Loose development datapack directory (repeatable; mounted "
                   "above --datapack, never cooked)");
    app.add_option("--datapack", datapack_dirs,
                   "External datapack directory to mount at runtime "
                   "(repeatable; later entries override earlier ones)")
        ->expected(0, 16);

    // CLI11_PARSE maps every parsing/validation error to its own exit code
    // (e.g. 105 for a validation failure). This host reserves exit 2 for usage
    // errors, so catch ParseError explicitly and normalise; --help/--version
    // (CLI::Success) keep their documented exit 0 and print as usual.
    try {
        app.parse(argc, argv);
    } catch (const CLI::Success &early_exit) {
        return app.exit(early_exit);
    } catch (const CLI::ParseError &parse_error) {
        // what() already carries the "refrain-server: " prefix.
        fmt::println(stderr, "{}", parse_error.what());
        return 2;
    }

    // The cutoff is `now - hours(24 * keep_days)`; values above the library
    // bound overflow the filesystem clock (UB) and silently disable the
    // retention window. Reject them as a usage error before anything runs.
    if (gc_keep_days < 0 || gc_keep_days > refrain::data::max_gc_keep_days) {
        fmt::println(stderr,
                     "refrain-server: --gc-keep-days must be in [0, {}]",
                     refrain::data::max_gc_keep_days);
        return 2;
    }

    if (force_auto_scan && no_auto_scan) {
        fmt::println(stderr,
                     "refrain-server: --auto-scan and --no-auto-scan are "
                     "mutually exclusive");
        return 2;
    }
    if (force_auto_install && no_auto_install) {
        fmt::println(stderr,
                     "refrain-server: --auto-install and --no-auto-install "
                     "are mutually exclusive");
        return 2;
    }
    // Silently ignoring a content option without --content-store would make
    // the operator believe the bootstrap ran; reject the combination instead
    // (same policy as --mods without auto_scan).
    const bool explicit_content_inputs{
        content_inbox_option->count() > 0 ||
        content_composition_option->count() > 0 ||
        auto_install_option->count() > 0 ||
        no_auto_install_option->count() > 0 || content_gc_option->count() > 0 ||
        gc_keep_days_option->count() > 0 || gc_budget_ms_option->count() > 0 ||
        lock_timeout_option->count() > 0 ||
        content_require_signature_option->count() > 0 ||
        content_trusted_key_option->count() > 0};
    if (content_store.empty() && explicit_content_inputs) {
        fmt::println(stderr, "refrain-server: --content-* / --gc-keep-days / "
                             "--gc-budget-ms / --lock-timeout-ms require "
                             "--content-store");
        return 2;
    }
    auto content_gc_mode{parse_gc_mode(content_gc)};
    if (!content_gc_mode) {
        fmt::println(stderr,
                     "refrain-server: --content-gc must be off, dry-run or "
                     "apply");
        return 2;
    }
    auto content_keys{parse_content_trusted_keys(content_trusted_keys)};
    if (!content_keys) {
        fmt::println(stderr, "{}", content_keys.error().message);
        return 2;
    }

    // `--dump-gamepack` returns before the auto_scan/mods.lock stage, so mod
    // inputs would otherwise be silently ignored. Reject the combination up
    // front with the same usage-error policy as the auto_scan section below.
    const bool explicit_mod_inputs{mods_option->count() > 0 ||
                                   mods_lock_option->count() > 0 ||
                                   update_lock_option->count() > 0};
    if (dump_gamepack && explicit_mod_inputs) {
        fmt::println(stderr,
                     "refrain-server: --mods/--mods-lock/--update-lock cannot "
                     "be combined with --dump-gamepack");
        return 2;
    }

    // CLI --log-level wins; otherwise refrain.toml [logging].level is read
    // before the full store exists so early startup logs already honor it.
    std::string effective_log_level{log_level};
    if (app.get_option("--log-level")->count() == 0) {
        const auto path{std::filesystem::path{config_dir} / "refrain.toml"};
        if (std::filesystem::exists(path)) {
            auto host_config{
                refrain::core::config::document::parse_file(path.string())};
            if (!host_config) {
                fmt::println(stderr, "refrain-server: {}",
                             host_config.error().message);
                return 1;
            }
            effective_log_level =
                host_config->get_string("logging.level", log_level);
        }
    }
    refrain::log::config cfg{};
    cfg.console_level = refrain::log::level_from_string(effective_log_level);
    if (!refrain::log::init(cfg)) {
        fmt::println(stderr, "refrain-server: failed to initialize logging");
        return 1;
    }
    // The server tick thread is the process main thread; the
    // name is what `[main/info]` and `top -H` display.
    refrain::log::set_thread_name("main");

    // The composition root owns teardown on every return path below: detach the
    // crash reporter first (its shutdown diagnostics go through log) and only
    // then tear logging down. The guard is declared once logging is up, so all
    // returns after this point share the same cleanup.
    struct shutdown_guard {
        ~shutdown_guard() {
            refrain::kernel::crash_reporter::shutdown();
            refrain::log::shutdown();
        }
    } shutdown_guard_instance{};

    log_cli.info("{}", refrain::version::string());

    // Crash reporting is opt-in. Without --crash-dir no crash
    // database is created (CI smoke runs and the working tree stay clean).
    // An empty handler_path is expected here: Crashpad is not bundled with the
    // server, so initialize() installs the fallback marker; a failure is
    // reported but never blocks startup.
    if (!crash_dir.empty()) {
        refrain::kernel::crash_config crash{};
        crash.product = "Refrain";
        crash.version = std::string{refrain::version::release_version};
        crash.database_path = crash_dir;
        crash.fallback_marker = true;
        crash.start_handler = true;
        crash.upload = refrain::kernel::crash_upload::disabled;
        auto crash_initialized{
            refrain::kernel::crash_reporter::initialize(crash)};
        if (!crash_initialized) {
            log_cli.warn("crash reporter disabled: {}",
                         crash_initialized.error().message);
        } else {
            log_crash_reporter.info(
                "{} db={}",
                refrain::kernel::crash_reporter::enabled() ? "crashpad"
                                                           : "fallback",
                refrain::kernel::crash_reporter::database_path());
        }
    }

    log_host.info("{}", gamepack);

    std::vector<std::string_view> services{};
    services.reserve(required_services.size());
    for (const auto &service : required_services) {
        services.push_back(service);
    }
    auto gamepack_spec{refrain::mod::load_gamepack_spec(gamepack, "server")};
    if (!gamepack_spec) {
        // report_error() goes through log::error; the shutdown guard runs
        // afterwards, so the diagnostic that explains the non-zero exit code
        // is never dropped.
        return report_error(gamepack_spec.error());
    }

    // The CLI override decides whether `--mods` is meaningful *before* any
    // scanning happens: an explicit mod input without auto_scan is a usage
    // error, not a silently ignored flag (one composition pipeline).
    const bool effective_auto_scan{force_auto_scan ||
                                   (gamepack_spec->auto_scan && !no_auto_scan)};
    if (!effective_auto_scan && explicit_mod_inputs) {
        fmt::println(stderr, "refrain-server: --mods/--mods-lock/--update-lock "
                             "require auto_scan (enable with --auto-scan or "
                             "gamepack auto_scan = true)");
        return 2;
    }

    // Verify the embedded built-in content image (manifests +
    // data of every internal mod) before it seeds the composition.
    const auto image_bytes{std::span<const std::byte>{
        refrain::builtin::image_data(), refrain::builtin::image_size()}};
    auto image{refrain::data::builtin_image::open(image_bytes)};
    if (!image) {
        return report_error(image.error());
    }

    log_content.info("files={} bytes={} digest={}", image->file_count(),
                     image_bytes.size(),
                     refrain::data::to_hex(image->digest()));

    // Composition name shared by the startup publish and the runtime
    // mount: explicit `--content-composition`, else the sanitized
    // gamepack-derived default.
    const std::string content_composition_name{
        content_composition.empty()
            ? "gamepack-" + sanitize_component(gamepack_spec->id)
            : content_composition};

    // Startup content bootstrap, before the mod host loads anything.
    // `--content-store` opts in; without it this stage is a no-op. The inbox
    // scan/install/index/GC order and the failure policy live in
    // mod/startup_content.hpp. This runs on the `--dump-gamepack` path too so
    // CI can smoke-test the whole pipeline without a world boot.
    if (!content_store.empty()) {
        refrain::mod::startup_content_options content{};
        content.store_root = content_store;
        content.inbox_dir = content_inbox;
        content.composition = content_composition_name;
        // Layer order is the priority list. The gamepack `[mods].list`
        // declaration order is authoritative (first entry = highest priority);
        // ids declared there without a content receipt (builtin mods) are
        // reported once and skipped by the startup index. `mods.lock` stays
        // id-sorted and its fingerprint is unchanged.
        content.base_priority = 1000;
        content.layer_order.reserve(gamepack_spec->mods.size());
        std::vector<refrain::mod::mod_ref> gamepack_pins{};
        gamepack_pins.reserve(gamepack_spec->mods.size());
        for (const auto &entry : gamepack_spec->mods) {
            content.layer_order.push_back(entry.ref.id);
            gamepack_pins.push_back(entry.ref);
        }
        // Pin baseline is mods.lock; the gamepack [mods].list is an explicit
        // override. With auto_scan disabled the lock is
        // deliberately never read: gamepack.toml is the sole source.
        std::optional<refrain::mod::mod_lock> baseline{};
        if (effective_auto_scan) {
            const auto lock_path{mods_lock.empty()
                                     ? default_mods_lock_path(gamepack)
                                     : mods_lock};
            if (refrain::core::io::exists(lock_path)) {
                auto stored{refrain::mod::read_mod_lock(lock_path)};
                if (!stored) {
                    return report_fatal(stored.error());
                }
                baseline.emplace(std::move(*stored));
                for (const auto &pin : gamepack_pins) {
                    const auto found{std::find_if(
                        baseline->mods.begin(), baseline->mods.end(),
                        [&pin](const refrain::mod::locked_mod &entry) {
                            return entry.id == pin.id;
                        })};
                    if (found != baseline->mods.end() &&
                        found->version != pin.version) {
                        log_content.info("pin {} {}->{}", pin.id,
                                         found->version, pin.version);
                    }
                }
            } else {
                log_content.info("pin: no lock");
            }
        }
        content.pinned_versions = refrain::mod::merge_pinned_versions(
            baseline ? &*baseline : nullptr, gamepack_pins);
        content.auto_install = force_auto_install ||
                               (gamepack_spec->auto_scan && !no_auto_install);
        content.keep_installed = true;
        content.require_signature = content_require_signature;
        content.trusted_keys = std::move(*content_keys);
        content.gc = *content_gc_mode;
        content.gc_keep_days = gc_keep_days;
        content.gc_budget_ms = gc_budget_ms;
        content.lock_timeout_ms = lock_timeout_ms;
        content.log = [](std::string_view line) { log_cli.info("{}", line); };
        auto content_report{refrain::mod::run_startup_content(content)};
        if (!content_report) {
            return report_fatal(content_report.error());
        }
        log_content.info(
            "scan={} install={} skip={} fail={} receipts={} "
            "entries={} index={} gc={}/{} deferred={}/{}",
            content_report->scanned, content_report->installed,
            content_report->skipped, content_report->failed,
            content_report->receipt_files, content_report->published_entries,
            content_report->index_published ? "published" : "not published",
            content_report->gc_deleted_blobs,
            content_report->gc_deleted_segments,
            content_report->gc_deferred_blobs,
            content_report->gc_deferred_segments);
    }

    // `--dump-gamepack` is a manifest-level inspection command: it prints the
    // gamepack's declared plan (`preview_composition`, no package reads), which
    // keeps the stable `startup-plan.txt` contract. The full catalog
    // composition above still validates the built-in image for the log line and
    // for later stages.
    if (dump_gamepack) {
        auto preview{
            refrain::mod::preview_composition(*gamepack_spec, services)};
        if (!preview) {
            return report_error(preview.error());
        }
        std::cout << preview->format();
        return 0;
    }

    std::vector<refrain::mod::discovery_root> discovery_roots{};
    discovery_roots.reserve(mods_roots.size());
    for (const auto &root : mods_roots) {
        discovery_roots.push_back(refrain::mod::discovery_root{
            .path = root, .origin = refrain::mod::mod_origin::user});
    }
    auto plan{refrain::app::resolve_server_mods(
        *gamepack_spec, refrain::app::server_mods_options{
                            .roots = discovery_roots,
                            .builtin_image = image_bytes,
                            .required_services = services,
                            .auto_scan = effective_auto_scan,
                            .discovery = {.require_mods_dir = true}})};
    if (!plan) {
        return report_error(plan.error());
    }
    log_host.info("{}@{} abi={} mods={} providers={}", plan->spec.id,
                  plan->spec.version, plan->spec.abi, plan->load_order.size(),
                  plan->providers.size());
    for (const auto &mod : plan->load_order) {
        log_host.info("resolve {}@{}", mod.id, mod.version);
    }

    // Load the resolved gamepack's internal mods through the mod host
    // (manifest gates -> static factories -> service providers). Composition is
    // frozen on success; a failure aborts startup with the offending mod.
    refrain::core::config::store config_store{};
    std::string config_world_path{};
    {
        const std::filesystem::path root{config_dir};
        const auto mount_namespace = [&](std::string name,
                                         const std::filesystem::path &path) {
            if (config_dir.empty()) {
                return refrain::status_ok();
            }
            return config_store.mount(
                {.name = std::move(name), .path = path.string()});
        };
        if (auto status{mount_namespace("host", root / "refrain.toml")};
            !status) {
            return report_error(status.error());
        }
        for (const auto *name : {"runtime", "network", "world", "storage"}) {
            if (auto status{mount_namespace(
                    name, root / (std::string{name} + ".toml"))};
                !status) {
                return report_error(status.error());
            }
        }
        for (const auto &provider : plan->providers) {
            auto id{provider.provider};
            if (const auto at{id.find('@')}; at != std::string::npos) {
                id.resize(at);
            }
            const auto path{root / "providers" / (id + ".toml")};
            std::error_code error;
            if (config_dir.empty() || !std::filesystem::exists(path, error)) {
                continue;
            }
            if (auto status{mount_namespace("provider/" + id, path)}; !status) {
                return report_error(status.error());
            }
        }
        for (const auto &selected : plan->mods) {
            const auto path{root / "mods" / (selected.id + ".toml")};
            std::error_code error;
            if (config_dir.empty() || !std::filesystem::exists(path, error)) {
                continue;
            }
            if (auto status{mount_namespace("mod/" + selected.id, path)};
                !status) {
                return report_error(status.error());
            }
        }
        if (auto status{config_store.load_all()}; !status) {
            return report_error(status.error());
        }
        if (!import_properties_path.empty()) {
            auto text{refrain::core::io::read_text(
                import_properties_path, {.max_bytes = 1ULL * 1024ULL * 1024ULL,
                                         .allow_symlink = false})};
            if (!text) {
                return report_error(text.error());
            }
            auto imported{refrain::server::apply_server_properties(
                *text, config_store, refrain::core::config::layer::fallback,
                false)};
            if (!imported) {
                return report_error(imported.error());
            }
            for (const auto &key : imported->unknown_keys) {
                log_config.warn("unknown server.properties key '{}'", key);
            }
            log_config.info("imported properties={} source={}",
                            imported->applied, import_properties_path);
        }
        if (auto view{config_store.module("world")}) {
            config_world_path = view->get_string("path", "");
        }
        const auto override_int =
            [&](const char *option, std::string_view name_space,
                std::string_view key, std::int64_t value) -> refrain::status {
            if (app.get_option(option)->count() == 0) {
                return refrain::status_ok();
            }
            return config_store.set_int64(
                name_space, key, value, refrain::core::config::layer::override);
        };
        const auto apply_override =
            [&](const char *option, std::string_view name_space,
                std::string_view key, std::int64_t value) {
                if (auto status{override_int(option, name_space, key, value)};
                    !status) {
                    return status;
                }
                return refrain::status_ok();
            };
        if (auto status{apply_override("--world-zstd-level", "storage",
                                       "zstd_level", world_zstd_level)};
            !status) {
            return report_error(status.error());
        }
        if (auto status{apply_override("--zstd-near-level", "storage",
                                       "zstd_near_level", zstd_near_level)};
            !status) {
            return report_error(status.error());
        }
        if (auto status{apply_override("--zstd-mid-level", "storage",
                                       "zstd_mid_level", zstd_mid_level)};
            !status) {
            return report_error(status.error());
        }
        if (auto status{apply_override("--zstd-far-level", "storage",
                                       "zstd_far_level", zstd_far_level)};
            !status) {
            return report_error(status.error());
        }
        if (auto status{apply_override("--zstd-near-radius", "storage",
                                       "zstd_near_radius", zstd_near_radius)};
            !status) {
            return report_error(status.error());
        }
        if (auto status{apply_override("--zstd-mid-radius", "storage",
                                       "zstd_mid_radius", zstd_mid_radius)};
            !status) {
            return report_error(status.error());
        }
        if (auto status{apply_override("--watchdog-timeout-ms", "runtime",
                                       "watchdog_timeout_ms",
                                       watchdog_timeout_ms)};
            !status) {
            return report_error(status.error());
        }
        if (auto status{apply_override(
                "--autosave-interval-ticks", "storage",
                "autosave_interval_ticks",
                static_cast<std::int64_t>(autosave_interval_ticks))};
            !status) {
            return report_error(status.error());
        }
        if (auto status{apply_override(
                "--compact-interval-ticks", "storage", "compact_interval_ticks",
                static_cast<std::int64_t>(compact_interval_ticks))};
            !status) {
            return report_error(status.error());
        }
        if (auto status{apply_override(
                "--store-compact-interval-ticks", "storage",
                "store_compact_interval_ticks",
                static_cast<std::int64_t>(store_compact_interval_ticks))};
            !status) {
            return report_error(status.error());
        }
        if (auto status{apply_override(
                "--compact-min-bytes", "storage", "compact_min_bytes",
                static_cast<std::int64_t>(compact_min_bytes))};
            !status) {
            return report_error(status.error());
        }
        if (auto status{apply_override("--compact-garbage-percent", "storage",
                                       "compact_garbage_percent",
                                       compact_garbage_percent)};
            !status) {
            return report_error(status.error());
        }
        log_config.info("namespaces={} source={}", config_store.names().size(),
                        config_dir);
    }

    std::string resolved_world_path{world_path.empty() ? config_world_path
                                                       : world_path};
    if (resolved_world_path.empty()) {
        resolved_world_path = "world";
    }
    log_config.info("world.path='{}'", resolved_world_path);
    if (!refrain::core::io::exists(resolved_world_path + "/meta.rfw")) {
        seed_given = seed_option->count() > 0;
        if (!seed_given) {
            seed_value =
                static_cast<std::int64_t>(std::random_device{}() & 0x7FFFFFFFU);
        }
        auto created{refrain::world::create_world(resolved_world_path,
                                                  seed_value, "world")};
        if (!created) {
            return report_fatal(created.error());
        }
        log_world.info("created '{}' seed={}", resolved_world_path, seed_value);
    }

    refrain::mod::host mods{};
    mods.set_config(config_store);
    // The host runtime API is a sealed service: register the process-owned
    // provider before any mod factory runs. Mods may query it through the
    // frozen registry, but cannot provide or replace it.
    refrain::app::host_context host_context{mods, {}, &config_store};
    const refrain::mod::provider_api host_core_api{
        .vtable = &refrain::host::core_vtable(),
        .self = &host_context,
        .abi_major = REFRAIN_HOST_CORE_ABI_MAJOR,
    };
    if (auto registered = mods.register_sealed_provider("refrain:host.core",
                                                        host_core_api, "1.0.0");
        !registered) {
        return report_fatal(registered.error());
    }
    const refrain::mod::provider_api host_platform_api{
        .vtable = &refrain::host::platform_vtable(),
        .self = static_cast<refrain::host::platform_context *>(&host_context),
        .abi_major = REFRAIN_HOST_PLATFORM_ABI_MAJOR,
    };
    if (auto registered = mods.register_sealed_provider(
            "refrain:host.platform", host_platform_api, "1.0.0");
        !registered) {
        return report_fatal(registered.error());
    }
    const refrain::mod::provider_api host_kernel_api{
        .vtable = &refrain::host::kernel_vtable(),
        .self = static_cast<refrain::host::kernel_context *>(&host_context),
        .abi_major = REFRAIN_HOST_KERNEL_ABI_MAJOR,
    };
    if (auto registered = mods.register_sealed_provider(
            "refrain:host.kernel", host_kernel_api, "1.0.0");
        !registered) {
        return report_fatal(registered.error());
    }
    const refrain::mod::provider_api host_metadata_api{
        .vtable = &refrain::host::metadata_vtable(),
        .self = &host_context,
        .abi_major = REFRAIN_HOST_METADATA_ABI_MAJOR,
    };
    if (auto registered = mods.register_sealed_provider(
            "refrain:host.metadata", host_metadata_api, "1.0.0");
        !registered) {
        return report_fatal(registered.error());
    }
    log_host.info("sealed=refrain:host.core,"
                  "refrain:host.platform,refrain:host.kernel,"
                  "refrain:host.metadata");
    auto mods_loaded{
        refrain::app::activate_server_mods(mods, *plan, image_bytes)};
    if (!mods_loaded) {
        return report_error(mods_loaded.error());
    }
    log_host.info("{} mod(s) loaded", mods.mods().size());
    for (const auto &mod : mods.mods()) {
        log_host.info("mod {}@{} abi={} {}{}", mod.id, mod.version, mod.abi,
                      mod.has_code ? "code" : "data only",
                      mod.provides_summary.empty()
                          ? std::string{}
                          : ", provides " + mod.provides_summary);
    }

    // --- runtime content mount ---------------------------------------------
    // Mount the composition published by the startup bootstrap as the
    // lowest-priority layer, then every `--datapack` directory in CLI order
    // (later = higher priority, matching data::vfs). The merged stack is
    // loaded and validated before the mod composition is resolved; malformed
    // or semantically invalid content aborts startup. The builtin RFSI image
    // is deliberately not mounted here: its paths live under `mods/<id>/...`,
    // not at a datapack root.
    refrain::data::vfs runtime_vfs{};
    if (!content_store.empty()) {
        auto opened{refrain::data::open_content_pack(content_store,
                                                     content_composition_name)};
        if (!opened) {
            if (opened.error().code == refrain::error_code::not_found) {
                log_content.warn("no composition {} in {}",
                                 content_composition_name, content_store);
            } else {
                return report_fatal(opened.error());
            }
        } else {
            auto mounted{runtime_vfs.mount(std::move(*opened))};
            if (!mounted) {
                return report_fatal(mounted.error());
            }
        }
    }
    std::vector<std::string> runtime_datapacks{datapack_dirs};
    runtime_datapacks.insert(runtime_datapacks.end(), dev_dirs.begin(),
                             dev_dirs.end());
    for (const auto &datapack_dir : runtime_datapacks) {
        auto opened{refrain::data::open_dir_pack(datapack_dir, datapack_dir)};
        if (!opened) {
            return report_fatal(opened.error());
        }
        auto mounted{runtime_vfs.mount(std::move(*opened))};
        if (!mounted) {
            return report_fatal(mounted.error());
        }
    }

    if (runtime_vfs.pack_count() == 0) {
        log_content.info("none");
    } else {
        runtime_vfs.freeze();
        auto loaded{refrain::data::datapack_set::load(
            runtime_vfs,
            refrain::data::datapack_load_options{.expected_pack_format = 0,
                                                 .parse_structures = true})};
        if (!loaded) {
            return report_fatal(loaded.error());
        }
        const auto validation{loaded->validate()};
        constexpr std::size_t max_reported_diagnostics{5};
        if (!validation.ok()) {
            const auto shown_errors{
                std::min(validation.errors.size(), max_reported_diagnostics)};
            for (std::size_t index{0}; index < shown_errors; ++index) {
                log_content.error("error: {}", validation.errors[index]);
            }
            return report_fatal(refrain::make_error(
                refrain::error_code::parse_error,
                fmt::format("runtime-content: {} errors, first {} shown",
                            validation.errors.size(), shown_errors)));
        }
        const auto shown_warnings{
            std::min(validation.warnings.size(), max_reported_diagnostics)};
        for (std::size_t index{0}; index < shown_warnings; ++index) {
            log_content.warn("warning: {}", validation.warnings[index]);
        }
        if (validation.warnings.size() > shown_warnings) {
            log_content.warn("{} more warnings",
                             validation.warnings.size() - shown_warnings);
        }
        log_content.info("layers={} files={} errors={} "
                         "warnings={}",
                         runtime_vfs.pack_count(), runtime_vfs.file_count(),
                         validation.errors.size(), validation.warnings.size());
    }

    // Runtime content and the mod host are live; auto_scan/mods.lock is the
    // last optional stage, so a gamepack with auto_scan disabled returns before
    // it (the lock is only meaningful for a scanned catalog).
    if (!effective_auto_scan) {
        log_discovery.info("off");
    } else {
        // --- mods.lock wiring ----------------------------------------------
        // The composition above already merged the builtin image candidates
        // with every `--mods` root through one pipeline
        // (`mod::resolve_composition`), so the resolved lock is authoritative
        // here. Drift is fatal by default (no silent composition change);
        // `--update-lock` opts into a rewrite.
        const refrain::mod::mod_lock &lock{*plan};

        // A resolved directory copy of a builtin id is not loadable yet
        // (external loading is not implemented); make the shadowing visible but
        // keep the resolved lock.
        for (const auto &selected : lock.mods) {
            const auto *runtime{mods.find(selected.id)};
            if (runtime == nullptr) {
                continue;
            }
            const auto runtime_hash{
                refrain::data::parse_digest(runtime->manifest_hash_hex)};
            if (selected.origin == refrain::mod::mod_origin::builtin &&
                runtime_hash && *runtime_hash == selected.manifest_hash) {
                continue;
            }
            log_cli.warn("mod {}: {} copy shadows builtin", selected.id,
                         refrain::mod::to_string(selected.origin));
        }

        const auto lock_path{
            mods_lock.empty() ? default_mods_lock_path(gamepack) : mods_lock};
        if (!refrain::core::io::exists(lock_path)) {
            auto written{refrain::mod::write_mod_lock(lock_path, lock)};
            if (!written) {
                return report_fatal(written.error());
            }
            log_mods.info("written {}", lock_path);
        } else {
            auto stored{refrain::mod::read_mod_lock(lock_path)};
            if (!stored) {
                return report_fatal(stored.error());
            }
            auto verified{
                refrain::mod::verify_mod_lock(*stored, plan->load_order)};
            const bool drifted{!verified ||
                               stored->fingerprint() != lock.fingerprint()};
            if (!drifted) {
                log_mods.info("verified {}", lock_path);
            } else if (update_lock) {
                if (!verified) {
                    log_mods.warn("{} drifted: {}", lock_path,
                                  verified.error().message);
                } else {
                    log_mods.warn("{} drift {}->{}", lock_path,
                                  stored->fingerprint(), lock.fingerprint());
                }
                auto rewritten{refrain::mod::write_mod_lock(lock_path, lock)};
                if (!rewritten) {
                    return report_fatal(rewritten.error());
                }
                log_mods.info("updated {}", lock_path);
            } else {
                const auto detail{
                    verified
                        ? fmt::format("fingerprint drift: lock says {}, "
                                      "resolved {}",
                                      stored->fingerprint(), lock.fingerprint())
                        : verified.error().message};
                return report_fatal(refrain::make_error(
                    refrain::error_code::version_mismatch,
                    fmt::format("mods.lock: {} is out of date: {} (re-run with "
                                "--update-lock to rewrite it)",
                                lock_path, detail)));
            }
        }

        std::size_t builtin_count{0};
        std::size_t first_party_count{0};
        std::size_t user_count{0};
        for (const auto &selected : lock.mods) {
            switch (selected.origin) {
            case refrain::mod::mod_origin::builtin:
                ++builtin_count;
                break;
            case refrain::mod::mod_origin::first_party:
                ++first_party_count;
                break;
            case refrain::mod::mod_origin::user:
                ++user_count;
                break;
            }
        }
        log_mods.info("{} builtin={} first_party={} user={}", lock.mods.size(),
                      builtin_count, first_party_count, user_count);
        log_cli.info("{}", refrain::mod::format_mod_lock(lock));
    }

    // Host/runtime split: the host owns bootstrap only. An
    // explicit `--world` asks the active `refrain:runtime` provider to run the
    // tick loop; without it the process keeps the startup-check
    // behaviour (`--dump-gamepack` and CI composition smokes).
    if (resolved_world_path.empty()) {
        return 0;
    }

    const auto runtime_provider{
        mods.services().active_provider("refrain:runtime")};
    if (!runtime_provider) {
        return report_fatal(refrain::make_error(
            refrain::error_code::not_found,
            "refrain-server: --world requires an active 'refrain:runtime' "
            "provider; the gamepack did not select one"));
    }
    const auto *runtime_api{static_cast<const refrain::mod::provider_api *>(
        runtime_provider->instance)};
    if (runtime_api == nullptr || runtime_api->vtable == nullptr) {
        return report_fatal(refrain::make_error(
            refrain::error_code::mod_error,
            "refrain-server: 'refrain:runtime' provider has no ABI vtable"));
    }
    const refrain::services::runtime::provider runtime{
        *static_cast<const refrain_services_runtime_vtable *>(
            runtime_api->vtable),
        runtime_api->self};

    // Pure data assembly: every string is borrowed from the CLI locals above
    // and stays alive for the whole `run` call.
    auto runtime_config{refrain::services::runtime::make_runtime_config()};
    runtime_config.world_path =
        refrain::contracts::to_borrowed(std::string_view{resolved_world_path});
    runtime_config.ticks = run_ticks;
    runtime_config.command_file =
        refrain::contracts::to_borrowed(std::string_view{command_file});
    runtime_config.script_file =
        refrain::contracts::to_borrowed(std::string_view{script_file});
    runtime_config.hash_every = hash_every;
    runtime_config.world_zstd_level = world_zstd_level;
    runtime_config.zstd_near_level = zstd_near_level;
    runtime_config.zstd_mid_level = zstd_mid_level;
    runtime_config.zstd_far_level = zstd_far_level;
    runtime_config.zstd_near_radius = zstd_near_radius;
    runtime_config.zstd_mid_radius = zstd_mid_radius;
    runtime_config.watchdog_timeout_ms = watchdog_timeout_ms;
    runtime_config.autosave_interval_ticks = autosave_interval_ticks;
    runtime_config.compact_interval_ticks = compact_interval_ticks;
    runtime_config.compact_min_bytes = compact_min_bytes;
    runtime_config.compact_garbage_percent = compact_garbage_percent;
    runtime_config.store_compact_interval_ticks = store_compact_interval_ticks;
    runtime_config.content_store =
        refrain::contracts::to_borrowed(std::string_view{content_store});
    runtime_config.content_gc_apply =
        content_gc_mode ==
        refrain::mod::startup_content_options::gc_mode::apply;
    runtime_config.content_gc_keep_days = gc_keep_days;
    runtime_config.content_gc_budget_ms = gc_budget_ms;
    runtime_config.content_gc_interval_ticks =
        content_gc_mode == refrain::mod::startup_content_options::gc_mode::off
            ? 0
            : content_gc_interval_ticks;

    auto exit_code{runtime.run(runtime_config)};
    if (!exit_code) {
        return report_fatal(refrain::make_error(
            refrain::error_code::mod_error,
            fmt::format("refrain-server: 'refrain:runtime' provider failed: {}",
                        runtime_error_name(exit_code.error()))));
    }
    return *exit_code;
}
