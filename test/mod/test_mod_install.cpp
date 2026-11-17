// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Install/update/uninstall semantics of the `.rfmod` content-store path:
// signer continuity (Android-style), version monotonicity, same-version
// content identity, staging/atomicity, uninstall + GC reclaim, the single
// receipt schema, and the receipt-only runtime catalog.

#include <refrain/core/io.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/content_maintenance.hpp>
#include <refrain/data/content_store.hpp>
#include <refrain/data/json.hpp>
#include <refrain/data/path.hpp>
#include <refrain/data/signature.hpp>
#include <refrain/mod/discovery.hpp>
#include <refrain/mod/install_policy.hpp>
#include <refrain/mod/package.hpp>
#include <refrain/mod/rfmod.hpp>
#include <refrain/platform/process.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>

namespace {

namespace fs = std::filesystem;

/// Compare two paths independently of the platform's directory separator.
[[nodiscard]] auto same_path(std::string_view left, std::string_view right)
    -> bool {
    return fs::path{std::string{left}}.lexically_normal().generic_string() ==
           fs::path{std::string{right}}.lexically_normal().generic_string();
}

using namespace boost::ut;

namespace mod = refrain::mod;
namespace io = refrain::core::io;
namespace data = refrain::data;

using refrain::error_code;
using refrain::data::content_store;
using refrain::data::digest32;
using refrain::data::ed25519_keypair;
using refrain::mod::install_policy_config;
using refrain::mod::rfmod_install_options;
using refrain::mod::rfmod_install_result;
using refrain::mod::rfmod_reader;
using refrain::mod::rfmod_signing_options;
using refrain::mod::rfmod_trusted_key;
using refrain::mod::rfmod_write_options;

constexpr std::string_view seed_a{
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"};
constexpr std::string_view seed_b{
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
constexpr std::string_view key_a{"test-key-a"};
constexpr std::string_view key_b{"test-key-b"};

// ---------------------------------------------------------------------------
// scratch directories / files
// ---------------------------------------------------------------------------

[[nodiscard]] auto make_workspace_path(std::string_view name) -> fs::path {
    static int counter{0};
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    const auto path{fs::temp_directory_path() /
                    fmt::format("refrain_mod_install_test_{}_{}_{}_{}", stamp,
                                counter++, refrain::platform::process_id(),
                                name)};
    std::error_code error;
    fs::create_directories(path, error);
    return path;
}

struct temp_workspace {
    fs::path root{};

    explicit temp_workspace(std::string_view name)
        : root{make_workspace_path(name)} {}
    temp_workspace(const temp_workspace &) = delete;
    auto operator=(const temp_workspace &) -> temp_workspace & = delete;
    ~temp_workspace() {
        std::error_code error;
        fs::remove_all(root, error);
    }

    [[nodiscard]] auto path(std::string_view relative) const -> fs::path {
        return root / fs::path{std::string{relative}};
    }

    [[nodiscard]] auto write(std::string_view relative,
                             std::string_view content) const -> bool {
        const auto target{path(relative)};
        std::error_code error;
        fs::create_directories(target.parent_path(), error);
        if (error) {
            return false;
        }
        return io::write_text(target.string(), content).has_value();
    }
};

[[nodiscard]] auto hex_of(const digest32 &digest) -> std::string {
    return data::to_hex(digest);
}

// ---------------------------------------------------------------------------
// keys / packages
// ---------------------------------------------------------------------------

[[nodiscard]] auto keypair_from(std::string_view seed_hex) -> ed25519_keypair {
    auto seed{data::parse_seed_hex(seed_hex)};
    expect(static_cast<bool>(seed)) << "test seed is invalid";
    if (!seed) {
        return {};
    }
    auto pair{data::keypair_from_seed(*seed)};
    expect(static_cast<bool>(pair)) << "test key derivation failed";
    return pair ? *pair : ed25519_keypair{};
}

[[nodiscard]] auto keypair_a() -> ed25519_keypair {
    return keypair_from(seed_a);
}

[[nodiscard]] auto keypair_b() -> ed25519_keypair {
    return keypair_from(seed_b);
}

[[nodiscard]] auto signing_with(std::string_view key_id,
                                const ed25519_keypair &pair)
    -> rfmod_signing_options {
    return rfmod_signing_options{.secret_key = pair.secret_key,
                                 .key_id = std::string{key_id}};
}

[[nodiscard]] auto trusted_with(std::string_view key_id,
                                const ed25519_keypair &pair)
    -> rfmod_trusted_key {
    return rfmod_trusted_key{.key_id = std::string{key_id},
                             .public_key = pair.public_key};
}

[[nodiscard]] auto source_manifest(std::string_view id,
                                   std::string_view version) -> std::string {
    return fmt::format("[mod]\nid = \"{}\"\nversion = \"{}\"\nabi = \"1\"\n"
                       "side = \"both\"\n\n[meta]\ndisplay_name = \"Install "
                       "semantics test\"\n",
                       id, version);
}

/// Materialise a mod tree for `id@version` with one content payload, so a
/// rebuilt same-version package can carry different bytes.
[[nodiscard]] auto materialise(const temp_workspace &work,
                               std::string_view directory, std::string_view id,
                               std::string_view version,
                               std::string_view payload = "alpha") -> bool {
    return work.write(fmt::format("{}/manifest.toml", directory),
                      source_manifest(id, version)) &&
           work.write(fmt::format("{}/data/test/{}.json", directory, payload),
                      fmt::format(R"({{"payload":"{}"}})", payload)) &&
           work.write(fmt::format("{}/LICENSE", directory), "MIT\n");
}

/// Pack `directory` and return the archive path.
[[nodiscard]] auto pack(const temp_workspace &work, std::string_view directory,
                        std::string_view output,
                        const rfmod_write_options &options = {})
    -> std::optional<std::string> {
    const auto target{work.path(output)};
    std::error_code error;
    fs::create_directories(target.parent_path(), error);
    auto packed{mod::pack_mod_directory(work.path(directory).string(),
                                        target.string(), options)};
    expect(static_cast<bool>(packed)) << "pack_mod_directory failed";
    if (!packed) {
        return std::nullopt;
    }
    return target.string();
}

[[nodiscard]] auto install_archive(content_store &store,
                                   const std::string &archive,
                                   const rfmod_install_options &options = {})
    -> refrain::result<rfmod_install_result> {
    auto reader{rfmod_reader::open_file(archive)};
    if (!reader) {
        return std::unexpected{reader.error()};
    }
    return mod::install_rfmod(*reader, store, options);
}

[[nodiscard]] auto make_store(const temp_workspace &work,
                              std::string_view name = "store")
    -> std::optional<content_store> {
    auto store{content_store::open(work.path(name).string())};
    expect(static_cast<bool>(store)) << "content_store::open failed";
    if (!store) {
        return std::nullopt;
    }
    return std::optional<content_store>{std::move(*store)};
}

[[nodiscard]] auto count_segments(const temp_workspace &work,
                                  std::string_view store_name = "store")
    -> std::size_t {
    std::error_code error;
    std::size_t count{0};
    fs::directory_iterator iterator{
        work.path(store_name) / "content" / "segments", error};
    const fs::directory_iterator end{};
    while (!error && iterator != end) {
        std::error_code entry_error;
        if (iterator->path().extension() == ".rfseg" &&
            iterator->is_regular_file(entry_error) && !entry_error) {
            ++count;
        }
        iterator.increment(error);
    }
    return count;
}

/// Parse an installed receipt, let `mutate` change its member map and write
/// the document back (a store rewritten by another build).
template <typename Mutate>
[[nodiscard]] auto rewrite_receipt(const std::string &receipt_path,
                                   Mutate mutate) -> bool {
    auto document{data::json::value::parse_file(receipt_path)};
    if (!document) {
        return false;
    }
    auto object{document->as_object()};
    if (!object) {
        return false;
    }
    using json_value = data::json::value;
    json_value::object_type members{object->get()};
    mutate(members);
    return io::write_text(receipt_path, json_value{std::move(members)}.dump())
        .has_value();
}

// ---------------------------------------------------------------------------
// signer continuity
// ---------------------------------------------------------------------------

const suite mod_install_signer_tests = [] {
    "install/first signed install pins the signer for the mod id"_test = [] {
        temp_workspace work{"pin-first"};
        expect(materialise(work, "mod", "refrain.test.install", "1.0.0"));
        auto archive{pack(
            work, "mod", "out/mod.rfmod",
            rfmod_write_options{.signing = signing_with(key_a, keypair_a())})};
        expect(archive.has_value());
        if (!archive) {
            return;
        }
        auto store{make_store(work)};
        expect(store.has_value());
        if (!store) {
            return;
        }
        auto installed{install_archive(*store, *archive)};
        // Untrusted signature with no anchors configured is accepted but the
        // signer is pinned by key id only (no fingerprint).
        expect(static_cast<bool>(installed)) << "install failed";
        if (!installed) {
            return;
        }
        expect(installed->signed_package);
        expect(installed->signer_pinned);
        auto pin{mod::read_signer_pin(work.path("store").string(),
                                      "refrain.test.install")};
        expect(static_cast<bool>(pin));
        if (!pin || !pin->has_value()) {
            return;
        }
        expect((*pin)->key_id == key_a);
        expect((*pin)->public_key_fingerprint.empty());

        auto receipt{mod::read_receipt(installed->receipt_path)};
        expect(static_cast<bool>(receipt));
        if (!receipt) {
            return;
        }
        expect(receipt->signed_package);
        expect(receipt->key_id == key_a);
        expect(receipt->policy == "strict");
        expect(receipt->pinned());

        // Idempotent reinstall: the receipt is not rewritten.
        auto first_text{io::read_text(installed->receipt_path)};
        auto again{install_archive(*store, *archive)};
        expect(static_cast<bool>(again));
        if (!again) {
            return;
        }
        expect(again->already_installed);
        expect(again->receipt_path == installed->receipt_path);
        auto second_text{io::read_text(installed->receipt_path)};
        expect(first_text.has_value() && second_text.has_value());
        if (first_text && second_text) {
            expect(*first_text == *second_text);
        }
    };

    "install/rejects a different signer unless a rotation entry allows it"_test =
        [] {
            temp_workspace work{"rotation"};
            expect(materialise(work, "mod1", "refrain.test.install", "1.0.0"));
            auto first{pack(work, "mod1", "out/v1.rfmod",
                            rfmod_write_options{
                                .signing = signing_with(key_a, keypair_a())})};
            expect(first.has_value());
            if (!first) {
                return;
            }
            auto store{make_store(work)};
            expect(store.has_value());
            if (!store) {
                return;
            }
            auto installed{install_archive(*store, *first)};
            expect(static_cast<bool>(installed)) << "first install failed";
            if (!installed) {
                return;
            }

            expect(materialise(work, "mod2", "refrain.test.install", "1.1.0"));
            auto second{pack(work, "mod2", "out/v2.rfmod",
                             rfmod_write_options{
                                 .signing = signing_with(key_b, keypair_b())})};
            expect(second.has_value());
            if (!second) {
                return;
            }
            rfmod_install_options options;
            options.require_signature = true;
            options.trusted_keys = {trusted_with(key_a, keypair_a()),
                                    trusted_with(key_b, keypair_b())};

            // A new signer is refused under the default strict policy.
            auto refused{install_archive(*store, *second, options)};
            expect(!refused);
            if (refused) {
                return;
            }
            expect(refused.error().code == error_code::permission_denied);
            expect(refused.error().message.find("rotation") !=
                   std::string::npos);
            // The refused update left the installed version untouched.
            auto receipts{mod::list_receipt_paths_for(
                work.path("store").string(), "refrain.test.install")};
            expect(receipts.has_value() && receipts->size() == 1U);

            // The explicit rotation allowlist admits the transition and moves
            // the pin to the new key.
            options.rotations.push_back(
                mod::rfmod_key_rotation{.id = "refrain.test.install",
                                        .old_key_id = std::string{key_a},
                                        .new_key_id = std::string{key_b}});
            auto rotated{install_archive(*store, *second, options)};
            expect(static_cast<bool>(rotated)) << "rotated install failed";
            if (!rotated) {
                return;
            }
            expect(rotated->signer_rotated);
            auto pin{mod::read_signer_pin(work.path("store").string(),
                                          "refrain.test.install")};
            expect(pin.has_value() && pin->has_value());
            if (!pin || !pin->has_value()) {
                return;
            }
            expect((*pin)->key_id == key_b);
            expect((*pin)->source == "rotation");

            // The pinned signer is now B: an update signed by A again is
            // rejected without a B -> A rotation entry.
            expect(materialise(work, "mod3", "refrain.test.install", "1.2.0"));
            auto third{pack(work, "mod3", "out/v3.rfmod",
                            rfmod_write_options{
                                .signing = signing_with(key_a, keypair_a())})};
            expect(third.has_value());
            if (!third) {
                return;
            }
            auto back{install_archive(*store, *third, options)};
            expect(!back);
            if (back) {
                return;
            }
            expect(back.error().code == error_code::permission_denied);
        };

    "install/trusted-any policy admits a new signer and re-pins"_test = [] {
        temp_workspace work{"trusted-any"};
        expect(materialise(work, "mod1", "refrain.test.install", "1.0.0"));
        auto first{pack(
            work, "mod1", "out/v1.rfmod",
            rfmod_write_options{.signing = signing_with(key_a, keypair_a())})};
        expect(first.has_value());
        if (!first) {
            return;
        }
        auto store{make_store(work)};
        expect(store.has_value());
        if (!store) {
            return;
        }
        expect(static_cast<bool>(install_archive(*store, *first)));
        expect(materialise(work, "mod2", "refrain.test.install", "1.1.0"));
        auto second{pack(
            work, "mod2", "out/v2.rfmod",
            rfmod_write_options{.signing = signing_with(key_b, keypair_b())})};
        expect(second.has_value());
        if (!second) {
            return;
        }
        rfmod_install_options options;
        options.require_signature = true;
        options.trusted_keys = {trusted_with(key_a, keypair_a()),
                                trusted_with(key_b, keypair_b())};
        options.signature_policy = mod::rfmod_signature_policy::trusted_any;
        auto installed{install_archive(*store, *second, options)};
        expect(static_cast<bool>(installed)) << "trusted-any update failed";
        if (!installed) {
            return;
        }
        expect(installed->signer_rotated);
        expect(installed->receipt_policy == "trusted-any");
        auto pin{mod::read_signer_pin(work.path("store").string(),
                                      "refrain.test.install")};
        expect(pin.has_value() && pin->has_value());
        if (pin && pin->has_value()) {
            expect((*pin)->key_id == key_b);
        }
    };

    "install/rejects a reused key id with a different public key"_test = [] {
        temp_workspace work{"fingerprint"};
        expect(materialise(work, "mod1", "refrain.test.install", "1.0.0"));
        auto first{pack(
            work, "mod1", "out/v1.rfmod",
            rfmod_write_options{.signing = signing_with(key_a, keypair_a())})};
        expect(first.has_value());
        if (!first) {
            return;
        }
        auto store{make_store(work)};
        expect(store.has_value());
        if (!store) {
            return;
        }
        rfmod_install_options first_options;
        first_options.require_signature = true;
        first_options.trusted_keys = {trusted_with(key_a, keypair_a())};
        auto installed{install_archive(*store, *first, first_options)};
        expect(static_cast<bool>(installed));
        if (!installed) {
            return;
        }
        auto pin{mod::read_signer_pin(work.path("store").string(),
                                      "refrain.test.install")};
        expect(pin.has_value() && pin->has_value());
        if (!pin || !pin->has_value()) {
            return;
        }
        expect((*pin)->key_id == key_a);
        expect(!(*pin)->public_key_fingerprint.empty());

        // The same key id is re-pointed at a different public key: the
        // continuity check compares fingerprints, not the caller-chosen id.
        expect(materialise(work, "mod2", "refrain.test.install", "1.1.0"));
        auto second{pack(
            work, "mod2", "out/v2.rfmod",
            rfmod_write_options{.signing = signing_with(key_a, keypair_b())})};
        expect(second.has_value());
        if (!second) {
            return;
        }
        rfmod_install_options options;
        options.require_signature = true;
        options.trusted_keys = {trusted_with(key_a, keypair_b())};
        auto refused{install_archive(*store, *second, options)};
        expect(!refused);
        if (refused) {
            return;
        }
        expect(refused.error().code == error_code::permission_denied);
        expect(refused.error().message.find("fingerprint") !=
               std::string::npos);
    };

    "install/rejects a signed-to-unsigned update and pins an unsigned-to-signed one"_test =
        [] {
            temp_workspace work{"unsigned"};
            // Signed first install.
            expect(materialise(work, "mod1", "refrain.test.install", "1.0.0"));
            auto signed_v1{pack(work, "mod1", "out/v1.rfmod",
                                rfmod_write_options{.signing = signing_with(
                                                        key_a, keypair_a())})};
            expect(signed_v1.has_value());
            if (!signed_v1) {
                return;
            }
            auto store{make_store(work)};
            expect(store.has_value());
            if (!store) {
                return;
            }
            expect(static_cast<bool>(install_archive(*store, *signed_v1)));

            // Unsigned update of a signed install is refused.
            expect(materialise(work, "mod2", "refrain.test.install", "1.1.0"));
            auto unsigned_v2{pack(work, "mod2", "out/v2.rfmod")};
            expect(unsigned_v2.has_value());
            if (!unsigned_v2) {
                return;
            }
            auto refused{install_archive(*store, *unsigned_v2)};
            expect(!refused);
            if (refused) {
                return;
            }
            expect(refused.error().code == error_code::permission_denied);

            // Unsigned first install, then a signed update: allowed and pinned.
            temp_workspace other{"unsigned-upgrade"};
            expect(materialise(other, "mod1", "refrain.test.upgrade", "1.0.0"));
            auto unsigned_v1{pack(other, "mod1", "out/v1.rfmod")};
            expect(unsigned_v1.has_value());
            if (!unsigned_v1) {
                return;
            }
            auto upgrade_store{make_store(other)};
            expect(upgrade_store.has_value());
            if (!upgrade_store) {
                return;
            }
            auto first{install_archive(*upgrade_store, *unsigned_v1)};
            expect(static_cast<bool>(first));
            if (!first) {
                return;
            }
            expect(!first->signer_pinned);
            expect(!first->signed_package);

            expect(materialise(other, "mod2", "refrain.test.upgrade", "1.1.0"));
            auto signed_v2{pack(other, "mod2", "out/v2.rfmod",
                                rfmod_write_options{.signing = signing_with(
                                                        key_a, keypair_a())})};
            expect(signed_v2.has_value());
            if (!signed_v2) {
                return;
            }
            rfmod_install_options options;
            options.require_signature = true;
            options.trusted_keys = {trusted_with(key_a, keypair_a())};
            auto upgraded{install_archive(*upgrade_store, *signed_v2, options)};
            expect(static_cast<bool>(upgraded))
                << "unsigned-to-signed upgrade failed";
            if (!upgraded) {
                return;
            }
            expect(upgraded->signer_pinned);
            auto pin{mod::read_signer_pin(other.path("store").string(),
                                          "refrain.test.upgrade")};
            expect(pin.has_value() && pin->has_value());
            if (pin && pin->has_value()) {
                expect((*pin)->key_id == key_a);
            }
        };
};

// ---------------------------------------------------------------------------
// version monotonicity / same-version content identity
// ---------------------------------------------------------------------------

const suite mod_install_version_tests = [] {
    "install/rejects downgrades unless allow_downgrade is set"_test = [] {
        temp_workspace work{"downgrade"};
        expect(materialise(work, "mod2", "refrain.test.install", "2.0.0"));
        expect(materialise(work, "mod1", "refrain.test.install", "1.0.0"));
        auto newer{pack(work, "mod2", "out/v2.rfmod")};
        auto older{pack(work, "mod1", "out/v1.rfmod")};
        expect(newer.has_value() && older.has_value());
        if (!newer || !older) {
            return;
        }
        auto store{make_store(work)};
        expect(store.has_value());
        if (!store) {
            return;
        }
        expect(static_cast<bool>(install_archive(*store, *newer)));

        auto refused{install_archive(*store, *older)};
        expect(!refused);
        if (refused) {
            return;
        }
        expect(refused.error().code == error_code::version_mismatch);
        expect(refused.error().message.find("--allow-downgrade") !=
               std::string::npos);

        rfmod_install_options options;
        options.allow_downgrade = true;
        auto accepted{install_archive(*store, *older, options)};
        expect(static_cast<bool>(accepted)) << "explicit downgrade failed";
        if (!accepted) {
            return;
        }
        expect(accepted->downgrade_override);
        auto receipts{mod::list_receipt_paths_for(work.path("store").string(),
                                                  "refrain.test.install")};
        expect(receipts.has_value() && receipts->size() == 2U);
    };

    "install/rejects the same version with different content unless replaced"_test =
        [] {
            temp_workspace work{"same-version"};
            expect(materialise(work, "mod1", "refrain.test.install", "1.0.0",
                               "alpha"));
            expect(materialise(work, "mod2", "refrain.test.install", "1.0.0",
                               "beta"));
            auto first{pack(work, "mod1", "out/a.rfmod")};
            auto second{pack(work, "mod2", "out/b.rfmod")};
            expect(first.has_value() && second.has_value());
            if (!first || !second) {
                return;
            }
            auto store{make_store(work)};
            expect(store.has_value());
            if (!store) {
                return;
            }
            auto installed{install_archive(*store, *first)};
            expect(static_cast<bool>(installed));
            if (!installed) {
                return;
            }
            auto second_bytes{io::read_file(*second)};
            expect(static_cast<bool>(second_bytes));
            if (!second_bytes) {
                return;
            }
            expect(hex_of(installed->archive_digest) !=
                   hex_of(data::blake3(*second_bytes)));

            auto refused{install_archive(*store, *second)};
            expect(!refused);
            if (refused) {
                return;
            }
            expect(refused.error().code == error_code::already_exists);
            expect(refused.error().message.find("--replace-same-version") !=
                   std::string::npos);

            rfmod_install_options options;
            options.replace_same_version = true;
            auto replaced{install_archive(*store, *second, options)};
            expect(static_cast<bool>(replaced)) << "explicit replace failed";
            if (!replaced) {
                return;
            }
            expect(replaced->same_version_override);
            auto receipt{mod::read_receipt(replaced->receipt_path)};
            expect(static_cast<bool>(receipt));
            if (receipt) {
                expect(hex_of(receipt->archive_blake3) ==
                       hex_of(replaced->archive_digest));
            }
        };
};

// ---------------------------------------------------------------------------
// atomicity / crash debris
// ---------------------------------------------------------------------------

const suite mod_install_atomicity_tests = [] {
    "install/failure leaves the previous install intact and prunes staging"_test =
        [] {
            temp_workspace work{"atomic"};
            expect(materialise(work, "mod1", "refrain.test.install", "1.0.0"));
            expect(materialise(work, "mod2", "refrain.test.install", "1.1.0"));
            auto first{pack(work, "mod1", "out/v1.rfmod")};
            auto second{pack(work, "mod2", "out/v2.rfmod")};
            expect(first.has_value() && second.has_value());
            if (!first || !second) {
                return;
            }
            auto store{make_store(work)};
            expect(store.has_value());
            if (!store) {
                return;
            }
            auto installed{install_archive(*store, *first)};
            expect(static_cast<bool>(installed));
            if (!installed) {
                return;
            }
            const auto segments_before{count_segments(work)};

            // Simulate the debris of a crashed install and obstruct the v2
            // receipt path with a directory so the write fails after the CAS
            // commit.
            const auto staging_root{work.path("store/content/.staging")};
            const auto crashed{staging_root /
                               "refrain.test.install-1.1.0-crash"};
            std::error_code error;
            fs::create_directories(crashed / "members", error);
            expect(!error);
            const auto obstructed{
                work.path("store/mods/refrain.test.install/1.1.0.json")};
            fs::create_directories(obstructed, error);
            expect(!error);

            auto failed{install_archive(*store, *second)};
            expect(!failed);
            if (failed) {
                return;
            }
            expect(failed.error().code == error_code::io_error);
            // The stale staging tree was pruned before the attempt, and the
            // failed attempt removed its own staging directory.
            expect(!fs::exists(crashed));
            if (fs::is_directory(staging_root)) {
                for (const auto &entry : fs::directory_iterator{staging_root}) {
                    expect(false)
                        << "leftover staging entry " << entry.path().string();
                }
            }
            // The previous install is untouched and still readable.
            auto receipt{mod::read_receipt(installed->receipt_path)};
            expect(static_cast<bool>(receipt));
            if (receipt) {
                expect(receipt->version == "1.0.0");
                expect(hex_of(receipt->archive_blake3) ==
                       hex_of(installed->archive_digest));
            }
            expect(fs::is_directory(obstructed));

            // Removing the obstruction lets the same install succeed; no
            // staging debris was left for it to prune.
            fs::remove_all(obstructed, error);
            expect(!error);
            auto retried{install_archive(*store, *second)};
            expect(static_cast<bool>(retried)) << "retry after failure failed";
            if (!retried) {
                return;
            }
            expect(retried->pruned_staging == 0U);
            auto receipts{mod::list_receipt_paths_for(
                work.path("store").string(), "refrain.test.install")};
            expect(receipts.has_value() && receipts->size() == 2U);
            expect(count_segments(work) >= segments_before);
        };

    "install/serializes concurrent writers on the store lock"_test = [] {
        temp_workspace work{"concurrent"};
        expect(materialise(work, "mod1", "refrain.test.concurrent.one", "1.0.0",
                           "one"));
        expect(materialise(work, "mod2", "refrain.test.concurrent.two", "1.0.0",
                           "two"));
        auto first{pack(work, "mod1", "out/one.rfmod")};
        auto second{pack(work, "mod2", "out/two.rfmod")};
        expect(first.has_value() && second.has_value());
        if (!first || !second) {
            return;
        }
        const auto store_root{work.path("store").string()};
        auto store{content_store::open(store_root)};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }

        std::array<std::string, 2> errors{};
        std::array<bool, 2> installed{};
        const auto install_one = [&](std::size_t index,
                                     const std::string &archive) {
            auto opened{content_store::open(store_root)};
            if (!opened) {
                errors[index] = "open failed";
                return;
            }
            auto result{install_archive(*opened, archive)};
            if (!result) {
                errors[index] = result.error().message;
                return;
            }
            installed[index] = true;
        };
        std::thread writer_one{install_one, 0U, *first};
        std::thread writer_two{install_one, 1U, *second};
        writer_one.join();
        writer_two.join();
        const bool no_errors{errors[0].empty() && errors[1].empty()};
        expect(no_errors) << (no_errors ? "" : errors[0] + errors[1]);
        expect(installed[0] && installed[1]);
        auto receipts{mod::list_receipt_paths(store_root)};
        expect(receipts.has_value() && receipts->size() == 2U);

        // Reinstalling the same archive from two threads is idempotent: both
        // calls succeed and exactly one receipt exists.
        std::array<bool, 2> again{};
        const auto reinstall = [&](std::size_t index) {
            auto opened{content_store::open(store_root)};
            if (!opened) {
                return;
            }
            auto result{install_archive(*opened, *first)};
            again[index] = static_cast<bool>(result);
        };
        std::thread repeat_one{reinstall, 0U};
        std::thread repeat_two{reinstall, 1U};
        repeat_one.join();
        repeat_two.join();
        expect(again[0] && again[1]);
        auto one_receipts{mod::list_receipt_paths_for(
            store_root, "refrain.test.concurrent.one")};
        expect(one_receipts.has_value() && one_receipts->size() == 1U);
    };
};

// ---------------------------------------------------------------------------
// uninstall / GC / pin lifecycle
// ---------------------------------------------------------------------------

const suite mod_uninstall_tests = [] {
    "uninstall/removes receipts, clears the pin and lets GC reclaim the blobs"_test =
        [] {
            temp_workspace work{"uninstall"};
            expect(materialise(work, "mod1", "refrain.test.install", "1.0.0"));
            expect(materialise(work, "mod2", "refrain.test.install", "1.1.0"));
            auto first{pack(work, "mod1", "out/v1.rfmod",
                            rfmod_write_options{
                                .signing = signing_with(key_a, keypair_a())})};
            auto second{pack(work, "mod2", "out/v2.rfmod",
                             rfmod_write_options{
                                 .signing = signing_with(key_a, keypair_a())})};
            expect(first.has_value() && second.has_value());
            if (!first || !second) {
                return;
            }
            auto store{make_store(work)};
            expect(store.has_value());
            if (!store) {
                return;
            }
            expect(static_cast<bool>(install_archive(*store, *first)));
            expect(static_cast<bool>(install_archive(*store, *second)));
            expect(count_segments(work) >= 1U);

            // Removing one version keeps the receipt of the other and the pin.
            mod::rfmod_uninstall_options uninstall{};
            auto one{mod::uninstall_rfmod(*store, "refrain.test.install",
                                          "1.0.0", uninstall)};
            expect(static_cast<bool>(one));
            if (!one) {
                return;
            }
            expect(one->versions.size() == 1U);
            expect(!one->last_version);
            auto remaining{mod::list_receipt_paths_for(
                work.path("store").string(), "refrain.test.install")};
            expect(remaining.has_value() && remaining->size() == 1U);
            auto pin{mod::read_signer_pin(work.path("store").string(),
                                          "refrain.test.install")};
            expect(pin.has_value() && pin->has_value());

            // Removing the last version clears the pin.
            auto all{mod::uninstall_rfmod(*store, "refrain.test.install",
                                          std::nullopt, uninstall)};
            expect(static_cast<bool>(all));
            if (!all) {
                return;
            }
            expect(all->last_version);
            expect(all->signer_pin_cleared);
            pin = mod::read_signer_pin(work.path("store").string(),
                                       "refrain.test.install");
            expect(pin.has_value() && !pin->has_value());
            auto none{mod::list_receipt_paths_for(work.path("store").string(),
                                                  "refrain.test.install")};
            expect(none.has_value() && none->empty());

            // No receipt and no resolution index references the blobs, so the
            // next mark-sweep reclaims the segment.
            data::gc_options options;
            options.apply = true;
            // No receipt and no resolution index remain, so the root set is
            // empty by design; the caller permits removing the last install.
            options.allow_empty_roots = true;
            auto report{
                data::collect_garbage(work.path("store").string(), options)};
            expect(static_cast<bool>(report)) << "content gc failed";
            if (!report) {
                return;
            }
            expect(report->root_extra == 0U);
            // The segment still referenced by the old table loses its last
            // reference in this rebuild and is collected now.
            expect(report->queued_segments >= 1U);
            expect(report->deleted_segments >= 1U);
            expect(count_segments(work) == 0U);
            // The rebuilt table is empty: nothing is referenced any more.
            auto table_bytes{
                io::read_file(work.path("store/content/blobs.idx").string())};
            expect(static_cast<bool>(table_bytes));
            if (table_bytes) {
                auto table{data::parse_blob_table(*table_bytes)};
                expect(static_cast<bool>(table));
                if (table) {
                    expect(table->entries.empty());
                }
            }
        };

    "uninstall/keep_signer_pin keeps the signer pinned for a reinstall"_test =
        [] {
            temp_workspace work{"uninstall-keep-pin"};
            expect(materialise(work, "mod1", "refrain.test.install", "1.0.0"));
            auto first{pack(work, "mod1", "out/v1.rfmod",
                            rfmod_write_options{
                                .signing = signing_with(key_a, keypair_a())})};
            expect(first.has_value());
            if (!first) {
                return;
            }
            auto store{make_store(work)};
            expect(store.has_value());
            if (!store) {
                return;
            }
            expect(static_cast<bool>(install_archive(*store, *first)));

            mod::rfmod_uninstall_options uninstall{};
            uninstall.keep_signer_pin = true;
            auto removed{mod::uninstall_rfmod(*store, "refrain.test.install",
                                              std::nullopt, uninstall)};
            expect(static_cast<bool>(removed));
            if (!removed) {
                return;
            }
            expect(removed->last_version);
            expect(removed->signer_pin_kept);
            expect(!removed->signer_pin_cleared);
            auto pin{mod::read_signer_pin(work.path("store").string(),
                                          "refrain.test.install")};
            expect(pin.has_value() && pin->has_value());
            if (!pin || !pin->has_value()) {
                return;
            }
            expect((*pin)->key_id == key_a);

            // Reinstalling with a different signer is refused because the
            // pin survived the uninstall.
            expect(materialise(work, "mod2", "refrain.test.install", "2.0.0"));
            auto other{pack(work, "mod2", "out/v2.rfmod",
                            rfmod_write_options{
                                .signing = signing_with(key_b, keypair_b())})};
            expect(other.has_value());
            if (!other) {
                return;
            }
            rfmod_install_options options;
            options.require_signature = true;
            options.trusted_keys = {trusted_with(key_b, keypair_b())};
            auto refused{install_archive(*store, *other, options)};
            expect(!refused);
            if (refused) {
                return;
            }
            expect(refused.error().code == error_code::permission_denied);
        };
};

// ---------------------------------------------------------------------------
// receipt schema
// ---------------------------------------------------------------------------

const suite mod_receipt_schema_tests = [] {
    "receipts/rejects an older development-store schema instead of migrating"_test =
        [] {
            temp_workspace work{"old-schema"};
            expect(materialise(work, "mod1", "refrain.test.install", "1.0.0"));
            auto first{pack(work, "mod1", "out/v1.rfmod",
                            rfmod_write_options{
                                .signing = signing_with(key_a, keypair_a())})};
            expect(first.has_value());
            if (!first) {
                return;
            }
            auto store{make_store(work)};
            expect(store.has_value());
            if (!store) {
                return;
            }
            auto installed{install_archive(*store, *first)};
            expect(static_cast<bool>(installed));
            if (!installed) {
                return;
            }
            expect(rewrite_receipt(installed->receipt_path, [](auto &members) {
                using json_value = data::json::value;
                members.insert_or_assign("schema",
                                         json_value{"refrain.mod-receipt/2"});
            }));

            // Fail closed and name both schemas; no rewrite happens.
            auto rejected{mod::read_receipt(installed->receipt_path)};
            expect(!rejected);
            if (!rejected) {
                expect(rejected.error().code == error_code::parse_error);
                expect(rejected.error().message.find("refrain.mod-receipt/2") !=
                       std::string::npos);
                expect(rejected.error().message.find("refrain.mod-receipt/1") !=
                       std::string::npos);
            }
            auto untouched{io::read_text(installed->receipt_path)};
            expect(untouched.has_value() &&
                   untouched->find("refrain.mod-receipt/2") !=
                       std::string::npos)
                << "an old receipt must never be rewritten in place";

            // The runtime catalog rejects the store instead of loading the
            // package, so the failure is visible at startup too.
            const std::vector<std::string> roots{work.path("store").string()};
            auto catalog{mod::scan_installed(roots)};
            expect(!catalog);
            if (!catalog) {
                expect(catalog.error().code == error_code::parse_error);
            }

            // Reinstalling the package (after removing the old receipt)
            // recreates the receipt with the current schema.
            std::error_code remove_error;
            fs::remove(installed->receipt_path, remove_error);
            expect(!remove_error);
            auto reinstalled{install_archive(*store, *first)};
            expect(static_cast<bool>(reinstalled))
                << "reinstalling the package did not recover the store";
            if (!reinstalled) {
                return;
            }
            auto parsed{mod::read_receipt(reinstalled->receipt_path)};
            expect(static_cast<bool>(parsed));
            if (parsed) {
                expect(parsed->pinned());
                expect(parsed->policy == "strict");
            }
        };

    "receipts/rejects the legacy policy and a receipt without signer fields"_test =
        [] {
            temp_workspace work{"legacy-fields"};
            expect(materialise(work, "mod1", "refrain.test.install", "1.0.0"));
            auto first{pack(work, "mod1", "out/v1.rfmod",
                            rfmod_write_options{
                                .signing = signing_with(key_a, keypair_a())})};
            expect(first.has_value());
            if (!first) {
                return;
            }
            auto store{make_store(work)};
            expect(store.has_value());
            if (!store) {
                return;
            }
            auto installed{install_archive(*store, *first)};
            expect(static_cast<bool>(installed));
            if (!installed) {
                return;
            }
            const auto original{io::read_text(installed->receipt_path)};
            expect(original.has_value());
            if (!original) {
                return;
            }

            // The single schema has no legacy policy value.
            expect(rewrite_receipt(installed->receipt_path, [](auto &members) {
                using json_value = data::json::value;
                members.insert_or_assign("policy", json_value{"legacy"});
            }));
            auto legacy_policy{mod::read_receipt(installed->receipt_path)};
            expect(!legacy_policy);
            if (!legacy_policy) {
                expect(legacy_policy.error().code == error_code::parse_error);
                expect(legacy_policy.error().message.find("legacy") !=
                       std::string::npos);
            }

            // A receipt without the signer identity or the audit fields is
            // not the current schema and is rejected, never defaulted.
            for (const auto *field : {"public_key_fingerprint", "policy",
                                      "installed_at", "installer_version"}) {
                expect(work.write("store/mods/refrain.test.install/1.0.0.json",
                                  *original));
                auto document{
                    data::json::value::parse_file(installed->receipt_path)};
                expect(static_cast<bool>(document));
                if (!document) {
                    return;
                }
                auto object{document->as_object()};
                expect(static_cast<bool>(object));
                if (!object) {
                    return;
                }
                using json_value = data::json::value;
                json_value::object_type members{object->get()};
                expect(members.erase(field) == 1U);
                expect(io::write_text(installed->receipt_path,
                                      json_value{std::move(members)}.dump())
                           .has_value());
                auto missing{mod::read_receipt(installed->receipt_path)};
                expect(!missing)
                    << "missing field " << field << " was accepted";
                if (!missing) {
                    expect(missing.error().message.find(field) !=
                           std::string::npos);
                }
            }
        };
};

// ---------------------------------------------------------------------------
// configuration policy
// ---------------------------------------------------------------------------

const suite mod_policy_tests = [] {
    "policy/loads trust anchors, rotations and install switches"_test = [] {
        temp_workspace work{"policy"};
        const auto policy_path{work.path("mods.toml").string()};
        const auto public_a{refrain::data::to_hex(keypair_a().public_key)};
        const auto public_b{refrain::data::to_hex(keypair_b().public_key)};
        expect(work.write("mods.toml",
                          fmt::format("[signature]\n"
                                      "require = true\n"
                                      "policy = \"trusted-any\"\n"
                                      "\n"
                                      "[trust]\n"
                                      "\"key-a\" = \"{}\"\n"
                                      "\"key-b\" = \"{}\"\n"
                                      "\n"
                                      "[install]\n"
                                      "allow_downgrade = true\n"
                                      "replace_same_version = true\n"
                                      "keep_signer_pin = true\n"
                                      "\n"
                                      "[rotation.\"refrain.test.install\"]\n"
                                      "old_key_id = \"key-a\"\n"
                                      "new_key_id = \"key-b\"\n",
                                      public_a, public_b)));
        auto policy{mod::load_install_policy(policy_path)};
        expect(static_cast<bool>(policy)) << "policy load failed";
        if (!policy) {
            return;
        }
        expect(policy->require_signature);
        expect(policy->signature_policy ==
               mod::rfmod_signature_policy::trusted_any);
        expect(policy->trusted_keys.size() == 2U);
        expect(policy->allow_downgrade);
        expect(policy->replace_same_version);
        expect(policy->keep_signer_pin);
        expect(policy->rotations.size() == 1U);
        if (!policy->rotations.empty()) {
            expect(policy->rotations.front().id == "refrain.test.install");
            expect(policy->rotations.front().old_key_id == "key-a");
            expect(policy->rotations.front().new_key_id == "key-b");
        }
        expect(policy->source == policy_path);
        const auto text{mod::format_install_policy(*policy)};
        expect(text.find("key-a=") != std::string::npos);
        expect(text.find("rotation: refrain.test.install key-a -> key-b") !=
               std::string::npos);

        // The policy maps onto install options, and one-run overrides only
        // touch the requested values.
        mod::rfmod_install_options options;
        mod::apply_install_policy(*policy, options);
        expect(options.require_signature);
        expect(options.trusted_keys.size() == 2U);
        expect(options.allow_downgrade);
        expect(options.replace_same_version);
        expect(options.signature_policy ==
               mod::rfmod_signature_policy::trusted_any);
        expect(options.rotations.size() == 1U);

        mod::install_policy_overrides overrides{};
        overrides.allow_downgrade = false;
        overrides.require_signature = false;
        overrides.trusted_keys = {trusted_with("key-c", keypair_b())};
        mod::apply_install_policy_overrides(*policy, overrides);
        expect(!policy->allow_downgrade);
        expect(!policy->require_signature);
        expect(policy->trusted_keys.size() == 3U);
        expect(policy->replace_same_version)
            << "unrelated switches must keep their configured value";
    };

    "policy/rejects unknown keys and malformed anchors"_test = [] {
        temp_workspace work{"policy-bad"};
        const auto path{work.path("mods.toml").string()};
        expect(work.write("mods.toml", "[signature]\nrequire = true\n"
                                       "unexpected = 1\n"));
        auto unknown{mod::load_install_policy(path)};
        expect(!unknown);
        if (!unknown) {
            expect(unknown.error().code == error_code::parse_error);
            expect(unknown.error().message.find("unexpected") !=
                   std::string::npos);
        }

        expect(work.write("mods.toml", "[trust]\n\"key-a\" = \"not-a-key\"\n"));
        auto bad_anchor{mod::load_install_policy(path)};
        expect(!bad_anchor);
        if (!bad_anchor) {
            expect(bad_anchor.error().code == error_code::parse_error);
        }

        expect(work.write("mods.toml", "[rotation.\"x\"]\n"
                                       "old_key_id = \"a\"\n"));
        auto bad_rotation{mod::load_install_policy(path)};
        expect(!bad_rotation) << "a rotation without new_key_id was accepted";

        expect(work.write("mods.toml", "[other]\nx = 1\n"));
        expect(!mod::load_install_policy(path));

        // A missing file is `not_found`, not a silent empty policy.
        auto missing{
            mod::load_install_policy(work.path("absent.toml").string())};
        expect(!missing);
        if (!missing) {
            expect(missing.error().code == error_code::not_found);
        }
    };

    "policy/default requires a trusted signature and strict continuity"_test =
        [] {
            const mod::install_policy_config defaults{};
            expect(defaults.require_signature);
            expect(defaults.signature_policy ==
                   mod::rfmod_signature_policy::strict);
            expect(!defaults.allow_downgrade);
            expect(!defaults.replace_same_version);
            expect(!defaults.keep_signer_pin);
            expect(defaults.trusted_keys.empty());
        };

    "policy/file drives an install end to end"_test = [] {
        temp_workspace work{"policy-install"};
        expect(materialise(work, "mod1", "refrain.test.policy", "1.0.0"));
        auto first{pack(
            work, "mod1", "out/v1.rfmod",
            rfmod_write_options{.signing = signing_with(key_a, keypair_a())})};
        expect(first.has_value());
        if (!first) {
            return;
        }
        const auto config_dir{work.path("config")};
        std::error_code error;
        fs::create_directories(config_dir, error);
        expect(!error);
        expect(work.write(
            "config/mods.toml",
            fmt::format("[signature]\nrequire = true\n"
                        "[trust]\n\"{}\" = \"{}\"\n"
                        "[rotation.\"refrain.test.policy\"]\n"
                        "old_key_id = \"{}\"\n"
                        "new_key_id = \"{}\"\n",
                        key_a, refrain::data::to_hex(keypair_a().public_key),
                        key_a, key_b)));
        auto policy{
            mod::load_install_policy((config_dir / "mods.toml").string())};
        expect(static_cast<bool>(policy));
        if (!policy) {
            return;
        }
        auto store{make_store(work)};
        expect(store.has_value());
        if (!store) {
            return;
        }
        mod::rfmod_install_options options;
        mod::apply_install_policy(*policy, options);
        auto installed{install_archive(*store, *first, options)};
        expect(static_cast<bool>(installed))
            << "policy-driven install failed: "
            << (installed ? std::string{} : installed.error().message);
        if (!installed) {
            return;
        }
        expect(installed->signed_package);

        // The policy's rotation allowlist admits a new signer without any
        // command-line flags.
        expect(materialise(work, "mod2", "refrain.test.policy", "1.1.0"));
        auto second{pack(
            work, "mod2", "out/v2.rfmod",
            rfmod_write_options{.signing = signing_with(key_b, keypair_b())})};
        expect(second.has_value());
        if (!second) {
            return;
        }
        policy->trusted_keys.push_back(trusted_with(key_b, keypair_b()));
        mod::rfmod_install_options update;
        mod::apply_install_policy(*policy, update);
        auto updated{install_archive(*store, *second, update)};
        expect(static_cast<bool>(updated)) << "policy rotation install failed";
        if (updated) {
            expect(updated->signer_rotated);
        }
    };
};

// ---------------------------------------------------------------------------
// runtime catalog: receipts only
// ---------------------------------------------------------------------------

const suite mod_catalog_tests = [] {
    "catalog/scan_installed reads receipts and ignores directory mods"_test =
        [] {
            temp_workspace work{"catalog"};
            expect(
                materialise(work, "mod1", "refrain.test.installed", "1.0.0"));
            auto archive{pack(work, "mod1", "out/mod.rfmod")};
            expect(archive.has_value());
            if (!archive) {
                return;
            }
            auto store{make_store(work)};
            expect(store.has_value());
            if (!store) {
                return;
            }
            auto installed{install_archive(*store, *archive)};
            expect(static_cast<bool>(installed));
            if (!installed) {
                return;
            }
            // A directory mod under the same root must not become a candidate.
            expect(work.write(
                "store/mods/refrain.test.sideloaded/manifest.toml",
                source_manifest("refrain.test.sideloaded", "1.0.0")));
            expect(work.write("store/mods/refrain.test.sideloaded/data/a.json",
                              "{}"));

            const std::vector<std::string> roots{work.path("store").string()};
            auto catalog{mod::scan_installed(roots)};
            expect(static_cast<bool>(catalog)) << "scan_installed failed";
            if (!catalog) {
                return;
            }
            expect(catalog->size() == 1U);
            if (catalog->size() != 1U) {
                return;
            }
            const auto &candidate{catalog->front()};
            expect(candidate.id == "refrain.test.installed");
            expect(candidate.version == "1.0.0");
            expect(candidate.kind == mod::package_kind::rfmod);
            expect(candidate.origin == mod::mod_origin::user);
            expect(same_path(candidate.path, installed->receipt_path));
            expect(hex_of(candidate.manifest_hash) ==
                   hex_of(installed->manifest_digest));
            expect(!candidate.is_external());

            // The candidate's package source reads the manifest back from the
            // content store and verifies it against the receipt.
            auto source{mod::make_receipt_package_source(candidate.path)};
            expect(static_cast<bool>(source)) << "receipt package source";
            if (!source) {
                return;
            }
            expect((*source)->kind() == mod::package_kind::rfmod);
            auto manifest{(*source)->read("manifest.toml")};
            expect(static_cast<bool>(manifest));
            if (manifest) {
                expect(hex_of(data::blake3(*manifest)) ==
                       hex_of(installed->manifest_digest));
            }
            auto missing{(*source)->read("does/not/exist")};
            expect(!missing);
        };

    "catalog/scan_installed fails closed on a damaged receipt or missing blob"_test =
        [] {
            temp_workspace work{"catalog-damaged"};
            expect(
                materialise(work, "mod1", "refrain.test.installed", "1.0.0"));
            auto archive{pack(work, "mod1", "out/mod.rfmod")};
            expect(archive.has_value());
            if (!archive) {
                return;
            }
            auto store{make_store(work)};
            expect(store.has_value());
            if (!store) {
                return;
            }
            auto installed{install_archive(*store, *archive)};
            expect(static_cast<bool>(installed));
            if (!installed) {
                return;
            }
            const std::vector<std::string> roots{work.path("store").string()};
            auto receipt_bytes{io::read_text(installed->receipt_path)};
            expect(receipt_bytes.has_value());
            if (!receipt_bytes) {
                return;
            }
            expect(work.write("store/mods/refrain.test.installed/1.0.0.json",
                              "{"));
            auto corrupt{mod::scan_installed(roots)};
            expect(!corrupt);
            if (!corrupt) {
                expect(corrupt.error().code == error_code::parse_error);
            }
            expect(work.write("store/mods/refrain.test.installed/1.0.0.json",
                              *receipt_bytes));

            // Drop the blob table: the receipt is valid but its manifest blob
            // cannot be read back, so the catalog fails closed instead of
            // reporting an unloadable candidate.
            std::error_code error;
            fs::remove(work.path("store/content/blobs.idx"), error);
            expect(!error);
            auto no_table{mod::scan_installed(roots)};
            expect(!no_table);
            if (!no_table) {
                expect(no_table.error().code == error_code::not_found);
            }
        };
};

} // namespace
