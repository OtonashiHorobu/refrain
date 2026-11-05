// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/app/composition.hpp>
#include <refrain/app/services.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
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
#include <refrain/data/rfs.hpp>
#include <refrain/data/store_lock.hpp>
#include <refrain/data/vfs.hpp>
#include <refrain/kernel/crash.hpp>
#include <refrain/mod/discovery.hpp>
#include <refrain/mod/gamepack.hpp>
#include <refrain/mod/host.hpp>
#include <refrain/mod/manifest.hpp>
#include <refrain/mod/mod_ref.hpp>
#include <refrain/mod/rfmod.hpp>
#include <refrain/script/wasm.hpp>
#include <schema/schema.hpp>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace refrain::app {

namespace archive = refrain::archive;
namespace config = refrain::core::config;
namespace core = refrain::core;
namespace data = refrain::data;
namespace io = refrain::core::io;
namespace log = refrain::log;
namespace mod = refrain::mod;
namespace schema = refrain::schema;
namespace script = refrain::script;

// --- mods (manifest tree) --------------------------------------------------

/// `mods list`: scan one or more instance roots (each containing
/// `mods/<id>/manifest.toml`) and optionally re-verify a `mods.lock`.
///
/// `--gamepack` + `--image` seed the same `origin=builtin` candidates the
/// server derives from the mod host, so a server-written lock (builtin + user)
/// can be re-verified here; both options must be given together.
/// Does `mod` declare the contract behind a gamepack provider slot?
/// Gamepack keys are short slot names (`data`, `protocol`, ...) while
/// `[provides]` lists full contract ids (`refrain:data.packs`,
/// `refrain:net.protocol`). A slot is satisfied by
/// the exact contract, a sub-contract in its namespace (`data` ->
/// `refrain:data.packs`) or the default network namespace (`protocol` ->
/// `refrain:net.protocol`, `session` -> `refrain:net.session`). The check stays
/// advisory: the first-party providers are not implemented yet.
[[nodiscard]] auto declares_provider_service(const mod::candidate &mod,
                                             std::string_view service) -> bool {
    const auto exact{fmt::format("refrain:{}", service)};
    const auto sub_contract{exact + "."};
    const auto network_contract{fmt::format("refrain:net.{}", service)};
    return std::any_of(mod.provides.begin(), mod.provides.end(),
                       [&](const std::string &provided) {
                           return provided == exact ||
                                  provided.starts_with(sub_contract) ||
                                  provided == network_contract;
                       });
}

auto run_mods_check(const mods_check_options &options) -> int {
    // One scanner for every caller: top-level metadata files
    // are skipped, but a directory under mods/ without a manifest is a broken
    // tree and stays an error.
    auto mods{mod::scan_mods_tree(
        options.mods_dir, mod::mod_origin::builtin,
        mod::discover_options{.require_manifest_in_directories = true})};
    if (!mods) {
        return report_error(mods.error());
    }
    std::cout << "mods: " << mods->size() << " manifest(s) in "
              << options.mods_dir << '\n';
    for (const auto &candidate : *mods) {
        std::cout << "  " << mod::format_candidate(candidate);
    }

    if (options.gamepack.empty()) {
        return 0;
    }

    auto spec{mod::load_gamepack_spec(options.gamepack, "server")};
    if (!spec) {
        return report_error(spec.error());
    }
    auto plan{mod::preview_composition(*spec)};
    if (!plan) {
        return report_error(plan.error());
    }
    std::size_t missing{0};
    std::size_t mismatched{0};
    for (const auto &entry : plan->spec.mods) {
        const auto found{std::find_if(mods->begin(), mods->end(),
                                      [&entry](const mod::candidate &mod) {
                                          return mod.id == entry.ref.id;
                                      })};
        if (found == mods->end()) {
            std::cout << "  missing manifest: " << entry.ref.text() << '\n';
            ++missing;
            continue;
        }
        if (found->version != entry.ref.version) {
            std::cout << "  version mismatch: " << entry.ref.text()
                      << " but mods/" << found->id << "/"
                      << mod::mod_manifest_filename << " declares "
                      << found->version << '\n';
            ++mismatched;
        }
    }
    for (const auto &provider : plan->providers) {
        if (!provider.provider.empty()) {
            const auto at{provider.provider.find('@')};
            const auto id{provider.provider.substr(0, at)};
            const auto found{std::find_if(
                mods->begin(), mods->end(),
                [&id](const mod::candidate &mod) { return mod.id == id; })};
            if (found == mods->end()) {
                std::cout << "  provider without manifest: service "
                          << provider.service << " -> " << provider.provider
                          << '\n';
                ++missing;
            }
        }
    }
    if (missing != 0 || mismatched != 0) {
        if (options.strict) {
            log::error(
                "mods check: {} missing manifest(s), {} version mismatch(es) "
                "(--strict)",
                missing, mismatched);
            return 1;
        }
        std::cout << "  note: " << missing << " manifest(s) missing and "
                  << mismatched << " version mismatch(es); not an error "
                  << "without --strict (mods may be installed incrementally)\n";
    }
    // A resolved provider whose mod does not declare the matching
    // `[provides]` contract is a planned (metadata-only) provider. Warn on
    // stdout without failing: first-party providers add their code and
    // declarations over time (--strict keeps its manifest-only meaning).
    std::size_t planned_provider_warnings{0};
    const auto check_provider = [&](std::string_view service,
                                    std::string_view reference) {
        const auto at{reference.find('@')};
        const auto id{std::string{reference.substr(0, at)}};
        const auto found{std::find_if(
            mods->begin(), mods->end(),
            [&id](const mod::candidate &mod) { return mod.id == id; })};
        if (found == mods->end() ||
            declares_provider_service(*found, service)) {
            return; // a missing manifest is reported above
        }
        std::cout << fmt::format(
            "warning: provider '{}' -> '{}' does not declare [provides] "
            "'refrain:{}' (planned; provider code is not implemented yet)\n",
            service, reference, service);
        ++planned_provider_warnings;
    };
    for (const auto &provider : plan->providers) {
        if (!provider.provider.empty()) {
            check_provider(provider.service, provider.provider);
        }
        for (const auto &instance : provider.instances) {
            check_provider(provider.service, instance.provider);
        }
        for (const auto &wrapper : provider.wrappers) {
            check_provider(provider.service, wrapper);
        }
    }
    std::cout << "mods check: OK (" << plan->spec.mods.size()
              << " declared by gamepack " << plan->spec.id;
    if (planned_provider_warnings != 0) {
        std::cout << ", " << planned_provider_warnings
                  << " planned provider warning(s)";
    }
    std::cout << ")\n";
    return 0;
}

/// Seed the builtin candidates from the image through the shared composition
/// pipeline (no host load: `mods list` only inspects the catalog).
[[nodiscard]] auto discover_builtin_mods(const mods_list_options &options)
    -> refrain::result<std::vector<mod::candidate>> {
    auto image{io::read_file(options.image)};
    if (!image) {
        return std::unexpected{image.error()};
    }
    auto composition{mod::load_composition(
        options.gamepack, {},
        mod::composition_load_options{.builtin_image =
                                          std::span<const std::byte>{*image},
                                      .host_side = "server"})};
    if (!composition) {
        return std::unexpected{composition.error()};
    }
    return std::move(composition->load_order);
}

auto run_mods_list(const mods_list_options &options) -> int {
    if (options.gamepack.empty() != options.image.empty()) {
        return report_error(refrain::make_error(
            refrain::error_code::invalid_argument,
            "mods list: --gamepack and --image must be given together (they "
            "seed the builtin composition for --check-lock)"));
    }
    if (options.roots.empty() && options.gamepack.empty()) {
        return report_error(refrain::make_error(
            refrain::error_code::invalid_argument,
            "mods list: at least one --mods <instance root> is required (a "
            "root contains mods/<id>/manifest.toml) unless --gamepack and "
            "--image provide the builtin composition"));
    }

    std::vector<mod::discovered_mod> discovered{};
    if (!options.gamepack.empty()) {
        auto builtin{discover_builtin_mods(options)};
        if (!builtin) {
            return report_error(builtin.error());
        }
        discovered = std::move(*builtin);
    }
    if (!options.roots.empty()) {
        std::vector<mod::discovery_root> roots{};
        roots.reserve(options.roots.size());
        for (const auto &root : options.roots) {
            roots.push_back(mod::discovery_root{
                .path = root, .origin = mod::mod_origin::user});
        }
        // An explicit `--mods` root must actually be an instance root
        // (contain a `mods/` directory): silently treating a typo as an empty
        // tree would make `--check-lock` pass without scanning anything.
        auto scanned{mod::scan(
            roots, mod::discover_options{.strict_entries = options.strict,
                                         .require_mods_dir = true})};
        if (!scanned) {
            return report_error(scanned.error());
        }
        for (auto &candidate : *scanned) {
            discovered.push_back(std::move(candidate));
        }
    }

    if (discovered.empty()) {
        std::cout << "mods: 0 candidate(s)\n";
    } else {
        std::cout << fmt::format("mods: {} candidate(s) in {} root(s)\n",
                                 discovered.size(), options.roots.size());
    }
    for (const auto &candidate : discovered) {
        std::cout << fmt::format(
            "  {}@{} [{}] {}\n", candidate.id, candidate.version,
            mod::to_string(candidate.origin), candidate.path);
        for (const auto &shadowed : candidate.shadowed) {
            std::cout << fmt::format(
                "    shadowed: {}@{} [{}] {}\n", candidate.id, shadowed.version,
                mod::to_string(shadowed.origin), shadowed.path);
        }
    }
    if (options.check_lock.empty()) {
        return 0;
    }
    auto lock{mod::read_mod_lock(options.check_lock)};
    if (!lock) {
        return report_error(lock.error());
    }
    const auto verified{mod::verify_mod_lock(
        *lock, std::span<const mod::discovered_mod>{discovered})};
    if (!verified) {
        return report_error(verified.error());
    }
    // verify_mod_lock() is one-way (every lock entry must be discovered): a
    // discovered mod missing from the lock means the lock predates the current
    // composition, so --check-lock would otherwise accept a stale lock.
    for (const auto &candidate : discovered) {
        const auto known{
            std::any_of(lock->mods.begin(), lock->mods.end(),
                        [&candidate](const mod::locked_mod &locked) {
                            return locked.id == candidate.id;
                        })};
        if (!known) {
            return report_error(refrain::make_error(
                refrain::error_code::version_mismatch,
                fmt::format("lock is stale: discovered '{}' is not in the lock",
                            candidate.id)));
        }
    }
    std::cout << fmt::format("lock: {} verified (fingerprint {})\n",
                             options.check_lock, lock->fingerprint());
    return 0;
}

// --- mod host (load built-in mods) -----------------------------------------

auto run_mods_load(const mods_load_options &options) -> int {
    if (options.image.empty()) {
        return report_error(refrain::make_error(
            refrain::error_code::invalid_argument,
            "mods load: --image <builtin.image> is required (the development "
            "tree has no embedded image in refrain-tools; the server/tests "
            "embed it at build time)"));
    }
    auto image{io::read_file(options.image)};
    if (!image) {
        return report_error(image.error());
    }
    auto spec{mod::load_gamepack_spec(options.gamepack, options.side)};
    if (!spec) {
        return report_error(spec.error());
    }
    auto loaded{load_server_mods(
        *spec,
        server_mods_options{.builtin_image = std::span<const std::byte>{*image},
                            .auto_scan = false})};
    if (!loaded) {
        return report_error(loaded.error());
    }
    std::cout << (*loaded)->host.format();
    return 0;
}

// --- mods (.rfmod packages) ------------------------------------------------

/// Build the trust anchors for `mods verify`/`mods install`. `--key-hex` and
/// `--key-id` are a pair: one without the other is a usage error, neither means
/// "no trust anchors configured" (unsigned packages stay acceptable unless
/// `--require-signature` is passed).
[[nodiscard]] auto make_rfmod_trusted_keys(std::string_view command,
                                           std::string_view key_hex,
                                           std::string_view key_id)
    -> refrain::result<std::vector<mod::rfmod_trusted_key>> {
    if (key_hex.empty() != key_id.empty()) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::invalid_argument,
            std::string{command} +
                ": --key-hex and --key-id must be given together")};
    }
    std::vector<mod::rfmod_trusted_key> keys{};
    if (key_hex.empty()) {
        return keys;
    }
    auto public_key{data::parse_public_key_hex(key_hex)};
    if (!public_key) {
        return std::unexpected{public_key.error()};
    }
    keys.push_back(mod::rfmod_trusted_key{.key_id = std::string{key_id},
                                          .public_key = *public_key});
    return keys;
}

[[nodiscard]] auto
signature_line(const std::optional<mod::rfmod_signature_info> &signature)
    -> std::string {
    if (!signature) {
        return "signature: unsigned";
    }
    return fmt::format("signature: {} key_id={}", signature->scheme,
                       signature->key_id);
}

auto run_mods_pack(const mods_pack_options &options) -> int {
    if (options.out.empty()) {
        return report_error(
            refrain::make_error(refrain::error_code::invalid_argument,
                                "mods pack: --out <out.rfmod> is required"));
    }
    if (options.level < 1 || options.level > 19) {
        return report_error(
            refrain::make_error(refrain::error_code::invalid_argument,
                                "mods pack: --level must be in [1, 19]"));
    }
    if (options.seed_hex.empty() != options.key_id.empty()) {
        return report_error(refrain::make_error(
            refrain::error_code::invalid_argument,
            "mods pack: --seed-hex and --key-id must be given together"));
    }

    mod::rfmod_write_options write_options{};
    write_options.zstd_level = options.level;
    std::string public_key_hex{};
    if (!options.seed_hex.empty()) {
        auto seed{data::parse_seed_hex(options.seed_hex)};
        if (!seed) {
            return report_error(seed.error());
        }
        auto keypair{data::keypair_from_seed(*seed)};
        if (!keypair) {
            return report_error(keypair.error());
        }
        write_options.signing = mod::rfmod_signing_options{
            .secret_key = keypair->secret_key, .key_id = options.key_id};
        public_key_hex = data::to_hex(keypair->public_key);
    }

    auto packed{
        mod::pack_mod_directory(options.mod_dir, options.out, write_options)};
    if (!packed) {
        return report_error(packed.error());
    }
    // Re-open and verify the package we just wrote (no trust anchors) so the
    // summary comes from the package itself instead of a second TOML parser.
    auto reader{mod::rfmod_reader::open_file(packed->archive_path)};
    if (!reader) {
        return report_error(reader.error());
    }
    auto verified{mod::verify_rfmod(*reader)};
    if (!verified) {
        return report_error(verified.error());
    }

    const auto &common{verified->manifest.common};
    std::cout << fmt::format("packed {}@{}: {} file(s), {} byte(s)\n",
                             common.id, common.version, packed->file_count,
                             packed->total_bytes);
    std::cout << "archive: " << packed->archive_path << '\n';
    std::cout << "blake3: " << data::to_hex(packed->archive_digest) << '\n';
    if (write_options.signing) {
        std::cout << "signature: ed25519 key_id=" << options.key_id << '\n';
        std::cout << "public key: " << public_key_hex << '\n';
    } else {
        std::cout << "signature: unsigned\n";
    }
    return 0;
}

auto run_mods_verify(const mods_verify_options &options) -> int {
    auto trusted_keys{make_rfmod_trusted_keys("mods verify", options.key_hex,
                                              options.key_id)};
    if (!trusted_keys) {
        return report_error(trusted_keys.error());
    }
    auto reader{mod::rfmod_reader::open_file(options.archive)};
    if (!reader) {
        return report_error(reader.error());
    }

    mod::rfmod_verify_options verify_options{};
    verify_options.trusted_keys = std::move(*trusted_keys);
    verify_options.require_signature = options.require_signature;
    auto verified{mod::verify_rfmod(*reader, verify_options)};
    if (!verified) {
        return report_error(verified.error());
    }

    const auto &common{verified->manifest.common};
    std::cout << fmt::format("mod {}@{} ({} file(s))\n", common.id,
                             common.version, reader->file_count());
    std::cout << "archive blake3: " << data::to_hex(verified->archive_digest)
              << '\n';
    std::cout << "manifest blake3: " << data::to_hex(verified->manifest_digest)
              << '\n';
    std::cout << signature_line(verified->signature) << '\n';
    std::cout << "trusted: " << (verified->trusted ? "yes" : "no") << '\n';
    std::cout << "verify: OK\n";
    return 0;
}

/// Best-effort install-time cook: pack the validated package members into an
/// RFS1 container under `<store>/content/cook/<id>-<version>.rfs`. Raw records
/// are enough at this layer; `data cook` applies JSON/NBT canonicalization on
/// loose directories.
[[nodiscard]] auto write_install_cook(const mod::rfmod_reader &reader,
                                      const data::content_store &store,
                                      std::string_view id,
                                      std::string_view version)
    -> refrain::result<std::string> {
    std::vector<data::rfs_input> records{};
    std::vector<std::vector<std::byte>> owners{};
    records.reserve(reader.entries().size());
    owners.reserve(reader.entries().size());
    for (const auto &entry : reader.entries()) {
        auto bytes{reader.read(entry.path)};
        if (!bytes) {
            return std::unexpected{bytes.error()};
        }
        owners.push_back(std::move(*bytes));
        records.push_back(
            data::rfs_input{.key = entry.path,
                            .data = std::span<const std::byte>{owners.back()}});
    }
    auto rfs{data::build_rfs(records, data::rfs_schema_hash{})};
    if (!rfs) {
        return std::unexpected{rfs.error()};
    }
    const auto directory{fmt::format("{}/content/cook", store.root())};
    if (auto created{core::io::create_directories(directory)}; !created) {
        return std::unexpected{created.error()};
    }
    auto safe = [](std::string_view value) {
        std::string out{value};
        std::replace(out.begin(), out.end(), '/', '_');
        std::replace(out.begin(), out.end(), '\\', '_');
        return out;
    };
    const auto path{
        fmt::format("{}/{}-{}.rfs", directory, safe(id), safe(version))};
    if (auto written{core::io::atomic_write_file(path, *rfs)}; !written) {
        return std::unexpected{written.error()};
    }
    return path;
}

auto run_mods_install(const mods_install_options &options) -> int {
    if (options.store.empty()) {
        return report_error(
            refrain::make_error(refrain::error_code::invalid_argument,
                                "mods install: --store <dir> is required"));
    }
    auto trusted_keys{make_rfmod_trusted_keys("mods install", options.key_hex,
                                              options.key_id)};
    if (!trusted_keys) {
        return report_error(trusted_keys.error());
    }
    auto reader{mod::rfmod_reader::open_file(options.archive)};
    if (!reader) {
        return report_error(reader.error());
    }
    auto store{data::content_store::open(options.store)};
    if (!store) {
        return report_error(store.error());
    }

    mod::rfmod_install_options install_options{};
    install_options.trusted_keys = std::move(*trusted_keys);
    install_options.require_signature = options.require_signature;
    // Receipts are mandatory for `mods install`: they are the install record
    // read by `mods installed`, startup indexing and GC. The library option
    // (`rfmod_install_options::write_receipt`) remains for internal/test use.
    install_options.write_receipt = true;
    auto installed{mod::install_rfmod(*reader, *store, install_options)};
    if (!installed) {
        return report_error(installed.error());
    }

    const auto &common{installed->manifest.common};
    std::cout << fmt::format("installed {}@{} into {}\n", common.id,
                             common.version, options.store);
    std::cout << fmt::format("files: {} (new blobs: {}, members: {})\n",
                             installed->files.size(), installed->new_blobs,
                             installed->stored_blobs);
    std::cout << "archive blake3: " << data::to_hex(installed->archive_digest)
              << '\n';
    std::cout << "manifest blake3: " << data::to_hex(installed->manifest_digest)
              << '\n';
    if (installed->signed_package) {
        std::cout << "signature: ed25519 key_id=" << installed->key_id << '\n';
    } else {
        std::cout << "signature: unsigned\n";
    }
    std::cout << "trusted: " << (installed->trusted ? "yes" : "no") << '\n';
    if (!installed->receipt_path.empty()) {
        std::cout << "receipt: " << installed->receipt_path << '\n';
    }
    if (options.cook) {
        auto cooked{
            write_install_cook(*reader, *store, common.id, common.version)};
        if (!cooked) {
            std::cerr << fmt::format("warning: install cook failed: {}\n",
                                     cooked.error().message);
        } else {
            std::cout << "cook: " << *cooked << '\n';
        }
    }
    std::cout << "install: OK\n";
    return 0;
}

// --- installed receipts ----------------------------------------------------

/// One parsed receipt plus the SemVer value used for deterministic ordering.
struct listed_receipt {
    mod::mod_receipt receipt{};
    core::semver parsed_version{};
};

auto run_mods_installed(const mods_installed_options &options) -> int {
    if (options.store.empty()) {
        return report_error(
            refrain::make_error(refrain::error_code::invalid_argument,
                                "mods installed: --store <dir> is required"));
    }
    auto paths{mod::list_receipt_paths(options.store)};
    if (!paths) {
        return report_error(paths.error());
    }
    std::vector<listed_receipt> receipts{};
    receipts.reserve(paths->size());
    for (const auto &path : *paths) {
        auto receipt{mod::read_receipt(path)};
        if (!receipt) {
            return report_error(refrain::error{
                .code = receipt.error().code,
                .message = "mods installed: " + receipt.error().message});
        }
        // A receipt lives at <id>/<version>.json; a mismatch means a
        // stale/moved record, which must not be presented as truth.
        const auto slash{path.find_last_of('/')};
        const auto filename{slash == std::string::npos
                                ? std::string_view{path}
                                : std::string_view{path}.substr(slash + 1)};
        const auto parent{slash == std::string::npos
                              ? std::string_view{}
                              : std::string_view{path}.substr(0, slash)};
        const auto parent_slash{parent.find_last_of('/')};
        const auto directory{parent_slash == std::string::npos
                                 ? parent
                                 : parent.substr(parent_slash + 1)};
        const auto stem{filename.size() > 5U
                            ? filename.substr(0, filename.size() - 5U)
                            : std::string_view{}};
        if (receipt->id != directory ||
            std::string_view{receipt->version} != stem) {
            return report_error(refrain::make_error(
                refrain::error_code::parse_error,
                fmt::format("mods installed: '{}': receipt id/version "
                            "does not match its path",
                            path)));
        }
        auto parsed_version{core::semver::parse(receipt->version)};
        if (!parsed_version) {
            // read_receipt already validated the SemVer; keep the listing
            // fail-closed if the two ever diverge.
            return report_error(parsed_version.error());
        }
        receipts.push_back(
            listed_receipt{.receipt = std::move(*receipt),
                           .parsed_version = std::move(*parsed_version)});
    }
    std::sort(receipts.begin(), receipts.end(),
              [](const listed_receipt &lhs, const listed_receipt &rhs) {
                  if (lhs.receipt.id != rhs.receipt.id) {
                      return lhs.receipt.id < rhs.receipt.id;
                  }
                  const auto order{
                      core::compare(lhs.parsed_version, rhs.parsed_version)};
                  if (order != std::strong_ordering::equal) {
                      return order == std::strong_ordering::less;
                  }
                  // Equal precedence (build metadata differs): keep the
                  // listing deterministic by comparing the raw version text.
                  return lhs.receipt.version < rhs.receipt.version;
              });

    std::cout << fmt::format("installed: {} mod(s) in {}\n", receipts.size(),
                             options.store);
    for (const auto &listed : receipts) {
        const auto &receipt{listed.receipt};
        const auto trust{
            receipt.signed_package
                ? (receipt.trusted ? "signed trusted" : "signed untrusted")
                : "unsigned"};
        const auto archive_hex{data::to_hex(receipt.archive_blake3)};
        const auto archive{std::string_view{archive_hex}.substr(0, 12)};
        std::cout << fmt::format("  {}@{} [{}] archive={} files={}\n",
                                 receipt.id, receipt.version, trust, archive,
                                 receipt.files.size());
    }
    return 0;
}

} // namespace refrain::app
